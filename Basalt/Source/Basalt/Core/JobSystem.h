#pragma once

#include "Basalt/Core/Base.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Basalt {

	class JobSystem;

	// A completion fence. Handles may outlive their pool; completion publishes all job writes.
	class JobHandle
	{
	public:
		explicit operator bool() const { return !!m_State; }
		bool IsComplete() const { return !m_State || m_State->Complete.load(std::memory_order_acquire); }
		bool Succeeded() const { return m_State && IsComplete() && m_State->Success; }

	private:
		friend class JobSystem;
		struct State
		{
			std::atomic<bool> Complete{ false };
			bool Success = false;
			const JobSystem* Owner = nullptr;
		};
		Ref<State> m_State;
	};

	// Shared CPU pool: workers take local work LIFO and steal FIFO. Waiting callers help execute
	// queued work, so a worker may submit and wait for children even with only one worker.
	// Callers must not create cyclic waits; nested cooperative waits consume stack depth.
	// Destroy on the constructing thread after producers stop;
	// destruction drains accepted work, including children submitted by running jobs.
	class JobSystem
	{
	public:
		explicit JobSystem(uint32_t workerCount = DefaultWorkerCount());
		~JobSystem();
		JobSystem(const JobSystem&) = delete;
		JobSystem& operator=(const JobSystem&) = delete;

		static JobSystem& Get();
		static uint32_t DefaultWorkerCount();
		uint32_t GetWorkerCount() const { return static_cast<uint32_t>(m_Workers.size()); }
		// Rejects empty functions and external submissions during shutdown; running jobs may submit children.
		[[nodiscard]] JobHandle Submit(std::function<void()> function);
		// Returns false for an empty/rejected/failed job. Exceptions are logged at the job boundary.
		// The caller can execute unrelated queued work, so this may take longer than the target job.
		bool Wait(const JobHandle& handle);
		// Synchronous parallel ranges [begin, end), useful for animation/culling with disjoint outputs.
		bool ParallelFor(uint32_t count, uint32_t grain, const std::function<void(uint32_t, uint32_t)>& function);
		// Caller must be outside this pool's jobs; waits for every accepted task and capture release.
		void WaitIdle();
		bool IsInJob() const;

	private:
		struct Task
		{
			std::function<void()> Function;
			Ref<JobHandle::State> State;
		};
		bool Take(Task& task, size_t queue);
		void Execute(Task& task, size_t queue);
		void Worker(size_t queue);

		std::mutex m_Mutex;
		std::condition_variable m_Wake;
		std::vector<std::deque<Task>> m_Queues;
		std::vector<std::thread> m_Workers;
		size_t m_NextQueue = 0;
		size_t m_Queued = 0;
		size_t m_Outstanding = 0;
		bool m_Stopping = false;
	};

}
