#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "GPUFixture.h"

#include <Basalt/Animation/AnimationPose.h>
#include <Basalt/Animation/AnimationSystem.h>
#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/ImageUtils.h>
#include <Basalt/Core/FileSystem.h>
#include <Basalt/Renderer/SceneRenderer.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <filesystem>

using namespace Basalt;

namespace {
	struct ClearAssetCache
	{
		~ClearAssetCache() { AssetManager::Clear(); }
	};
	std::vector<uint8_t> Capture(SceneRenderer& renderer, Scene& scene, const RenderCamera& camera)
	{
		renderer.Render(scene, camera, 0.0f);
		REQUIRE_FALSE(renderer.GetStatistics().Failed);
		REQUIRE(renderer.GetStatistics().DrawCalls > 0);
		std::vector<uint8_t> pixels;
		uint32_t width = 0, height = 0;
		REQUIRE(renderer.ReadOutputPixels(pixels, width, height));
		return pixels;
	}
	void Compare(const std::vector<uint8_t>& gpu, const std::vector<uint8_t>& cpu)
	{
		REQUIRE(gpu.size() == cpu.size());
		size_t different = 0;
		double total = 0;
		for (size_t i = 0; i < gpu.size(); i++)
		{
			const int error = std::abs(static_cast<int>(gpu[i]) - cpu[i]);
			total += error;
			if (error > 3)
				different++;
		}
		CHECK(total / static_cast<double>(gpu.size()) < .2);
		CHECK(static_cast<double>(different) / static_cast<double>(gpu.size()) < .005);
	}
	void Populate(Scene& scene)
	{
		scene.GetRendererSettings().SSAOEnabled = false;
		auto ground = scene.CreateEntity("Floor");
		ground.AddComponent<MeshComponent>().Mesh = "builtin://Plane";
		ground.GetTransform().Scale = glm::vec3(8);
		ground.GetTransform().Translation.y = -.05f;
		auto sun = scene.CreateEntity("Sun");
		sun.AddComponent<DirectionalLightComponent>().Intensity = 2;
		sun.GetTransform().Rotation = glm::quat(glm::radians(glm::vec3(-40, 20, 0)));
		auto light = scene.CreateEntity("Point");
		light.GetTransform().Translation = { 2, 3, 2 };
		auto& point = light.AddComponent<PointLightComponent>();
		point.Intensity = 10;
		point.Radius = 12;
		point.CastShadows = true;
		auto spot = scene.CreateEntity("Spot");
		spot.GetTransform().Translation = { -2, 3, 3 };
		spot.GetTransform().Rotation = glm::quat(glm::radians(glm::vec3(-30, -20, 0)));
		auto& cone = spot.AddComponent<SpotLightComponent>();
		cone.CastShadows = true;
		cone.Range = 12;
	}
}

