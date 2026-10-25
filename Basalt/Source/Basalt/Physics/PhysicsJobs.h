#pragma once

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/JobSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Physics/JoltUtils.h"

#include <Jolt/Core/JobSystem.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

namespace Basalt {

	// Jolt dependencies and barriers backed by the engine pool, without a second set of workers.
	// Process lifetime keeps queued job references valid after a world finishes with its barriers.
	// Barriers are Basalt's own rather than JPH::JobSystemWithBarrier's: on Linux those wait in sem_wait and ignore its
	// result, so a signal handler installed without SA_RESTART (libFuzzer's timer) ended a wait early and a physics step
	// continued past running jobs.
	class PhysicsJobs final : public JPH::JobSystem
	{
	public:
		static PhysicsJobs& Get()
		{
			// Construct the pool first: at exit this adapter waits for the whole pool to become idle (including imports),
			// since queued tasks hold job references whose release calls FreeJob here. World teardown never waits for
			// unrelated work.
			(void)Basalt::JobSystem::Get();
			static PhysicsJobs jobs;
			return jobs;
		}
		~PhysicsJobs() override { Basalt::JobSystem::Get().WaitIdle(); }
		// Barrier waits (PhysicsBarrier::Wait) let the calling thread execute ready jobs too.
		int GetMaxConcurrency() const override { return static_cast<int>(Basalt::JobSystem::Get().GetWorkerCount()) + 1; }
		JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function, JPH::uint32 dependencies = 0) override
		{
			// Guard the Jolt function itself: either a worker or a barrier-waiting thread can execute it.
			// Jolt cannot unwind a running job: it would never report to its barrier, and the wait would block forever.
			JobFunction guarded = [function, label = std::string(name)]() noexcept {
				try
				{
					function();
				}
				catch (const std::exception& error)
				{
					BS_CORE_CRITICAL("Physics job '{}': {}", label, error.what());
					std::terminate();
				}
				catch (...)
				{
					BS_CORE_CRITICAL("Physics job '{}': unknown exception", label);
					std::terminate();
				}
			};
			auto* job = new Job(name, color, this, guarded, dependencies);
			JobHandle handle(job);
			if (dependencies == 0)
				QueueJob(job);
			return handle;
		}
		Barrier* CreateBarrier() override
		{
			// Jolt uses the barrier without checking it (PhysicsSystem::Update), so a failed allocation is fatal.
			try
			{
				return new PhysicsBarrier();
			}
			catch (...)
			{
				BS_CORE_CRITICAL("Physics barrier allocation failed");
				std::terminate();
			}
		}
		void DestroyBarrier(Barrier* barrier) override { delete static_cast<PhysicsBarrier*>(barrier); }
		void WaitForJobs(Barrier* barrier) override { static_cast<PhysicsBarrier*>(barrier)->Wait(); }

	private:
		PhysicsJobs() = default;

		// Jobs added to a barrier, waited for under a mutex and condition variable. Jobs can still be added while a
		// thread waits (physics jobs add follow-up jobs), and the waiting thread runs jobs that are ready meanwhile. A
		// dependency removed from outside the barrier's jobs does not wake it: a pool worker runs that job unless
		// another report or addition wakes the waiter first. Every change notifies, so any number of threads may wait.
		class PhysicsBarrier final : public Barrier
		{
		public:
			// A job that has not reported yet still points here: destroy only after Wait.
			~PhysicsBarrier() override { BS_CORE_ASSERT(m_Unfinished == 0, "Physics barrier destroyed with {} unfinished jobs", m_Unfinished); }

			void AddJob(const JPH::JobHandle& job) override { AddJobs(&job, 1); }
			void AddJobs(const JPH::JobHandle* jobs, JPH::uint count) override
			{
				// SetBarrier and the count change together under the lock, so a job that finishes at once cannot report
				// before it is counted. Room is reserved first: a job that joined must be counted.
				std::scoped_lock lock(m_Mutex);
				try
				{
					// Geometric growth: Jolt adds follow-up jobs one at a time.
					if (m_Jobs.capacity() - m_Jobs.size() < count)
						m_Jobs.reserve(std::max(m_Jobs.size() + count, m_Jobs.capacity() * 2));
				}
				catch (const std::exception& error)
				{
					BS_CORE_CRITICAL("Physics barrier allocation failed: {}", error.what());
					std::terminate();
				}
				for (JPH::uint i = 0; i < count; ++i)
				{
					// SetBarrier fails once a job's function has returned (Execute closes the barrier slot before marking
					// the job done, so IsDone may still be false) and, against Jolt's contract, for a job added twice or
					// in another barrier, which Jolt asserts in Debug.
					if (jobs[i].GetPtr()->SetBarrier(this))
					{
						m_Jobs.push_back(jobs[i]);
						++m_Unfinished;
					}
				}
				m_Changed.notify_all();
			}

			void Wait()
			{
				std::unique_lock lock(m_Mutex);
				// Waits for every OnJobFinished, not for jobs to look done: a job is done just before it reports, and
				// the barrier must outlive that call. Spurious and signal wake-ups just scan again.
				while (m_Unfinished > 0)
				{
					while (m_First < m_Jobs.size() && m_Jobs[m_First].IsDone())
						++m_First;
					JPH::JobHandle ready;
					for (size_t i = m_First; i < m_Jobs.size(); ++i)
					{
						if (m_Jobs[i].GetPtr()->CanBeExecuted())
						{
							ready = m_Jobs[i];
							break;
						}
					}
					if (!ready.IsValid())
					{
						m_Changed.wait(lock);
						continue;
					}
					// Unlocked: the job may add jobs or report here. Execute does nothing when a worker started it first.
					lock.unlock();
					ready.GetPtr()->Execute();
					lock.lock();
				}
				m_Jobs.clear();
				m_First = 0;
			}

		protected:
			void OnJobFinished(Job* job) override
			{
				// Notified before unlocking: the waiter may return and destroy the barrier as soon as it gets the lock.
				std::scoped_lock lock(m_Mutex);
				BS_CORE_ASSERT(m_Unfinished > 0, "Physics job reported to a barrier it did not join");
				--m_Unfinished;
				m_Changed.notify_all();
			}

		private:
			std::mutex m_Mutex;
			std::condition_variable m_Changed;
			std::vector<JPH::JobHandle> m_Jobs;
			size_t m_First = 0;
			uint32_t m_Unfinished = 0;
		};

	protected:
		void FreeJob(Job* job) override { delete job; }
		void QueueJob(Job* job) noexcept override
		{
			try
			{
				// Jolt's intrusive handle keeps the queued reference alive through execution.
				JobHandle reference(job);
				if (!Basalt::JobSystem::Get().Submit([reference] { reference.GetPtr()->Execute(); }))
				{
					BS_CORE_CRITICAL("Physics job submission rejected");
					std::terminate();
				}
			}
			catch (...)
			{
				BS_CORE_CRITICAL("Physics job submission failed");
				std::terminate();
			}
		}
		void QueueJobs(Job** jobs, JPH::uint count) override
		{
			for (JPH::uint i = 0; i < count; ++i)
				QueueJob(jobs[i]);
		}
	};

}
