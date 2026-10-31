#pragma once

#include "Basalt/Asset/MeshSource.h"

#include <span>

namespace Basalt {

	// Import-time geometry processing, before publication or GPU upload. Vertex records move together.
	// Blended primitives retain triangle order; skinned and morphed primitives receive no reduced LODs.
	namespace MeshOptimizer {
		// meshoptimizer's vertex cache order rescans the remaining triangles of every cached vertex after each triangle it
		// emits, and its simplifier finds edges by scanning per-vertex lists, so both take time that grows with the sum of
		// squared vertex valences (index references per vertex) rather than with the index count: 46116 indices naming one
		// vertex took 22 s to order in the fuzz build (Debug with ASan, UBSan and coverage; found by FuzzGltf). The
		// simplifier also welds vertices by position, so for it the sum is a lower bound. Each primitive may cost
		// MaxValenceCostPerIndex per index, which keeps the squared work (vertex cache order, three simplifier passes and
		// their orders) a small part of the linear import work; ordinary meshes cost about 6 per index (a closed grid
		// exactly 6; seams lower it, poles raise it a little) and never touch the allowance. Costs past that draw on one
		// ValenceCostAllowance per model, since primitives can share their indices (eight sharing a busy accessor took 12 s;
		// found by review). The allowance fits a vertex named by about 2,000 triangle corners, and such a primitive imports,
		// LODs included, in a few tenths of a second in the fuzz build. 64-bit, since they scale index counts up to
		// MaxModelIndices.
		inline constexpr uint64_t MaxValenceCostPerIndex = 16;
		inline constexpr uint64_t ValenceCostAllowance = uint64_t{ 1 } << 22;

		// Whether the indices' squared vertex valences sum to at most MaxValenceCostPerIndex per index plus allowance, which
		// is reduced by the part of the cost past MaxValenceCostPerIndex per index when they do. False (and allowance kept)
		// for an index past vertexCount. Allocates one counter per vertex.
		bool WithinValenceCost(std::span<const uint32_t> indices, size_t vertexCount, uint64_t& allowance);
		// The same with a whole ValenceCostAllowance.
		bool WithinValenceCost(std::span<const uint32_t> indices, size_t vertexCount);

		// How Optimize ended; the importer generates LODs only for Optimized geometry.
		enum class OptimizeResult
		{
			Optimized,
			// Reordering was requested, but the valence cost is past the limit, so the triangle order was kept; unused
			// vertices were still removed and the rest ordered for vertex fetch.
			ValenceCostExceeded,
			// Empty, not whole triangles, an index past the vertices or a morph delta buffer of the wrong size; nothing changed.
			Invalid,
			// A non-finite position; nothing changed.
			NonFinite,
		};

		// Orders triangles for the vertex cache and overdraw (when reorderTriangles is set and the valence cost fits, drawing
		// on *valenceAllowance, a model's shared allowance, or on a whole one when it is null), then vertices for fetch,
		// dropping unused ones. Morph deltas (morphTargets * vertices.size(), target-major) follow their vertices.
		[[nodiscard]] OptimizeResult Optimize(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices, bool reorderTriangles, std::vector<MorphDelta>* morphDeltas = nullptr, uint32_t morphTargets = 0, uint64_t* valenceAllowance = nullptr);
		// Appends reduced ranges for an unpublished unskinned, unmorphed, non-blended submesh, once. A submesh past the
		// valence cost limit (with a whole allowance), or with an index past its vertices, gets none (a backstop: the
		// importer only calls it for Optimized geometry). A reduced level whose collapses push it past the limit keeps the
		// simplifier's triangle order.
		void GenerateLODs(MeshSource& source, uint32_t submeshIndex);
		// 0 means full detail; N selects LODs[N-1]. Non-finite inputs or lodDistance <= 0 disable it.
		// Thresholds double for each stored level, capped at LODs.size().
		uint32_t SelectLOD(const Submesh& submesh, float distance, float lodDistance);
	}

}
