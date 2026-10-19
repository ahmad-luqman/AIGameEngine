#pragma once

#include <cstdint>
#include <functional>
#include <tuple>

namespace Basalt {

	// State shared by instances of an indexed mesh draw. Model/normal matrices, material index,
	// pick token and palette offset/count vary per instance. Non-owning identity pointers keep
	// this key independent of GPU headers; std::less gives them a total order for key lookup.
	struct DrawBatchKey
	{
		const void* Mesh = nullptr;
		const void* MaterialSet = nullptr;
		uint32_t BaseIndex = 0;
		uint32_t BaseVertex = 0;
		uint32_t IndexCount = 0;
		bool DoubleSided = false;
		bool AlphaTested = false;

		bool operator==(const DrawBatchKey&) const = default;
		bool operator<(const DrawBatchKey& other) const
		{
			if (Mesh != other.Mesh)
				return std::less<const void*>{}(Mesh, other.Mesh);
			if (MaterialSet != other.MaterialSet)
				return std::less<const void*>{}(MaterialSet, other.MaterialSet);
			return std::tie(BaseIndex, BaseVertex, IndexCount, DoubleSided, AlphaTested) <
				   std::tie(other.BaseIndex, other.BaseVertex, other.IndexCount, other.DoubleSided, other.AlphaTested);
		}
	};

	// Contiguous draw records and startInstanceLocation == 0 are required: shaders use firstDraw + gl_InstanceIndex.
	inline bool CanInstanceDraw(const DrawBatchKey& first, uint32_t firstDraw, uint32_t count, const DrawBatchKey& next, uint32_t nextDraw)
	{
		return first == next && static_cast<uint64_t>(firstDraw) + count == nextDraw;
	}
}
