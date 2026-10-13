#pragma once

#include "Basalt/Core/UUID.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace Basalt {

	constexpr uint32_t LocalShadowAtlasSize = 4096;
	constexpr uint32_t LocalShadowTileSize = 512;
	constexpr uint32_t LocalShadowTilesPerRow = LocalShadowAtlasSize / LocalShadowTileSize;
	constexpr uint32_t MaxLocalShadowMaps = LocalShadowTilesPerRow * LocalShadowTilesPerRow;
	constexpr float MaxLocalShadowSoftness = 4.0f;
	// Widen both point faces and spot projections to keep the PCF footprint inside rendered geometry.
	// The extra texel beyond the radius covers the hardware comparison filter.
	constexpr float LocalShadowGuardTexels = 6.0f;
	static_assert(LocalShadowGuardTexels >= MaxLocalShadowSoftness + 1.0f);
	static_assert(LocalShadowAtlasSize % LocalShadowTileSize == 0);

	std::string ValidateLocalShadowSoftness(float softness);
	float SanitizeLocalShadowSoftness(float softness);

	enum class LocalShadowType
	{
		Point,
		Spot
	};

	// One eligible light in the current frame's light buffer; allocation never changes that buffer's order.
	struct LocalShadowRequest
	{
		UUID ID = 0;
		uint32_t LightIndex = 0;
		LocalShadowType Type = LocalShadowType::Point;
		glm::vec3 Position = glm::vec3(0.0f);
		float Range = 10.0f;
		// Spot-only: world direction and cone half-angle in degrees.
		glm::vec3 Direction = { 0.0f, 0.0f, -1.0f };
		float OuterAngle = 30.0f;
	};

	// Contiguous atlas tiles: a spot owns one, a point owns all six (+X, -X, +Y, -Y, +Z, -Z).
	struct LocalShadowAllocation
	{
		uint32_t LightIndex = 0;
		uint32_t FirstMap = 0;
		uint32_t MapCount = 0;
	};

	// Shared CPU/std430 layout, including a normalized atlas rectangle (scale.xy, offset.zw).
	struct LocalShadowMap
	{
		glm::mat4 ViewProjection = glm::mat4(1.0f);
		glm::vec4 AtlasRect = glm::vec4(0.0f);
	};
	static_assert(sizeof(LocalShadowMap) == 80);
	static_assert(offsetof(LocalShadowMap, AtlasRect) == 64);
	static_assert(std::is_trivially_copyable_v<LocalShadowMap>);

	// Rebuilt each frame: invalid lights or wholly offscreen influence bounds get no maps.
	// Largest projected influence first (capped at full screen), then
	// UUID and type. Equal-sized tiles bound memory and draw work; a point allocation is all-or-nothing.
	struct LocalShadowPlan
	{
		std::vector<LocalShadowAllocation> Allocations;
		std::vector<LocalShadowMap> Maps;
		uint32_t Skipped = 0; // Visible eligible lights that did not fit the map budget.
	};

	// Headless per-frame planning, kept separate from GPU resources so allocation and projection are testable.
	LocalShadowPlan BuildLocalShadowPlan(std::span<const LocalShadowRequest> requests, const glm::mat4& view,
										 const glm::mat4& projection, uint32_t mapBudget = MaxLocalShadowMaps);

}
