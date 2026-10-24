#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "GPUFixture.h"

#include <Basalt/Renderer/GraphicsDevice.h>
#include <Basalt/Renderer/ShaderUtils.h>
#include <Basalt/Renderer/SsaoBlur.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

using namespace Basalt;

namespace {

	constexpr float Near = 0.1f;
	constexpr float Far = 100.0f;

	// One blur pass's inputs: occlusion and depth over a target, of which the top-left area is rendered.
	struct BlurInput
	{
		glm::ivec2 Target;
		glm::ivec2 Area;
		std::function<float(int, int)> Occlusion;
		std::function<float(int, int)> Depth;
		bool Orthographic = false;
	};

	float LinearDepth(float depth, bool orthographic)
	{
		if (orthographic)
			return Near + depth * (Far - Near);
		return Near * Far / (Far - depth * (Far - Near));
	}

	// SSAOBlur.frag on the CPU: taps at i * 1.5 texels along the axis, odd ones split over the two texels they fall
	// between, clamped to the area and weighted by depth difference.
	float Reference(const BlurInput& input, glm::ivec2 axis, glm::ivec2 center)
	{
		const float weights[5] = { 0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f };
		const float centerDepth = LinearDepth(input.Depth(center.x, center.y), input.Orthographic);
		float total = input.Occlusion(center.x, center.y) * weights[0];
		float weightSum = weights[0];
		auto tap = [&](int offset, float weight) {
			const glm::ivec2 texel = glm::clamp(center + axis * offset, glm::ivec2(0), input.Area - 1);
			const float sampleDepth = LinearDepth(input.Depth(texel.x, texel.y), input.Orthographic);
			weight *= std::exp(-std::abs(sampleDepth - centerDepth) * 4.0f / std::max(centerDepth * 0.05f, 1e-3f));
			total += input.Occlusion(texel.x, texel.y) * weight;
			weightSum += weight;
		};
		for (int i = 1; i < 5; i++)
		{
			const int lower = i * 3 / 2;
			const bool split = (i & 1) == 1;
			for (int side = -1; side <= 1; side += 2)
			{
				tap(side * lower, split ? weights[i] * 0.5f : weights[i]);
				if (split)
					tap(side * (lower + 1), weights[i] * 0.5f);
			}
		}
		return total / weightSum;
	}

