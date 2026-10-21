#pragma once

#include "Basalt/Asset/MeshSource.h"

namespace Basalt {

	// Import-time geometry processing, before publication or GPU upload. Vertex records move together.
	// Blended primitives retain triangle order; skinned primitives receive no reduced LODs.
	namespace MeshOptimizer {
		// Rejects empty, malformed or non-finite geometry without changing any buffer. Morph deltas (morphTargets *
		// vertices.size(), target-major) follow their vertices through the reorder and the removal of unused vertices.
		bool Optimize(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices, bool reorderTriangles, std::vector<MorphDelta>* morphDeltas = nullptr, uint32_t morphTargets = 0);
		// Appends reduced ranges for an unpublished static, non-blended submesh, once.
		void GenerateLODs(MeshSource& source, uint32_t submeshIndex);
		// 0 means full detail; N selects LODs[N-1]. Non-finite inputs or lodDistance <= 0 disable it.
		// Thresholds double for each stored level, capped at LODs.size().
		uint32_t SelectLOD(const Submesh& submesh, float distance, float lodDistance);
	}

}
