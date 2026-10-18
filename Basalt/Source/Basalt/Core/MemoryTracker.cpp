#include "Basalt/Core/MemoryTracker.h"

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

	void MemoryTracker::Allocate(MemoryCategory category, size_t bytes)
	{
		if (bytes == 0)
			return;
		auto& state = GetState();
		std::lock_guard lock(state.Mutex);
		auto& stats = state.Statistics.at(static_cast<size_t>(category));
		stats.Bytes += bytes;
		stats.PeakBytes = std::max(stats.PeakBytes, stats.Bytes);
		++stats.Allocations;
	}

	void MemoryTracker::Free(MemoryCategory category, size_t bytes)
	{
		if (bytes == 0)
			return;
		auto& state = GetState();
		std::lock_guard lock(state.Mutex);
		auto& stats = state.Statistics.at(static_cast<size_t>(category));
		stats.Bytes -= bytes;
		--stats.Allocations;
	}

	void* MemoryTracker::LuaAllocator(void*, void* pointer, size_t oldSize, size_t newSize)
	{
		// Lua passes a type tag as oldSize when pointer is null.
		if (!pointer)
			oldSize = 0;
		if (newSize == 0)
		{
			std::free(pointer);
			Free(MemoryCategory::Lua, oldSize);
			return nullptr;
		}
		void* result = std::realloc(pointer, newSize);
		if (result)
		{
			Free(MemoryCategory::Lua, oldSize);
			Allocate(MemoryCategory::Lua, newSize);
		}
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
		MemoryTracker::Free(m_Category, m_Bytes);
		m_Bytes = bytes;
		MemoryTracker::Allocate(m_Category, m_Bytes);
	}

}
