#include <doctest/doctest.h>

#include <Basalt/Core/Application.h>

#include <thread>

using namespace Basalt;

namespace {

	struct CountingLayer : Layer
	{
		int Updates = 0;
		int Renders = 0;
		float TotalTime = 0.0f;
		void OnUpdate(Timestep ts) override
		{
			Updates++;
			TotalTime += ts.GetSeconds();
		}
		void OnRender() override { Renders++; }
	};

	ApplicationSpecification MakeHeadlessSpecification()
	{
		ApplicationSpecification specification;
		specification.Name = "HeadlessTest";
		specification.Headless = true;
		specification.FixedTimestep = 1.0f / 60.0f;
		return specification;
	}

}

TEST_SUITE("Application")
{
	TEST_CASE("Headless application runs a fixed number of deterministic frames")
	{
		ApplicationSpecification specification = MakeHeadlessSpecification();
		specification.MaxFrames = 10;

		Application application(specification);
		REQUIRE(application.IsInitialized());
		CHECK(application.IsHeadless());
		CHECK(application.GetWindow() == nullptr);
		CHECK(application.GetGraphicsDevice() == nullptr);

		auto* layer = new CountingLayer();
		application.PushLayer(layer);
		CHECK(application.Run() == 0);

		CHECK(layer->Updates == 10);
		CHECK(layer->Renders == 0);
		CHECK(layer->TotalTime == doctest::Approx(10.0f / 60.0f));
		CHECK(application.GetFrameCount() == 10);
	}

	TEST_CASE("SubmitToMainThread runs queued work on the main loop, including from other threads")
	{
		ApplicationSpecification specification = MakeHeadlessSpecification();
		specification.MaxFrames = 1000;
		Application application(specification);

		int executed = 0;
		std::thread worker([&]
						   {
			application.SubmitToMainThread([&] { executed++; });
			application.SubmitToMainThread([&] {
				executed++;
				Application::Get().Close();
			}); });
		worker.join();

		CHECK(application.Run() == 0);
		CHECK(executed == 2);
		CHECK(application.GetFrameCount() == 1);
	}

	TEST_CASE("Layers pushed from a layer callback are added safely on the next frame")
	{
		struct SpawningLayer : Layer
		{
			CountingLayer* Spawned = nullptr;
			void OnUpdate(Timestep) override
			{
				if (Spawned)
					return;
				// Several pushes in one frame would reallocate the layer vector mid-iteration.
				for (int i = 0; i < 8; i++)
				{
					auto* layer = new CountingLayer();
					if (!Spawned)
						Spawned = layer;
					Application::Get().PushLayer(layer);
				}
			}
		};

		ApplicationSpecification specification = MakeHeadlessSpecification();
		specification.MaxFrames = 3;
		Application application(specification);
		auto* spawner = new SpawningLayer();
		application.PushLayer(spawner);
		CHECK(application.Run() == 0);

		REQUIRE(spawner->Spawned != nullptr);
		// Spawned during frame 1, added at the start of frame 2: updated in frames 2 and 3.
		CHECK(spawner->Spawned->Updates == 2);
	}

	TEST_CASE("Application singleton lifetime")
	{
		CHECK_FALSE(Application::HasInstance());
		{
			Application application(MakeHeadlessSpecification());
			CHECK(Application::HasInstance());
			CHECK(&Application::Get() == &application);
		}
		CHECK_FALSE(Application::HasInstance());
	}
}