TEST_CASE("GPU skinning matches CPU posed geometry in lighting, normals, depth and shadow passes")
{
	ClearAssetCache cacheGuard;
	auto graphics = CreateTestGraphics();
	const auto fixture = std::filesystem::path(BASALT_ANIMATION_DATA_DIR) / "TwoJoint.gltf";
	// A separate cache key keeps CPU vertex deformation from modifying the GPU rig's shared source.
	const auto referencePath = std::filesystem::path(BASALT_ANIMATION_OUTPUT_DIR) / "Reference.gltf";
	std::filesystem::create_directories(referencePath.parent_path());
	std::filesystem::copy_file(fixture, referencePath, std::filesystem::copy_options::overwrite_existing);
	Scene gpuScene, cpuScene;
	Populate(gpuScene);
	Populate(cpuScene);
	auto rig = gpuScene.CreateEntity("Rig");
	rig.AddComponent<AnimatorComponent>().Mesh = fixture.string();
	auto reference = cpuScene.CreateEntity("Reference");
	reference.AddComponent<MeshComponent>().Mesh = referencePath.string();
	auto second = gpuScene.CreateEntity("Independent rig");
	second.AddComponent<AnimatorComponent>() = { fixture.string(), "Bend", 1, true, false, true };
	second.GetTransform().Translation.x = 1.5f;
	auto secondReference = cpuScene.CreateEntity("Independent reference");
	secondReference.AddComponent<MeshComponent>().Mesh = fixture.string();
	secondReference.GetTransform() = second.GetTransform();
	rig.GetTransform().Scale = reference.GetTransform().Scale = glm::vec3(.8f, 1.1f, .9f);
	gpuScene.OnSimulationStart();
	auto source = AssetManager::GetMesh(fixture.string());
	REQUIRE(source);
	bool smallUnits = false;
	SUBCASE("Ordinary rig units") {}
	SUBCASE("Small rig units compensated by entity scale")
	{
		smallUnits = true;
		source->Nodes[2].Scale = glm::vec3(.001f);
		// Rotate out of plane so losing the joint rotation changes the normal image.
		source->Animations[0].Tracks[0].Values[1] = { 0, std::sqrt(.5f), 0, std::sqrt(.5f) };
	}
	SUBCASE("Nonuniform small rig units retain the surface normal")
	{
		smallUnits = true;
		source->Nodes[2].Scale = { .001f, .001f, 1 };
	}
	if (smallUnits)
	{
		rig.GetTransform().Scale *= 1000;
		reference.GetTransform().Scale *= 1000;
		second.GetTransform().Scale *= 1000;
		REQUIRE(gpuScene.GetAnimationSystem()->Seek(second, 0));
	}
	SceneRenderer gpuRenderer(graphics->GetDevice()), cpuRenderer(graphics->GetDevice());
	REQUIRE(gpuRenderer.Initialize());
	REQUIRE(cpuRenderer.Initialize());
	gpuRenderer.SetViewportSize(320, 240);
	cpuRenderer.SetViewportSize(320, 240);
	RenderCamera camera;
	camera.Position = { 0, 2, 6 };
	camera.View = glm::lookAt(camera.Position, glm::vec3(0, 1, 0), glm::vec3(0, 1, 0));
	camera.Projection = glm::perspective(glm::radians(45.0f), 320.0f / 240.0f, .1f, 30.0f);
	camera.Near = .1f;
	camera.Far = 30;
	std::vector<uint8_t> restImage;
	for (AlphaMode alpha : { AlphaMode::Opaque, AlphaMode::Mask, AlphaMode::Blend })
		for (float time : { 0.0f, .5f, 1.0f })
		{
			INFO("alpha=" << static_cast<int>(alpha) << " time=" << time);
			REQUIRE(gpuScene.GetAnimationSystem()->Seek(rig, time));
			source->Materials[0].Alpha = alpha;
			source->Materials[0].AlbedoColor.a = alpha == AlphaMode::Blend ? .6f : 1;
			AssetManager::Reload(referencePath.string());
			auto cpuSource = AssetManager::GetMesh(referencePath.string());
			REQUIRE(cpuSource);
			cpuSource->Materials = source->Materials;
			const auto palette = BuildSkinPalette(source->Skins[0], *gpuScene.GetAnimationSystem()->GetPose(rig));
			for (auto& vertex : cpuSource->Vertices)
			{
				glm::mat4 skin(0);
				for (int i = 0; i < 4; i++)
					skin += vertex.Weights[i] * palette[vertex.Joints[i]];
				vertex.Position = glm::vec3(skin * glm::vec4(vertex.Position, 1));
				vertex.Normal = glm::normalize(glm::transpose(glm::inverse(glm::mat3(skin))) * vertex.Normal);
				vertex.Tangent = glm::vec4(glm::normalize(glm::mat3(skin) * glm::vec3(vertex.Tangent)), vertex.Tangent.w);
			}
			cpuSource->Submeshes[0].Bounds = SkinBounds(*source, source->Submeshes[0], palette);
			cpuSource->UpdateBounds();
			for (RendererDebugView view : { RendererDebugView::None, RendererDebugView::Normals, RendererDebugView::Depth })
			{
				gpuScene.GetRendererSettings().DebugView = view;
				cpuScene.GetRendererSettings().DebugView = view;
				const auto gpu = Capture(gpuRenderer, gpuScene, camera);
				const auto batched = gpuRenderer.GetStatistics();
				gpuRenderer.SetInstancingEnabled(false);
				Compare(Capture(gpuRenderer, gpuScene, camera), gpu);
				CHECK(gpuRenderer.GetStatistics().Instances == batched.Instances);
				CHECK(gpuRenderer.GetStatistics().Triangles == batched.Triangles);
				if (alpha != AlphaMode::Blend)
					CHECK(gpuRenderer.GetStatistics().DrawCalls > batched.DrawCalls);
				gpuRenderer.SetInstancingEnabled(true);
				const auto cpu = Capture(cpuRenderer, cpuScene, camera);
				Compare(gpu, cpu);
				if (alpha == AlphaMode::Opaque && view == RendererDebugView::None)
				{
					if (time == 0)
					{
						restImage = gpu;
						std::string error;
						REQUIRE(WritePng(referencePath.parent_path() / "Rest.png", gpu, 320, 240, error));
					}
					else
					{
						CHECK(gpu != restImage);
						std::string error;
						REQUIRE(WritePng(referencePath.parent_path() / "Bent.png", gpu, 320, 240, error));
					}
					CHECK(gpuRenderer.GetStatistics().ShadowDrawCalls > 0);
					CHECK(gpuRenderer.GetStatistics().LocalShadowMaps > 0);
				}
			}
		}
	if (!smallUnits)
	{
		// Frame only the bent tip: the bind-pose box is entirely outside this camera.
		source->Materials[0].Alpha = AlphaMode::Opaque;
		AssetManager::GetMesh(referencePath.string())->Materials = source->Materials;
		gpuScene.GetRendererSettings().DebugView = RendererDebugView::None;
		cpuScene.GetRendererSettings().DebugView = RendererDebugView::None;
		camera.Position = { -.8f, 1.2f, 4 };
		camera.View = glm::lookAt(camera.Position, glm::vec3(-.8f, 1.2f, 0), glm::vec3(0, 1, 0));
		camera.Projection = glm::ortho(-.3f, .3f, -.3f, .3f, .1f, 10.0f);
		camera.Far = 10;
		const auto tipGPU = Capture(gpuRenderer, gpuScene, camera);
		const auto tipCPU = Capture(cpuRenderer, cpuScene, camera);
		Compare(tipGPU, tipCPU);
	}
	gpuScene.OnSimulationStop();
	graphics->GetDevice()->waitForIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
	AssetManager::Clear();
}