	// Runs the renderer's blur pipeline (Fullscreen.vert + SSAOBlur.frag, the renderer's binding layout and push
	// constants) once along axis, outside the area poisoning both inputs with NaN, and returns the area's results.
	std::vector<float> RunBlur(GraphicsDevice& graphics, const BlurInput& input, glm::ivec2 axis)
	{
		nvrhi::IDevice* device = graphics.GetDevice();
		auto makeTexture = [&](const char* name, bool renderTarget) {
			nvrhi::TextureDesc desc;
			desc.width = static_cast<uint32_t>(input.Target.x);
			desc.height = static_cast<uint32_t>(input.Target.y);
			desc.format = nvrhi::Format::R32_FLOAT;
			desc.isRenderTarget = renderTarget;
			desc.initialState = renderTarget ? nvrhi::ResourceStates::RenderTarget : nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = name;
			return device->createTexture(desc);
		};
		const auto occlusion = makeTexture("BlurTestOcclusion", false);
		const auto depth = makeTexture("BlurTestDepth", false);
		const auto output = makeTexture("BlurTestOutput", true);
		REQUIRE(occlusion);
		REQUIRE(depth);
		REQUIRE(output);

		nvrhi::BindingLayoutDesc layoutDesc = MakeLayout(0, nvrhi::ShaderType::Pixel);
		layoutDesc.bindings = {
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(SsaoBlurPush)),
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Texture_SRV(1),
			nvrhi::BindingLayoutItem::Sampler(2),
		};
		const auto layout = device->createBindingLayout(layoutDesc);
		const auto sampler = device->createSampler(nvrhi::SamplerDesc().setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
		const auto set = device->createBindingSet(nvrhi::BindingSetDesc()
													  .addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(SsaoBlurPush)))
													  .addItem(nvrhi::BindingSetItem::Texture_SRV(0, occlusion))
													  .addItem(nvrhi::BindingSetItem::Texture_SRV(1, depth))
													  .addItem(nvrhi::BindingSetItem::Sampler(2, sampler)),
												  layout);
		const auto framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(output));
		const auto vertex = CreateEmbeddedShader(device, "Fullscreen.vert", nvrhi::ShaderType::Vertex);
		const auto pixel = CreateEmbeddedShader(device, "SSAOBlur.frag", nvrhi::ShaderType::Pixel);
		REQUIRE(layout);
		REQUIRE(set);
		REQUIRE(vertex);
		REQUIRE(pixel);
		const auto pipeline = CreateFullscreenPipeline(device, vertex, pixel, layout, framebuffer);
		REQUIRE(pipeline);

		const float nan = std::numeric_limits<float>::quiet_NaN();
		std::vector<float> occlusionTexels(static_cast<size_t>(input.Target.x) * input.Target.y, nan);
		std::vector<float> depthTexels(occlusionTexels.size(), nan);
		for (int y = 0; y < input.Area.y; y++)
			for (int x = 0; x < input.Area.x; x++)
			{
				occlusionTexels[static_cast<size_t>(y) * input.Target.x + x] = input.Occlusion(x, y);
				depthTexels[static_cast<size_t>(y) * input.Target.x + x] = input.Depth(x, y);
			}

		const SsaoBlurPush push{ axis, Near, Far, input.Orthographic ? 1.0f : 0.0f, 0.0f, input.Area };
		const auto commands = device->createCommandList();
		commands->open();
		commands->writeTexture(occlusion, 0, 0, occlusionTexels.data(), static_cast<size_t>(input.Target.x) * sizeof(float));
		commands->writeTexture(depth, 0, 0, depthTexels.data(), static_cast<size_t>(input.Target.x) * sizeof(float));
		commands->clearTextureFloat(output, nvrhi::AllSubresources, nvrhi::Color(-1.0f));
		DrawFullscreen(commands, pipeline, framebuffer, set, nvrhi::Viewport(static_cast<float>(input.Area.x), static_cast<float>(input.Area.y)), &push, sizeof(push));
		const auto staging = device->createStagingTexture(output->getDesc(), nvrhi::CpuAccessMode::Read);
		REQUIRE(staging);
		commands->copyTexture(staging, nvrhi::TextureSlice(), output, nvrhi::TextureSlice());
		commands->close();
		device->executeCommandList(commands);
		device->waitForIdle();

		size_t pitch = 0;
		const auto* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
		REQUIRE(mapped);
		std::vector<float> result(static_cast<size_t>(input.Area.x) * input.Area.y);
		for (int y = 0; y < input.Area.y; y++)
			std::memcpy(result.data() + static_cast<size_t>(y) * input.Area.x, mapped + static_cast<size_t>(y) * pitch, static_cast<size_t>(input.Area.x) * sizeof(float));
		device->unmapStagingTexture(staging);
		return result;
	}

	// Blurs along both axes and compares every texel of the area with the CPU reference.
	void CheckBlur(GraphicsDevice& graphics, const BlurInput& input)
	{
		for (const glm::ivec2 axis : { glm::ivec2(1, 0), glm::ivec2(0, 1) })
		{
			CAPTURE(axis.x);
			const std::vector<float> result = RunBlur(graphics, input, axis);
			float maxError = 0.0f;
			for (int y = 0; y < input.Area.y; y++)
				for (int x = 0; x < input.Area.x; x++)
				{
					const float actual = result[static_cast<size_t>(y) * input.Area.x + x];
					REQUIRE_MESSAGE(std::isfinite(actual), "texel (" << x << ", " << y << ") read outside the render area");
					maxError = std::max(maxError, std::abs(actual - Reference(input, axis, { x, y })));
				}
			// A tap that point-sampled one of the two texels instead of splitting misses by a tenth of the occlusion
			// step or more; float rounding stays orders of magnitude below that.
			CHECK(maxError < 1e-4f);
		}
	}

	// Distinct, irregular values per texel, so every tap's texel and weight show in the result.
	float Pattern(int x, int y)
	{
		return static_cast<float>((x * 7 + y * 13) % 17) / 16.0f;
	}

}

TEST_CASE("SSAO blur taps split between texels, stay in the render area and respect depth")
{
	auto graphics = CreateTestGraphics();
	const auto flat = [](int, int) { return 0.5f; };

	SUBCASE("A non-dyadic area inside larger targets")
	{
		CheckBlur(*graphics, { { 41, 29 }, { 23, 17 }, Pattern, flat });
	}
	SUBCASE("Degenerate areas clamp every tap to the one row or column")
	{
		for (const glm::ivec2 area : { glm::ivec2(1, 1), glm::ivec2(1, 7), glm::ivec2(7, 1) })
		{
			CAPTURE(area.x);
			CAPTURE(area.y);
			CheckBlur(*graphics, { { 9, 9 }, area, Pattern, flat });
		}
	}
	SUBCASE("A depth step stops taps that cross it")
	{
		const auto step = [](int x, int y) { return x + y < 14 ? 0.2f : 0.995f; };
		CheckBlur(*graphics, { { 24, 24 }, { 19, 15 }, Pattern, step });
	}
	SUBCASE("Orthographic depth is already linear")
	{
		const auto ramp = [](int x, int y) { return 0.01f * static_cast<float>(x) + (y > 6 ? 0.5f : 0.0f); };
		CheckBlur(*graphics, { { 24, 24 }, { 19, 15 }, Pattern, ramp, true });
	}
	graphics->WaitIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
}
