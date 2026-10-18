#include <doctest/doctest.h>

#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Core/JobSystem.h>
#include <Basalt/Core/MemoryTracker.h>
#include <Basalt/Project/Project.h>

#include "TestUtils.h"

#include <latch>

using namespace Basalt;

TEST_SUITE("AsyncAssets")
{
	TEST_CASE("Blocking getters publish only their own keys and mesh dependencies")
	{
		AssetManager::Clear();
		const std::string texture = std::string(BASALT_TEST_DATA_DIR) + "/FeatureTest/Assets/Textures/Checker.png";
		AssetManager::RequestMesh("builtin://Cube");
		AssetManager::RequestMesh("builtin://Sphere");
		AssetManager::RequestTexture(texture);
		JobSystem::Get().WaitIdle();
		REQUIRE(AssetManager::GetMesh("builtin://Cube"));
		CHECK_FALSE(AssetManager::FindLoadedMesh("builtin://Sphere"));
		CHECK_FALSE(AssetManager::RequestTexture(texture));
		CHECK(AssetManager::IsLoading("builtin://Sphere"));
		CHECK(AssetManager::IsLoading(texture));
		REQUIRE(AssetManager::GetTexture(texture));
		CHECK_FALSE(AssetManager::FindLoadedMesh("builtin://Sphere"));
		AssetManager::PublishCompleted();
		CHECK(AssetManager::FindLoadedMesh("builtin://Sphere"));
		AssetManager::Clear();
	}

	TEST_CASE("Streamed image requests made before their mesh finishes progress through frame fences")
	{
		AssetManager::Clear();
		auto& pool = JobSystem::Get();
		pool.WaitIdle();
		std::latch started(pool.GetWorkerCount());
		std::latch release(1);
		auto unblock = std::shared_ptr<std::latch>(&release, [](std::latch* latch) {
			latch->count_down();
			JobSystem::Get().WaitIdle();
		});
		for (uint32_t i = 0; i < pool.GetWorkerCount(); ++i)
			(void)pool.Submit([&] { started.count_down(); release.wait(); });
		started.wait();
		const std::string mesh = std::string(BASALT_TEST_DATA_DIR) + "/FeatureTest/Assets/Models/TexturedCube.glb";
		const auto image = mesh + "#image/0";
		AssetManager::RequestMesh(mesh);
		AssetManager::RequestTexture(image);
		CHECK(AssetManager::IsLoading(mesh));
		CHECK(AssetManager::IsLoading(image));
		unblock.reset();
		Ref<TextureSource> decoded;
		for (uint32_t frame = 0; frame < 4 && !decoded; ++frame)
		{
			pool.WaitIdle();
			AssetManager::PublishCompleted();
			decoded = AssetManager::RequestTexture(image);
		}
		REQUIRE_MESSAGE(decoded, AssetManager::GetError(image));
		CHECK_FALSE(AssetManager::IsLoading(image));
		AssetManager::Clear();
	}

	TEST_CASE("Requests deduplicate and publish only at the main thread fence")
	{
		AssetManager::Clear();
		CHECK_FALSE(AssetManager::RequestMesh("builtin://Cube"));
		CHECK_FALSE(AssetManager::RequestMesh("builtin://Cube"));
		CHECK(AssetManager::IsLoading("builtin://Cube"));
		CHECK_FALSE(AssetManager::IsLoading("builtin://Cube#image/0"));
		JobSystem::Get().WaitIdle();
		CHECK_FALSE(AssetManager::FindLoadedMesh("builtin://Cube"));
		AssetManager::PublishCompleted();
		auto loaded = AssetManager::FindLoadedMesh("builtin://Cube");
		REQUIRE(loaded);
		CHECK_FALSE(AssetManager::IsLoading("builtin://Cube"));
		CHECK(AssetManager::GetMesh("builtin://Cube") == loaded);
		CHECK(AssetManager::RequestMesh("builtin://Cube") == loaded);
		CHECK(AssetManager::GetLoadedKeys().size() == 1);
		AssetManager::Clear();
	}

	TEST_CASE("Blocking getters join pending work and embedded textures chain to their mesh")
	{
		AssetManager::Clear();
		const std::string mesh = std::string(BASALT_TEST_DATA_DIR) + "/FeatureTest/Assets/Models/TexturedCube.glb";
		const std::string texture = mesh + "#image/0";
		CHECK_FALSE(AssetManager::RequestTexture(texture));
		CHECK(AssetManager::IsLoading(mesh));
		CHECK(AssetManager::IsLoading(texture));
		auto decoded = AssetManager::GetTexture(texture);
		REQUIRE_MESSAGE(decoded, AssetManager::GetError(texture));
		CHECK(decoded->Width > 0);
		CHECK(AssetManager::GetTexture(texture) == decoded);
		AssetManager::WaitForLoads();
		CHECK(AssetManager::FindLoadedMesh(mesh));
		CHECK_FALSE(AssetManager::IsLoading(mesh));
		CHECK_FALSE(AssetManager::IsLoading(texture));
		AssetManager::Clear();
	}

	TEST_CASE("Failed loads are cached and explicitly reloadable")
	{
		AssetManager::Clear();
		const std::string key = "builtin://MissingPrimitive";
		CHECK_FALSE(AssetManager::RequestMesh(key));
		AssetManager::WaitForLoads();
		CHECK_FALSE(AssetManager::GetError(key).empty());
		CHECK_FALSE(AssetManager::RequestMesh(key));
		CHECK_FALSE(AssetManager::IsLoading(key));
		AssetManager::Reload(key);
		CHECK_FALSE(AssetManager::RequestMesh(key));
		CHECK(AssetManager::IsLoading(key));
		AssetManager::WaitForLoads();
		CHECK_FALSE(AssetManager::RequestTexture("missing.png"));
		AssetManager::WaitForLoads();
		CHECK_FALSE(AssetManager::GetError("missing.png").empty());
		CHECK_FALSE(AssetManager::GetTexture("missing.png"));
		CHECK_FALSE(AssetManager::GetTexture("missing.gltf#image/0"));
		CHECK(AssetManager::GetError("missing.gltf#image/0").find("file not found") != std::string::npos);
		CHECK_FALSE(AssetManager::IsLoading("missing.png"));
		CHECK_FALSE(AssetManager::GetMesh(""));
		CHECK_FALSE(AssetManager::RequestTexture(""));
		AssetManager::Clear();
	}

	TEST_CASE("Readable malformed images fail decoding without leaking a charge")
	{
		BasaltTest::TempProject project("AsyncInvalidImage");
		project.WriteFile("Assets/bad.png", "this is not an image");
		AssetManager::Clear();
		JobSystem::Get().WaitIdle();
		const auto before = MemoryTracker::Get(MemoryCategory::Assets).Bytes;
		CHECK_FALSE(AssetManager::RequestTexture("Assets/bad.png"));
		AssetManager::WaitForLoads();
		CHECK_FALSE(AssetManager::GetTexture("Assets/bad.png"));
		CHECK_FALSE(AssetManager::GetError("Assets/bad.png").empty());
		CHECK_FALSE(AssetManager::IsLoading("Assets/bad.png"));
		CHECK(MemoryTracker::Get(MemoryCategory::Assets).Bytes == before);
		AssetManager::Clear();
	}

	TEST_CASE("Evicted and reloaded requests cannot repopulate the cache")
	{
		AssetManager::Clear();
		AssetManager::RequestMesh("builtin://Cube");
		AssetManager::Reload("builtin://Cube");
		AssetManager::RequestMesh("builtin://Sphere");
		AssetManager::Clear();
		JobSystem::Get().WaitIdle();
		AssetManager::PublishCompleted();
		CHECK(AssetManager::GetLoadedKeys().empty());
		AssetManager::RequestMesh("builtin://Cube");
		CHECK(AssetManager::ReloadChanged({ "Assets/unknown.bin" }).empty());
		JobSystem::Get().WaitIdle();
		AssetManager::PublishCompleted();
		CHECK(AssetManager::FindLoadedMesh("builtin://Cube"));
		AssetManager::Clear();
	}

	TEST_CASE("Project switching discards pending entries")
	{
		AssetManager::Clear();
		// Own the old project: SetActive replaces the reference returned by GetActive.
		auto previous = Project::GetActive(); // NOLINT(performance-unnecessary-copy-initialization)
		Project::SetActive(nullptr);
		AssetManager::RequestMesh("builtin://Cube");
		Project::SetActive(CreateRef<Project>());
		AssetManager::PublishCompleted();
		JobSystem::Get().WaitIdle();
		CHECK(AssetManager::GetLoadedKeys().empty());
		Project::SetActive(previous);
		AssetManager::Clear();
	}

	TEST_CASE("Temporary preview scope discards its pending requests")
	{
		AssetManager::Clear();
		auto retained = AssetManager::GetMesh("builtin://Sphere");
		{
			AssetManager::TemporaryLoads scope;
			AssetManager::RequestMesh("builtin://Cube");
		}
		AssetManager::WaitForLoads();
		CHECK(AssetManager::GetLoadedKeys() == std::vector<std::string>{ "builtin://Sphere" });
		CHECK(AssetManager::FindLoadedMesh("builtin://Sphere") == retained);
		AssetManager::Clear();
	}
	TEST_CASE("Publishing a parent leaves its completed result intact for queued image jobs")
	{
		AssetManager::Clear();
		const std::string mesh = std::string(BASALT_TEST_DATA_DIR) + "/FeatureTest/Assets/Models/TexturedCube.glb";
		AssetManager::RequestMesh(mesh);
		auto& pool = JobSystem::Get();
		pool.WaitIdle();
		std::latch started(pool.GetWorkerCount());
		std::latch release(1);
		auto unblock = std::shared_ptr<std::latch>(&release, [](std::latch* latch) {
			latch->count_down();
			JobSystem::Get().WaitIdle();
		});
		for (uint32_t i = 0; i < pool.GetWorkerCount(); ++i)
			(void)pool.Submit([&] { started.count_down(); release.wait(); });
		started.wait();
		AssetManager::RequestTexture(mesh + "#image/0");
		AssetManager::PublishCompleted();
		REQUIRE(AssetManager::FindLoadedMesh(mesh));
		unblock.reset();
		AssetManager::WaitForLoads();
		CHECK(AssetManager::GetTexture(mesh + "#image/0"));
		CHECK(AssetManager::GetError(mesh + "#image/0").empty());
		AssetManager::Clear();
	}

	TEST_CASE("Unrelated saves preserve a pending file mesh and its memory charge")
	{
		AssetManager::Clear();
		const std::string mesh = std::string(BASALT_TEST_DATA_DIR) + "/FeatureTest/Assets/Models/TexturedCube.glb";
		AssetManager::RequestMesh(mesh);
		JobSystem::Get().WaitIdle();
		const auto bytes = MemoryTracker::Get(MemoryCategory::Assets).Bytes;
		CHECK(AssetManager::ReloadChanged({ "Assets/Scripts/unrelated.lua" }).empty());
		AssetManager::PublishCompleted();
		CHECK(AssetManager::FindLoadedMesh(mesh));
		CHECK_FALSE(AssetManager::IsLoading(mesh));
		CHECK(MemoryTracker::Get(MemoryCategory::Assets).Bytes == bytes);
		AssetManager::Clear();
	}

	TEST_CASE("Completed project assets are replaced on project switch while builtins survive")
	{
		AssetManager::Clear();
		BasaltTest::TempProject first("AsyncProjectA");
		std::filesystem::copy_file(std::filesystem::path(BASALT_TEST_DATA_DIR) / "FeatureTest/Assets/Models/TexturedCube.glb", first.GetDirectory() / "Assets/model.glb");
		auto builtin = AssetManager::GetMesh("builtin://Cube");
		REQUIRE(AssetManager::GetMesh("Assets/model.glb"));
		REQUIRE(AssetManager::GetTexture("Assets/model.glb#image/0"));
		BasaltTest::TempProject second("AsyncProjectB");
		second.WriteFile("Assets/model.glb", "invalid glb");
		CHECK(AssetManager::GetMesh("builtin://Cube") == builtin);
		CHECK_FALSE(AssetManager::GetMesh("Assets/model.glb"));
		CHECK_FALSE(AssetManager::GetTexture("Assets/model.glb#image/0"));
		CHECK_FALSE(AssetManager::GetError("Assets/model.glb#image/0").empty());
		AssetManager::Clear();
	}
}
