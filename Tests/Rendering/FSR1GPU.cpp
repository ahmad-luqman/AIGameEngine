#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "GPUFixture.h"

#include <Basalt/Core/Log.h>
#include <Basalt/Renderer/Fsr1Settings.h>
#include <Basalt/Renderer/Fsr1Upscaler.h>
#include <Basalt/Renderer/GraphicsDevice.h>
#include <Basalt/Renderer/SceneRenderer.h>
#include <Basalt/Renderer/ShaderUtils.h>
#include <Basalt/Renderer/VulkanLoader.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/Scene/Entity.h>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace Basalt;

TEST_CASE("FSR 1 clamps every tap to the active area including degenerate dimensions")
{
	auto graphics = CreateTestGraphics();
	nvrhi::IDevice* device = graphics->GetDevice();
	const auto fullscreen = CreateEmbeddedShader(device, "Fullscreen.vert", nvrhi::ShaderType::Vertex);
	const auto sampler = device->createSampler(nvrhi::SamplerDesc().setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
	REQUIRE(fullscreen);
	REQUIRE(sampler);
	Fsr1Upscaler upscaler;
	const auto commands = device->createCommandList();
	REQUIRE(commands);

	auto render = [&](glm::uvec2 target, glm::uvec2 area, glm::vec3 poison, bool flat, float value = 0.25f, bool ramp = false, float sharpness = DefaultFsr1Sharpness) {
		REQUIRE(graphics->BeginFrame());
		REQUIRE(upscaler.Prepare(device, target, graphics->GetCurrentFramebuffer(), fullscreen, sampler));
		const auto texture = upscaler.GetInputFramebuffer()->getDesc().colorAttachments[0].texture;
		REQUIRE(texture->getDesc().width == target.x);
		std::vector<uint16_t> pixels(static_cast<size_t>(target.x) * target.y * 4);
		for (uint32_t y = 0; y < target.y; y++)
			for (uint32_t x = 0; x < target.x; x++)
			{
				// Asymmetric RGB stripes and diagonals exercise all four sides and gather ordering.
				glm::vec3 color = poison;
				if (x < area.x && y < area.y)
					color = flat ? glm::vec3(value) : glm::vec3(static_cast<float>((x + 2 * y) % 7) / 8.0f, static_cast<float>((3 * x + y) % 5) / 6.0f, x > y ? 0.75f : 0.125f);
				if (ramp && x < area.x && y < area.y)
					color = { (static_cast<float>(x) + 0.5f) / static_cast<float>(area.x), (static_cast<float>(y) + 0.5f) / static_cast<float>(area.y), 0.25f };
				const size_t offset = (static_cast<size_t>(y) * target.x + x) * 4;
				for (int c = 0; c < 3; c++)
					pixels[offset + static_cast<size_t>(c)] = glm::packHalf1x16(color[c]);
				pixels[offset + 3] = glm::packHalf1x16(1.0f);
			}
		commands->open();
		commands->writeTexture(texture, 0, 0, pixels.data(), static_cast<size_t>(target.x) * 8);
		upscaler.Render(commands, graphics->GetCurrentFramebuffer(), area, sharpness);
		commands->close();
		device->executeCommandList(commands);
		std::vector<uint8_t> captured;
		graphics->RequestBackBufferCapture([&](const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
			CHECK(width == 64);
			CHECK(height == 64);
			captured = rgba;
		});
		graphics->Present();
		REQUIRE(captured.size() == 64 * 64 * 4);
		return captured;
	};

	for (glm::uvec2 area : { glm::uvec2(1, 1), { 1, 7 }, { 7, 1 }, { 17, 13 }, { 32, 32 }, { 63, 61 }, { 64, 31 }, { 31, 64 } })
	{
		CAPTURE(area.x);
		CAPTURE(area.y);
		upscaler.Reset();
		const auto tight = render(area, area, glm::vec3(0), false);
		// Prepare must detect the target change without requiring a caller Reset.
		const auto padded = render({ 79, 73 }, area, { 1, 0, 1 }, false);
		CHECK((tight == padded));
		// Same allocation, different render areas and fresh poison outside the area: inactive texels cannot leak in.
		const auto changed = render({ 79, 73 }, { 3, 2 }, { 0, 1, 0 }, false);
		CHECK((changed != tight));
		const auto reused = render({ 79, 73 }, area, { 0, 1, 0 }, false);
		CHECK((tight == reused));
	}
	// Flat black, quarter-intensity and white must stay finite and retain their perceptual values.
	for (float value : { 0.0f, 0.25f, 1.0f })
	{
		upscaler.Reset();
		const auto flat = render({ 7, 9 }, { 3, 5 }, { 1, 0, 1 }, true, value);
		const int expected = static_cast<int>(std::lround(value * 255.0f));
		for (size_t i = 0; i < flat.size(); i += 4)
		{
			CHECK(std::abs(static_cast<int>(flat[i]) - expected) <= 1);
			CHECK(flat[i] == flat[i + 1]);
			CHECK(flat[i] == flat[i + 2]);
			CHECK(flat[i + 3] == 255);
		}
	}
	// A linear ramp pins gather ordering and pixel-centre mapping independently of allocation equality.
	upscaler.Reset();
	const auto ramp = render({ 23, 19 }, { 17, 13 }, { 1, 0, 1 }, false, 0.25f, true);
	float maxRedError = 0.0f, maxGreenError = 0.0f;
	for (uint32_t y = 8; y < 56; y++)
		for (uint32_t x = 8; x < 56; x++)
		{
			const size_t offset = (static_cast<size_t>(y) * 64 + x) * 4;
			maxRedError = std::max(maxRedError, std::abs(static_cast<float>(ramp[offset]) - (static_cast<float>(x) + 0.5f) * 255.0f / 64.0f));
			maxGreenError = std::max(maxGreenError, std::abs(static_cast<float>(ramp[offset + 1]) - (static_cast<float>(y) + 0.5f) * 255.0f / 64.0f));
		}
	// EASU's approximate nonlinear filter is not an exact linear interpolant. Three quarters of an input texel
	// allows that reconstruction error while catching a one-texel shift or a flipped gather.
	CHECK(maxRedError < 0.75f * 255.0f / 17.0f);
	CHECK(maxGreenError < 0.75f * 255.0f / 13.0f);
	for (uint32_t i = 0; i < 64; i++)
	{
		// Along the top and left borders, the ramp clamps to its first sample's half-texel value.
		CHECK(std::abs(static_cast<float>(ramp[(static_cast<size_t>(i) * 64) * 4]) - 0.5f * 255.0f / 17.0f) < 3.0f);
		CHECK(std::abs(static_cast<float>(ramp[static_cast<size_t>(i) * 4 + 1]) - 0.5f * 255.0f / 13.0f) < 3.0f);
	}
	const auto standard = render({ 23, 19 }, { 17, 13 }, { 1, 0, 1 }, false);
	CHECK((standard == render({ 23, 19 }, { 17, 13 }, { 1, 0, 1 }, false, 0.25f, false, std::numeric_limits<float>::quiet_NaN())));
	const auto weakest = render({ 23, 19 }, { 17, 13 }, { 1, 0, 1 }, false, 0.25f, false, MaxFsr1Sharpness);
	CHECK((weakest == render({ 23, 19 }, { 17, 13 }, { 1, 0, 1 }, false, 0.25f, false, 5.0f)));

	// A bad configuration reports its stage and dimensions once, then retries on reactivation.
	upscaler.SetActive(true);
	auto failures = []() {
		size_t count = 0;
		for (const auto& entry : Log::GetHistory().GetAll())
			if (entry.Text.find("FSR 1 targets: invalid dimensions") != std::string::npos)
				count++;
		return count;
	};
	const size_t before = failures();
	REQUIRE_FALSE(upscaler.Prepare(device, { 0, 9 }, graphics->GetCurrentFramebuffer(), fullscreen, sampler));
	CHECK(failures() == before + 1);
	REQUIRE_FALSE(upscaler.Prepare(device, { 0, 9 }, graphics->GetCurrentFramebuffer(), fullscreen, sampler));
	CHECK(failures() == before + 1);
	upscaler.SetActive(false);
	upscaler.SetActive(true);
	REQUIRE_FALSE(upscaler.Prepare(device, { 0, 9 }, graphics->GetCurrentFramebuffer(), fullscreen, sampler));
	CHECK(failures() == before + 2);
	// Correcting the configuration also retries automatically, without a resize call or toggle.
	REQUIRE(upscaler.Prepare(device, { 7, 9 }, graphics->GetCurrentFramebuffer(), fullscreen, sampler));
	const nvrhi::FramebufferHandle prepared = upscaler.GetInputFramebuffer();
	upscaler.SetActive(false);
	upscaler.SetActive(true);
	REQUIRE(upscaler.Prepare(device, { 7, 9 }, graphics->GetCurrentFramebuffer(), fullscreen, sampler));
	CHECK(upscaler.GetInputFramebuffer() == prepared.Get());
	const auto otherSampler = device->createSampler(nvrhi::SamplerDesc().setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
	REQUIRE(otherSampler);
	REQUIRE(upscaler.Prepare(device, { 7, 9 }, graphics->GetCurrentFramebuffer(), fullscreen, otherSampler));
	CHECK(upscaler.GetInputFramebuffer() != prepared.Get());
	// Output formats and shader identity are cache inputs too; changing either must rebuild.
	const nvrhi::FramebufferHandle previousInput = upscaler.GetInputFramebuffer();
	const auto otherVS = CreateEmbeddedShader(device, "Fullscreen.vert", nvrhi::ShaderType::Vertex);
	REQUIRE(otherVS);
	REQUIRE(upscaler.Prepare(device, { 7, 9 }, graphics->GetCurrentFramebuffer(), otherVS, otherSampler));
	CHECK(upscaler.GetInputFramebuffer() != previousInput.Get());
	nvrhi::TextureDesc outputDesc;
	outputDesc.width = 47;
	outputDesc.height = 39;
	outputDesc.format = nvrhi::Format::RGBA16_FLOAT;
	outputDesc.isRenderTarget = true;
	outputDesc.initialState = nvrhi::ResourceStates::RenderTarget;
	outputDesc.keepInitialState = true;
	const auto otherOutput = device->createTexture(outputDesc);
	REQUIRE(otherOutput);
	const auto otherFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(otherOutput));
	REQUIRE(otherFramebuffer);
	const nvrhi::FramebufferHandle previousFormat = upscaler.GetInputFramebuffer();
	REQUIRE(upscaler.Prepare(device, { 7, 9 }, otherFramebuffer, otherVS, otherSampler));
	CHECK(upscaler.GetInputFramebuffer() != previousFormat.Get());
	commands->open();
	commands->clearTextureFloat(upscaler.GetInputFramebuffer()->getDesc().colorAttachments[0].texture, nvrhi::AllSubresources, nvrhi::Color(0.25f));
	upscaler.Render(commands, otherFramebuffer, { 7, 9 }, DefaultFsr1Sharpness);
	commands->close();
	device->executeCommandList(commands);

	// One SceneRenderer survives viewport, render-scale and dynamic-mode changes with FSR enabled.
	// A fresh renderer supplies an independent result for each configuration.
	Scene scene;
	scene.GetRendererSettings().FSR1Enabled = true;
	scene.GetRendererSettings().SSAOEnabled = false;
	Entity cube = scene.CreateEntity("Resize reference");
	cube.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	cube.AddComponent<MaterialComponent>().EmissiveColor = { 0.8f, 0.2f, 0.05f };
	cube.GetComponent<TransformComponent>().Translation = { 0.1f, -0.1f, 0.5f };
	cube.GetComponent<TransformComponent>().Scale = glm::vec3(0.7f);
	RenderCamera camera;
	SceneRendererSpecification rendererSpec;
	rendererSpec.GpuTimers = graphics->SupportsTimerQueries();
	REQUIRE(rendererSpec.GpuTimers);
	SceneRenderer resized(device, rendererSpec);
	REQUIRE(resized.Initialize());
	auto readScene = [&](SceneRenderer& renderer, glm::uvec2 size, float scale, bool dynamic) {
		// Force scale feedback to arrive even on fast GPUs; the comparison below follows the actual area.
		renderer.SetDynamicResolution({ dynamic, 1000000.0f, 0.25f });
		renderer.SetRenderScale(scale);
		renderer.SetViewportSize(size.x, size.y);
		renderer.Render(scene, camera);
		REQUIRE_FALSE(renderer.GetStatistics().Failed);
		CHECK(renderer.GetStatistics().FSR1Active);
		CHECK(renderer.IsDynamicResolutionActive() == dynamic);
		CHECK(renderer.GetStatistics().DrawCalls > 0);
		std::vector<uint8_t> pixels;
		uint32_t width = 0, height = 0;
		REQUIRE(renderer.ReadOutputPixels(pixels, width, height));
		CHECK(width == size.x);
		CHECK(height == size.y);
		return pixels;
	};
	bool sawDynamicReduction = false;
	for (const glm::uvec2 size : { glm::uvec2(64, 64), { 83, 47 }, { 31, 51 } })
		for (const float scale : { 0.5f, 0.75f })
		{
			SceneRenderer fresh(device, rendererSpec);
			REQUIRE(fresh.Initialize());
			const auto expected = readScene(fresh, size, scale, false);
			CHECK((readScene(resized, size, scale, false) == expected));
			const auto dynamicImage = readScene(resized, size, scale, true);
			const float actualScale = resized.GetCurrentRenderScale();
			sawDynamicReduction |= actualScale < scale;
			// A software GPU may already have lowered the active scale from earlier timer samples.
			// Compare the larger dynamic allocation against a fresh renderer at that exact render area.
			CHECK((dynamicImage == readScene(fresh, size, actualScale, false)));
			CHECK(resized.GetRenderWidth() == fresh.GetRenderWidth());
			CHECK(resized.GetRenderHeight() == fresh.GetRenderHeight());
			scene.GetRendererSettings().FSR1Enabled = false;
			resized.Render(scene, camera);
			CHECK_FALSE(resized.GetStatistics().FSR1Active);
			scene.GetRendererSettings().FSR1Enabled = true;
			CHECK((readScene(resized, size, scale, false) == expected));
		}
	CHECK(sawDynamicReduction);
	graphics->WaitIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
}