TEST_CASE("Mesh nodes share one palette upload per instance and fully culled skins upload nothing")
{
	ClearAssetCache cacheGuard;
	auto graphics = CreateTestGraphics();
	const auto fixture = std::filesystem::path(BASALT_ANIMATION_DATA_DIR) / "TwoJoint.gltf";
	auto source = AssetManager::GetMesh(fixture.string());
	REQUIRE(source);
	const auto meshNode = source->Nodes[1];
	source->NodeOrder.push_back(static_cast<uint32_t>(source->Nodes.size()));
	source->Nodes.push_back(meshNode);
	Scene scene;
	Populate(scene);
	auto rig = scene.CreateEntity("Shared skin nodes");
	rig.AddComponent<AnimatorComponent>().Mesh = fixture.string();
	scene.OnSimulationStart();
	SceneRenderer renderer(graphics->GetDevice());
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(320, 240);
	RenderCamera camera;
	camera.Position = { 0, 2, 6 };
	camera.View = glm::lookAt(camera.Position, glm::vec3(0, 1, 0), glm::vec3(0, 1, 0));
	camera.Projection = glm::perspective(glm::radians(45.0f), 320.0f / 240.0f, .1f, 30.0f);
	camera.Near = .1f;
	camera.Far = 30;
	Capture(renderer, scene, camera);
	CHECK(renderer.GetStatistics().SkinMatrices == 2);
	rig.GetTransform().Translation.x = 4;
	Capture(renderer, scene, camera);
	CHECK(renderer.GetStatistics().SkinMatrices == 2); // Outside the camera, but still a shadow caster.
	source->Materials[0].Alpha = AlphaMode::Blend;
	source->Materials[0].AlbedoColor.a = .6f;
	Capture(renderer, scene, camera);
	CHECK(renderer.GetStatistics().SkinMatrices == 0); // Blended geometry casts no shadows.
	rig.GetComponent<AnimatorComponent>().CastShadows = false;
	rig.GetTransform().Translation.x = 10000;
	Capture(renderer, scene, camera);
	CHECK(renderer.GetStatistics().SkinMatrices == 0);
	scene.OnSimulationStop();
	graphics->GetDevice()->waitForIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
	AssetManager::Clear();
}
