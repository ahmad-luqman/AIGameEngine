#pragma once

#include "Basalt/Math/AABB.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace Basalt {

	// View-frustum planes (ax + by + cz + d >= 0 is inside) of a view-projection matrix with a 0..1
	// depth range.
	struct Frustum
	{
		glm::vec4 Planes[6];

		explicit Frustum(const glm::mat4& viewProjection);
		bool Intersects(const AABB& box) const;
	};

	// The morph targets a draw blends: the nonzero finite weights, strongest (by |weight|) first and in target order
	// among equals, at most `maxActive` of them. Returns how many active weights were dropped. Upload order is the
	// shader's summation order, so it must not depend on anything but the weights.
	size_t SelectMorphWeights(std::span<const float> weights, size_t maxActive, std::vector<uint32_t>& outTargets);

	// Avoid a degenerate look-at basis for nearly vertical light directions.
	inline glm::vec3 ShadowUpVector(const glm::vec3& direction)
	{
		return std::abs(direction.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
	}

	// True for orthographic projections (no perspective divide).
	inline bool IsOrthographic(const glm::mat4& projection)
	{
		return projection[3][3] == 1.0f;
	}

	// Spatial upscalers allow one unchanged axis, but never downsample either axis.
	inline bool NeedsSpatialUpscale(glm::uvec2 renderSize, glm::uvec2 outputSize)
	{
		return renderSize.x > 0 && renderSize.y > 0 && renderSize.x <= outputSize.x && renderSize.y <= outputSize.y && renderSize != outputSize;
	}

	// FSR only runs below native size, so its input never needs the supersampled allocation ceiling.
	inline glm::uvec2 ComputeFsr1TargetSize(glm::uvec2 sceneTargetSize, glm::uvec2 outputSize)
	{
		return glm::min(sceneTargetSize, outputSize);
	}

	// Render scale range: below a quarter the image is unusable, above 2x (supersampling) a 4K window
	// would need 8K render targets.
	constexpr float MinRenderScale = 0.25f;
	constexpr float MaxRenderScale = 2.0f;
	// Largest render target side: Vulkan devices commonly support 16384 (MoltenVK, lavapipe), and windows
	// and screenshots are capped at that size already.
	constexpr uint32_t MaxRenderTargetSize = 16384;

	// Scene size before tonemapping and optional spatial upscaling to the output size. The scale is
	// clamped to [MinRenderScale, MaxRenderScale] (non-finite values mean 1), then lowered as far as needed
	// to keep both sides within MaxRenderTargetSize; each side is at least 1.
	inline glm::uvec2 ComputeRenderSize(uint32_t outputWidth, uint32_t outputHeight, float scale)
	{
		scale = std::isfinite(scale) ? std::clamp(scale, MinRenderScale, MaxRenderScale) : 1.0f;
		const uint32_t largest = std::max(outputWidth, outputHeight);
		if (static_cast<double>(largest) * scale > MaxRenderTargetSize)
			scale = static_cast<float>(static_cast<double>(MaxRenderTargetSize) / largest);
		auto scaled = [scale](uint32_t size) {
			return std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(size) * scale)));
		};
		return { scaled(outputWidth), scaled(outputHeight) };
	}

	// Size of a dynamic-resolution frame rendered at `scale` into targets of targetSize (sized for the render
	// scale, the highest `scale`). ComputeRenderSize grows monotonically with the scale, so this only clips
	// when the scale exceeds the one the targets were made for; the clip keeps the viewport inside them.
	inline glm::uvec2 ComputeDynamicRenderSize(uint32_t outputWidth, uint32_t outputHeight, float scale, glm::uvec2 targetSize)
	{
		return glm::min(ComputeRenderSize(outputWidth, outputHeight, scale), targetSize);
	}

	// Effective scale of a render of `renderedPixels` for an output of outputSize: sqrt of the pixel ratio.
	// Rounding each side makes it differ slightly from the requested scale; GPU time follows the pixel count,
	// so dynamic resolution uses this.
	inline float ComputePixelScale(uint64_t renderedPixels, glm::uvec2 outputSize)
	{
		const double output = std::max(static_cast<double>(outputSize.x) * outputSize.y, 1.0);
		return static_cast<float>(std::sqrt(static_cast<double>(renderedPixels) / output));
	}

	// View-space distances at which shadow cascades end, covering [near, shadowFar]. Perspective cameras use
	// the practical split scheme (lambda blends logarithmic and uniform splits); orthographic cameras and
	// cameras with a non-positive near plane use uniform splits (the logarithm is undefined there).
	template<size_t Count>
	std::array<float, Count> ComputeCascadeSplits(float nearClip, float shadowFar, float lambda, bool orthographic)
	{
		std::array<float, Count> splits{};
		const bool logarithmic = !orthographic && nearClip > 0.0f && shadowFar > nearClip;
		const float farClip = std::max(shadowFar, nearClip + 1e-3f);
		for (size_t i = 0; i < Count; i++)
		{
			const float p = static_cast<float>(i + 1) / static_cast<float>(Count);
			const float uniform = nearClip + (farClip - nearClip) * p;
			splits[i] = logarithmic ? lambda * (nearClip * std::pow(farClip / nearClip, p)) + (1.0f - lambda) * uniform : uniform;
		}
		return splits;
	}

}
