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
		// simplifier also welds vertices by position, so for it the sum is a lower bound. At 64 per index the squared term
		// stays near the linear work (both measured in the fuzz build); ordinary meshes cost at most about 6 per index (a
		// closed grid exactly 6), and the allowance (about 0.2 s there) keeps a vertex in up to about 4,000 triangles
		// affordable (fewer when several vertices are that busy). 64-bit, since they scale index counts up to
		// MaxModelIndices (64 x 2^26 = 2^32).
		inline constexpr uint64_t MaxValenceCostPerIndex = 64;
		inline constexpr uint64_t ValenceCostAllowance = uint64_t{ 1 } << 24;

		// Whether the indices' squared vertex valences sum to at most MaxValenceCostPerIndex per index plus
		// ValenceCostAllowance. False for an index past vertexCount.
		bool WithinValenceCost(std::span<const uint32_t> indices, size_t vertexCount);

		// How Optimize ended; only Optimized geometry gets LODs.
		enum class OptimizeResult
		{
			Optimized,
			// Past the valence cost limit, so the triangle order was kept; unused vertices were still removed and the rest
			// ordered for vertex fetch.
			TriangleOrderKept,
			// Empty, not whole triangles, an index past the vertices or a morph delta buffer of the wrong size; nothing changed.
			Invalid,
			// A non-finite position; nothing changed.
			NonFinite,
		};

		// Orders triangles for the vertex cache and overdraw (when reorderTriangles is set and the valence cost is within
		// the limit), then vertices for fetch, dropping unused ones. Morph deltas (morphTargets * vertices.size(),
		// target-major) follow their vertices.
		[[nodiscard]] OptimizeResult Optimize(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices, bool reorderTriangles, std::vector<MorphDelta>* morphDeltas = nullptr, uint32_t morphTargets = 0);
		// Appends reduced ranges for an unpublished unskinned, unmorphed, non-blended submesh, once. A submesh past the
		// valence cost limit, or with an index past its vertices, gets none (the importer only asks for Optimized geometry),
		// and a reduced level whose collapses push it past the limit keeps the simplifier's triangle order.
		void GenerateLODs(MeshSource& source, uint32_t submeshIndex);
		// 0 means full detail; N selects LODs[N-1]. Non-finite inputs or lodDistance <= 0 disable it.
		// Thresholds double for each stored level, capped at LODs.size().
		uint32_t SelectLOD(const Submesh& submesh, float distance, float lodDistance);
	}

}
