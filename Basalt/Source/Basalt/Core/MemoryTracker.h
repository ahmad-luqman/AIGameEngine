#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>

namespace Basalt {

	enum class MemoryCategory : uint8_t
	{
		Assets,
		GpuBuffers,
		Lua,
		Count
	};

	// Logical owned bytes, rather than process RSS or driver heap residency. Snapshots are thread safe.
	struct MemoryStatistics
	{
		uint64_t Bytes = 0;
		uint64_t PeakBytes = 0;
		uint64_t Allocations = 0;
	};

	// Subsystem counters independent of Tracy, available in headless and shipping builds.
	class MemoryTracker
	{
	public:
		static MemoryStatistics Get(MemoryCategory category);
		static nlohmann::json ToJson();
		static const char* GetName(MemoryCategory category);
		static void Allocate(MemoryCategory category, size_t bytes);
		static void Free(MemoryCategory category, size_t bytes);
		// Lua allocator: failed reallocations preserve both the original memory and its accounting.
		static void* LuaAllocator(void* user, void* pointer, size_t oldSize, size_t newSize);
	};

	// Lifetime accounting for an owned resource; moving transfers the charge and copying duplicates it.
	class MemoryAllocation
	{
	public:
		explicit MemoryAllocation(MemoryCategory category = MemoryCategory::Assets, size_t bytes = 0);
		~MemoryAllocation();
		MemoryAllocation(const MemoryAllocation& other);
		MemoryAllocation& operator=(const MemoryAllocation& other);
		MemoryAllocation(MemoryAllocation&& other) noexcept;
		MemoryAllocation& operator=(MemoryAllocation&& other) noexcept;
		void Resize(size_t bytes);

	private:
		MemoryCategory m_Category;
		size_t m_Bytes = 0;
	};

}
