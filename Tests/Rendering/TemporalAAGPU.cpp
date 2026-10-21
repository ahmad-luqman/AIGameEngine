#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "GPUFixture.h"

#include <Basalt/Animation/AnimatedModel.h>
#include <Basalt/Animation/AnimationSystem.h>
#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Renderer/RenderMath.h>
#include <Basalt/Renderer/SceneRenderer.h>
#include <Basalt/Renderer/TemporalHistory.h>
#include <Basalt/Renderer/TemporalAA.h>
#include <Basalt/Renderer/ShaderUtils.h>
#include <glm/gtc/packing.hpp>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>

using namespace Basalt;

namespace {
	std::vector<glm::vec4> ReadMotion(nvrhi::IDevice* device, SceneRenderer& renderer)
	{
		auto* texture = renderer.GetMotionTexture();
		REQUIRE(texture);
		const auto staging = device->createStagingTexture(texture->getDesc(), nvrhi::CpuAccessMode::Read);
		REQUIRE(staging);
		const auto commands = device->createCommandList();
		REQUIRE(commands);
		commands->open();
		commands->copyTexture(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());
		commands->close();
		device->executeCommandList(commands);
		device->waitForIdle();
		size_t pitch = 0;
		const auto* data = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
		REQUIRE(data);
		const auto& desc = texture->getDesc();
		std::vector<glm::vec4> motion(static_cast<size_t>(desc.width) * desc.height);
		for (uint32_t y = 0; y < desc.height; y++)
			std::memcpy(motion.data() + static_cast<size_t>(y) * desc.width, data + y * pitch, desc.width * sizeof(glm::vec4));
		device->unmapStagingTexture(staging);
		return motion;
	}
	void Render(SceneRenderer& renderer, Scene& scene, const RenderCamera& camera = {})
	{
		renderer.Render(scene, camera, 1.0f / 60);
		REQUIRE_FALSE(renderer.GetStatistics().Failed);
	}
	PostProcessComponent& Populate(Scene& scene)
	{
		scene.CreateEntity("No sky").AddComponent<SkyLightComponent>().ShowBackground = false;
		auto& post = scene.CreateEntity("TAA").AddComponent<PostProcessComponent>();
		post.TAAEnabled = true;
		post.SSAOEnabled = false;
		return post;
	}
}

