#include "Basalt/Asset/MeshOptimizer.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>

namespace Basalt::MeshOptimizer {

	bool Optimize(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices, bool reorderTriangles)
	{
		if (vertices.empty() || indices.empty() || indices.size() % 3 != 0)
			return false;
		for (const auto index : indices)
			if (index >= vertices.size())
				return false;
		for (const auto& vertex : vertices)
			for (int axis = 0; axis < 3; ++axis)
				if (!std::isfinite(vertex.Position[axis]))
					return false;
		if (reorderTriangles)
		{
			meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), vertices.size());
			meshopt_optimizeOverdraw(indices.data(), indices.data(), indices.size(), &vertices[0].Position.x, vertices.size(), sizeof(Vertex), 1.05f);
		}
		const auto count = meshopt_optimizeVertexFetch(vertices.data(), indices.data(), indices.size(), vertices.data(), vertices.size(), sizeof(Vertex));
		vertices.resize(count);
		return true;
	}

	void GenerateLODs(MeshSource& source, uint32_t submeshIndex)
	{
		if (submeshIndex >= source.Submeshes.size())
			return;
		auto& submesh = source.Submeshes[submeshIndex];
		if (!submesh.LODs.empty() || !submesh.JointBounds.empty() || submesh.IndexCount < 12 || submesh.VertexCount == 0)
			return;
		if (submesh.MaterialIndex < source.Materials.size() && source.Materials[submesh.MaterialIndex].IsBlended())
			return;
		std::vector<uint32_t> base(source.Indices.begin() + submesh.BaseIndex, source.Indices.begin() + submesh.BaseIndex + submesh.IndexCount);
		const auto* positions = &source.Vertices[submesh.BaseVertex].Position.x;
		size_t previous = base.size();
		for (uint32_t level = 1; level <= 3; ++level)
		{
			const size_t target = (base.size() >> level) / 3 * 3;
			if (target < 3)
				break;
			std::vector<uint32_t> reduced(base.size());
			float error = 0;
			const size_t count = meshopt_simplify(reduced.data(), base.data(), base.size(), positions, submesh.VertexCount, sizeof(Vertex), target, 0.01f, meshopt_SimplifyLockBorder, &error);
			if (count == 0 || count >= previous)
				continue;
			reduced.resize(count);
			meshopt_optimizeVertexCache(reduced.data(), reduced.data(), reduced.size(), submesh.VertexCount);
			submesh.LODs.push_back({ static_cast<uint32_t>(source.Indices.size()), static_cast<uint32_t>(count), error });
			source.Indices.insert(source.Indices.end(), reduced.begin(), reduced.end());
			previous = count;
		}
	}

	uint32_t SelectLOD(const Submesh& submesh, float distance, float lodDistance)
	{
		if (!std::isfinite(distance) || !std::isfinite(lodDistance) || lodDistance <= 0 || distance < lodDistance)
			return 0;
		uint32_t level = 0;
		while (level < submesh.LODs.size() && distance >= lodDistance)
		{
			++level;
			lodDistance *= 2;
		}
		return level;
	}

}
