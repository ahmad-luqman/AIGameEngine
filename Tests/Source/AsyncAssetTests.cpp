#include <doctest/doctest.h>

#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Core/JobSystem.h>
#include <Basalt/Project/Project.h>

#include "TestUtils.h"

using namespace Basalt;

TEST_SUITE("AsyncAssets")
{
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
		CHECK_FALSE(AssetManager::RequestTexture("Assets/bad.png"));
		AssetManager::WaitForLoads();
		CHECK_FALSE(AssetManager::GetTexture("Assets/bad.png"));
		CHECK_FALSE(AssetManager::GetError("Assets/bad.png").empty());
		CHECK_FALSE(AssetManager::IsLoading("Assets/bad.png"));
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
		CHECK(AssetManager::ReloadChanged({ "Assets/unknown.bin" }) == std::vector<std::string>{ "builtin://Cube" });
		JobSystem::Get().WaitIdle();
		AssetManager::PublishCompleted();
		CHECK(AssetManager::GetLoadedKeys().empty());
	}

	TEST_CASE("Project switching discards requests without worker reads of the active project")
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
}