TEST_CASE("TAA motion follows independently moving batched instances and resets at boundaries")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	auto& post = Populate(scene);
	auto left = scene.CreateEntity("Left"), right = scene.CreateEntity("Right");
	for (auto entity : { left, right })
	{
		entity.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
		entity.GetTransform().Scale = glm::vec3(.35f);
		entity.GetTransform().Translation.z = .5f;
	}
	left.GetTransform().Translation.x = -.5f;
	right.GetTransform().Translation.x = .5f;
	SceneRenderer renderer(graphics->GetDevice(), { .Picking = true });
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(64, 64);
	Render(renderer, scene);
	CHECK(renderer.GetStatistics().TAAActive);
	CHECK_FALSE(renderer.GetStatistics().TAAHistoryValid);
	left.GetTransform().Translation.x += .1f;
	right.GetTransform().Translation.x -= .2f;
	Render(renderer, scene);
	CHECK(renderer.GetStatistics().TAAHistoryValid);
	CHECK(renderer.GetStatistics().DrawCalls == 1);
	const auto motion = ReadMotion(graphics->GetDevice(), renderer);
	size_t counts[2]{};
	for (uint32_t y = 0; y < 64; y++)
		for (uint32_t x = 0; x < 64; x++)
		{
			const auto value = motion[y * 64 + x];
			if (value.w < .5f)
				continue;
			const uint32_t index = x < 32 ? 0 : 1;
			counts[index]++;
			const float displacement = index == 0 ? .1f : -.2f;
			CHECK(value.x == doctest::Approx((static_cast<float>(x) + .5f) / 64 - displacement * .5f + .25f / 64).epsilon(.0001));
			CHECK(value.y == doctest::Approx((static_cast<float>(y) + .5f) / 64 + (1.0f / 3) / 64).epsilon(.0001));
			CHECK(value.z == doctest::Approx(.675f));
		}
	CHECK(counts[0] > 80);
	CHECK(counts[1] > 80);

	SUBCASE("Explicit cut")
	{
		renderer.ResetTemporalHistory();
	}
	SUBCASE("Capture resets exposure and temporal samples")
	{
		renderer.ResetExposure();
	}
	SUBCASE("Resize")
	{
		renderer.SetViewportSize(80, 60);
	}
	SUBCASE("Render scale")
	{
		renderer.SetRenderScale(.75f);
	}
	SUBCASE("Disabled then enabled")
	{
		post.TAAEnabled = false;
		Render(renderer, scene);
		CHECK_FALSE(renderer.GetMotionTexture());
		CHECK_FALSE(renderer.GetStatistics().TAAActive);
		CHECK_FALSE(renderer.GetStatistics().TAAHistoryValid);
		post.TAAEnabled = true;
	}
	SUBCASE("Debug bypass")
	{
		scene.GetRendererSettings().DebugView = RendererDebugView::Depth;
		Render(renderer, scene);
		CHECK_FALSE(renderer.GetMotionTexture());
		CHECK_FALSE(renderer.GetStatistics().TAAActive);
		scene.GetRendererSettings().DebugView = RendererDebugView::None;
	}
	SUBCASE("Failure")
	{
		post.Exposure = -1;
		RenderCamera camera;
		renderer.Render(scene, camera, 0);
		CHECK(renderer.GetStatistics().Failed);
		post.Exposure = 1;
	}
	SUBCASE("Projection cut")
	{
		RenderCamera camera;
		camera.Projection[0][0] = 2;
		Render(renderer, scene, camera);
		CHECK_FALSE(renderer.GetStatistics().TAAHistoryValid);
	}
	Render(renderer, scene);
	CHECK_FALSE(renderer.GetStatistics().TAAHistoryValid);
	Render(renderer, scene);
	CHECK(renderer.GetStatistics().TAAHistoryValid);
	CHECK(graphics->GetValidationErrorCount() == 0);
}

namespace {
	struct Triangle
	{
		glm::vec4 Position[3];
	};
	using PosedTriangles = std::map<std::pair<uint32_t, uint32_t>, std::vector<Triangle>>;
	PosedTriangles PoseTriangles(Entity entity, const glm::mat4& viewProjection)
	{
		PosedTriangles result;
		ForEachAnimatedMesh(entity, [&](const Ref<MeshSource>& source, const MeshNode& node, const glm::mat4& model,
										const std::vector<glm::mat4>& palette, std::span<const float> weights) {
			for (uint32_t submeshIndex : source->Meshes[node.MeshIndex].Submeshes)
			{
				const auto& submesh = source->Submeshes[submeshIndex];
				auto& triangles = result[{ static_cast<uint32_t>(&node - source->Nodes.data()), submeshIndex }];
				const auto drawn = DrawnMorphWeights(weights);
				for (uint32_t i = 0; i < submesh.IndexCount; i += 3)
				{
					Triangle triangle;
					for (uint32_t corner = 0; corner < 3; corner++)
					{
						const auto vertex = MorphVertex(*source, submesh, source->Indices[submesh.BaseIndex + i + corner], drawn);
						glm::mat4 skin(1);
						if (!palette.empty())
						{
							skin = glm::mat4(0);
							for (int joint = 0; joint < 4; joint++)
								skin += vertex.Weights[joint] * palette[vertex.Joints[joint]];
						}
						triangle.Position[corner] = viewProjection * model * skin * glm::vec4(vertex.Position, 1);
					}
					triangles.push_back(triangle);
				}
			}
		});
		return result;
	}
}

