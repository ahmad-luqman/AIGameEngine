#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <Basalt/Core/Log.h>
#include <Basalt/Renderer/Fsr1Upscaler.h>
#include <Basalt/Renderer/GraphicsDevice.h>
#include <Basalt/Renderer/ShaderUtils.h>
#include <Basalt/Renderer/VulkanLoader.h>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace Basalt;

TEST_CASE("FSR 1 clamps every tap to the active area including degenerate dimensions")
{
	Log::Init();
	REQUIRE(VulkanLoader::Load());
	GraphicsDeviceSpecification spec;
	spec.EnableValidation = true;
	auto graphics = GraphicsDevice::CreateOffscreen(64, 64, spec);
	REQUIRE(graphics);
	REQUIRE(graphics->IsValidationActive());
	nvrhi::IDevice* device = graphics->GetDevice();
	const auto fullscreen = CreateEmbeddedShader(device, "Fullscreen.vert", nvrhi::ShaderType::Vertex);
	const auto blit = CreateEmbeddedShader(device, "Blit.frag", nvrhi::ShaderType::Pixel);
	nvrhi::BindingLayoutDesc desc;
	desc.bindingOffsets = ZeroBindingOffsets;
	desc.registerSpaceIsDescriptorSet = true;
	desc.visibility = nvrhi::ShaderType::Pixel;
	desc.bindings = { nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(1) };
	const auto layout = device->createBindingLayout(desc);
	REQUIRE(fullscreen);
	REQUIRE(blit);
	REQUIRE(layout);
	Fsr1Upscaler upscaler;
	const auto commands = device->createCommandList();
	REQUIRE(commands);

	auto render = [&](glm::uvec2 target, glm::uvec2 area, glm::vec3 poison, bool flat, float value = 0.25f, bool ramp = false) {
		REQUIRE(graphics->BeginFrame());
		// Use Blit only to construct the unused tonemap pipeline; upload perceptual test pixels directly.
		REQUIRE(upscaler.Prepare(device, target, graphics->GetCurrentFramebuffer(), fullscreen, blit, layout));
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
		upscaler.Render(commands, graphics->GetCurrentFramebuffer(), area, 0.2f);
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

	for (glm::uvec2 area : { glm::uvec2(1, 1), { 1, 7 }, { 7, 1 }, { 17, 13 }, { 32, 32 }, { 63, 61 } })
	{
		CAPTURE(area.x);
		CAPTURE(area.y);
		upscaler.Reset();
		const auto tight = render(area, area, glm::vec3(0), false);
		upscaler.Reset();
		const auto padded = render({ 79, 73 }, area, { 1, 0, 1 }, false);
		CHECK((tight == padded));
		// Same allocation, different render areas, and different poison: old content cannot leak in.
		const auto changed = render({ 79, 73 }, { 3, 2 }, { 0, 1, 0 }, false);
		CHECK_FALSE(changed.empty());
		const auto reused = render({ 79, 73 }, area, { 0, 1, 0 }, false);
		CHECK((tight == reused));
	}
	// Saturated and black channels must remain finite; midgray must not be gamma-converted twice.
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
	graphics->WaitIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
}
