#include "Basalt/Renderer/GraphicsSettings.h"

#include "Basalt/Core/EnumNames.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Renderer/RenderMath.h"

namespace Basalt {
	nlohmann::json SerializeGraphicsSettings(const GraphicsSettings& s)
	{
		return { { "VSync", s.VSync }, { "RenderScale", s.RenderScale }, { "ShadowQuality", GraphicsQualityNames[static_cast<size_t>(s.ShadowQuality)] }, { "SSAOQuality", GraphicsQualityNames[static_cast<size_t>(s.SSAOQuality)] }, { "DynamicResolution", s.DynamicResolution.Enabled }, { "TargetFrameRate", s.DynamicResolution.TargetFrameRate }, { "MinRenderScale", s.DynamicResolution.MinScale } };
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
				(key == "VSync" ? next.VSync : next.DynamicResolution.Enabled) = value.get<bool>();
			}
			else if (key == "ShadowQuality" || key == "SSAOQuality")
			{
				const auto quality = value.is_string() ? EnumFromName<GraphicsQuality>(GraphicsQualityNames, value.get<std::string>()) : std::nullopt;
				if (!quality)
				{
					error = key + " must be Off, Low or High";
					return false;
				}
				(key == "ShadowQuality" ? next.ShadowQuality : next.SSAOQuality) = *quality;
			}
			else if (key == "RenderScale" || key == "MinRenderScale" || key == "TargetFrameRate")
			{
				float number = 0;
				const float min = key == "TargetFrameRate" ? MinTargetFrameRate : MinRenderScale;
				const float max = key == "TargetFrameRate" ? maxTargetFrameRate : MaxRenderScale;
				if (!JsonToFloat(value, number) || number < min || number > max)
				{
					error = key + " must be a number in [" + std::to_string(min) + ", " + std::to_string(max) + "]";
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
