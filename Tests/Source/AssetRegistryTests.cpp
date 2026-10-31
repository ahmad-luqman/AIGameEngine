#include <doctest/doctest.h>

#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/AssetRegistry.h>
#include <Basalt/Asset/MeshImporter.h>
#include <Basalt/Asset/MeshOptimizer.h>
#include <Basalt/Asset/Thumbnails.h>
#include <Basalt/Core/JsonUtils.h>
#include <Basalt/Project/ProjectValidator.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/SceneSerializer.h>

#include "AutomationTestUtils.h"
#include "TestUtils.h"
#include "MeshTestUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

using namespace Basalt;
using json = nlohmann::json;

TEST_SUITE("AssetPipeline")
{
	TEST_CASE("Metadata parsing rejects malformed and lossy identities")
	{
		std::string error;
		for (const auto& value : { json(), json::array(), json::object(), json{ { "Version", 2 }, { "UUID", "1" } }, json{ { "Version", 1 }, { "UUID", 1 } }, json{ { "Version", 1 }, { "UUID", "0" } }, json{ { "Version", 1 }, { "UUID", "01" } }, json{ { "Version", 1 }, { "UUID", "-1" } }, json{ { "Version", 1 }, { "UUID", "18446744073709551616" } }, json{ { "Version", 1 }, { "UUID", "1x" } }, json{ { "Version", 1 }, { "UUID", "1" }, { "Unknown", 1 } } })
		{
			CHECK_FALSE(AssetRegistry::ParseMeta(value, error));
			CHECK_FALSE(error.empty());
		}
		REQUIRE(AssetRegistry::ParseMeta({ { "Version", 1 }, { "UUID", "18446744073709551615" } }, error));
	}

	TEST_CASE("UUID resolution survives restart and paired external moves and never substitutes a hint")
	{
		BasaltTest::TempProject project("AssetIdentity");
		auto& assets = Project::GetActive()->GetAssets();
		std::string error;
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/a.lua"), "return {}"));
		const auto id = assets.Ensure("Assets/a.lua", error);
		REQUIRE_MESSAGE(id, error);
		CHECK(assets.Ensure("Assets/a.lua", error) == id);
		json reference;
		REQUIRE(assets.Encode("Assets/a.lua", reference, error));
		REQUIRE(reference.is_object());
		std::filesystem::create_directories(Project::ResolvePath("Assets/nested"));
		std::filesystem::rename(Project::ResolvePath("Assets/a.lua"), Project::ResolvePath("Assets/nested/b.lua"));
		std::filesystem::rename(Project::ResolvePath("Assets/a.lua.meta"), Project::ResolvePath("Assets/nested/b.lua.meta"));
		AssetRegistry restarted(Project::GetActive()->GetDirectory());
		REQUIRE(restarted.Refresh(error));
		std::string path;
		REQUIRE(restarted.Decode(reference, path, error));
		CHECK(path == "Assets/nested/b.lua");
		CHECK(restarted.Find(*id) == path);
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/a.lua"), "replacement"));
		REQUIRE(restarted.Refresh(error));
		REQUIRE(restarted.Decode(reference, path, error));
		CHECK(path == "Assets/nested/b.lua");
		std::filesystem::remove(Project::ResolvePath("Assets/nested/b.lua"));
		REQUIRE(restarted.Refresh(error));
		CHECK_FALSE(restarted.Decode(reference, path, error));
		CHECK(error.find("unresolved") != std::string::npos);
	}

	TEST_CASE("Registry rejects duplicates, corrupt sidecars, escapes and occupied move destinations")
	{
		BasaltTest::TempProject project("AssetConflicts");
		auto& assets = Project::GetActive()->GetAssets();
		std::string error;
		for (const auto* name : { "Assets/a.lua", "Assets/b.lua" })
			REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(name), "return {}"));
		const auto id = assets.Ensure("Assets/a.lua", error);
		REQUIRE(id);
		CHECK_FALSE(assets.Move("Assets/a.lua", "Assets/b.lua", error));
		CHECK(FileSystem::Exists(Project::ResolvePath("Assets/a.lua")));
		for (const auto* path : { "../escape", "Assets/../../escape", "Project.bproject", "Assets/a.lua.meta", "Assets" })
			CHECK_FALSE(assets.Ensure(path, error));
		const auto meta = FileSystem::ReadTextFile(Project::ResolvePath("Assets/a.lua.meta"));
		REQUIRE(meta);
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/b.lua.meta"), *meta));
		REQUIRE(assets.Refresh(error));
		CHECK(assets.GetProblems().size() == 2);
		CHECK(assets.GetProblems().at("Assets/a.lua").find("duplicate") != std::string::npos);
		CHECK(assets.Find(*id).empty()); // Neither copy may win a duplicate identity.
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/b.lua.meta"), "invalid"));
		CHECK_FALSE(assets.Ensure("Assets/b.lua", error));
		CHECK(assets.Ensure("Assets/a.lua", error) == id);
		CHECK(FileSystem::ReadTextFile(Project::ResolvePath("Assets/b.lua.meta")) == "invalid");
	}

	TEST_CASE("Symlink aliases cannot move or delete their target assets")
	{
		BasaltTest::TempProject project("AssetSymlinks");
		auto& assets = Project::GetActive()->GetAssets();
		std::string error;
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/real.lua"), "return {}"));
		const auto id = assets.Ensure("Assets/real.lua", error);
		REQUIRE(id);
		const auto meta = FileSystem::ReadTextFile(Project::ResolvePath("Assets/real.lua.meta"));
		REQUIRE(meta);
		std::error_code linkError;
		std::filesystem::create_symlink("real.lua", Project::ResolvePath("Assets/alias.lua"), linkError);
		if (linkError)
		{
			WARN_MESSAGE(false, "Symlink creation unavailable: ", linkError.message());
			return;
		}
		CHECK_FALSE(assets.Remove("Assets/alias.lua", error));
		CHECK(error.find("symlink") != std::string::npos);
		CHECK_FALSE(assets.Move("Assets/alias.lua", "Assets/moved.lua", error));
		CHECK_FALSE(assets.Ensure("Assets/alias.lua", error));
		CHECK(FileSystem::ReadTextFile(Project::ResolvePath("Assets/real.lua")) == "return {}");
		CHECK(FileSystem::ReadTextFile(Project::ResolvePath("Assets/real.lua.meta")) == meta);
		CHECK(std::filesystem::is_symlink(Project::ResolvePath("Assets/alias.lua")));
		CHECK_FALSE(FileSystem::Exists(Project::ResolvePath("Assets/moved.lua")));
		std::filesystem::create_directory_symlink(".", Project::ResolvePath("Assets/linked"), linkError);
		REQUIRE_FALSE(linkError);
		CHECK_FALSE(assets.Remove("Assets/linked/real.lua", error));
		CHECK_FALSE(assets.Move("Assets/linked/real.lua", "Assets/moved.lua", error));
		CHECK_FALSE(assets.Move("Assets/real.lua", "Assets/linked/moved.lua", error));
		CHECK(FileSystem::ReadTextFile(Project::ResolvePath("Assets/real.lua.meta")) == meta);
		CHECK(assets.Find(*id) == "Assets/real.lua");
	}

	TEST_CASE("Scene and prefab persistence use identity while snapshots stay path based")
	{
		BasaltTest::TempProject project("AssetPersistence");
		std::string error;
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/tex.png"), "texture"));
		Scene scene;
		auto entity = scene.CreateEntity("Mesh");
		entity.AddComponent<MeshComponent>().Mesh = "Assets/model.glb"; // Missing references stay editable legacy paths.
		entity.AddComponent<MaterialComponent>().AlbedoMap = "Assets/tex.png";
		const auto hash = SceneSerializer::ComputeStateHash(scene);
		const auto path = Project::ResolvePath("Assets/Scenes/Identity.bscene");
		REQUIRE_MESSAGE(SceneSerializer::SaveScene(scene, path, error), error);
		CHECK(SceneSerializer::ComputeStateHash(scene) == hash);
		CHECK(SceneSerializer::SerializeEntity(entity)["Components"]["Material"]["AlbedoMap"] == "Assets/tex.png");
		const auto text = FileSystem::ReadTextFile(path);
		REQUIRE(text);
		const auto data = ParseJson(*text);
		CHECK(data["Entities"][0]["Components"]["Material"]["AlbedoMap"].is_object());
		CHECK(data["Entities"][0]["Components"]["Mesh"]["Mesh"] == "Assets/model.glb");
		REQUIRE(SceneSerializer::SavePrefab(entity, Project::ResolvePath("Assets/Prefabs/Identity.bprefab"), error));
		REQUIRE(Project::GetActive()->GetAssets().Move("Assets/tex.png", "Assets/new.png", error));
		auto loaded = SceneSerializer::LoadScene(path, error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetAllEntitiesOrdered()[0].GetComponent<MaterialComponent>().AlbedoMap == "Assets/new.png");
		auto instance = SceneSerializer::InstantiatePrefab(*loaded, Project::ResolvePath("Assets/Prefabs/Identity.bprefab"), "Assets/Prefabs/Identity.bprefab", {}, error);
		REQUIRE_MESSAGE(instance, error);
		CHECK(instance.GetComponent<MaterialComponent>().AlbedoMap == "Assets/new.png");
		json embedded;
		REQUIRE(Project::GetActive()->GetAssets().Encode("Assets/new.png#image/7", embedded, error));
		std::string decoded;
		REQUIRE(Project::GetActive()->GetAssets().Decode(embedded, decoded, error));
		CHECK(decoded == "Assets/new.png#image/7");
	}

	TEST_CASE("Move commands migrate legacy disk references and preserve IDs and open scene edits")
	{
		BasaltTest::TempProject project("AssetCommands");
		CommandRegistry registry;
		AutomationSession session;
		using BasaltTest::Run;
		const auto from = "Assets/file.lua";
		Run(registry, session, "asset.write", { { "path", from }, { "content", "return {}" } });
		Run(registry, session, "scene.new", { { "name", "Identity" }, { "empty", true } });
		Run(registry, session, "entity.create", { { "name", "Driver" } });
		Run(registry, session, "component.set", { { "entity", "Driver" }, { "component", "Script" }, { "data", { { "Script", from } } } });
		std::string error;
		const auto legacy = SceneSerializer::SerializeScene(*session.GetScene());
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Scenes/Legacy.bscene"), legacy.dump()));
		const auto id = Run(registry, session, "asset.id", { { "path", from } })["uuid"];
		Run(registry, session, "asset.move", { { "from", from }, { "to", "Assets/renamed.lua" } });
		CHECK(session.IsDirty());
		CHECK(Run(registry, session, "asset.resolve", { { "uuid", id } })["path"] == "Assets/renamed.lua");
		CHECK(session.GetScene()->GetAllEntitiesOrdered()[0].GetComponent<ScriptComponent>().Script == "Assets/renamed.lua");
		CHECK(Run(registry, session, "asset.id", { { "path", "Assets/renamed.lua" } })["uuid"] == id);
		auto loaded = SceneSerializer::LoadScene(Project::ResolvePath("Assets/Scenes/Legacy.bscene"), error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetAllEntitiesOrdered()[0].GetComponent<ScriptComponent>().Script == "Assets/renamed.lua");
		Run(registry, session, "asset.rename", { { "from", "Assets/renamed.lua" }, { "to", from } });
		CHECK(Run(registry, session, "asset.id", { { "path", from } })["uuid"] == id);
		const auto files = AssetManager::ListAssets();
		CHECK(std::none_of(files.begin(), files.end(), [](const auto& path) { return path.ends_with(".meta"); }));
	}

	TEST_CASE("Mesh optimization preserves complete vertex attributes and blended triangle order")
	{
		auto source = Primitives::CreateSphere(16, 8);
		auto vertices = source->Vertices;
		auto indices = source->Indices;
		vertices.emplace_back(); // Unused vertices are discarded.
		std::vector<Vertex> ordered;
		ordered.reserve(indices.size());
		for (const auto index : indices)
			ordered.push_back(vertices[index]);
		REQUIRE(MeshOptimizer::Optimize(vertices, indices, false));
		CHECK(vertices.size() == source->Vertices.size());
		for (size_t i = 0; i < indices.size(); ++i)
		{
			const auto& actual = vertices[indices[i]];
			const auto& expected = ordered[i];
			CHECK(actual.Position == expected.Position);
			CHECK(actual.Normal == expected.Normal);
			CHECK(actual.Tangent == expected.Tangent);
			CHECK(actual.TexCoord == expected.TexCoord);
			CHECK(actual.Joints == expected.Joints);
			CHECK(actual.Weights == expected.Weights);
		}
		indices[0] = UINT32_MAX;
		CHECK_FALSE(MeshOptimizer::Optimize(vertices, indices, true));
	}

	TEST_CASE("Static LODs reduce triangles with valid shared vertices and bounded selection")
	{
		auto source = Primitives::CreateSphere(32, 16);
		const auto full = source->Submeshes[0].IndexCount;
		MeshOptimizer::GenerateLODs(*source, 0);
		const auto& submesh = source->Submeshes[0];
		REQUIRE_FALSE(submesh.LODs.empty());
		uint32_t previous = full;
		for (const auto& lod : submesh.LODs)
		{
			CHECK(lod.IndexCount > 0);
			CHECK(lod.IndexCount < previous);
			CHECK(lod.IndexCount % 3 == 0);
			CHECK(lod.Error <= .01f);
			for (uint32_t i = 0; i < lod.IndexCount; ++i)
				CHECK(source->Indices[lod.BaseIndex + i] < submesh.VertexCount);
			previous = lod.IndexCount;
		}
		CHECK(submesh.IndexCount == full);
		CHECK(MeshOptimizer::SelectLOD(submesh, 100, 0) == 0);
		CHECK(MeshOptimizer::SelectLOD(submesh, 9.9f, 10) == 0);
		CHECK(MeshOptimizer::SelectLOD(submesh, 10, 10) == 1);
		CHECK(MeshOptimizer::SelectLOD(submesh, 1e6f, 10) == submesh.LODs.size());
		CHECK(MeshOptimizer::SelectLOD(submesh, std::numeric_limits<float>::infinity(), 10) == 0);
		auto skinned = Primitives::CreateSphere();
		skinned->Submeshes[0].JointBounds.push_back({});
		MeshOptimizer::GenerateLODs(*skinned, 0);
		CHECK(skinned->Submeshes[0].LODs.empty());
	}

	// Regression (FuzzGltf, nightly): meshoptimizer's vertex cache order rescans a vertex's remaining triangles whenever
	// the vertex is touched, so 46116 indices naming one vertex took 22 s with ASan.
	TEST_CASE("Mesh optimization and LODs skip geometry whose vertex valences cost too much")
	{
		// The most indices naming a single vertex that the limit allows, in whole triangles.
		auto allowed = [](size_t count) { return count * count <= MeshOptimizer::MaxValenceCostPerIndex * count + MeshOptimizer::ValenceCostAllowance; };
		size_t limit = 3;
		while (allowed(limit + 3))
			limit += 3;
		std::vector<Vertex> vertices(3);
		std::vector<uint32_t> indices(limit, 0);
		CHECK(MeshOptimizer::Optimize(vertices, indices, true));
		vertices.assign(3, Vertex{});
		indices.assign(limit + 3, 0);
		CHECK_FALSE(MeshOptimizer::Optimize(vertices, indices, true));
		CHECK(vertices.size() == 3);
		CHECK(indices == std::vector<uint32_t>(limit + 3, 0));

		// A closed double cone: each apex is in every triangle on its side, each rim vertex in four.
		auto bicone = [](uint32_t segments) {
			std::vector<Vertex> coneVertices(segments + 2);
			for (uint32_t i = 0; i < segments; i++)
			{
				const float angle = 6.2831853f * static_cast<float>(i) / static_cast<float>(segments);
				coneVertices[i].Position = { std::cos(angle), 0, std::sin(angle) };
			}
			coneVertices[segments].Position = { 0, 1, 0 };
			coneVertices[segments + 1].Position = { 0, -1, 0 };
			std::vector<uint32_t> coneIndices;
			for (uint32_t i = 0; i < segments; i++)
			{
				const uint32_t next = (i + 1) % segments;
				coneIndices.insert(coneIndices.end(), { segments, next, i, segments + 1, i, next });
			}
			auto source = CreateRef<MeshSource>();
			source->AddSubmesh(coneVertices, coneIndices, 0);
			return source;
		};
		auto small = bicone(64);
		MeshOptimizer::GenerateLODs(*small, 0);
		CHECK_FALSE(small->Submeshes[0].LODs.empty());
		auto large = bicone(4096);
		MeshOptimizer::GenerateLODs(*large, 0);
		CHECK(large->Submeshes[0].LODs.empty());
		CHECK_FALSE(MeshOptimizer::Optimize(large->Vertices, large->Indices, true));
	}
	TEST_CASE("Unrelated corrupt metadata does not lock out authoring or prefab spawning")
	{
		BasaltTest::TempProject project("AssetDiagnostics");
		std::string error;
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/bad.png"), "x"));
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/bad.png.meta"), "foreign metadata"));
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/orphan.png.meta"), "ignored orphan"));
		auto loaded = Project::Load(Project::GetActive()->GetDirectory(), error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetAssets().GetProblems().size() == 1);
		Scene scene;
		auto entity = scene.CreateEntity("Unaffected");
		REQUIRE(SceneSerializer::SaveScene(scene, Project::ResolvePath("Assets/Scenes/Good.bscene"), error));
		REQUIRE(SceneSerializer::SavePrefab(entity, Project::ResolvePath("Assets/Prefabs/Good.bprefab"), error));
		CHECK(SceneSerializer::InstantiatePrefab(scene, Project::ResolvePath("Assets/Prefabs/Good.bprefab"), "Assets/Prefabs/Good.bprefab", {}, error));
		CHECK_FALSE(Project::GetActive()->GetAssets().Ensure("Assets/bad.png", error));
		const auto report = ValidateProject(*Project::GetActive());
		CHECK(report.GetErrorCount() > 0);
	}

	TEST_CASE("Legacy references outside Assets and literal hash filenames remain saveable")
	{
		BasaltTest::TempProject project("AssetLegacyPaths");
		Scene scene;
		auto entity = scene.CreateEntity("Legacy");
		auto& material = entity.AddComponent<MaterialComponent>();
		material.AlbedoMap = "Shared/outside.png";
		material.NormalMap = "Assets/tex#1.png";
		material.EmissiveMap = FileSystem::PathToUtf8(Project::ResolvePath("Shared/absolute.png"));
		for (const auto& path : { material.AlbedoMap, material.NormalMap, material.EmissiveMap })
			REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(path), "x"));
		std::string error;
		const auto path = Project::ResolvePath("Assets/Scenes/Legacy.bscene");
		REQUIRE_MESSAGE(SceneSerializer::SaveScene(scene, path, error), error);
		const auto loaded = SceneSerializer::LoadScene(path, error);
		REQUIRE_MESSAGE(loaded, error);
		const auto entities = loaded->GetAllEntitiesOrdered();
		const auto& actual = entities[0].GetComponent<MaterialComponent>();
		CHECK(actual.AlbedoMap == material.AlbedoMap);
		CHECK(actual.NormalMap == material.NormalMap);
		CHECK(actual.EmissiveMap == material.EmissiveMap);
	}

	TEST_CASE("Start scene IDs survive paired moves and remain repairable when unresolved")
	{
		BasaltTest::TempProject project("StartSceneIdentity");
		CommandRegistry registry;
		AutomationSession session;
		using BasaltTest::Run;
		Run(registry, session, "scene.new", { { "name", "Start" }, { "empty", true } });
		Run(registry, session, "scene.save", { { "path", "Assets/Scenes/Start.bscene" } });
		Run(registry, session, "project.set", { { "startScene", "Assets/Scenes/Start.bscene" } });
		CHECK(Run(registry, session, "asset.refresh")["changed"].empty());
		Run(registry, session, "asset.move", { { "from", "Assets/Scenes/Start.bscene" }, { "to", "Assets/Scenes/New.bscene" } });
		CHECK(session.GetScenePath() == "Assets/Scenes/New.bscene");
		CHECK(session.GetEditScene()->GetPath() == session.GetScenePath());
		CHECK(Project::GetActive()->GetConfig().StartScene == session.GetScenePath());
		std::string error;
		auto loaded = Project::Load(Project::GetActive()->GetDirectory(), error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetConfig().StartScene == session.GetScenePath());
		const auto encoded = ParseJson(*FileSystem::ReadTextFile(loaded->GetProjectFilePath()))["StartScene"];
		CHECK(encoded.is_object());
		std::filesystem::remove(Project::ResolvePath("Assets/Scenes/New.bscene"));
		loaded = Project::Load(Project::GetActive()->GetDirectory(), error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetConfig().StartScene.empty());
		CHECK_FALSE(loaded->GetStartSceneError().empty());
		REQUIRE(loaded->Save(error));
		CHECK(ParseJson(*FileSystem::ReadTextFile(loaded->GetProjectFilePath()))["StartScene"] == encoded);
		Project::SetActive(loaded);
		Run(registry, session, "project.set", { { "startScene", "" } });
		CHECK(loaded->GetStartSceneError().empty());
		CHECK(ParseJson(*FileSystem::ReadTextFile(loaded->GetProjectFilePath()))["StartScene"] == "");
	}

	TEST_CASE("Failed moves preserve documents and embedded image moves retarget disk and open references")
	{
		BasaltTest::TempProject project("MoveFailures");
		CommandRegistry registry;
		AutomationSession session;
		using BasaltTest::Run;
		using BasaltTest::RunError;
		Run(registry, session, "scene.new", { { "empty", true } });
		Run(registry, session, "entity.create", { { "name", "Model" }, { "components", { { "Material", { { "AlbedoMap", "./Assets/model.glb#image/0" } } } } } });
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/model.glb"), "binary"));
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/blocker.lua"), "blocked"));
		const auto path = Project::ResolvePath("Assets/Scenes/Legacy.bscene");
		const auto legacy = SceneSerializer::SerializeScene(*session.GetScene()).dump();
		REQUIRE(FileSystem::WriteTextFile(path, legacy));
		CHECK_FALSE(RunError(registry, session, "asset.move", { { "from", "Assets/model.glb" }, { "to", "Assets/blocker.lua/child.glb" } }).empty());
		CHECK(FileSystem::ReadTextFile(path) == legacy);
		CHECK(FileSystem::Exists(Project::ResolvePath("Assets/model.glb")));
		CHECK_FALSE(RunError(registry, session, "asset.move", { { "from", "Assets/model.glb" }, { "to", "Assets/blocker.lua" } }).empty());
		CHECK(FileSystem::ReadTextFile(path) == legacy);
		Run(registry, session, "asset.move", { { "from", "Assets/model.glb" }, { "to", "Assets/new.glb" } });
		CHECK(session.GetScene()->GetAllEntitiesOrdered()[0].GetComponent<MaterialComponent>().AlbedoMap == "Assets/new.glb#image/0");
		std::string error;
		auto loaded = SceneSerializer::LoadScene(path, error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetAllEntitiesOrdered()[0].GetComponent<MaterialComponent>().AlbedoMap == "Assets/new.glb#image/0");
	}

	TEST_CASE("Deletion removes identity so a replacement file cannot inherit it")
	{
		BasaltTest::TempProject project("AssetDeletion");
		CommandRegistry registry;
		AutomationSession session;
		using BasaltTest::Run;
		Run(registry, session, "asset.write", { { "path", "Assets/deleted.lua" }, { "content", "return {}" } });
		const auto old = Run(registry, session, "asset.id", { { "path", "Assets/deleted.lua" } })["uuid"];
		Run(registry, session, "asset.delete", { { "path", "Assets/deleted.lua" } });
		CHECK_FALSE(FileSystem::Exists(Project::ResolvePath("Assets/deleted.lua.meta")));
		CHECK_FALSE(BasaltTest::RunError(registry, session, "asset.resolve", { { "uuid", old } }).empty());
		Run(registry, session, "asset.write", { { "path", "Assets/deleted.lua" }, { "content", "replacement" } });
		CHECK(Run(registry, session, "asset.id", { { "path", "Assets/deleted.lua" } })["uuid"] != old);
		CHECK(Run(registry, session, "asset.refresh")["changed"].empty());
	}

	TEST_CASE("UUID prefab thumbnail dependencies follow moved assets")
	{
		BasaltTest::TempProject project("UUIDThumbnails");
		Scene scene;
		auto entity = scene.CreateEntity("Preview");
		entity.AddComponent<MaterialComponent>().AlbedoMap = "Assets/image.png";
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/image.png"), "image"));
		std::string error;
		REQUIRE(SceneSerializer::SavePrefab(entity, Project::ResolvePath("Assets/Prefabs/Preview.bprefab"), error));
		ThumbnailCache cache(Project::GetActive()->GetDirectory());
		CHECK(cache.GetDependencies("Assets/Prefabs/Preview.bprefab") == std::vector<std::string>{ "Assets/image.png", "Assets/image.png.meta" });
		REQUIRE(Project::GetActive()->GetAssets().Move("Assets/image.png", "Assets/moved.png", error));
		CHECK(cache.GetDependencies("Assets/Prefabs/Preview.bprefab") == std::vector<std::string>{ "Assets/moved.png", "Assets/moved.png.meta" });
	}

	TEST_CASE("Imported opaque meshes generate LODs while blended and skinned meshes retain full detail")
	{
		BasaltTest::TempDirectory directory("ImportedLODs");
		std::string error;
		const auto opaquePath = BasaltTest::WriteSphereGltf(directory.GetPath());
		auto opaque = MeshImporter::LoadGltf(opaquePath, "Sphere.gltf", error);
		REQUIRE_MESSAGE(opaque, error);
		REQUIRE_FALSE(opaque->Submeshes[0].LODs.empty());
		const auto blendedPath = BasaltTest::WriteSphereGltf(directory.GetPath(), true);
		auto blended = MeshImporter::LoadGltf(blendedPath, "Blend.gltf", error);
		REQUIRE_MESSAGE(blended, error);
		CHECK(blended->Submeshes[0].LODs.empty());
		const auto original = Primitives::CreateSphere(32, 16);
		for (size_t i = 0; i < original->Indices.size(); ++i)
			CHECK(blended->Vertices[blended->Indices[i]].Position == original->Vertices[original->Indices[i]].Position);
		// Transmission draws in the blended pass too, so it also keeps its triangle order and full detail.
		const auto transmissivePath = BasaltTest::WriteSphereGltf(directory.GetPath() / "Transmissive");
		auto json = nlohmann::json::parse(*FileSystem::ReadTextFile(transmissivePath));
		json["materials"][0]["extensions"]["KHR_materials_transmission"] = { { "transmissionFactor", 0.5 } };
		REQUIRE(FileSystem::WriteTextFile(transmissivePath, json.dump()));
		auto transmissive = MeshImporter::LoadGltf(transmissivePath, "Transmissive.gltf", error);
		REQUIRE_MESSAGE(transmissive, error);
		REQUIRE(transmissive->Materials[0].IsBlended());
		CHECK(transmissive->Submeshes[0].LODs.empty());
		for (size_t i = 0; i < original->Indices.size(); ++i)
			CHECK(transmissive->Vertices[transmissive->Indices[i]].Position == original->Vertices[original->Indices[i]].Position);
		auto skinned = MeshImporter::LoadGltf(std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/TwoJoint.gltf", "TwoJoint.gltf", error);
		REQUIRE_MESSAGE(skinned, error);
		for (const auto& submesh : skinned->Submeshes)
			CHECK(submesh.LODs.empty());
	}

	TEST_CASE("Opaque optimization preserves oriented triangles and all vertex attributes")
	{
		auto source = Primitives::CreateSphere(16, 8);
		for (size_t i = 0; i < source->Vertices.size(); ++i)
		{
			source->Vertices[i].Joints = glm::uvec4(static_cast<uint32_t>(i));
			source->Vertices[i].Weights = glm::vec4(static_cast<float>(i) / 1000);
		}
		auto triangles = [](const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
			std::vector<std::string> result;
			for (size_t i = 0; i < indices.size(); i += 3)
			{
				std::string triangle;
				for (size_t j = 0; j < 3; ++j)
					triangle.append(reinterpret_cast<const char*>(&vertices[indices[i + j]]), sizeof(Vertex));
				result.push_back(std::move(triangle));
			}
			std::sort(result.begin(), result.end());
			return result;
		};
		const auto before = triangles(source->Vertices, source->Indices);
		REQUIRE(MeshOptimizer::Optimize(source->Vertices, source->Indices, true));
		CHECK(triangles(source->Vertices, source->Indices) == before);
	}
	TEST_CASE("Metadata import locks prevent replacing concurrent identities and failed scans keep the index")
	{
		BasaltTest::TempProject project("AssetImportLock");
		const auto root = Project::GetActive()->GetDirectory();
		REQUIRE(FileSystem::WriteTextFile(root / "Assets/a.lua", "x"));
		std::filesystem::create_directory(root / "Assets/a.lua.meta.importing");
		auto& assets = Project::GetActive()->GetAssets();
		std::string error;
		CHECK_FALSE(assets.Ensure("Assets/a.lua", error));
		CHECK_FALSE(FileSystem::Exists(root / "Assets/a.lua.meta"));
		std::filesystem::remove(root / "Assets/a.lua.meta.importing");
		const auto id = assets.Ensure("Assets/a.lua", error);
		REQUIRE_MESSAGE(id, error);
		std::filesystem::rename(root / "Assets", root / "StoredAssets");
		REQUIRE(FileSystem::WriteTextFile(root / "Assets", "not a directory"));
		CHECK_FALSE(assets.Refresh(error));
		CHECK(assets.Find(*id) == "Assets/a.lua");
	}

	TEST_CASE("Component readers share UUID decoding and LOD validation across APIs")
	{
		BasaltTest::TempProject project("AssetReaders");
		std::string error;
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/source.png"), "texture"));
		json reference;
		REQUIRE(Project::GetActive()->GetAssets().Encode("Assets/source.png", reference, error));
		Scene scene;
		auto entity = scene.CreateEntity("Reader");
		REQUIRE(ComponentRegistry::AddOrPatch(entity, "Material", { { "AlbedoMap", reference } }, error));
		CHECK(entity.GetComponent<MaterialComponent>().AlbedoMap == "Assets/source.png");
		for (const auto distance : { -1.0, 2000000.0, std::numeric_limits<double>::infinity() })
			CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Mesh", { { "LODDistance", distance } }, error));
		Project::SetActive(nullptr);
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Material", { { "AlbedoMap", reference } }, error));
		CHECK(error.find("active project") != std::string::npos);
	}
	TEST_CASE("Asset migration preserves version-one timestep meaning and normalized start scene paths")
	{
		BasaltTest::TempProject project("LegacyVersionMigration");
		CommandRegistry registry;
		AutomationSession session;
		using BasaltTest::Run;
		Run(registry, session, "scene.new", { { "empty", true } });
		Run(registry, session, "entity.create", { { "name", "Reference" }, { "components", { { "Material", { { "AlbedoMap", "Assets/old.png" } } } } } });
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/old.png"), "texture"));
		auto legacy = SceneSerializer::SerializeScene(*session.GetScene());
		legacy["Version"] = 1;
		legacy["Physics"]["FixedTimestep"] = DefaultFixedTimestep;
		const auto scenePath = Project::ResolvePath("Assets/Scenes/Legacy.bscene");
		REQUIRE(FileSystem::WriteTextFile(scenePath, legacy.dump()));
		Project::GetActive()->GetConfig().FixedTimestep = 1.0f / 120;
		Run(registry, session, "asset.move", { { "from", "Assets/old.png" }, { "to", "Assets/new.png" } });
		std::string error;
		auto loaded = SceneSerializer::LoadScene(scenePath, error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK_FALSE(loaded->GetPhysicsSettings().HasFixedTimestepOverride());
		CHECK(loaded->GetFixedTimestep() == doctest::Approx(1.0f / 120));
		Run(registry, session, "scene.open", { { "path", "Assets/Scenes/Legacy.bscene" } });
		Run(registry, session, "project.set", { { "startScene", "./Assets/Scenes/Legacy.bscene" } });
		const auto before = FileSystem::ReadTextFile(Project::GetActive()->GetProjectFilePath());
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/blocker"), "file"));
		CHECK_FALSE(BasaltTest::RunError(registry, session, "asset.move", { { "from", "Assets/Scenes/Legacy.bscene" }, { "to", "Assets/blocker/failed.bscene" } }).empty());
		CHECK(FileSystem::ReadTextFile(Project::GetActive()->GetProjectFilePath()) == before);
		Run(registry, session, "asset.move", { { "from", "Assets/Scenes/Legacy.bscene" }, { "to", "Assets/Scenes/Current.bscene" } });
		CHECK(Project::GetActive()->GetConfig().StartScene == "Assets/Scenes/Current.bscene");
		CHECK(session.GetScenePath() == "Assets/Scenes/Current.bscene");
		Run(registry, session, "project.set", { { "name", "StillValid" } });
		CHECK(Project::Load(Project::GetActive()->GetDirectory(), error)->GetConfig().StartScene == "Assets/Scenes/Current.bscene");
	}

	TEST_CASE("Deleted and ambiguous component identities stay editable without selecting their hints")
	{
		BasaltTest::TempProject project("UnresolvedComponents");
		const auto source = Project::ResolvePath("Assets/model.glb");
		const auto scenePath = Project::ResolvePath("Assets/Scenes/Unresolved.bscene");
		REQUIRE(FileSystem::WriteTextFile(source, "model"));
		Scene scene;
		auto entity = scene.CreateEntity("Texture");
		entity.AddComponent<MaterialComponent>().AlbedoMap = "Assets/model.glb#image/0";
		std::string error;
		REQUIRE(SceneSerializer::SaveScene(scene, scenePath, error));
		const auto reference = ParseJson(*FileSystem::ReadTextFile(scenePath))["Entities"][0]["Components"]["Material"]["AlbedoMap"];
		const auto metadata = FileSystem::ReadTextFile(Project::ResolvePath("Assets/model.glb.meta"));
		REQUIRE(metadata);
		REQUIRE(Project::GetActive()->GetAssets().Remove("Assets/model.glb", error));
		auto loaded = SceneSerializer::LoadScene(scenePath, error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(AssetRegistry::IsUnresolvedKey(loaded->GetAllEntitiesOrdered()[0].GetComponent<MaterialComponent>().AlbedoMap));
		CHECK(Project::ResolvePath(loaded->GetAllEntitiesOrdered()[0].GetComponent<MaterialComponent>().AlbedoMap).empty());
		REQUIRE(SceneSerializer::SaveScene(*loaded, scenePath, error));
		CHECK(ParseJson(*FileSystem::ReadTextFile(scenePath))["Entities"][0]["Components"]["Material"]["AlbedoMap"] == reference);
		REQUIRE(FileSystem::WriteTextFile(source, "unrelated replacement"));
		REQUIRE(Project::GetActive()->GetAssets().Ensure("Assets/model.glb", error));
		loaded = SceneSerializer::LoadScene(scenePath, error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(AssetRegistry::IsUnresolvedKey(loaded->GetAllEntitiesOrdered()[0].GetComponent<MaterialComponent>().AlbedoMap));
		for (const auto* name : { "Assets/a.glb", "Assets/b.glb" })
		{
			REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(name), "copy"));
			REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(std::string(name) + ".meta"), *metadata));
		}
		REQUIRE(Project::GetActive()->GetAssets().Refresh(error));
		json duplicate = reference;
		duplicate["Path"] = "Assets/a.glb#image/0";
		std::string key;
		CHECK_FALSE(Project::GetActive()->GetAssets().Decode(duplicate, key, error));
		CHECK(error.find("duplicate") != std::string::npos);
		REQUIRE(ComponentRegistry::AddOrPatch(entity, "Material", { { "AlbedoMap", duplicate } }, error));
		CHECK(AssetRegistry::IsUnresolvedKey(entity.GetComponent<MaterialComponent>().AlbedoMap));
		REQUIRE(SceneSerializer::SavePrefab(entity, Project::ResolvePath("Assets/Prefabs/Unresolved.bprefab"), error));
		CHECK(SceneSerializer::InstantiatePrefab(scene, Project::ResolvePath("Assets/Prefabs/Unresolved.bprefab"), "Assets/Prefabs/Unresolved.bprefab", {}, error));
		json encoded;
		for (const auto* malformed : { "asset-uuid://1/ff", "asset-uuid://0/aa", "asset-uuid://1/a", "asset-uuid://1/xx", "asset-uuid://" })
			CHECK_FALSE(Project::GetActive()->GetAssets().Encode(malformed, encoded, error));
	}
}
