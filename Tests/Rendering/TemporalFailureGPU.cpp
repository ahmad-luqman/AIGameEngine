#include "FailingDevice.h"
#include "GPUFixture.h"

#include <Basalt/Renderer/SceneRenderer.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

using namespace Basalt;

TEST_CASE("Temporal and reset-frame spatial allocation failures abort cleanly and require an explicit retry")
{
	auto exercise = [](const char* failedTexture) {
		auto graphics = CreateTestGraphics();
		nvrhi::RefCountPtr<Tests::FailingDevice> device;
		device.Attach(new Tests::FailingDevice(graphics->GetDevice()));
		Scene scene;
		scene.CreateEntity("No sky").AddComponent<SkyLightComponent>().ShowBackground = false;
		auto& post = scene.CreateEntity("TAA").AddComponent<PostProcessComponent>();
		post.TAAEnabled = true;
		post.SSAOEnabled = false;
		auto mesh = scene.CreateEntity("Opaque mesh");
		mesh.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
		mesh.GetTransform().Translation.z = .5f;
		mesh.GetTransform().Scale = glm::vec3(.4f);
		SceneRenderer renderer(device);
		REQUIRE(renderer.Initialize());
		renderer.SetViewportSize(32, 32);
		device->FailTexture(failedTexture);
		const RenderCamera camera;
		auto render = [&]() { renderer.Render(scene, camera, 1.0f / 60); };
		auto checkFailed = [&]() {
			CHECK(renderer.GetStatistics().Failed);
			CHECK_FALSE(renderer.GetStatistics().TAAActive);
			CHECK_FALSE(renderer.GetStatistics().TAAHistoryValid);
		};
		// Both failures occur after camera history begins; a live timing scope here used to assert on abort.
		render();
		REQUIRE(device->GetFailureCount() == 1);
		checkFailed();
		device->FailTexture("");
		// A transient allocator recovery alone does not silently change the requested antialiasing policy.
		render();
		checkFailed();
		CHECK(device->GetFailureCount() == 1);
		SUBCASE("Retry by toggling TAA")
		{
			post.TAAEnabled = false;
			render();
			REQUIRE_FALSE(renderer.GetStatistics().Failed);
			CHECK_FALSE(renderer.GetStatistics().TAAActive);
			post.TAAEnabled = true;
		}
		SUBCASE("Retry by resizing")
		{
			renderer.SetViewportSize(40, 32);
		}
		render();
		REQUIRE_FALSE(renderer.GetStatistics().Failed);
		CHECK(renderer.GetStatistics().TAAActive);
		CHECK_FALSE(renderer.GetStatistics().TAAHistoryValid);
		render();
		REQUIRE_FALSE(renderer.GetStatistics().Failed);
		CHECK(renderer.GetStatistics().TAAActive);
		CHECK(renderer.GetStatistics().TAAHistoryValid);
		CHECK(device->GetFailureCount() == 1);
		CHECK(graphics->GetValidationErrorCount() == 0);
	};
	SUBCASE("Temporal history allocation")
	{
		exercise("TAAColor");
	}
	SUBCASE("Reset-frame FXAA allocation")
	{
		exercise("FXAAInput");
	}
}
