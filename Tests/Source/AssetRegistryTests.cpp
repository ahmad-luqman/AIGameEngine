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
#include <span>

using namespace Basalt;
using json = nlohmann::json;

namespace {

	// k indices naming one vertex cost 1 + 3 + ... + (2k - 1) = k^2: the most the valence cost limit allows.
	size_t SingleVertexValenceLimit()
	{
		size_t count = 0;
		while ((count + 1) * (count + 1) <= MeshOptimizer::MaxValenceCostPerIndex * (count + 1) + MeshOptimizer::ValenceCostAllowance)
			count++;
		return count;
	}

	// Average vertex cache misses per triangle for a 16-entry FIFO cache (about 0.7 after meshoptimizer's ordering, near
	// 3 for shuffled triangles).
	float AverageCacheMisses(std::span<const uint32_t> indices)
	{
		std::vector<uint32_t> cache;
		size_t misses = 0;
		for (const uint32_t index : indices)
		{
			if (std::find(cache.begin(), cache.end(), index) != cache.end())
				continue;
			misses++;
			cache.push_back(index);
			if (cache.size() > 16)
				cache.erase(cache.begin());
		}
		return 3.0f * static_cast<float>(misses) / static_cast<float>(indices.size());
	}

	// Shuffles whole triangles with a fixed linear congruential generator (std::shuffle differs between standard libraries).
	void ShuffleTriangles(std::vector<uint32_t>& indices)
	{
		uint32_t state = 12345;
		for (size_t i = indices.size() / 3; i > 1; i--)
		{
			state = state * 1664525u + 1013904223u;
			const size_t j = state % i;
			std::swap_ranges(indices.begin() + static_cast<std::ptrdiff_t>((i - 1) * 3), indices.begin() + static_cast<std::ptrdiff_t>(i * 3), indices.begin() + static_cast<std::ptrdiff_t>(j * 3));
		}
	}

	// A fan of triangles around vertex 0, whose valence is the triangle count; rim vertices are in at most two.
	void MakeFan(uint32_t triangles, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices)
	{
		vertices.assign(triangles + 2, Vertex{});
		indices.clear();
		for (uint32_t i = 0; i <= triangles; i++)
		{
			const float angle = 3.14159265f * static_cast<float>(i) / static_cast<float>(triangles);
			vertices[i + 1].Position = { std::cos(angle), 0, std::sin(angle) };
		}
		for (uint32_t i = 1; i <= triangles; i++)
			indices.insert(indices.end(), { 0, i + 1, i });
	}

