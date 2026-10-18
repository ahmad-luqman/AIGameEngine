#pragma once

#include "Basalt/Core/JobSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Physics/JoltUtils.h"

#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Physics/PhysicsSettings.h>

#include <exception>
#include <string>

namespace Basalt {

	// Jolt dependencies and barriers backed by the engine pool, without a second set of workers.
	// Process lifetime keeps queued references valid after a world releases its completed barriers.
	class PhysicsJobs final : public JPH::JobSystemWithBarrier
	{
	public:
		static PhysicsJobs& Get()
		{
			// Construct the pool first: at exit this adapter waits for the whole pool to become idle
			// (including imports) before releasing barriers. World teardown never waits for unrelated work.
			(void)Basalt::JobSystem::Get();
			static PhysicsJobs jobs;
			return jobs;
		}
		~PhysicsJobs() override { Basalt::JobSystem::Get().WaitIdle(); }
		// Jolt barrier waits let the calling thread execute ready jobs too.
		int GetMaxConcurrency() const override { return static_cast<int>(Basalt::JobSystem::Get().GetWorkerCount()) + 1; }
		JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function, JPH::uint32 dependencies = 0) override
		{
			// Guard the Jolt function itself: either a worker or a barrier-waiting thread can execute it.
			// Jolt cannot unwind a running job: it would leave the barrier permanently unsignalled.
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

	private:
		PhysicsJobs()
			: JPH::JobSystemWithBarrier(JPH::cMaxPhysicsBarriers)
		{}

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
