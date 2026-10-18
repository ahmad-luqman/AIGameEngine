#include "Basalt/Core/MemoryTracker.h"

#include "Basalt/Core/Assert.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <mutex>
#include <utility>

namespace Basalt {

	namespace {
		struct TrackerState
		{
			std::mutex Mutex;
			std::array<MemoryStatistics, static_cast<size_t>(MemoryCategory::Count)> Statistics;
		};

		TrackerState& GetState()
		{
			// Asset caches and worker results can release their last charge during static destruction.
			// Keep the tiny accounting state alive through process exit, regardless of TU init order.
			static auto* const state = new TrackerState;
			return *state;
		}
	}

	MemoryStatistics MemoryTracker::Get(MemoryCategory category)
	{
		auto& state = GetState();
		std::lock_guard lock(state.Mutex);
		return state.Statistics.at(static_cast<size_t>(category));
	}

	nlohmann::json MemoryTracker::ToJson()
	{
		nlohmann::json result = nlohmann::json::object();
		for (uint8_t i = 0; i < static_cast<uint8_t>(MemoryCategory::Count); ++i)
		{
			const auto category = static_cast<MemoryCategory>(i);
			const auto stats = Get(category);
			result[GetName(category)] = { { "bytes", stats.Bytes }, { "peakBytes", stats.PeakBytes }, { "allocations", stats.Allocations } };
		}
		return result;
	}

	const char* MemoryTracker::GetName(MemoryCategory category)
	{
		constexpr std::array Names{ "assets", "gpuBuffers", "lua" };
		return Names.at(static_cast<size_t>(category));
	}

	namespace {
		void AdjustStatistics(MemoryStatistics& stats, size_t oldSize, size_t newSize) noexcept
		{
			BS_CORE_ASSERT(stats.Bytes >= oldSize && (oldSize == 0 || stats.Allocations > 0), "Unbalanced memory charge");
			// Guard even in Dist: an accounting mistake must not wrap to enormous values.
			stats.Bytes -= std::min(stats.Bytes, static_cast<uint64_t>(oldSize));
			if (oldSize != 0 && stats.Allocations != 0)
				--stats.Allocations;
			stats.Bytes += newSize;
			if (newSize != 0)
				++stats.Allocations;
			stats.PeakBytes = std::max(stats.PeakBytes, stats.Bytes);
		}
	}

	void MemoryTracker::Adjust(MemoryCategory category, size_t oldSize, size_t newSize) noexcept
	{
		auto& state = GetState();
		std::lock_guard lock(state.Mutex);
		AdjustStatistics(state.Statistics.at(static_cast<size_t>(category)), oldSize, newSize);
	}

	void* MemoryTracker::LuaAllocator(void*, void* pointer, size_t oldSize, size_t newSize) noexcept
	{
		// Lock before realloc: once it succeeds, reporting failure would lose the moved allocation.
		// A broken accounting mutex is fatal, never an exception unwinding through Lua's C frames.
		auto& state = GetState();
		std::lock_guard lock(state.Mutex);
		if (!pointer)
			oldSize = 0; // Lua passes a type tag for a new allocation.
		void* result = nullptr;
		if (newSize == 0)
			std::free(pointer);
		else
		{
			result = std::realloc(pointer, newSize);
			if (!result)
				return nullptr;
		}
		AdjustStatistics(state.Statistics[static_cast<size_t>(MemoryCategory::Lua)], oldSize, newSize);
		return result;
	}

	MemoryAllocation::MemoryAllocation(MemoryCategory category, size_t bytes)
		: m_Category(category)
	{
		Resize(bytes);
	}

	MemoryAllocation::~MemoryAllocation()
	{
		Resize(0);
	}
	MemoryAllocation::MemoryAllocation(const MemoryAllocation& other)
		: MemoryAllocation(other.m_Category, other.m_Bytes)
	{}
	MemoryAllocation& MemoryAllocation::operator=(const MemoryAllocation& other)
	{
		if (this != &other)
		{
			Resize(0);
			m_Category = other.m_Category;
			Resize(other.m_Bytes);
		}
		return *this;
	}
	MemoryAllocation::MemoryAllocation(MemoryAllocation&& other) noexcept
		: m_Category(other.m_Category)
		, m_Bytes(std::exchange(other.m_Bytes, 0))
	{}
	MemoryAllocation& MemoryAllocation::operator=(MemoryAllocation&& other) noexcept
	{
		if (this != &other)
		{
			Resize(0);
			m_Category = other.m_Category;
			m_Bytes = std::exchange(other.m_Bytes, 0);
		}
		return *this;
	}
	void MemoryAllocation::Resize(size_t bytes)
	{
		if (bytes == m_Bytes)
			return;
		MemoryTracker::Adjust(m_Category, m_Bytes, bytes);
		m_Bytes = bytes;
	}

}
