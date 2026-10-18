#include <doctest/doctest.h>

#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Core/JobSystem.h>
#include <Basalt/Core/MemoryTracker.h>

#include <sol/sol.hpp>

using namespace Basalt;

TEST_SUITE("Memory")
{
	TEST_CASE("Mesh metadata charges external file elements but excludes string backing storage")
	{
		AssetManager::Clear();
		JobSystem::Get().WaitIdle();
		const auto before = MemoryTracker::Get(MemoryCategory::Assets).Bytes;
		{
			MeshSource mesh;
			mesh.UpdateMemoryUsage();
			const auto empty = MemoryTracker::Get(MemoryCategory::Assets).Bytes;
			mesh.ExternalFiles.reserve(32);
			mesh.UpdateMemoryUsage();
			const auto metadata = MemoryTracker::Get(MemoryCategory::Assets).Bytes;
			CHECK(metadata == empty + mesh.ExternalFiles.capacity() * sizeof(std::string));
			mesh.ExternalFiles.emplace_back(4096, 'x');
			mesh.UpdateMemoryUsage();
			CHECK(MemoryTracker::Get(MemoryCategory::Assets).Bytes == metadata);
		}
		CHECK(MemoryTracker::Get(MemoryCategory::Assets).Bytes == before);
	}

	TEST_CASE("Charges follow copy move resize and destruction")
	{
		const auto before = MemoryTracker::Get(MemoryCategory::GpuBuffers);
		{
			MemoryAllocation first(MemoryCategory::GpuBuffers, 100);
			MemoryAllocation copy(first);
			MemoryAllocation moved(std::move(copy));
			CHECK(MemoryTracker::Get(MemoryCategory::GpuBuffers).Bytes == before.Bytes + 200);
			CHECK(MemoryTracker::Get(MemoryCategory::GpuBuffers).Allocations == before.Allocations + 2);
			moved.Resize(60);
			first = std::move(moved);
			CHECK(MemoryTracker::Get(MemoryCategory::GpuBuffers).Bytes == before.Bytes + 60);
			copy = first;
			CHECK(MemoryTracker::Get(MemoryCategory::GpuBuffers).Bytes == before.Bytes + 120);
			CHECK(MemoryTracker::Get(MemoryCategory::GpuBuffers).PeakBytes >= before.Bytes + 200);
		}
		CHECK(MemoryTracker::Get(MemoryCategory::GpuBuffers).Bytes == before.Bytes);
		CHECK(MemoryTracker::Get(MemoryCategory::GpuBuffers).Allocations == before.Allocations);
	}

	TEST_CASE("Counters accept concurrent charges without lost updates")
	{
		JobSystem pool(2);
		const auto before = MemoryTracker::Get(MemoryCategory::GpuBuffers);
		CHECK(pool.ParallelFor(1000, 1, [](uint32_t, uint32_t) {
			MemoryAllocation charge(MemoryCategory::GpuBuffers, 32);
			charge.Resize(64);
		}));
		CHECK(MemoryTracker::Get(MemoryCategory::GpuBuffers).Bytes == before.Bytes);
		CHECK(MemoryTracker::Get(MemoryCategory::GpuBuffers).Allocations == before.Allocations);
	}

	TEST_CASE("Lua heap accounting includes state creation growth collection and close")
	{
		const auto before = MemoryTracker::Get(MemoryCategory::Lua);
		{
			sol::state lua{ sol::default_at_panic, &MemoryTracker::LuaAllocator };
			lua.open_libraries(sol::lib::base, sol::lib::string);
			const auto initial = MemoryTracker::Get(MemoryCategory::Lua).Bytes;
			CHECK(initial > before.Bytes);
			lua.script("payload = string.rep('x', 65536)");
			const auto grown = MemoryTracker::Get(MemoryCategory::Lua).Bytes;
			CHECK(grown > initial);
			lua.script("payload = nil; collectgarbage('collect')");
			CHECK(MemoryTracker::Get(MemoryCategory::Lua).Bytes < grown);
		}
		CHECK(MemoryTracker::Get(MemoryCategory::Lua).Bytes == before.Bytes);
		CHECK(MemoryTracker::Get(MemoryCategory::Lua).Allocations == before.Allocations);
	}

	TEST_CASE("Asset charge survives cache eviction while a consumer owns the source")
	{
		AssetManager::Clear();
		JobSystem::Get().WaitIdle();
		const auto before = MemoryTracker::Get(MemoryCategory::Assets).Bytes;
		auto mesh = AssetManager::GetMesh("builtin://Cube");
		REQUIRE(mesh);
		const auto loaded = MemoryTracker::Get(MemoryCategory::Assets).Bytes;
		CHECK(loaded > before);
		AssetManager::Clear();
		CHECK(MemoryTracker::Get(MemoryCategory::Assets).Bytes == loaded);
		mesh.reset();
		CHECK(MemoryTracker::Get(MemoryCategory::Assets).Bytes == before);
	}
}