	// A closed double cone: each apex is in every triangle on its side, each rim vertex in four.
	Ref<MeshSource> MakeBicone(uint32_t segments)
	{
		std::vector<Vertex> vertices(segments + 2);
		for (uint32_t i = 0; i < segments; i++)
		{
			const float angle = 6.2831853f * static_cast<float>(i) / static_cast<float>(segments);
			vertices[i].Position = { std::cos(angle), 0, std::sin(angle) };
		}
		vertices[segments].Position = { 0, 1, 0 };
		vertices[segments + 1].Position = { 0, -1, 0 };
		std::vector<uint32_t> indices;
		for (uint32_t i = 0; i < segments; i++)
		{
			const uint32_t next = (i + 1) % segments;
			indices.insert(indices.end(), { segments, next, i, segments + 1, i, next });
		}
		auto source = CreateRef<MeshSource>();
		source->AddSubmesh(vertices, indices, 0);
		return source;
	}

}

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
		REQUIRE(MeshOptimizer::Optimize(vertices, indices, false) == MeshOptimizer::OptimizeResult::Optimized);
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
		CHECK(MeshOptimizer::Optimize(vertices, indices, true) == MeshOptimizer::OptimizeResult::Invalid);
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

	TEST_CASE("Valence cost sums squared index references per vertex")
	{
		const size_t limit = SingleVertexValenceLimit();
		CHECK(MeshOptimizer::WithinValenceCost(std::vector<uint32_t>(limit, 0), 1));
		CHECK_FALSE(MeshOptimizer::WithinValenceCost(std::vector<uint32_t>(limit + 1, 0), 1));
		// The same references spread over two vertices cost half as much.
		std::vector<uint32_t> split(limit + 1, 0);
		std::fill(split.begin(), split.begin() + static_cast<std::ptrdiff_t>(split.size() / 2), 1u);
		CHECK(MeshOptimizer::WithinValenceCost(split, 2));
		// A closed grid (a torus: every vertex in six triangles) costs 6 per index, far below the limit at any size.
		constexpr uint32_t Size = 256;
		std::vector<uint32_t> torus;
		for (uint32_t y = 0; y < Size; y++)
			for (uint32_t x = 0; x < Size; x++)
			{
				const uint32_t a = y * Size + x, b = y * Size + (x + 1) % Size, c = (y + 1) % Size * Size + x, d = (y + 1) % Size * Size + (x + 1) % Size;
				torus.insert(torus.end(), { a, c, b, b, c, d });
			}
		CHECK(MeshOptimizer::WithinValenceCost(torus, size_t{ Size } * Size));
		CHECK_FALSE(MeshOptimizer::WithinValenceCost(std::vector<uint32_t>{ 0, 1, 2 }, 2));
	}

	// Regression (FuzzGltf, nightly): meshoptimizer's vertex cache order rescans the remaining triangles of every cached
	// vertex after each triangle it emits, so 46116 indices naming one vertex took 22 s in the fuzz build.
	TEST_CASE("Mesh optimization keeps the triangle order of geometry whose vertex valences cost too much")
	{
		// Whole triangles of one degenerate vertex, at and just past the limit.
		const size_t limit = SingleVertexValenceLimit() / 3 * 3;
		std::vector<Vertex> vertices(3);
		std::vector<uint32_t> indices(limit, 0);
		CHECK(MeshOptimizer::Optimize(vertices, indices, true) == MeshOptimizer::OptimizeResult::Optimized);
		vertices.assign(3, Vertex{}); // the call above removed the unused vertices
		indices.assign(limit + 3, 0);
		CHECK(MeshOptimizer::Optimize(vertices, indices, true) == MeshOptimizer::OptimizeResult::TriangleOrderKept);

		// A fan past the limit, with its triangles shuffled and a morph target that moves each vertex by its own position.
		MakeFan(5000, vertices, indices);
		ShuffleTriangles(indices);
		std::vector<MorphDelta> deltas(vertices.size());
		for (size_t i = 0; i < vertices.size(); i++)
			deltas[i].Position = vertices[i].Position;
		std::vector<glm::vec3> corners;
		corners.reserve(indices.size());
		for (const uint32_t index : indices)
			corners.push_back(vertices[index].Position);
		const auto original = vertices;
		auto kept = indices;
		auto keptDeltas = deltas;
		REQUIRE(MeshOptimizer::Optimize(vertices, kept, true, &keptDeltas, 1) == MeshOptimizer::OptimizeResult::TriangleOrderKept);
		// The triangles keep their order, while the vertices (and their deltas) are renumbered in fetch order.
		REQUIRE(kept.size() == corners.size());
		for (size_t i = 0; i < kept.size(); i++)
			CHECK(vertices[kept[i]].Position == corners[i]);
		for (size_t i = 0; i < vertices.size(); i++)
			CHECK(keptDeltas[i].Position == vertices[i].Position);
		CHECK(kept[0] == 0);

		// Blended geometry is never reordered, so its valences do not matter.
		vertices = original;
		auto blended = indices;
		CHECK(MeshOptimizer::Optimize(vertices, blended, false) == MeshOptimizer::OptimizeResult::Optimized);

		// A fan within the limit is ordered for the vertex cache.
		MakeFan(2000, vertices, indices);
		ShuffleTriangles(indices);
		REQUIRE(AverageCacheMisses(indices) > 1.8f);
		CHECK(MeshOptimizer::Optimize(vertices, indices, true) == MeshOptimizer::OptimizeResult::Optimized);
		CHECK(AverageCacheMisses(indices) < 1.2f);
	}

	TEST_CASE("LODs skip geometry whose vertex valences cost too much and order their triangles for the vertex cache")
	{
		// About 8 million against a limit near 17.6 million, and about 33.6 million against one near 18.4 million.
		auto affordable = MakeBicone(2048);
		auto copy = affordable->Vertices;
		auto copyIndices = affordable->Indices;
		CHECK(MeshOptimizer::Optimize(copy, copyIndices, true) == MeshOptimizer::OptimizeResult::Optimized);
		MeshOptimizer::GenerateLODs(*affordable, 0);
		CHECK_FALSE(affordable->Submeshes[0].LODs.empty());
		auto large = MakeBicone(4096);
		const size_t indexCount = large->Indices.size();
		MeshOptimizer::GenerateLODs(*large, 0);
		CHECK(large->Submeshes[0].LODs.empty());
		CHECK(large->Indices.size() == indexCount);
		CHECK(MeshOptimizer::Optimize(large->Vertices, large->Indices, true) == MeshOptimizer::OptimizeResult::TriangleOrderKept);

		// An index past the submesh's vertices gets no LODs (the simplifier would read past them).
		auto broken = MakeBicone(64);
		broken->Indices[5] = broken->Submeshes[0].VertexCount;
		MeshOptimizer::GenerateLODs(*broken, 0);
		CHECK(broken->Submeshes[0].LODs.empty());

		// Reduced levels of a shuffled sphere are ordered for the vertex cache too.
		const auto sphere = Primitives::CreateSphere(64, 32);
		auto shuffled = sphere->Indices;
		ShuffleTriangles(shuffled);
		auto source = CreateRef<MeshSource>();
		source->AddSubmesh(sphere->Vertices, shuffled, 0);
		MeshOptimizer::GenerateLODs(*source, 0);
		REQUIRE_FALSE(source->Submeshes[0].LODs.empty());
		for (const auto& lod : source->Submeshes[0].LODs)
			CHECK(AverageCacheMisses(std::span(source->Indices).subspan(lod.BaseIndex, lod.IndexCount)) < 1.2f);
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
		REQUIRE(MeshOptimizer::Optimize(source->Vertices, source->Indices, true) == MeshOptimizer::OptimizeResult::Optimized);
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