TEST_CASE("TAA previous clip coordinates match CPU skinning and morphing for every node")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	Populate(scene);
	auto rig = scene.CreateEntity("Face and Teeth");
	rig.AddComponent<AnimatorComponent>().Mesh = (std::filesystem::path(BASALT_ANIMATION_DATA_DIR) / "Morph.gltf").string();
	scene.OnSimulationStart();
	auto& animation = *scene.GetAnimationSystem();
	REQUIRE(animation.Play(rig, "Nod"));
	REQUIRE(animation.Seek(rig, .1f));
	REQUIRE(animation.SetMorphWeight(rig, "Smile", .2f, 0));
	RenderCamera camera;
	camera.Position = { 0, .5f, 3 };
	camera.View = glm::lookAt(camera.Position, glm::vec3(0, .5f, 0), glm::vec3(0, 1, 0));
	camera.Projection = glm::perspective(glm::radians(40.0f), 1.0f, .1f, 10.0f);
	camera.Near = .1f;
	camera.Far = 10;
	SceneRenderer renderer(graphics->GetDevice());
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(64, 64);
	const auto previous = PoseTriangles(rig, JitterProjection(camera.Projection, TemporalJitter(0), { 64, 64 }) * camera.View);
	Render(renderer, scene, camera);
	REQUIRE(animation.Seek(rig, .65f));
	REQUIRE(animation.SetMorphWeight(rig, "Smile", .9f, 0));
	rig.GetTransform().Translation.x = .1f;
	const auto current = PoseTriangles(rig, JitterProjection(camera.Projection, TemporalJitter(1), { 64, 64 }) * camera.View);
	Render(renderer, scene, camera);
	REQUIRE(renderer.GetStatistics().TAAHistoryValid);
	const auto motion = ReadMotion(graphics->GetDevice(), renderer);
	size_t checked = 0;
	for (uint32_t y = 0; y < 64; y++)
		for (uint32_t x = 0; x < 64; x++)
		{
			const glm::vec2 pixel((static_cast<float>(x) + .5f) / 32 - 1, 1 - (static_cast<float>(y) + .5f) / 32);
			float nearest = 1;
			glm::vec4 expected(0);
			for (const auto& [key, triangles] : current)
				for (size_t i = 0; i < triangles.size(); i++)
				{
					const auto& triangle = triangles[i];
					glm::vec3 ndc[3];
					for (int v = 0; v < 3; v++)
						ndc[v] = glm::vec3(triangle.Position[v]) / triangle.Position[v].w;
					const glm::vec2 a(ndc[0]), b(ndc[1]), c(ndc[2]);
					const float determinant = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
					if (std::abs(determinant) < 1e-8f)
						continue;
					glm::vec3 barycentric;
					barycentric.x = ((b.y - c.y) * (pixel.x - c.x) + (c.x - b.x) * (pixel.y - c.y)) / determinant;
					barycentric.y = ((c.y - a.y) * (pixel.x - c.x) + (a.x - c.x) * (pixel.y - c.y)) / determinant;
					barycentric.z = 1 - barycentric.x - barycentric.y;
					// Ignore rasterization-edge conventions and overlapping triangle edges.
					if (glm::any(glm::lessThan(barycentric, glm::vec3(.02f))))
						continue;
					const float depth = glm::dot(barycentric, glm::vec3(ndc[0].z, ndc[1].z, ndc[2].z));
					if (depth >= nearest)
						continue;
					nearest = depth;
					glm::vec4 clip(0);
					for (int v = 0; v < 3; v++)
						clip += barycentric[v] / triangle.Position[v].w * previous.at(key)[i].Position[v];
					expected = glm::vec4(clip.x / clip.w * .5f + .5f, .5f - clip.y / clip.w * .5f, clip.z / clip.w, 1);
				}
			if (expected.w == 0 || motion[y * 64 + x].w == 0)
				continue;
			checked++;
			for (int c = 0; c < 3; c++)
				CHECK(motion[y * 64 + x][c] == doctest::Approx(expected[c]).epsilon(.0002));
		}
	CHECK(checked > 200);
	scene.OnSimulationStop();
	CHECK(graphics->GetValidationErrorCount() == 0);
	AssetManager::Clear();
}

