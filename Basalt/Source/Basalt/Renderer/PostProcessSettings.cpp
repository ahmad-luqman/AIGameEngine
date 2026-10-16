#include "Basalt/Renderer/PostProcessSettings.h"

#include "Basalt/Core/EnumNames.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Scene/Entity.h"

#include <cmath>

namespace Basalt {
	namespace {
		constexpr float MaxHalfFloat = 65504.0f;
		bool FiniteInRange(float value, float min, float max)
		{
			return std::isfinite(value) && value >= min && value <= max;
		}
	}
	const char* TonemapperName(Tonemapper tonemapper)
	{
		if (const char* name = EnumNameOrNull(TonemapperNames, tonemapper))
			return name;
		BS_CORE_ERROR("PostProcess: invalid tonemapper {}; writing None", static_cast<int>(tonemapper));
		return TonemapperNames[0];
	}

	std::string ValidatePostProcess(const PostProcessComponent& c)
	{
		if (!EnumNameOrNull(TonemapperNames, c.Tonemap))
			return "Tonemapper is invalid";
		if (!FiniteInRange(c.Exposure, 0.0001f, MaxHalfFloat))
			return "Exposure must be in [0.0001, 65504.0]";
		if (!FiniteInRange(c.MinExposure, 0.0001f, MaxHalfFloat))
			return "MinExposure must be in [0.0001, 65504.0]";
		if (!FiniteInRange(c.MaxExposure, 0.0001f, MaxHalfFloat))
			return "MaxExposure must be in [0.0001, 65504.0]";
		if (!FiniteInRange(c.ExposureCompensation, -16.0f, 16.0f))
			return "ExposureCompensation must be in [-16.0, 16.0]";
		if (!FiniteInRange(c.AdaptationSpeed, 0.0f, 100.0f))
			return "AdaptationSpeed must be in [0.0, 100.0]";
		if (!FiniteInRange(c.SSAORadius, 0.0001f, 100.0f))
			return "SSAORadius must be in [0.0001, 100.0]";
		if (!FiniteInRange(c.SSAOIntensity, 0.0f, 10.0f))
			return "SSAOIntensity must be in [0.0, 10.0]";
		if (!FiniteInRange(c.SSAOBias, 0.0f, 1.0f))
			return "SSAOBias must be in [0.0, 1.0]";
		if (!FiniteInRange(c.BloomThreshold, 0.0f, MaxHalfFloat))
			return "BloomThreshold must be in [0.0, 65504.0]";
		if (!FiniteInRange(c.BloomIntensity, 0.0f, 10.0f))
			return "BloomIntensity must be in [0.0, 10.0]";
		if (!FiniteInRange(c.BloomScatter, 0.0f, 1.0f))
			return "BloomScatter must be in [0.0, 1.0]";
		if (c.MinExposure > c.MaxExposure)
			return "MinExposure must not exceed MaxExposure";
		return {};
	}

	PostProcessComponent ResolvePostProcess(const Scene& scene, UUID* selected)
	{
		const PostProcessComponent* winner = nullptr;
		UUID id = 0;
		for (auto entity : scene.GetRegistry().view<const IDComponent, const PostProcessComponent>())
		{
			const auto& c = scene.GetRegistry().get<PostProcessComponent>(entity);
			const UUID candidate = scene.GetRegistry().get<IDComponent>(entity).ID;
			if (c.Enabled && (!winner || c.Priority > winner->Priority || (c.Priority == winner->Priority && candidate < id)))
			{
				winner = &c;
				id = candidate;
			}
		}
		if (selected)
			*selected = id;
		if (winner)
			return *winner;
		const auto& legacy = scene.GetRendererSettings();
		PostProcessComponent result;
		CopyPostProcessSettings(legacy, result);
		return result;
	}
}
