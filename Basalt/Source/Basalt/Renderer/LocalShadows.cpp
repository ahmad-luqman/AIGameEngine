#include "Basalt/Renderer/LocalShadows.h"

#include "Basalt/Renderer/RenderMath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

namespace Basalt {

	namespace {

		bool IsFinite(const glm::vec3& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}

		struct Candidate
		{
			const LocalShadowRequest* Request;
			double Importance;
		};

	}

	LocalShadowPlan BuildLocalShadowPlan(std::span<const LocalShadowRequest> requests, const glm::mat4& view,
										 const glm::mat4& projection, uint32_t mapBudget)
	{
		LocalShadowPlan plan;
		mapBudget = std::min(mapBudget, MaxLocalShadowMaps);
		const Frustum frustum(projection * view);
		std::vector<Candidate> candidates;
		for (const auto& request : requests)
		{
			if (!IsFinite(request.Position) || !std::isfinite(request.Range) || request.Range < 0.001f)
				continue;
			if (request.Type == LocalShadowType::Spot && (!IsFinite(request.Direction) ||
														  !std::isfinite(glm::length(request.Direction)) || glm::length(request.Direction) < 1e-6f ||
														  !std::isfinite(request.OuterAngle) || request.OuterAngle <= 0.0f || request.OuterAngle >= 90.0f))
				continue;

			// Bound the illuminated spherical sector, rather than prioritizing a narrow spotlight as if
			// it illuminated its entire range sphere. The bound also catches offscreen lights whose
			// receivers are visible.
			glm::vec3 center = request.Position;
			float boundsRadius = request.Range;
			if (request.Type == LocalShadowType::Spot)
			{
				const float angle = glm::radians(request.OuterAngle);
				const float centerDistance = request.OuterAngle <= 45.0f ? request.Range / (2.0f * std::cos(angle)) : request.Range * std::cos(angle);
				center += glm::normalize(request.Direction) * centerDistance;
				boundsRadius = request.OuterAngle <= 45.0f ? centerDistance : request.Range * std::sin(angle);
			}
			bool visible = true;
			for (const auto& plane : frustum.Planes)
				visible &= glm::dot(glm::vec3(plane), center) + plane.w >= -boundsRadius;
			if (!visible)
				continue;
			const double depth = -static_cast<double>((view * glm::vec4(center, 1.0f)).z);
			const double radius = boundsRadius;
			const double denominator = IsOrthographic(projection) ? 1.0 : std::max(depth - radius, 0.001);
			const double width = std::min(2.0, std::abs(static_cast<double>(projection[0][0])) * radius / denominator);
			const double height = std::min(2.0, std::abs(static_cast<double>(projection[1][1])) * radius / denominator);
			const double importance = width * height;
			if (std::isfinite(importance) && importance > 0.0)
				candidates.push_back({ &request, importance });
		}
		std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
			return std::make_tuple(-a.Importance, static_cast<uint64_t>(a.Request->ID), a.Request->Type) <
				   std::make_tuple(-b.Importance, static_cast<uint64_t>(b.Request->ID), b.Request->Type);
		});

		constexpr std::array<glm::vec3, 6> Directions = { glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0),
														  glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1) };
		constexpr std::array<glm::vec3, 6> Ups = { glm::vec3(0, -1, 0), glm::vec3(0, -1, 0),
												   glm::vec3(0, 0, 1), glm::vec3(0, 0, -1), glm::vec3(0, -1, 0), glm::vec3(0, -1, 0) };
		for (const auto& candidate : candidates)
		{
			const auto& request = *candidate.Request;
			const bool point = request.Type == LocalShadowType::Point;
			const uint32_t count = point ? 6u : 1u;
			if (plan.Maps.size() + count > mapBudget)
				continue;
			const auto firstMap = static_cast<uint32_t>(plan.Maps.size());
			plan.Allocations.push_back({ request.LightIndex, firstMap, count });
			// Relative near plane supports small lights without clipping away their whole range.
			const float nearClip = std::min(0.05f, request.Range * 0.01f);
			const float tangent = point ? 1.0f : std::tan(glm::radians(request.OuterAngle));
			const float expandedTangent = tangent * static_cast<float>(LocalShadowTileSize) /
										  (static_cast<float>(LocalShadowTileSize) - 2.0f * LocalShadowGuardTexels);
			const glm::mat4 lightProjection = glm::perspective(2.0f * std::atan(expandedTangent), 1.0f, nearClip, request.Range);
			for (uint32_t face = 0; face < count; face++)
			{
				const glm::vec3 direction = point ? Directions[face] : glm::normalize(request.Direction);
				const glm::vec3 up = point ? Ups[face] : (std::abs(direction.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0));
				const uint32_t tile = firstMap + face;
				const uint32_t row = tile / LocalShadowTilesPerRow;
				const float scale = 1.0f / static_cast<float>(LocalShadowTilesPerRow);
				plan.Maps.push_back({ lightProjection * glm::lookAt(glm::vec3(0.0f), direction, up) * glm::translate(glm::mat4(1.0f), -request.Position),
									  glm::vec4(scale, scale, static_cast<float>(tile % LocalShadowTilesPerRow) * scale, static_cast<float>(row) * scale) });
			}
		}
		return plan;
	}

}
