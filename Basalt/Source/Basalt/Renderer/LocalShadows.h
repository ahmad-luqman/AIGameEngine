#pragma once

#include "Basalt/Core/UUID.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

namespace Basalt {

	constexpr uint32_t LocalShadowAtlasSize = 4096;
	constexpr uint32_t LocalShadowTileSize = 512;
	constexpr uint32_t LocalShadowTilesPerRow = LocalShadowAtlasSize / LocalShadowTileSize;
	constexpr uint32_t MaxLocalShadowMaps = LocalShadowTilesPerRow * LocalShadowTilesPerRow;
	constexpr float MaxLocalShadowSoftness = 4.0f;
	// Overlapping cube faces keep the entire PCF footprint inside rendered geometry at face edges.
	constexpr float LocalShadowGuardTexels = 6.0f;

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

	// Rebuilt each frame: offscreen/invalid lights get no maps; largest projected influence first, then
	// UUID and type. Equal-sized tiles bound memory and draw work; a point allocation is all-or-nothing.
	struct LocalShadowPlan
	{
		std::vector<LocalShadowAllocation> Allocations;
		std::vector<LocalShadowMap> Maps;
	};

	LocalShadowPlan BuildLocalShadowPlan(std::span<const LocalShadowRequest> requests, const glm::mat4& view,
										 const glm::mat4& projection, uint32_t mapBudget = MaxLocalShadowMaps);

}
