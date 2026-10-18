#pragma once

#include "Basalt/Core/JobSystem.h"
#include "Basalt/Physics/JoltUtils.h"

#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Physics/PhysicsSystem.h>

namespace Basalt {

	// Jolt dependencies and barriers backed by the engine pool, without a second set of workers.
	// The completion fence includes queue references, which may outlive Jolt's barrier notification.
	class PhysicsJobs final : public JPH::JobSystemWithBarrier
	{
	public:
		PhysicsJobs()
			: JPH::JobSystemWithBarrier(JPH::cMaxPhysicsBarriers)
		{}
		~PhysicsJobs() override { Basalt::JobSystem::Get().WaitIdle(); }
		int GetMaxConcurrency() const override { return static_cast<int>(Basalt::JobSystem::Get().GetWorkerCount()) + 1; }
		JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function, JPH::uint32 dependencies = 0) override
		{
			auto* job = new Job(name, color, this, function, dependencies);
			JobHandle handle(job);
			if (dependencies == 0)
				QueueJob(job);
			return handle;
		}

	protected:
		void FreeJob(Job* job) override { delete job; }
		void QueueJob(Job* job) override
		{
			job->AddRef();
			// A shared capture releases the queue's reference even if submission throws.
			auto reference = std::shared_ptr<Job>(job, [](Job* queued) { queued->Release(); });
			(void)Basalt::JobSystem::Get().Submit([reference] { reference->Execute(); });
		}
		void QueueJobs(Job** jobs, JPH::uint count) override
		{
			for (JPH::uint i = 0; i < count; ++i)
				QueueJob(jobs[i]);
		}
	};

}
