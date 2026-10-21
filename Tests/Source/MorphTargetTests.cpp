#include <doctest/doctest.h>

#include "AnimationTestUtils.h"
#include "TestUtils.h"

#include <Basalt/Animation/AnimatedModel.h>
#include <Basalt/Asset/MeshOptimizer.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Renderer/RenderMath.h>
#include <Basalt/Scene/VertexSnap.h>

#include <nlohmann/json.hpp>

#include <fstream>

using namespace Basalt;
using namespace BasaltTest;
using AnimationTest::ClearAssetCache;
using AnimationTest::Clip;
using AnimationTest::MutableClip;
using AnimationTest::Node;

namespace {
	std::filesystem::path MorphPath()
	{
		return AnimationTest::FixturePath("Morph.gltf");
	}

	nlohmann::json MorphJson()
	{
		return AnimationTest::ReadJson(MorphPath());
	}

	// The importer reorders vertices: find one of a submesh by its bind position.
	uint32_t VertexAt(const MeshSource& source, const Submesh& submesh, const glm::vec3& position)
	{
		for (uint32_t v = 0; v < submesh.VertexCount; v++)
			if (glm::all(glm::epsilonEqual(source.Vertices[submesh.BaseVertex + v].Position, position, 1e-6f)))
				return v;
		FAIL("no vertex at the position");
		return 0;
	}

	const MorphDelta& Delta(const MeshSource& source, const Submesh& submesh, uint32_t target, uint32_t vertex)
	{
		return source.MorphDeltas[submesh.MorphDeltaOffset + target * submesh.VertexCount + vertex];
	}

	float Slot(const MeshSource& source, const AnimationLocalPose& pose, const std::string& node, uint32_t target)
	{
		const MeshNode& meshNode = source.Nodes[Node(source, node)];
		REQUIRE(meshNode.MorphWeightOffset >= 0);
		return pose.MorphWeights[static_cast<size_t>(meshNode.MorphWeightOffset) + target];
	}

	std::string ImportError(const nlohmann::json& data)
	{
		TempDirectory directory("MorphImport");
		const auto path = directory.GetPath() / "Bad.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		std::string error;
		CHECK_FALSE(MeshImporter::LoadGltf(path, path.string(), error));
		return error;
	}
}

