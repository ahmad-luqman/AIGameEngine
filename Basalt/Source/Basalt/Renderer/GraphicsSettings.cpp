#include "Basalt/Renderer/GraphicsSettings.h"

#include "Basalt/Core/EnumNames.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Renderer/RenderMath.h"

namespace Basalt {
	const char* GraphicsQualityName(GraphicsQuality quality)
	{
		if (const char* name = EnumNameOrNull(GraphicsQualityNames, quality))
			return name;
		BS_CORE_ERROR("GraphicsSettings: invalid quality {}; writing Off", static_cast<int>(quality));
		return GraphicsQualityNames[0];
	}

	GraphicsSettings GraphicsOverrides::Apply(GraphicsSettings settings) const
	{
		if (VSync)
			settings.VSync = *VSync;
		if (RenderScale)
			settings.RenderScale = *RenderScale;
		if (DynamicResolution)
			settings.DynamicResolution.Enabled = *DynamicResolution;
		if (TargetFrameRate)
			settings.DynamicResolution.TargetFrameRate = *TargetFrameRate;
		if (MinRenderScale)
			settings.DynamicResolution.MinScale = *MinRenderScale;
		return settings;
	}
	nlohmann::json SerializeGraphicsSettings(const GraphicsSettings& s)
	{
		return { { "VSync", s.VSync }, { "RenderScale", s.RenderScale }, { "ShadowQuality", GraphicsQualityName(s.ShadowQuality) }, { "SSAOQuality", GraphicsQualityName(s.SSAOQuality) }, { "DynamicResolution", s.DynamicResolution.Enabled }, { "TargetFrameRate", s.DynamicResolution.TargetFrameRate }, { "MinRenderScale", s.DynamicResolution.MinScale } };
	}
	bool PatchGraphicsSettings(const nlohmann::json& patch, GraphicsSettings& settings, std::string& error, float maxTargetFrameRate)
	{
		if (!patch.is_object())
		{
			error = "graphics settings must be an object";
			return false;
		}
		GraphicsSettings next = settings;
		for (auto it = patch.begin(); it != patch.end(); ++it)
		{
			const auto& key = it.key();
			const auto& value = it.value();
			if (key == "VSync" || key == "DynamicResolution")
			{
				if (!value.is_boolean())
				{
					error = key + " must be true or false";
					return false;
				}
				if (key == "VSync")
					next.VSync = value.get<bool>();
				else
					next.DynamicResolution.Enabled = value.get<bool>();
			}
			else if (key == "ShadowQuality" || key == "SSAOQuality")
			{
				const auto quality = value.is_string() ? EnumFromName<GraphicsQuality>(GraphicsQualityNames, value.get<std::string>()) : std::nullopt;
				if (!quality)
				{
					error = key + " must be Off, Low or High";
					return false;
				}
				if (key == "ShadowQuality")
					next.ShadowQuality = *quality;
				else
					next.SSAOQuality = *quality;
			}
			else if (key == "RenderScale" || key == "MinRenderScale" || key == "TargetFrameRate")
			{
				float number = 0;
				const float min = key == "TargetFrameRate" ? MinTargetFrameRate : MinRenderScale;
				const float max = key == "TargetFrameRate" ? maxTargetFrameRate : MaxRenderScale;
				if (!JsonToFloat(value, number) || number < min || number > max)
				{
					error = fmt::format("{} must be a number in [{}, {}]", key, min, max);
					return false;
				}
				if (key == "RenderScale")
					next.RenderScale = number;
				else if (key == "MinRenderScale")
					next.DynamicResolution.MinScale = number;
				else
					next.DynamicResolution.TargetFrameRate = number;
			}
			else
			{
				error = "unknown graphics setting '" + key + "'";
				return false;
			}
		}
		settings = next;
		return true;
	}
}
