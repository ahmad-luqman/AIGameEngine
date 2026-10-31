#pragma once

#include "Basalt/Asset/MeshSource.h"

namespace Basalt {

	// Import-time geometry processing, before publication or GPU upload. Vertex records move together.
	// Blended primitives retain triangle order; skinned and morphed primitives receive no reduced LODs.
	namespace MeshOptimizer {
		// meshoptimizer's vertex cache ordering and simplifier revisit a vertex's remaining triangles each time the vertex
		// is touched, so their time grows with the sum of squared vertex valences (index references per vertex) rather
		// than with the index count: 46116 indices naming one vertex took 22 s to order in the fuzz build (found by
		// FuzzGltf). Geometry may cost this much per index, plus ValenceCostAllowance in total, before both are skipped.
		// At 64 per index the squared term stays near the linear work (both measured with ASan); ordinary meshes cost about
		// 6 per index (a closed grid exactly 6), and the allowance (about 0.2 s with ASan) keeps a small fan optimized.
		inline constexpr uint64_t MaxValenceCostPerIndex = 64;
		inline constexpr uint64_t ValenceCostAllowance = uint64_t{ 1 } << 24;

		// Rejects empty, malformed or non-finite geometry, geometry past the valence cost limit, or a morph delta buffer
		// of the wrong size, without changing any buffer. Morph deltas (morphTargets * vertices.size(), target-major) follow their vertices through the reorder
		// and the removal of unused vertices.
		bool Optimize(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices, bool reorderTriangles, std::vector<MorphDelta>* morphDeltas = nullptr, uint32_t morphTargets = 0);
		// Appends reduced ranges for an unpublished unskinned, unmorphed, non-blended submesh within the valence cost
		// limit, once.
		void GenerateLODs(MeshSource& source, uint32_t submeshIndex);
		// 0 means full detail; N selects LODs[N-1]. Non-finite inputs or lodDistance <= 0 disable it.
		// Thresholds double for each stored level, capped at LODs.size().
		uint32_t SelectLOD(const Submesh& submesh, float distance, float lodDistance);
	}

}
