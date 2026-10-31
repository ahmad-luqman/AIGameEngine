#include "Basalt/Asset/MeshOptimizer.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>

namespace Basalt::MeshOptimizer {

	bool WithinValenceCost(std::span<const uint32_t> indices, size_t vertexCount, uint64_t& allowance)
	{
		const uint64_t perIndex = MaxValenceCostPerIndex * indices.size();
		const uint64_t limit = perIndex + allowance;
		std::vector<uint32_t> valences(vertexCount);
		uint64_t cost = 0;
		// Each new reference to a vertex of valence v raises its square by 2v + 1. Stopping at the limit keeps the sum and
		// the valences far from wrapping.
		for (const uint32_t index : indices)
		{
			if (index >= vertexCount)
				return false;
			cost += 2 * uint64_t{ valences[index]++ } + 1;
			if (cost > limit)
				return false;
		}
		if (cost > perIndex)
			allowance -= cost - perIndex;
		return true;
	}

	bool WithinValenceCost(std::span<const uint32_t> indices, size_t vertexCount)
	{
		uint64_t allowance = ValenceCostAllowance;
		return WithinValenceCost(indices, vertexCount, allowance);
	}

	OptimizeResult Optimize(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices, bool reorderTriangles, std::vector<MorphDelta>* morphDeltas, uint32_t morphTargets, uint64_t* valenceAllowance)
	{
		if (vertices.empty() || indices.empty() || indices.size() % 3 != 0)
			return OptimizeResult::Invalid;
		if (morphTargets > 0 && (!morphDeltas || morphDeltas->size() != static_cast<size_t>(morphTargets) * vertices.size()))
			return OptimizeResult::Invalid;
		for (const auto index : indices)
			if (index >= vertices.size())
				return OptimizeResult::Invalid;
		for (const auto& vertex : vertices)
			for (int axis = 0; axis < 3; ++axis)
				if (!std::isfinite(vertex.Position[axis]))
					return OptimizeResult::NonFinite;
		// Vertex fetch order stays linear in the index count, so only triangle reordering depends on the valences.
		uint64_t wholeAllowance = ValenceCostAllowance;
		const bool pastValenceLimit = reorderTriangles && !WithinValenceCost(indices, vertices.size(), valenceAllowance ? *valenceAllowance : wholeAllowance);
		if (reorderTriangles && !pastValenceLimit)
		{
			meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), vertices.size());
			meshopt_optimizeOverdraw(indices.data(), indices.data(), indices.size(), &vertices[0].Position.x, vertices.size(), sizeof(Vertex), 1.05f);
		}
		// A remap (rather than meshopt_optimizeVertexFetch) so the morph deltas move with their vertices.
		std::vector<uint32_t> remap(vertices.size());
		const size_t count = meshopt_optimizeVertexFetchRemap(remap.data(), indices.data(), indices.size(), vertices.size());
		meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
		// Into a separate buffer: meshoptimizer's API does not promise in-place vertex remaps (index remaps are per element).
		std::vector<Vertex> remapped(count);
		meshopt_remapVertexBuffer(remapped.data(), vertices.data(), vertices.size(), sizeof(Vertex), remap.data());
		if (morphTargets > 0)
		{
			std::vector<MorphDelta> reordered(static_cast<size_t>(morphTargets) * count);
			for (size_t target = 0; target < morphTargets; target++)
				meshopt_remapVertexBuffer(reordered.data() + target * count, morphDeltas->data() + target * vertices.size(), vertices.size(), sizeof(MorphDelta), remap.data());
			*morphDeltas = std::move(reordered);
		}
		vertices = std::move(remapped);
		return pastValenceLimit ? OptimizeResult::ValenceCostExceeded : OptimizeResult::Optimized;
	}

	void GenerateLODs(MeshSource& source, uint32_t submeshIndex)
	{
		if (submeshIndex >= source.Submeshes.size())
			return;
		auto& submesh = source.Submeshes[submeshIndex];
		// Morphed shapes differ from the base the simplifier would see; skinned ones are never reduced either.
		if (!submesh.LODs.empty() || !submesh.JointBounds.empty() || submesh.MorphTargetCount > 0 || submesh.IndexCount < 12 || submesh.VertexCount == 0)
			return;
		if (submesh.MaterialIndex < source.Materials.size() && source.Materials[submesh.MaterialIndex].IsBlended())
			return;
		const auto range = std::span(source.Indices).subspan(submesh.BaseIndex, submesh.IndexCount);
		if (!WithinValenceCost(range, submesh.VertexCount))
			return;
		std::vector<uint32_t> base(range.begin(), range.end());
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
			// Collapses can raise the valences of the vertices that remain, so a level past the limit keeps the simplifier's
			// triangle order (defensive: removing rim vertices lowers the hubs' valences, so in the tested shapes each reduced
			// level costs a fraction of its base).
			if (WithinValenceCost(reduced, submesh.VertexCount))
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