TEST_CASE("TAA resolve blends history, clips color, rejects depth and clamps to the active area")
{
	auto graphics = CreateTestGraphics();
	auto* device = graphics->GetDevice();
	const glm::uvec2 area(4, 4);
	auto target = [&](nvrhi::Format format) {
		nvrhi::TextureDesc desc;
		desc.width = desc.height = 8;
		desc.format = format;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		return device->createTexture(desc);
	};
	const auto color = target(nvrhi::Format::RGBA16_FLOAT);
	const auto depth = target(nvrhi::Format::R32_FLOAT);
	const auto motion = target(nvrhi::Format::RGBA32_FLOAT);
	REQUIRE(color);
	REQUIRE(depth);
	REQUIRE(motion);
	const auto linear = device->createSampler(nvrhi::SamplerDesc().setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
	const auto point = device->createSampler(nvrhi::SamplerDesc().setAllAddressModes(nvrhi::SamplerAddressMode::Clamp).setAllFilters(false));
	const auto shader = CreateEmbeddedShader(device, "Fullscreen.vert", nvrhi::ShaderType::Vertex);
	REQUIRE(linear);
	REQUIRE(point);
	REQUIRE(shader);
	TemporalAA taa;
	REQUIRE(taa.Prepare(device, color, depth, motion, shader, linear, point));
	const auto commands = device->createCommandList();
	REQUIRE(commands);
	std::vector<uint16_t> colors(8 * 8 * 4);
	std::vector<float> depths(8 * 8, .5f);
	std::vector<glm::vec4> motions(8 * 8);
	for (uint32_t y = 0; y < 8; y++)
		for (uint32_t x = 0; x < 8; x++)
		{
			const size_t pixel = y * 8 + x;
			for (size_t c = 0; c < 4; c++)
				colors[pixel * 4 + c] = glm::packHalf1x16(x < 4 && y < 4 ? .8f : 100.0f);
			motions[pixel] = glm::vec4((static_cast<float>(x) + .5f) / 4, (static_cast<float>(y) + .5f) / 4, .5f, 1);
		}
	glm::vec2 jitterDelta(0);
	glm::mat4 skyReprojection(1);
	bool gradient = false, sky = false, skyDisocclusion = false;
	SUBCASE("Jitter correction samples a two-dimensional gradient")
	{
		gradient = true;
		jitterDelta = { .125f, -.125f };
	}
	SUBCASE("Sky reprojects with camera rotation")
	{
		sky = true;
		skyReprojection = glm::rotate(glm::mat4(1), .1f, glm::vec3(0, 0, 1));
	}
	SUBCASE("Sky rejects geometry history")
	{
		sky = true;
		skyDisocclusion = true;
	}
	if (gradient)
		for (uint32_t y = 0; y < 4; y++)
			for (uint32_t x = 0; x < 4; x++)
				for (size_t c = 0; c < 3; c++)
					colors[(y * 8 + x) * 4 + c] = glm::packHalf1x16(.1f * x + .05f * y);
	if (sky && !skyDisocclusion)
		std::fill(depths.begin(), depths.end(), 1.0f);
	auto resolve = [&](bool valid) {
		commands->open();
		commands->writeTexture(color, 0, 0, colors.data(), 8 * 8);
		commands->writeTexture(depth, 0, 0, depths.data(), 8 * sizeof(float));
		commands->writeTexture(motion, 0, 0, motions.data(), 8 * sizeof(glm::vec4));
		taa.Render(commands, area, valid, skyReprojection, jitterDelta);
		commands->close();
		device->executeCommandList(commands);
		device->waitForIdle();
	};
	resolve(false);
	for (uint32_t y = 0; y < 4; y++)
		for (uint32_t x = 0; x < 4; x++)
			for (size_t c = 0; c < 4; c++)
				colors[(y * 8 + x) * 4 + c] = glm::packHalf1x16(.2f);
	// A real color range admits the previous center, while every active edge must clamp away poisoned padding.
	for (size_t c = 0; c < 3; c++)
	{
		colors[(1 * 8 + 1) * 4 + c] = glm::packHalf1x16(0);
		colors[(1 * 8 + 2) * 4 + c] = glm::packHalf1x16(1);
	}
	float expected = .74f;
	if (gradient)
	{
		const float weight = .9f - .4f * std::sqrt(.5f) / 16;
		expected = .2f + (.325f - .2f) * weight;
	}
	if (sky)
	{
		std::fill(depths.begin(), depths.end(), 1.0f);
		std::fill(motions.begin(), motions.end(), glm::vec4(0));
		const glm::vec4 previous = skyReprojection * glm::vec4(.25f, -.25f, 1, 1);
		const glm::vec2 shift = (glm::vec2(previous) * glm::vec2(.5f, -.5f) + .5f - glm::vec2(.625f)) * 4.0f;
		expected = skyDisocclusion ? .2f : .2f + .6f * (.9f - .4f * glm::length(shift) / 16);
	}
	SUBCASE("Accepted history") {}
	SUBCASE("Disocclusion")
	{
		motions[2 * 8 + 2].z = .3f;
		expected = .2f;
	}
	SUBCASE("New geometry")
	{
		motions[2 * 8 + 2].w = 0;
		expected = .2f;
	}
	SUBCASE("Outside previous view")
	{
		motions[2 * 8 + 2].x = -1;
		expected = .2f;
	}
	SUBCASE("Camera cut")
	{
		resolve(false);
		expected = .2f;
	}
	resolve(true);
	const auto staging = device->createStagingTexture(color->getDesc(), nvrhi::CpuAccessMode::Read);
	REQUIRE(staging);
	commands->open();
	commands->copyTexture(staging, nvrhi::TextureSlice(), color, nvrhi::TextureSlice());
	commands->close();
	device->executeCommandList(commands);
	device->waitForIdle();
	size_t pitch = 0;
	const auto* data = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
	REQUIRE(data);
	auto sample = [&](uint32_t x, uint32_t y) {
		uint16_t value = 0;
		std::memcpy(&value, data + y * pitch + x * 8, sizeof(value));
		return glm::unpackHalf1x16(value);
	};
	CHECK(sample(2, 2) == doctest::Approx(expected).epsilon(.005));
	CHECK(sample(3, 3) == doctest::Approx(.2f).epsilon(.005));
	device->unmapStagingTexture(staging);
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("TAA reduces static silhouette shimmer across the jitter cycle")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	auto& post = Populate(scene);
	auto cube = scene.CreateEntity("Diagonal emissive edge");
	cube.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	cube.GetTransform().Rotation = glm::quat(glm::vec3(.2f, .1f, .45f));
	cube.GetTransform().Scale = glm::vec3(.6f);
	auto& material = cube.AddComponent<MaterialComponent>();
	material.EmissiveColor = glm::vec3(1);
	material.AlbedoColor = glm::vec4(0, 0, 0, 1);
	RenderCamera camera;
	camera.Position = { 0, 0, 3 };
	camera.View = glm::lookAt(camera.Position, glm::vec3(0), glm::vec3(0, 1, 0));
	camera.Projection = glm::perspective(glm::radians(40.0f), 1.0f, .1f, 10.0f);
	camera.Near = .1f;
	camera.Far = 10;
	SceneRenderer temporal(graphics->GetDevice()), reference(graphics->GetDevice());
	REQUIRE(temporal.Initialize());
	REQUIRE(reference.Initialize());
	temporal.SetViewportSize(64, 64);
	reference.SetViewportSize(64, 64);
	std::vector<uint8_t> previousTemporal, previousRaw;
	uint64_t temporalVariation = 0, rawVariation = 0;
	auto capture = [&](SceneRenderer& renderer, const RenderCamera& view) {
		Render(renderer, scene, view);
		std::vector<uint8_t> pixels;
		uint32_t width = 0, height = 0;
		REQUIRE(renderer.ReadOutputPixels(pixels, width, height));
		return pixels;
	};
	for (uint32_t frame = 0; frame < 24; frame++)
	{
		post.TAAEnabled = true;
		const auto filtered = capture(temporal, camera);
		CHECK(temporal.GetStatistics().TAAHistoryValid == (frame > 0));
		post.TAAEnabled = false;
		RenderCamera jittered = camera;
		jittered.Projection = JitterProjection(camera.Projection, TemporalJitter(frame), { 64, 64 });
		const auto raw = capture(reference, jittered);
		if (frame >= 8)
			for (size_t i = 0; i < raw.size(); i++)
			{
				temporalVariation += static_cast<uint64_t>(std::abs(static_cast<int>(filtered[i]) - previousTemporal[i]));
				rawVariation += static_cast<uint64_t>(std::abs(static_cast<int>(raw[i]) - previousRaw[i]));
			}
		previousTemporal = filtered;
		previousRaw = raw;
	}
	INFO("temporal variation=" << temporalVariation << ", raw variation=" << rawVariation);
	CHECK(rawVariation > 10000);
	CHECK(static_cast<double>(temporalVariation) < static_cast<double>(rawVariation) * .4);
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("TAA rejects replaced and newly visible entity geometry")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	Populate(scene);
	auto cube = scene.CreateEntity("History identity");
	cube.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	cube.GetTransform().Scale = glm::vec3(.5f);
	cube.GetTransform().Translation.z = .5f;
	SceneRenderer renderer(graphics->GetDevice());
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(32, 32);
	Render(renderer, scene);
	Render(renderer, scene);
	auto validPixels = [&]() {
		const auto values = ReadMotion(graphics->GetDevice(), renderer);
		return std::count_if(values.begin(), values.end(), [](glm::vec4 value) { return value.w > .5f; });
	};
	CHECK(validPixels() > 20);
	SUBCASE("Replacement model")
	{
		cube.GetComponent<MeshComponent>().Mesh = "builtin://Sphere";
	}
	SUBCASE("New entity")
	{
		scene.DestroyEntity(cube);
		cube = scene.CreateEntity("Replacement entity");
		cube.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
		cube.GetTransform().Scale = glm::vec3(.5f);
		cube.GetTransform().Translation.z = .5f;
	}
	SUBCASE("Culled for a frame")
	{
		cube.GetTransform().Translation.x = 100;
		Render(renderer, scene);
		cube.GetTransform().Translation.x = 0;
	}
	Render(renderer, scene);
	CHECK(renderer.GetStatistics().TAAHistoryValid);
	CHECK(validPixels() == 0);
	Render(renderer, scene);
	CHECK(validPixels() > 20);
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("TAA supplies spatial AA on reset and takes precedence over FXAA with valid history")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	auto& post = Populate(scene);
	auto cube = scene.CreateEntity("AA edge");
	cube.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	cube.GetTransform().Scale = glm::vec3(.7f);
	cube.GetTransform().Translation.z = .5f;
	cube.GetTransform().Rotation = glm::quat(glm::vec3(0, 0, .3f));
	cube.AddComponent<MaterialComponent>().EmissiveColor = glm::vec3(1);
	SceneRenderer temporal(graphics->GetDevice()), reference(graphics->GetDevice());
	REQUIRE(temporal.Initialize());
	REQUIRE(reference.Initialize());
	temporal.SetViewportSize(32, 32);
	reference.SetViewportSize(32, 32);
	auto pixels = [&](SceneRenderer& renderer) {
		std::vector<uint8_t> result;
		uint32_t width = 0, height = 0;
		REQUIRE(renderer.ReadOutputPixels(result, width, height));
		return result;
	};
	Render(temporal, scene);
	const auto first = pixels(temporal);
	post.TAAEnabled = false;
	post.FXAAEnabled = true;
	RenderCamera jittered;
	jittered.Projection = JitterProjection(jittered.Projection, TemporalJitter(0), { 32, 32 });
	Render(reference, scene, jittered);
	CHECK(first == pixels(reference));
	// Build identical histories with opposite authored FXAA settings.
	post.TAAEnabled = true;
	reference.ResetTemporalHistory();
	temporal.ResetTemporalHistory();
	for (int frame = 0; frame < 3; frame++)
	{
		post.FXAAEnabled = false;
		Render(temporal, scene);
		post.FXAAEnabled = true;
		Render(reference, scene);
		CHECK(pixels(temporal) == pixels(reference));
		CHECK(temporal.GetStatistics().TAAHistoryValid == (frame > 0));
	}
	CHECK(graphics->GetValidationErrorCount() == 0);
}