TEST_SUITE("MorphTargets")
{
	TEST_CASE("Morph targets import with unique names, default weights and node weights")
	{
		const auto source = AnimationTest::LoadModel(MorphPath());
		REQUIRE(source->Meshes.size() == 2);
		CHECK(source->Meshes[0].MorphTargets == std::vector<std::string>{ "Smile", "Blink" });
		CHECK(source->Meshes[0].MorphWeights == std::vector<float>{ 0.0f, 0.25f });
		CHECK(source->Meshes[1].MorphTargets == std::vector<std::string>{ "Smile" });
		CHECK(source->Meshes[1].MorphWeights == std::vector<float>{ 0.0f });
		// The Face node overrides the mesh's defaults; Teeth keeps its mesh's.
		const MeshNode& face = source->Nodes[Node(*source, "Face")];
		const MeshNode& teeth = source->Nodes[Node(*source, "Teeth")];
		CHECK(face.MorphWeights == std::vector<float>{ 0.5f, 0.25f });
		CHECK(teeth.MorphWeights == std::vector<float>{ 0.0f });
		CHECK(source->Nodes[Node(*source, "Head")].MorphWeightOffset == -1);
		CHECK(source->MorphWeightCount == 3);
		CHECK(face.MorphWeightOffset != teeth.MorphWeightOffset);
		const Submesh& faceSubmesh = source->Submeshes[source->Meshes[0].Submeshes[0]];
		CHECK(faceSubmesh.MorphTargetCount == 2);
		CHECK(faceSubmesh.MorphBounds.size() == 2);
		CHECK(source->MorphDeltas.size() == 2 * 9 + 3);

		SUBCASE("Duplicate and missing names become unique selection keys")
		{
			auto data = MorphJson();
			data["meshes"][0]["extras"]["targetNames"] = { "Same", "Same" };
			data["meshes"][1]["extras"].erase("targetNames");
			TempDirectory directory("MorphNames");
			const auto since = Log::GetHistory().GetTotalCount();
			const auto renamed = AnimationTest::LoadModified(data, directory.GetPath(), "Names.gltf");
			CHECK(CountLogMessages(since, "morph target 1 is named 'Same' like an earlier one; it is 'Same_1'") == 1);
			CHECK(renamed->Meshes[0].MorphTargets == std::vector<std::string>{ "Same", "Same_1" });
			CHECK(renamed->Meshes[1].MorphTargets == std::vector<std::string>{ "Target0" });
		}
	}

	TEST_CASE("Morph deltas follow their vertices through import optimization, sparse targets included")
	{
		const auto source = AnimationTest::LoadModel(MorphPath());
		const Submesh& submesh = source->Submeshes[source->Meshes[0].Submeshes[0]];
		const uint32_t center = VertexAt(*source, submesh, { 0, 0.5f, 0 });
		const uint32_t corner = VertexAt(*source, submesh, { -0.5f, 0, 0 });
		const uint32_t top = VertexAt(*source, submesh, { 0.5f, 1, 0 });
		// Vertex 4 of the authored grid is no longer at index 4: the fetch optimization moved it.
		CHECK(Delta(*source, submesh, 0, center).Position == glm::vec3(0, 0, 0.3f));
		CHECK(Delta(*source, submesh, 0, center).Normal == glm::vec3(0, -0.2f, 0));
		CHECK(Delta(*source, submesh, 0, corner).Position == glm::vec3(0));
		CHECK(Delta(*source, submesh, 1, top).Position == glm::vec3(0, 0, -0.25f));
		CHECK(Delta(*source, submesh, 1, center).Position == glm::vec3(0));
		CHECK(Delta(*source, submesh, 1, corner).Position == glm::vec3(0));
		CHECK(submesh.MorphBounds[0].Min == glm::vec3(0));
		CHECK(submesh.MorphBounds[0].Max == glm::vec3(0, 0, 0.3f));
		CHECK(submesh.MorphBounds[1].Min == glm::vec3(0, 0, -0.25f));

		SUBCASE("The optimizer keeps deltas with their vertices")
		{
			std::vector<Vertex> vertices(4);
			for (size_t i = 0; i < vertices.size(); i++)
				vertices[i].Position = glm::vec3(static_cast<float>(i), 0, 0);
			// Vertex 0 is unused and 3 comes first, so the fetch order is 3, 1, 2.
			std::vector<uint32_t> indices = { 3, 1, 2 };
			std::vector<MorphDelta> deltas(8);
			for (size_t i = 0; i < deltas.size(); i++)
				deltas[i].Position = glm::vec3(0, static_cast<float>(i), 0);
			REQUIRE(MeshOptimizer::Optimize(vertices, indices, false, &deltas, 2));
			REQUIRE(vertices.size() == 3);
			REQUIRE(deltas.size() == 6);
			for (size_t target = 0; target < 2; target++)
				for (size_t v = 0; v < vertices.size(); v++)
					CHECK(deltas[target * 3 + v].Position.y == static_cast<float>(target * 4) + vertices[v].Position.x);
			std::vector<MorphDelta> mismatched(3);
			CHECK_FALSE(MeshOptimizer::Optimize(vertices, indices, false, &mismatched, 2));
		}
	}

	TEST_CASE("Targets without NORMAL deltas take the change in generated normals")
	{
		const auto source = AnimationTest::LoadModel(MorphPath());
		const Submesh& submesh = source->Submeshes[source->Meshes[0].Submeshes[0]];
		std::vector<Vertex> base(source->Vertices.begin() + submesh.BaseVertex, source->Vertices.begin() + submesh.BaseVertex + submesh.VertexCount);
		std::vector<uint32_t> indices(source->Indices.begin() + submesh.BaseIndex, source->Indices.begin() + submesh.BaseIndex + submesh.IndexCount);
		std::vector<Vertex> morphed = base;
		for (uint32_t v = 0; v < submesh.VertexCount; v++)
			morphed[v].Position += Delta(*source, submesh, 1, v).Position;
		MeshSource::GenerateNormals(base, indices);
		MeshSource::GenerateNormals(morphed, indices);
		bool tilted = false;
		for (uint32_t v = 0; v < submesh.VertexCount; v++)
		{
			const glm::vec3 expected = morphed[v].Normal - base[v].Normal;
			CHECK(glm::all(glm::epsilonEqual(Delta(*source, submesh, 1, v).Normal, expected, 1e-5f)));
			tilted = tilted || glm::length(expected) > 0.1f;
		}
		CHECK(tilted);
		// Smile's authored NORMAL deltas are kept as they are.
		CHECK(Delta(*source, submesh, 0, VertexAt(*source, submesh, { 0.5f, 0.5f, 0 })).Normal == glm::vec3(0, -0.2f, 0));
	}

	TEST_CASE("Malformed morph targets and weight channels are rejected with their location")
	{
		auto data = MorphJson();
		std::string expected;
		SUBCASE("primitives with different target counts")
		{
			auto primitive = data["meshes"][0]["primitives"][0];
			primitive.erase("targets");
			data["meshes"][0]["primitives"].push_back(primitive);
			// cgltf_validate rejects these before the importer's own checks see them.
			expected = "validation failed";
		}
		SUBCASE("a target whose count differs from POSITION")
		{
			data["meshes"][0]["primitives"][0]["targets"][0]["POSITION"] = data["meshes"][1]["primitives"][0]["targets"][0]["POSITION"];
			expected = "validation failed";
		}
		SUBCASE("node weights that do not match the targets")
		{
			data["nodes"][3]["weights"] = { 0.5, 0.5 };
			expected = "validation failed";
		}
		SUBCASE("a weights channel on a node without targets")
		{
			data["animations"][0]["channels"][0]["target"]["node"] = 1;
			expected = "validation failed";
		}
		SUBCASE("weight values that do not match the keys")
		{
			data["animations"][0]["samplers"][0]["output"] = data["animations"][0]["samplers"][1]["output"];
			expected = "validation failed";
		}
		SUBCASE("a target accessor of the wrong type")
		{
			auto accessor = data["accessors"][data["meshes"][1]["primitives"][0]["targets"][0]["POSITION"].get<size_t>()];
			accessor["type"] = "VEC4";
			accessor["count"] = 2;
			data["accessors"].push_back(accessor);
			data["meshes"][1]["primitives"][0]["targets"][0]["POSITION"] = data["accessors"].size() - 1;
			expected = "validation failed";
		}
		const std::string error = ImportError(data);
		CHECK_MESSAGE(error.find(expected) != std::string::npos, error);
	}

	TEST_CASE("Weights tracks sample linear, step and cubic keys over the node's defaults")
	{
		const auto source = AnimationTest::LoadModel(MorphPath());
		const AnimationLocalPose rest = SampleAnimation(*source, nullptr, 0);
		CHECK(Slot(*source, rest, "Face", 0) == 0.5f);
		CHECK(Slot(*source, rest, "Face", 1) == 0.25f);
		CHECK(Slot(*source, rest, "Teeth", 0) == 0.0f);

		const AnimationClip& talk = Clip(*source, "Talk");
		const AnimationLocalPose half = SampleAnimation(*source, &talk, 0.5f);
		CHECK(Slot(*source, half, "Face", 0) == doctest::Approx(0.5f));
		CHECK(Slot(*source, half, "Face", 1) == doctest::Approx(0.25f));
		CHECK(Slot(*source, half, "Teeth", 0) == 0.2f);
		const AnimationLocalPose later = SampleAnimation(*source, &talk, 1.5f);
		CHECK(Slot(*source, later, "Face", 0) == doctest::Approx(0.5f));
		CHECK(Slot(*source, later, "Teeth", 0) == 0.8f);
		CHECK(Slot(*source, SampleAnimation(*source, &talk, 5.0f), "Face", 0) == 0.0f);

		const AnimationClip& cubic = Clip(*source, "CubicSmile");
		const AnimationLocalPose curve = SampleAnimation(*source, &cubic, 0.25f);
		// Hermite with unit end tangents: 0.25 + t^3 - t^2 ... = h10 + h01 + h11 at t = 0.25.
		const float t = 0.25f;
		CHECK(Slot(*source, curve, "Face", 0) == doctest::Approx((t * t * t - 2 * t * t + t) + (-2 * t * t * t + 3 * t * t) + (t * t * t - t * t)));
		CHECK(Slot(*source, curve, "Face", 1) == 0.0f);
		CHECK(Slot(*source, curve, "Teeth", 0) == 0.0f);

		const auto channels = AnimatedChannels(*source, talk);
		CHECK(channels[Node(*source, "Face")] == AnimatedChannel::Weights);
		CHECK(channels[Node(*source, "Head")] == 0);
	}

	TEST_CASE("Morph weights blend, and layers move them by the node mask")
	{
		const auto source = AnimationTest::LoadModel(MorphPath());
		const AnimationClip& talk = Clip(*source, "Talk");
		AnimationLocalPose target = SampleAnimation(*source, &talk, 1.0f);
		const AnimationLocalPose from = SampleAnimation(*source, nullptr, 0);
		BlendAnimationPose(target, from, 0.25f);
		CHECK(Slot(*source, target, "Face", 0) == doctest::Approx(0.5f + (1.0f - 0.5f) * 0.25f));

		AnimationLayerMask mask{ .Weights = std::vector<float>(source->Nodes.size(), 1.0f), .Channels = AnimatedChannels(*source, talk) };
		mask.Weights[Node(*source, "Teeth")] = 0;
		MorphMaskWeights(*source, mask);
		CHECK(mask.MorphWeights.size() == source->MorphWeightCount);

		AnimationLocalPose overridden = SampleAnimation(*source, nullptr, 0);
		const AnimationLocalPose layer = SampleAnimation(*source, &talk, 1.0f);
		OverrideAnimationPose(overridden, layer, mask, 0.5f);
		CHECK(Slot(*source, overridden, "Face", 0) == doctest::Approx(0.75f));
		CHECK(Slot(*source, overridden, "Teeth", 0) == 0.0f);

		AnimationLocalPose added = SampleAnimation(*source, nullptr, 0);
		AddAnimationPose(added, layer, SampleAnimation(*source, &talk, 0), mask, 1.0f);
		CHECK(Slot(*source, added, "Face", 0) == doctest::Approx(0.5f + 1.0f));
		CHECK(Slot(*source, added, "Face", 1) == doctest::Approx(0.25f + 0.5f));

		// A clip that animates no weights leaves them to the pose below.
		AnimationLayerMask nod{ .Weights = std::vector<float>(source->Nodes.size(), 1.0f), .Channels = AnimatedChannels(*source, Clip(*source, "Nod")) };
		MorphMaskWeights(*source, nod);
		AnimationLocalPose kept = SampleAnimation(*source, nullptr, 0);
		OverrideAnimationPose(kept, layer, nod, 1.0f);
		CHECK(Slot(*source, kept, "Face", 0) == 0.5f);
	}

	TEST_CASE("Scripts set morph weights by name, fading from the weight shown, until cleared")
	{
		ClearAssetCache cacheGuard;
		Scene scene;
		auto entity = scene.CreateEntity("Face");
		auto& animator = entity.AddComponent<AnimatorComponent>();
		animator.Mesh = MorphPath().string();
		animator.Clip = "Talk";
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const auto source = AssetManager::GetMesh(animator.Mesh);
		REQUIRE(source);
		const uint32_t faceSmile = static_cast<uint32_t>(source->Nodes[Node(*source, "Face")].MorphWeightOffset);
		const uint32_t faceBlink = faceSmile + 1;
		const uint32_t teethSmile = static_cast<uint32_t>(source->Nodes[Node(*source, "Teeth")].MorphWeightOffset);

		CHECK(system.GetMorphTargets(entity) == std::vector<std::string>{ "Smile", "Blink" });
		system.Update(0.5f);
		CHECK((*system.GetMorphWeights(entity))[faceSmile] == doctest::Approx(0.5f));

		// One name sets every mesh with that target.
		REQUIRE(system.SetMorphWeight(entity, "Smile", 1.0f, 0.0f));
		CHECK((*system.GetMorphWeights(entity))[faceSmile] == 1.0f);
		CHECK((*system.GetMorphWeights(entity))[teethSmile] == 1.0f);
		CHECK(system.GetMorphWeight(entity, "Smile").value_or(-1.0f) == 1.0f);

		// A fade starts from the weight shown and continues while the clip plays.
		const float blink = (*system.GetMorphWeights(entity))[faceBlink];
		REQUIRE(system.SetMorphWeight(entity, "Blink", 1.0f, 1.0f));
		system.Update(0.5f);
		CHECK((*system.GetMorphWeights(entity))[faceBlink] == doctest::Approx(blink + (1.0f - blink) * 0.5f));
		system.Update(0.75f);
		CHECK((*system.GetMorphWeights(entity))[faceBlink] == 1.0f);

		// Overrides name targets, so they survive a playback reset.
		system.Stop(entity);
		animator.Clip = "CubicSmile";
		system.Update(0.0f);
		CHECK((*system.GetMorphWeights(entity))[faceSmile] == 1.0f);

		CHECK(system.ClearMorphWeight(entity, "Smile"));
		CHECK_FALSE(system.ClearMorphWeight(entity, "Smile"));
		CHECK((*system.GetMorphWeights(entity))[faceSmile] != 1.0f);
		// Only nil clears every target; an empty name is an unknown target.
		const auto since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(system.ClearMorphWeight(entity, ""));
		CHECK(CountLogMessages(since, "no morph target named ''") == 1);
		CHECK(system.ClearMorphWeight(entity, std::nullopt));
		CHECK((*system.GetMorphWeights(entity))[faceBlink] != 1.0f);

		CHECK_FALSE(system.SetMorphWeight(entity, "Frown", 1.0f, 0.0f));
		CHECK_FALSE(system.SetMorphWeight(entity, "Frown", 1.0f, 0.0f));
		CHECK(CountLogMessages(since, "no morph target named 'Frown'") == 1);
		CHECK_FALSE(system.SetMorphWeight(entity, "Smile", 1.5f, 0.0f));
		CHECK_FALSE(system.SetMorphWeight(entity, "Smile", 0.5f, -1.0f));
		CHECK_FALSE(system.GetMorphWeight(entity, "Frown").has_value());
		CHECK(CountLogMessages(since, "Animator 'Face': no morph target named 'Frown'") == 2); // SetMorphWeight's, then GetMorphWeight's.
		scene.OnRuntimeStop();
	}

	TEST_CASE("Morph weights widen culling and framing bounds")
	{
		const auto source = AnimationTest::LoadModel(MorphPath());
		const Submesh& face = source->Submeshes[source->Meshes[0].Submeshes[0]];
		const float weights[] = { 1.0f, -2.0f };
		const AABB range = MorphDisplacementBounds(face, weights);
		CHECK(range.Min == glm::vec3(0, 0, 0));
		CHECK(range.Max == glm::vec3(0, 0, 0.3f + 0.5f));
		CHECK_FALSE(MorphDisplacementBounds(Submesh{}, weights).IsValid());

		std::vector<glm::mat4> globals;
		REQUIRE(EvaluateAnimation(*source, &Clip(*source, "Nod"), 0.7f, globals));
		const auto palette = BuildSkinPalette(source->Skins[0], globals);
		for (const auto& set : { std::vector<float>{ 0, 0 }, std::vector<float>{ 1, 0.25f }, std::vector<float>{ -1, 1 }, std::vector<float>{ 0.5f, -3 } })
		{
			const AABB exact = SkinBounds(*source, face, palette, set);
			const AABB cached = CachedSkinBounds(*source, face, palette, set);
			CHECK(glm::all(glm::lessThanEqual(cached.Min, exact.Min + 1e-5f)));
			CHECK(glm::all(glm::greaterThanEqual(cached.Max, exact.Max - 1e-5f)));
		}
		// At rest the unmorphed grid is flat; Smile pushes it forward.
		REQUIRE(EvaluateAnimation(*source, nullptr, 0, globals));
		const auto rest = BuildSkinPalette(source->Skins[0], globals);
		CHECK(SkinBounds(*source, face, rest, std::vector<float>{ 0, 0 }).Max.z == 0.0f);
		CHECK(SkinBounds(*source, face, rest, std::vector<float>{ 1, 0 }).Max.z == doctest::Approx(0.3f));

		ClearAssetCache cacheGuard;
		Scene scene;
		auto entity = scene.CreateEntity("Face");
		entity.AddComponent<AnimatorComponent>().Mesh = MorphPath().string();
		// Edit mode draws the node's weights (Smile 0.5): the middle row is 0.15 in front.
		CHECK(AnimatedModelBounds(entity).Max.z == doctest::Approx(0.15f));
	}

	TEST_CASE("Morph targets past the import limits are dropped with a warning; the geometry still loads")
	{
		auto data = MorphJson();
		std::string expected;
		SUBCASE("too many targets")
		{
			auto& targets = data["meshes"][1]["primitives"][0]["targets"];
			while (targets.size() <= MaxMorphTargets)
				targets.push_back(targets[0]);
			data["meshes"][1]["extras"].erase("targetNames");
			// Teeth's animated weights would need 257 values per key.
			data["animations"][0]["channels"].erase(1);
			expected = "257 morph targets exceed the limit of 256 per mesh";
		}
		SUBCASE("more deltas than a model may hold")
		{
			// Zero-filled accessors without a bufferView keep the file small: 4 targets x 1048578 vertices pass MaxMorphDeltas.
			const nlohmann::json zeros = { { "componentType", 5126 }, { "count", 1048578 }, { "type", "VEC3" }, { "min", { 0, 0, 0 } }, { "max", { 0, 0, 0 } } };
			data["accessors"].push_back(zeros);
			const size_t index = data["accessors"].size() - 1;
			data["meshes"][1]["primitives"][0] = { { "attributes", { { "POSITION", index } } }, { "targets", { { { "POSITION", index } }, { { "POSITION", index } }, { { "POSITION", index } }, { { "POSITION", index } } } } };
			data["meshes"][1]["extras"].erase("targetNames");
			// Teeth's animated weights are now four wide.
			data["animations"][0]["channels"].erase(1);
			expected = "its morph targets need 4194312 deltas, past the 4194286 left of the limit of 4194304 per model";
		}
		ClearAssetCache cacheGuard;
		TempDirectory directory("MorphLimits");
		const auto since = Log::GetHistory().GetTotalCount();
		const auto source = AnimationTest::LoadModified(data, directory.GetPath(), "Limits.gltf");
		CHECK(CountLogMessages(since, expected) == 1);
		// Teeth keeps its base shape; Face keeps its targets.
		CHECK(source->Meshes[1].MorphTargets.empty());
		CHECK(source->Nodes[Node(*source, "Teeth")].MorphWeightOffset == -1);
		CHECK(source->Meshes[0].MorphTargets.size() == 2);
	}

	TEST_CASE("Weights channels of meshes whose targets were dropped are skipped with a warning")
	{
		auto data = MorphJson();
		auto& targets = data["meshes"][1]["primitives"][0]["targets"];
		while (targets.size() <= MaxMorphTargets)
			targets.push_back(targets[0]);
		data["meshes"][1]["extras"].erase("targetNames");
		// The Talk clip's Teeth channel now has 257 values per key, which cgltf accepts for 257 targets.
		const auto output = data["animations"][0]["samplers"][1]["output"].get<size_t>();
		std::vector<float> values(2 * targets.size(), 0.5f);
		data["buffers"].push_back({ { "byteLength", values.size() * 4 }, { "uri", "data:application/octet-stream;base64," + Base64(values.data(), values.size() * 4) } });
		data["bufferViews"].push_back({ { "buffer", data["buffers"].size() - 1 }, { "byteLength", values.size() * 4 } });
		data["accessors"][output] = { { "bufferView", data["bufferViews"].size() - 1 }, { "componentType", 5126 }, { "count", values.size() }, { "type", "SCALAR" } };
		TempDirectory directory("MorphDroppedChannel");
		const auto since = Log::GetHistory().GetTotalCount();
		const auto source = AnimationTest::LoadModified(data, directory.GetPath(), "Dropped.gltf");
		CHECK(CountLogMessages(since, "skipping the morph weights of node 'Teeth', which has no morph targets (dropped or none)") == 1);
		CHECK(std::ranges::none_of(Clip(*source, "Talk").Tracks, [&](const AnimationTrack& track) { return track.Node == Node(*source, "Teeth"); }));
	}

	TEST_CASE("FinalizeHierarchy names what does not fit and changes nothing then")
	{
		auto source = CreateRef<MeshSource>(*AnimationTest::LoadModel(MorphPath()));
		const auto offsets = source->Nodes[Node(*source, "Teeth")].MorphWeightOffset;
		std::string error;
		source->Nodes[Node(*source, "Teeth")].MorphWeights.push_back(1.0f);
		CHECK_FALSE(source->FinalizeHierarchy(&error));
		CHECK(error == "node 'Teeth' has 2 morph weights for 1 morph targets");
		CHECK(source->Nodes[Node(*source, "Teeth")].MorphWeightOffset == offsets);
		CHECK(source->MorphWeightCount == 3);
		source->Nodes[Node(*source, "Teeth")].MorphWeights.pop_back();
		source->Meshes[1].MorphWeights.clear();
		CHECK_FALSE(source->FinalizeHierarchy(&error));
		CHECK(error == "mesh 'Teeth' has 0 default morph weights for 1 targets");
		source->Meshes[1].MorphWeights = { 0.0f };
		source->Submeshes[source->Meshes[1].Submeshes[0]].MorphTargetCount = 2;
		CHECK_FALSE(source->FinalizeHierarchy(&error));
		CHECK(error.find("mesh 'Teeth' has a submesh whose morph targets do not match") == 0);
		source->Submeshes[source->Meshes[1].Submeshes[0]].MorphTargetCount = 1;
		source->Nodes[0].Parent = 0;
		CHECK_FALSE(source->FinalizeHierarchy(&error));
		CHECK(error == "the node hierarchy has a cycle");
	}

	TEST_CASE("Models with morph targets import as an Animator root, keeping node weights")
	{
		auto data = MorphJson();
		// A static, unskinned morph model: no skin, joints or clips.
		data.erase("animations");
		data.erase("skins");
		data["nodes"][2].erase("skin");
		data["meshes"][0]["primitives"][0]["attributes"].erase("JOINTS_0");
		data["meshes"][0]["primitives"][0]["attributes"].erase("WEIGHTS_0");
		TempDirectory directory("MorphImportHierarchy");
		const auto source = AnimationTest::LoadModified(data, directory.GetPath(), "Static.gltf");
		REQUIRE(source->MorphWeightCount == 3);
		Scene scene;
		const Entity root = MeshImporter::InstantiateHierarchy(scene, *source, "Face", {});
		CHECK(root.HasComponent<AnimatorComponent>());
	}

	TEST_CASE("Layers move morph weights by the node mask at runtime, override and additive")
	{
		ClearAssetCache cacheGuard;
		Scene scene;
		auto entity = scene.CreateEntity("Face");
		auto& animator = entity.AddComponent<AnimatorComponent>();
		animator.Mesh = MorphPath().string();
		animator.Clip = "Nod";
		AnimationLayer layer;
		layer.Name = "Talk";
		layer.Clip = "Talk";
		SUBCASE("override")
		{
			animator.Layers = { layer };
		}
		SUBCASE("masked to the face")
		{
			layer.Mask = { AnimationMaskJoint{ .Joint = "Face" } };
			animator.Layers = { layer };
		}
		SUBCASE("additive")
		{
			layer.Blend = AnimationLayerBlend::Additive;
			animator.Layers = { layer };
		}
		const bool masked = !animator.Layers[0].Mask.empty();
		const bool additive = animator.Layers[0].Blend == AnimationLayerBlend::Additive;
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		system.Update(1.5f);
		const auto source = AssetManager::GetMesh(animator.Mesh);
		const auto& weights = *system.GetMorphWeights(entity);
		const auto face = static_cast<size_t>(source->Nodes[Node(*source, "Face")].MorphWeightOffset);
		const auto teeth = static_cast<size_t>(source->Nodes[Node(*source, "Teeth")].MorphWeightOffset);
		// Talk at 1.5 s: Face (0.5, 0.25), Teeth 0.8 (stepped). The base (Nod) shows the node defaults (0.5, 0.25) and 0.
		// Additive adds Talk's change from its first frame (Face 0 and 0, Teeth 0.2).
		CHECK(weights[face] == doctest::Approx(additive ? 0.5f + 0.5f : 0.5f));
		CHECK(weights[face + 1] == doctest::Approx(additive ? 0.25f + 0.25f : 0.25f));
		CHECK(weights[teeth] == doctest::Approx(masked ? 0.0f : (additive ? 0.8f - 0.2f : 0.8f)));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Script morph weights fade while the Animator is stopped or paused")
	{
		ClearAssetCache cacheGuard;
		Scene scene;
		auto entity = scene.CreateEntity("Face");
		auto& animator = entity.AddComponent<AnimatorComponent>();
		animator.Mesh = MorphPath().string();
		animator.Clip = "Nod";
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		SUBCASE("stopped")
		{
			system.Stop(entity);
		}
		SUBCASE("paused")
		{
			system.Pause(entity, true);
		}
		system.Update(0.1f);
		REQUIRE(system.SetMorphWeight(entity, "Blink", 1.0f, 1.0f));
		system.Update(0.5f);
		const auto source = AssetManager::GetMesh(animator.Mesh);
		const auto blink = static_cast<size_t>(source->Nodes[Node(*source, "Face")].MorphWeightOffset) + 1;
		CHECK((*system.GetMorphWeights(entity))[blink] == doctest::Approx(0.25f + 0.75f * 0.5f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Script morph weights follow their target name to another model, and wait while it lacks the name")
	{
		ClearAssetCache cacheGuard;
		TempDirectory directory("MorphOverrideModels");
		auto data = MorphJson();
		data["meshes"][0]["extras"]["targetNames"] = { "Blink", "Smile" };
		const auto reordered = directory.GetPath() / "Reordered.gltf";
		REQUIRE(FileSystem::WriteTextFile(reordered, data.dump()));
		Scene scene;
		auto entity = scene.CreateEntity("Face");
		auto& animator = entity.AddComponent<AnimatorComponent>();
		animator.Mesh = MorphPath().string();
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.SetMorphWeight(entity, "Smile", 0.9f, 0));
		animator.Mesh = reordered.string();
		system.Update(0);
		auto source = AssetManager::GetMesh(animator.Mesh);
		// Smile is the second Face target now.
		CHECK((*system.GetMorphWeights(entity))[static_cast<size_t>(source->Nodes[Node(*source, "Face")].MorphWeightOffset) + 1] == doctest::Approx(0.9f));
		CHECK(system.GetMorphWeight(entity, "Smile").value_or(-1.0f) == doctest::Approx(0.9f));

		// A model without the target logs once; the weight applies again when a model with it returns.
		const auto since = Log::GetHistory().GetTotalCount();
		animator.Mesh = (std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/TwoJoint.gltf").string();
		system.Update(0);
		system.Update(0);
		CHECK(CountLogMessages(since, "morph target 'Smile' is not in") == 1);
		animator.Mesh = MorphPath().string();
		system.Update(0);
		source = AssetManager::GetMesh(animator.Mesh);
		CHECK((*system.GetMorphWeights(entity))[static_cast<size_t>(source->Nodes[Node(*source, "Face")].MorphWeightOffset)] == doctest::Approx(0.9f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("A reload that changes a model's morph targets keeps drawing within its weights")
	{
		ClearAssetCache cacheGuard;
		TempDirectory directory("MorphReload");
		auto data = MorphJson();
		const auto path = directory.GetPath() / "Face.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		Scene scene;
		auto entity = scene.CreateEntity("Face");
		auto& animator = entity.AddComponent<AnimatorComponent>();
		animator.Mesh = path.string();
		animator.Clip = "Talk";
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		system.Update(0.5f);
		CHECK(system.GetMorphWeights(entity)->size() == 3);
		// Face loses Blink: the weights shrink with the reloaded model before anything reads them.
		data["meshes"][0]["primitives"][0]["targets"].erase(1);
		data["meshes"][0]["weights"] = { 0 };
		data["meshes"][0]["extras"]["targetNames"] = { "Smile" };
		data["nodes"][2]["weights"] = { 0.5 };
		data.erase("animations");
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		AssetManager::Reload(path.string());
		size_t visited = 0;
		ForEachAnimatedMesh(entity, [&](const Ref<MeshSource>& source, const MeshNode& node, const glm::mat4&, const std::vector<glm::mat4>&, std::span<const float> weights) {
			CHECK(weights.size() == node.MorphWeights.size());
			CHECK(source->MorphWeightCount == 2);
			visited++;
		});
		CHECK(visited == 2);
		CHECK(system.GetMorphWeights(entity)->size() == 2);
		CHECK(AnimatedModelBounds(entity).IsValid());
		scene.OnRuntimeStop();
	}

	TEST_CASE("A non-finite pose shows the nodes' morph weights; non-finite weights show as 0")
	{
		ClearAssetCache cacheGuard;
		Scene scene;
		auto entity = scene.CreateEntity("Face");
		auto& animator = entity.AddComponent<AnimatorComponent>();
		animator.Mesh = MorphPath().string();
		animator.Clip = "Talk";
		const auto source = AssetManager::GetMesh(animator.Mesh);
		REQUIRE(source);
		auto& talk = MutableClip(*source, "Talk");
		const auto face = static_cast<size_t>(source->Nodes[Node(*source, "Face")].MorphWeightOffset);
		SUBCASE("non-finite pose")
		{
			const float nan = std::numeric_limits<float>::quiet_NaN();
			talk.Tracks.push_back({ .Node = Node(*source, "Head"), .Path = AnimationPath::Translation, .Times = { 0, 2 }, .Values = { glm::vec4(nan), glm::vec4(nan) } });
			scene.OnRuntimeStart();
			scene.GetAnimationSystem()->Update(0.5f);
			// RestoreRestPose shows the rest pose's weights, not Talk's (0.5 at 0.5 s would be the same for Smile; Blink is 0).
			CHECK((*scene.GetAnimationSystem()->GetMorphWeights(entity))[face + 1] == 0.25f);
		}
		SUBCASE("overflowing weights")
		{
			auto& weights = std::ranges::find(talk.Tracks, AnimationPath::Weights, &AnimationTrack::Path)->Weights;
			std::ranges::fill(weights, std::numeric_limits<float>::infinity());
			const auto since = Log::GetHistory().GetTotalCount();
			scene.OnRuntimeStart();
			scene.GetAnimationSystem()->Update(0.5f);
			CHECK((*scene.GetAnimationSystem()->GetMorphWeights(entity))[face] == 0.0f);
			CHECK(CountLogMessages(since, "non-finite morph weights (first 'Smile' of node 'Face') from clip 'Talk'; showing them as 0") == 1);
		}
		scene.OnRuntimeStop();
	}

	TEST_CASE("Static and edit-mode morph bounds and vertex snapping see the shown weights")
	{
		const auto source = AnimationTest::LoadModel(MorphPath());
		const Submesh& teeth = source->Submeshes[source->Meshes[1].Submeshes[0]];
		CHECK(MorphedBounds(*source, teeth, std::vector<float>{ 1.0f }).Max.x == doctest::Approx(0.2f));
		CHECK(MorphedBounds(*source, teeth, std::vector<float>{ 0.0f }).Max.x == doctest::Approx(0.1f));
		// A MeshComponent shows the mesh defaults: Blink 0.25 pushes the top row back by 0.0625.
		CHECK(MorphedMeshBounds(*source, 0).Min.z == doctest::Approx(-0.0625f));
		CHECK(MorphedMeshBounds(*source, 1).Max.x == doctest::Approx(0.1f));

		// Edit mode snaps to the node weights' surface: the centre vertex sits 0.15 in front (Smile 0.5).
		ClearAssetCache cacheGuard;
		Scene scene;
		auto entity = scene.CreateEntity("Face");
		entity.AddComponent<AnimatorComponent>().Mesh = MorphPath().string();
		const glm::mat4 viewProjection = glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 10.0f) * glm::lookAt(glm::vec3(0, 0.5f, 5), glm::vec3(0, 0.5f, 0), glm::vec3(0, 1, 0));
		const auto vertex = FindViewportVertex(entity, viewProjection, { 200, 200 }, { 100, 100 }, 4);
		REQUIRE(vertex);
		CHECK(vertex->World.z == doctest::Approx(0.15f));
	}

	TEST_CASE("Draws keep their strongest morph weights, in a stable order")
	{
		std::vector<uint32_t> targets;
		const float weights[] = { 0.1f, 0.0f, -0.5f, std::numeric_limits<float>::quiet_NaN(), 0.3f, 0.1f, -0.1f };
		CHECK(SelectMorphWeights(weights, 8, targets) == 0);
		CHECK(targets == std::vector<uint32_t>{ 2, 4, 0, 5, 6 });
		CHECK(SelectMorphWeights(weights, 3, targets) == 2);
		CHECK(targets == std::vector<uint32_t>{ 2, 4, 0 });
		CHECK(SelectMorphWeights({}, 3, targets) == 0);
		CHECK(targets.empty());
	}

	TEST_CASE("Script weights waiting for their target can be changed and cleared by name")
	{
		ClearAssetCache cacheGuard;
		Scene scene;
		auto entity = scene.CreateEntity("Face");
		auto& animator = entity.AddComponent<AnimatorComponent>();
		animator.Mesh = MorphPath().string();
		animator.Clip = "Nod"; // Leaves the weights at the node's defaults (Smile 0.5).
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.SetMorphWeight(entity, "Smile", 0.9f, 0));
		animator.Mesh = AnimationTest::FixturePath("TwoJoint.gltf").string();
		animator.Clip.clear();
		system.Update(0);
		// The waiting weight can change, and clears by name although the model lacks the target.
		CHECK(system.SetMorphWeight(entity, "Smile", 0.4f, 0));
		CHECK(system.ClearMorphWeight(entity, "Smile"));
		CHECK_FALSE(system.ClearMorphWeight(entity, "Smile"));
		animator.Mesh = MorphPath().string();
		animator.Clip = "Nod";
		system.Update(0);
		const auto source = AssetManager::GetMesh(animator.Mesh);
		CHECK((*system.GetMorphWeights(entity))[static_cast<size_t>(source->Nodes[Node(*source, "Face")].MorphWeightOffset)] == 0.5f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Script weights survive a model that briefly fails to load")
	{
		ClearAssetCache cacheGuard;
		TempDirectory directory("MorphBrokenReload");
		const auto data = MorphJson();
		const auto path = directory.GetPath() / "Face.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		Scene scene;
		auto entity = scene.CreateEntity("Face");
		entity.AddComponent<AnimatorComponent>().Mesh = path.string();
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.SetMorphWeight(entity, "Blink", 0.8f, 0));
		// A half-written file: the playback goes away with the model.
		REQUIRE(FileSystem::WriteTextFile(path, "{ \"asset\": "));
		AssetManager::Reload(path.string());
		system.Update(0);
		CHECK(system.GetMorphWeights(entity) == nullptr);
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		AssetManager::Reload(path.string());
		system.Update(0);
		const auto source = AssetManager::GetMesh(path.string());
		REQUIRE(source);
		CHECK((*system.GetMorphWeights(entity))[static_cast<size_t>(source->Nodes[Node(*source, "Face")].MorphWeightOffset) + 1] == doctest::Approx(0.8f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Import warns about clips emptied by dropped targets and node weights static geometry cannot show")
	{
		SUBCASE("a clip whose only channel moved dropped targets")
		{
			auto data = MorphJson();
			auto& targets = data["meshes"][1]["primitives"][0]["targets"];
			while (targets.size() <= MaxMorphTargets)
				targets.push_back(targets[0]);
			data["meshes"][1]["extras"].erase("targetNames");
			// A clip with only Teeth's weights, sized for the 257 targets cgltf sees.
			std::vector<float> values(2 * targets.size(), 0.5f);
			data["buffers"].push_back({ { "byteLength", values.size() * 4 }, { "uri", "data:application/octet-stream;base64," + Base64(values.data(), values.size() * 4) } });
			data["bufferViews"].push_back({ { "buffer", data["buffers"].size() - 1 }, { "byteLength", values.size() * 4 } });
			data["accessors"].push_back({ { "bufferView", data["bufferViews"].size() - 1 }, { "componentType", 5126 }, { "count", values.size() }, { "type", "SCALAR" } });
			data["animations"] = { { { "name", "Chatter" }, { "samplers", { { { "input", data["animations"][0]["samplers"][1]["input"] }, { "output", data["accessors"].size() - 1 } } } }, { "channels", { { { "sampler", 0 }, { "target", { { "node", 3 }, { "path", "weights" } } } } } } } };
			TempDirectory directory("MorphEmptiedClip");
			const auto since = Log::GetHistory().GetTotalCount();
			const auto source = AnimationTest::LoadModified(data, directory.GetPath(), "Emptied.gltf");
			CHECK(CountLogMessages(since, "clip 'Chatter' has no tracks left after skipping its morph weights, so it is dropped") == 1);
			CHECK(source->Animations.empty());
		}
		SUBCASE("node weights on a model with unsupported influence sets")
		{
			auto data = MorphJson();
			auto& attributes = data["meshes"][0]["primitives"][0]["attributes"];
			attributes["JOINTS_1"] = attributes["JOINTS_0"];
			attributes["WEIGHTS_1"] = attributes["WEIGHTS_0"];
			TempDirectory directory("MorphUnsupported");
			const auto since = Log::GetHistory().GetTotalCount();
			const auto source = AnimationTest::LoadModified(data, directory.GetPath(), "Unsupported.gltf");
			CHECK(source->AnimationUnsupported);
			CHECK(CountLogMessages(since, "node 'Face' has its own morph weights, which the static geometry cannot show") == 1);
			CHECK(CountLogMessages(since, "node 'Teeth' has its own morph weights") == 0);
		}
	}
}
