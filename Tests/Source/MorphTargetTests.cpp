#include <doctest/doctest.h>

#include "AnimationTestUtils.h"
#include "TestUtils.h"

#include <Basalt/Animation/AnimatedModel.h>
#include <Basalt/Asset/MeshOptimizer.h>
#include <Basalt/Core/Log.h>

#include <nlohmann/json.hpp>

#include <fstream>

using namespace Basalt;
using namespace BasaltTest;
using AnimationTest::ClearAssetCache;
using AnimationTest::Node;

namespace {
	std::filesystem::path MorphPath()
	{
		return std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/Morph.gltf";
	}

	nlohmann::json MorphJson()
	{
		std::ifstream stream(MorphPath());
		return nlohmann::json::parse(stream);
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

	const AnimationClip& Clip(const MeshSource& source, const char* name)
	{
		const auto clip = std::ranges::find(source.Animations, name, &AnimationClip::Name);
		REQUIRE_MESSAGE(clip != source.Animations.end(), name);
		return *clip;
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
			const auto path = directory.GetPath() / "Names.gltf";
			REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
			const auto renamed = AnimationTest::LoadModel(path);
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
			expected = "invalid";
		}
		SUBCASE("too many targets")
		{
			auto& targets = data["meshes"][1]["primitives"][0]["targets"];
			while (targets.size() <= MaxMorphTargets)
				targets.push_back(targets[0]);
			data["meshes"][1]["extras"].erase("targetNames");
			data.erase("animations");
			expected = "morph targets exceed the limit of 256";
		}
		SUBCASE("more deltas than a model may hold")
		{
			// Zero-filled sparse-less accessors keep the file small: 4 targets x 1048578 vertices pass MaxMorphDeltas.
			const nlohmann::json zeros = { { "componentType", 5126 }, { "count", 1048578 }, { "type", "VEC3" }, { "min", { 0, 0, 0 } }, { "max", { 0, 0, 0 } } };
			data["accessors"].push_back(zeros);
			const size_t index = data["accessors"].size() - 1;
			data["meshes"][1]["primitives"][0] = { { "attributes", { { "POSITION", index } } }, { "targets", { { { "POSITION", index } }, { { "POSITION", index } }, { { "POSITION", index } }, { { "POSITION", index } } } } };
			data["meshes"][1]["extras"].erase("targetNames");
			data["nodes"][3].erase("mesh");
			data.erase("animations");
			expected = "morph targets exceed the limit of 4194304 deltas per model";
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
		CHECK(system.ClearMorphWeight(entity, ""));
		CHECK((*system.GetMorphWeights(entity))[faceBlink] != 1.0f);

		const auto since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(system.SetMorphWeight(entity, "Frown", 1.0f, 0.0f));
		CHECK_FALSE(system.SetMorphWeight(entity, "Frown", 1.0f, 0.0f));
		CHECK(CountLogMessages(since, "no morph target named 'Frown'") == 1);
		CHECK_FALSE(system.SetMorphWeight(entity, "Smile", 1.5f, 0.0f));
		CHECK_FALSE(system.SetMorphWeight(entity, "Smile", 0.5f, -1.0f));
		CHECK_FALSE(system.GetMorphWeight(entity, "Frown").has_value());
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
}
