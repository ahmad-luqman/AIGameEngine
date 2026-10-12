#include "Basalt/Renderer/SceneLights.h"

#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <algorithm>
#include <tuple>

namespace Basalt {

	std::vector<DirectionalLightInfo> CollectDirectionalLights(Scene& scene)
	{
		std::vector<DirectionalLightInfo> lights;
		auto view = scene.GetAllEntitiesWith<DirectionalLightComponent>();
		for (entt::entity handle : view)
		{
			Entity entity(handle, &scene);
			const auto& light = view.get<DirectionalLightComponent>(handle);
			const glm::vec3 direction = glm::vec3(scene.GetWorldTransform(entity) * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
			const float length = glm::length(direction);
			if (!(length > 1e-6f))
				continue;

			DirectionalLightInfo& info = lights.emplace_back();
			info.ID = entity.GetUUID();
			info.Direction = direction / length;
			info.Radiance = light.Color * light.Intensity;
			info.CastShadows = light.CastShadows;
			info.ShadowSoftness = light.ShadowSoftness;
		}

		auto rank = [](const DirectionalLightInfo& light) {
			const float luminance = glm::dot(light.Radiance, glm::vec3(0.2126f, 0.7152f, 0.0722f));
			// Negated so that an ascending sort puts shadow casters and bright lights first.
			return std::make_tuple(light.CastShadows ? 0 : 1, -luminance, static_cast<uint64_t>(light.ID));
		};
		std::sort(lights.begin(), lights.end(), [&](const DirectionalLightInfo& a, const DirectionalLightInfo& b) { return rank(a) < rank(b); });
		return lights;
	}

}
