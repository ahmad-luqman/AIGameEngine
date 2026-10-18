#include <doctest/doctest.h>

#include <Basalt/Core/JobSystem.h>

#include <atomic>
#include <latch>
#include <stdexcept>
#include <vector>

using namespace Basalt;

TEST_SUITE("JobSystem")
{
	TEST_CASE("Nested children complete with one worker and cooperative waits")
	{
		JobSystem pool(1);
		std::atomic<uint32_t> visits{ 0 };
		const auto parent = pool.Submit([&] {
			const auto child = pool.Submit([&] {
				const auto grandchild = pool.Submit([&] { ++visits; });
				if (pool.Wait(grandchild))
					++visits;
			});
			if (pool.Wait(child))
				++visits;
		});
		CHECK(pool.Wait(parent));
		CHECK(visits == 3);
		CHECK(parent.IsComplete());
		CHECK(parent.Succeeded());
	}

	TEST_CASE("Parallel ranges cover every item exactly once and publish writes")
	{
		JobSystem pool(3);
		std::vector<uint32_t> values(1003);
		CHECK(pool.ParallelFor(static_cast<uint32_t>(values.size()), 17, [&](uint32_t begin, uint32_t end) {
			for (uint32_t i = begin; i < end; ++i)
				++values[i];
		}));
		for (const auto value : values)
			CHECK(value == 1);
		CHECK(pool.ParallelFor(0, 17, [](uint32_t, uint32_t) {}));
		CHECK_FALSE(pool.ParallelFor(10, 0, [](uint32_t, uint32_t) {}));
		CHECK_FALSE(pool.ParallelFor(10, 1, {}));
	}

	TEST_CASE("Concurrent producers do not lose work")
	{
		JobSystem pool(3);
		std::atomic<uint32_t> sum{ 0 };
		std::vector<std::thread> producers;
		producers.reserve(4);
		for (uint32_t p = 0; p < 4; ++p)
			producers.emplace_back([&] {
				for (uint32_t i = 0; i < 100; ++i)
					(void)pool.Submit([&] { ++sum; });
			});
		for (auto& producer : producers)
			producer.join();
		pool.WaitIdle();
		CHECK(sum == 400);
	}

	TEST_CASE("Completion releases queue captures before publishing the fence")
	{
		JobSystem pool(1);
		std::latch run(1);
		std::latch destructionStarted(1);
		std::latch finishDestruction(1);
		struct Captured
		{
			std::latch& Run;
			std::latch& Started;
			std::latch& Finish;
			Captured(std::latch& runJob, std::latch& started, std::latch& finish)
				: Run(runJob)
				, Started(started)
				, Finish(finish)
			{}
			~Captured()
			{
				Started.count_down();
				Finish.wait();
			}
		};
		auto object = CreateRef<Captured>(run, destructionStarted, finishDestruction);
		const auto handle = pool.Submit([object] { object->Run.wait(); });
		object.reset();
		run.count_down();
		destructionStarted.wait();
		// A moved-from std::function may still hold its callable. The last queue capture must be
		// destroyed before Complete, even when its destructor blocks and no caller helps execute.
		CHECK_FALSE(handle.IsComplete());
		finishDestruction.count_down();
		pool.WaitIdle();
		CHECK(handle.Succeeded());
	}

	TEST_CASE("Exceptions fail their fence without killing a worker or the remaining batch")
	{
		JobSystem pool(1);
		CHECK_FALSE(pool.Wait(pool.Submit([] { throw std::runtime_error("expected job test failure"); })));
		// Exercise containment of non-standard exceptions at the worker boundary.
		CHECK_FALSE(pool.Wait(pool.Submit([] { throw 1; }))); // NOLINT(bugprone-std-exception-baseclass)
		CHECK_FALSE(pool.Wait({}));
		CHECK_FALSE(pool.Wait(pool.Submit({})));
		CHECK(pool.Wait(pool.Submit([] {})));
		CHECK_FALSE(pool.ParallelFor(5, 1, [](uint32_t begin, uint32_t) {
			if (begin == 2)
				throw std::runtime_error("expected range failure");
		}));
	}

	TEST_CASE("Destruction drains children and completion handles outlive the pool")
	{
		JobHandle handle;
		std::atomic<uint32_t> visits{ 0 };
		{
			JobSystem pool(2);
			handle = pool.Submit([&] {
				for (uint32_t i = 0; i < 64; ++i)
					(void)pool.Submit([&] { ++visits; });
			});
		}
		CHECK(handle.IsComplete());
		CHECK(handle.Succeeded());
		CHECK(visits == 64);
	}

	TEST_CASE("An unfinished foreign pool handle is rejected without waiting on the wrong wakeup")
	{
		JobSystem owner(1);
		JobSystem other(1);
		std::latch started(1);
		std::latch release(1);
		const auto job = owner.Submit([&] {
			started.count_down();
			release.wait();
		});
		started.wait();
		CHECK_FALSE(other.Wait(job));
		release.count_down();
		CHECK(owner.Wait(job));
		CHECK(other.Wait(job)); // Completed fences may be observed anywhere.
	}

	TEST_CASE("Idle workers steal children from an occupied worker queue")
	{
		JobSystem pool(2);
		std::latch started(1);
		std::latch childFinished(1);
		const auto parent = pool.Submit([&] {
			(void)pool.Submit([&] { childFinished.count_down(); });
			started.count_down();
			// No cooperative wait: another worker must steal the child to release this worker.
			childFinished.wait();
		});
		started.wait();
		// WaitIdle does not execute jobs on this thread, so stealing is required.
		pool.WaitIdle();
		CHECK(parent.Succeeded());
	}
}
