#include "Basalt/Renderer/LocalShadows.h"

#include "Basalt/Renderer/RenderMath.h"

#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

namespace Basalt {

	std::string ValidateLocalShadowSoftness(float softness)
	{
		return softness >= 0.0f && softness <= MaxLocalShadowSoftness ? std::string() : fmt::format("ShadowSoftness {} must be finite and in [0, {}]", softness, MaxLocalShadowSoftness);
	}

	float SanitizeLocalShadowSoftness(float softness)
	{
		return std::isfinite(softness) ? std::clamp(softness, 0.0f, MaxLocalShadowSoftness) : 1.0f;
	}

	namespace {

		bool IsFinite(const glm::vec3& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}

		bool IsValidRequest(const LocalShadowRequest& request, glm::vec3& direction)
		{
			if (!IsFinite(request.Position) || !std::isfinite(request.Range) || request.Range < 0.001f)
				return false;
			if (request.Type == LocalShadowType::Point)
				return true;
			const float length = glm::length(request.Direction);
			if (!IsFinite(request.Direction) || !std::isfinite(length) || length < 1e-6f ||
				!std::isfinite(request.OuterAngle) || request.OuterAngle <= 0.0f || request.OuterAngle >= 90.0f)
				return false;
			direction = request.Direction / length;
			return true;
		}

		struct Candidate
		{
			const LocalShadowRequest* Request;
			double Importance;
			glm::vec3 Direction;
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
			glm::vec3 direction(0.0f);
			if (!IsValidRequest(request, direction))
				continue;

			// Bound the illuminated spherical sector, rather than prioritizing a narrow spotlight as if
			// it illuminated its entire range sphere. The bound also catches offscreen lights whose
			// receivers are visible.
			glm::vec3 center = request.Position;
			float boundsRadius = request.Range;
			if (request.Type == LocalShadowType::Spot)
			{
				const float angle = glm::radians(request.OuterAngle);
				// Up to 45 degrees the sphere touches the apex and rim. Wider cones are bounded by
				// the rim's sphere, which already contains the apex; using the narrow formula wastes tiles.
				float centerDistance;
				if (request.OuterAngle <= 45.0f)
					centerDistance = boundsRadius = request.Range / (2.0f * std::cos(angle));
				else
				{
					centerDistance = request.Range * std::cos(angle);
					boundsRadius = request.Range * std::sin(angle);
				}
				center += direction * centerDistance;
			}
			bool visible = true;
			for (const auto& plane : frustum.Planes)
				visible &= glm::dot(glm::vec3(plane), center) + plane.w >= -boundsRadius;
			if (!visible)
				continue;
			// Conservative projected sphere area. Orthographic size ignores depth; perspective uses the
			// nearest sphere depth. The full-screen cap makes nearby screen-filling lights tie by UUID/type.
			const double depth = -static_cast<double>((view * glm::vec4(center, 1.0f)).z);
			const double radius = boundsRadius;
			const double denominator = IsOrthographic(projection) ? 1.0 : std::max(depth - radius, 0.001);
			const double width = std::min(2.0, std::abs(static_cast<double>(projection[0][0])) * radius / denominator);
			const double height = std::min(2.0, std::abs(static_cast<double>(projection[1][1])) * radius / denominator);
			const double importance = width * height;
			if (std::isfinite(importance) && importance > 0.0)
				candidates.push_back({ &request, importance, direction });
		}
		// One entity may carry both types, so UUID alone does not define a total order.
		std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
			return std::make_tuple(-a.Importance, static_cast<uint64_t>(a.Request->ID), a.Request->Type) <
				   std::make_tuple(-b.Importance, static_cast<uint64_t>(b.Request->ID), b.Request->Type);
		});

		struct CubeFace
		{
			glm::vec3 Direction;
			glm::vec3 Up;
		};
		// Must match PointShadowFace in Shadows.glsl: +X, -X, +Y, -Y, +Z, -Z.
		constexpr std::array<CubeFace, 6> CubeFaces = { CubeFace{ { 1, 0, 0 }, { 0, -1, 0 } },
														CubeFace{ { -1, 0, 0 }, { 0, -1, 0 } }, CubeFace{ { 0, 1, 0 }, { 0, 0, 1 } },
														CubeFace{ { 0, -1, 0 }, { 0, 0, -1 } }, CubeFace{ { 0, 0, 1 }, { 0, -1, 0 } }, CubeFace{ { 0, 0, -1 }, { 0, -1, 0 } } };
		constexpr float Scale = 1.0f / static_cast<float>(LocalShadowTilesPerRow);
		for (const auto& candidate : candidates)
		{
			const auto& request = *candidate.Request;
			const bool point = request.Type == LocalShadowType::Point;
			const uint32_t count = point ? 6u : 1u;
			if (plan.Maps.size() + count > mapBudget)
			{
				plan.Skipped++;
				continue;
			}
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
				const glm::vec3 direction = point ? CubeFaces[face].Direction : candidate.Direction;
				const glm::vec3 up = point ? CubeFaces[face].Up : ShadowUpVector(direction);
				const uint32_t tile = firstMap + face;
				const uint32_t row = tile / LocalShadowTilesPerRow;
				plan.Maps.push_back({ lightProjection * glm::lookAt(glm::vec3(0.0f), direction, up) * glm::translate(glm::mat4(1.0f), -request.Position),
									  glm::vec4(Scale, Scale, static_cast<float>(tile % LocalShadowTilesPerRow) * Scale, static_cast<float>(row) * Scale) });
			}
		}
		return plan;
	}

}
