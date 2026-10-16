#pragma once

#include "Basalt/Renderer/DynamicResolution.h"

#include <nlohmann/json_fwd.hpp>

#include <iterator>
#include <optional>
#include <string>

namespace Basalt {
	enum class GraphicsQuality
	{
		Off,
		Low,
		High
	};
	inline constexpr const char* GraphicsQualityNames[] = { "Off", "Low", "High" };
	inline constexpr size_t GraphicsQualityCount = 3;
	static_assert(std::size(GraphicsQualityNames) == GraphicsQualityCount);
	const char* GraphicsQualityName(GraphicsQuality quality);
	// Shared shape for project defaults and transient Play preferences. Scripts only change the latter.
	struct GraphicsSettings
	{
		bool VSync = true;
		float RenderScale = 1.0f;
		GraphicsQuality ShadowQuality = GraphicsQuality::High;
		GraphicsQuality SSAOQuality = GraphicsQuality::High;
		DynamicResolutionSettings DynamicResolution;
	};
	// Explicit host overrides stay above script preferences, keeping diagnostic CLI runs reproducible.
	struct GraphicsOverrides
	{
		std::optional<bool> VSync;
		std::optional<float> RenderScale;
		std::optional<bool> DynamicResolution;
		std::optional<float> TargetFrameRate;
		std::optional<float> MinRenderScale;
		GraphicsSettings Apply(GraphicsSettings settings) const;
	};
	nlohmann::json SerializeGraphicsSettings(const GraphicsSettings& settings);
	// Strict, atomic partial update. The same spelling is used by project defaults and Lua.
	// All Play sessions accept the diagnostic frame-rate ceiling; project defaults use MaxTargetFrameRate.
	bool PatchGraphicsSettings(const nlohmann::json& patch, GraphicsSettings& settings, std::string& error, float maxTargetFrameRate = MaxTargetFrameRate);
}
