#pragma once

#include "Basalt/Renderer/DynamicResolution.h"

#include <nlohmann/json.hpp>

#include <string>

namespace Basalt {
	enum class GraphicsQuality
	{
		Off,
		Low,
		High
	};
	inline constexpr const char* GraphicsQualityNames[] = { "Off", "Low", "High" };
	// Session preferences initialized from the project; scripts never mutate the saved project.
	struct GraphicsSettings
	{
		bool VSync = true;
		float RenderScale = 1.0f;
		GraphicsQuality ShadowQuality = GraphicsQuality::High;
		GraphicsQuality SSAOQuality = GraphicsQuality::High;
		DynamicResolutionSettings DynamicResolution;
	};
	nlohmann::json SerializeGraphicsSettings(const GraphicsSettings& settings);
	// Strict, atomic partial update. The same spelling is used by project defaults and Lua.
	// Runtime sessions accept the CLI diagnostic frame-rate ceiling; project defaults use MaxTargetFrameRate.
	bool PatchGraphicsSettings(const nlohmann::json& patch, GraphicsSettings& settings, std::string& error, float maxTargetFrameRate = MaxTargetFrameRate);
}
