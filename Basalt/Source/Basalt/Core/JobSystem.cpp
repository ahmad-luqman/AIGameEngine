#include "Basalt/Core/JobSystem.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/Profiler.h"

#include <algorithm>
#include <exception>

namespace Basalt {

	namespace {
		thread_local JobSystem* s_CurrentPool = nullptr;
		thread_local size_t s_CurrentQueue = 0;
	}

	uint32_t JobSystem::DefaultWorkerCount()
	{
		const uint32_t cores = std::thread::hardware_concurrency();
		return cores > 1 ? cores - 1 : 1;
	}

	JobSystem& JobSystem::Get()
	{
		static JobSystem pool;
		return pool;
	}

	JobSystem::JobSystem(uint32_t workerCount)
		: m_Queues(std::max(1u, workerCount))
	{
		try
		{
			for (size_t i = 0; i < m_Queues.size(); ++i)
				m_Workers.emplace_back([this, i] { Worker(i); });
		}
		catch (const std::exception& error)
		{
			if (m_Workers.empty())
			{
				BS_CORE_CRITICAL("Cannot start any job worker: {}", error.what());
				std::terminate();
			}
			BS_CORE_WARN("Started only {} of {} job workers: {}", m_Workers.size(), m_Queues.size(), error.what());
		}
	}

	JobSystem::~JobSystem()
	{
		BS_CORE_ASSERT(!IsInJob(), "A job cannot destroy its own pool");
		{
			std::lock_guard lock(m_Mutex);
			m_Stopping = true;
		}
		m_Wake.notify_all();
		for (auto& worker : m_Workers)
			worker.join();
	}

	JobHandle JobSystem::Submit(std::function<void()> function)
	{
		JobHandle handle;
		if (!function)
			return handle;
		{
			std::unique_lock lock(m_Mutex);
			if (m_Stopping && s_CurrentPool != this)
			{
				lock.unlock();
				BS_CORE_WARN("Job submission rejected: pool is stopping");
				return handle;
			}
			auto state = CreateRef<JobHandle::State>();
			state->Owner = this;
			const size_t queue = s_CurrentPool == this ? s_CurrentQueue : m_NextQueue++ % m_Queues.size();
			m_Queues[queue].push_back({ std::move(function), state });
			handle.m_State = std::move(state);
			++m_Queued;
			++m_Outstanding;
		}
		m_Wake.notify_all();
		return handle;
	}

	bool JobSystem::Take(Task& task, size_t queue)
	{
		// m_Mutex is held by every caller.
		for (size_t offset = 0; offset < m_Queues.size(); ++offset)
		{
			auto& jobs = m_Queues[(queue + offset) % m_Queues.size()];
			if (jobs.empty())
				continue;
			if (offset == 0)
			{
				task = std::move(jobs.back());
				jobs.pop_back();
			}
			else
			{
				task = std::move(jobs.front());
				jobs.pop_front();
			}
			--m_Queued;
			return true;
		}
		return false;
	}

	void JobSystem::Execute(Task& task, size_t queue)
	{
		BS_PROFILE_SCOPE("Job");
		JobSystem* previousPool = s_CurrentPool;
		const size_t previousQueue = s_CurrentQueue;
		s_CurrentPool = this;
		s_CurrentQueue = queue;
		try
		{
			task.Function();
			task.State->Success = true;
		}
		catch (const std::exception& error)
		{
			BS_CORE_ERROR("Job failed: {}", error.what());
		}
		catch (...)
		{
			BS_CORE_ERROR("Job failed: unknown exception");
		}
		// Wait/WaitIdle require queue-owned captures (including Jolt references) released before completion.
		// Execute by reference: moving std::function may retain a callable in its source.
		task.Function = {};
		s_CurrentPool = previousPool;
		s_CurrentQueue = previousQueue;
		{
			std::lock_guard lock(m_Mutex);
			task.State->Complete.store(true, std::memory_order_release);
			--m_Outstanding;
		}
		m_Wake.notify_all();
	}

	void JobSystem::Worker(size_t queue)
	{
		BS_PROFILE_THREAD("Basalt worker");
		for (;;)
		{
			Task task;
			{
				std::unique_lock lock(m_Mutex);
				m_Wake.wait(lock, [this] { return m_Queued != 0 || (m_Stopping && m_Outstanding == 0); });
				if (!Take(task, queue))
					return;
			}
			Execute(task, queue);
		}
	}

	bool JobSystem::Wait(const JobHandle& handle)
	{
		if (handle.m_State && handle.m_State->Owner != this && !handle.IsComplete())
		{
			BS_CORE_ERROR("Cannot wait on an unfinished job from another pool");
			return false;
		}
		const size_t queue = s_CurrentPool == this ? s_CurrentQueue : 0;
		while (!handle.IsComplete())
		{
			Task task;
			{
				std::unique_lock lock(m_Mutex);
				m_Wake.wait(lock, [&] { return handle.IsComplete() || m_Queued != 0; });
				if (handle.IsComplete())
					break;
				if (!Take(task, queue))
					continue;
			}
			Execute(task, queue);
		}
		return handle.Succeeded();
	}

	bool JobSystem::IsInJob() const
	{
		return s_CurrentPool == this;
	}

	void JobSystem::WaitIdle()
	{
		BS_CORE_ASSERT(!IsInJob(), "A job cannot wait for its own outstanding count");
		std::unique_lock lock(m_Mutex);
		m_Wake.wait(lock, [this] { return m_Outstanding == 0; });
	}

	bool JobSystem::ParallelFor(uint32_t count, uint32_t grain, const std::function<void(uint32_t, uint32_t)>& function)
	{
		BS_CORE_ASSERT(grain > 0, "ParallelFor grain must be positive");
		// Keep the invalid-grain guard in Dist, where the programmer assertion is compiled out.
		if (!function || grain == 0)
		{
			BS_CORE_ERROR("ParallelFor requires a function and positive grain");
			return false;
		}
		std::vector<JobHandle> jobs;
		bool success = true;
		try
		{
			// Reserve before submitting, so every accepted job always has a fence we can join.
			jobs.reserve(count / grain + (count % grain != 0 ? 1u : 0u));
			for (uint32_t begin = 0; begin < count;)
			{
				const uint32_t end = begin + std::min(grain, count - begin);
				jobs.push_back(Submit([&function, begin, end] { function(begin, end); }));
				begin = end;
			}
		}
		catch (const std::exception& error)
		{
			BS_CORE_ERROR("ParallelFor submission failed: {}", error.what());
			success = false;
		}
		catch (...)
		{
			BS_CORE_ERROR("ParallelFor submission failed: unknown exception");
			success = false;
		}
		for (const auto& job : jobs)
			success = Wait(job) && success;
		return success;
	}

}
