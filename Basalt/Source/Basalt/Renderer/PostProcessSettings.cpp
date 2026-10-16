#include "Basalt/Renderer/PostProcessSettings.h"

#include "Basalt/Core/EnumNames.h"
#include "Basalt/Scene/Entity.h"

#include <cmath>

namespace Basalt {
	std::string ValidatePostProcess(const PostProcessComponent& c)
	{
		if (!EnumNameOrNull(TonemapperNames, c.Tonemap))
			return "Tonemapper is invalid";
		if (!std::isfinite(c.Exposure) || c.Exposure < 0.0001f || c.Exposure > 65504.0f)
			return "Exposure must be in [0.0001, 65504.0]";
		if (!std::isfinite(c.MinExposure) || c.MinExposure < 0.0001f || c.MinExposure > 65504.0f)
			return "MinExposure must be in [0.0001, 65504.0]";
		if (!std::isfinite(c.MaxExposure) || c.MaxExposure < 0.0001f || c.MaxExposure > 65504.0f)
			return "MaxExposure must be in [0.0001, 65504.0]";
		if (!std::isfinite(c.ExposureCompensation) || c.ExposureCompensation < -16.0f || c.ExposureCompensation > 16.0f)
			return "ExposureCompensation must be in [-16.0, 16.0]";
		if (!std::isfinite(c.AdaptationSpeed) || c.AdaptationSpeed < 0.0f || c.AdaptationSpeed > 100.0f)
			return "AdaptationSpeed must be in [0.0, 100.0]";
		if (!std::isfinite(c.SSAORadius) || c.SSAORadius < 0.0001f || c.SSAORadius > 100.0f)
			return "SSAORadius must be in [0.0001, 100.0]";
		if (!std::isfinite(c.SSAOIntensity) || c.SSAOIntensity < 0.0f || c.SSAOIntensity > 10.0f)
			return "SSAOIntensity must be in [0.0, 10.0]";
		if (!std::isfinite(c.SSAOBias) || c.SSAOBias < 0.0f || c.SSAOBias > 1.0f)
			return "SSAOBias must be in [0.0, 1.0]";
		if (!std::isfinite(c.BloomThreshold) || c.BloomThreshold < 0.0f || c.BloomThreshold > 65504.0f)
			return "BloomThreshold must be in [0.0, 65504.0]";
		if (!std::isfinite(c.BloomIntensity) || c.BloomIntensity < 0.0f || c.BloomIntensity > 10.0f)
			return "BloomIntensity must be in [0.0, 10.0]";
		if (!std::isfinite(c.BloomScatter) || c.BloomScatter < 0.0f || c.BloomScatter > 1.0f)
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
		result.Exposure = legacy.Exposure;
		result.Tonemap = legacy.Tonemap;
		result.SSAOEnabled = legacy.SSAOEnabled;
		result.SSAORadius = legacy.SSAORadius;
		result.SSAOIntensity = legacy.SSAOIntensity;
		result.SSAOBias = legacy.SSAOBias;
		return result;
	}
}
