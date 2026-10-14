#include <doctest/doctest.h>

#include "TestUtils.h"

#include <Basalt/Animation/AnimatedModel.h>
#include <Basalt/Animation/AnimationPose.h>
#include <Basalt/Animation/AnimationSystem.h>
#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/MeshImporter.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/SceneSerializer.h>

#include <glm/gtc/epsilon.hpp>

#include <fstream>
#include <limits>

using namespace Basalt;
using namespace BasaltTest;

namespace {
	std::filesystem::path FixturePath()
	{
		return std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/TwoJoint.gltf";
	}
	Ref<MeshSource> LoadFixture()
	{
		std::string error;
		auto source = MeshImporter::LoadGltf(FixturePath(), FixturePath().string(), error);
		REQUIRE_MESSAGE(source, error);
		return source;
	}
	nlohmann::json FixtureJson()
	{
		std::ifstream stream(FixturePath());
		return nlohmann::json::parse(stream);
	}
	void ReplaceAccessorBytes(nlohmann::json& model, size_t accessor, const void* bytes, size_t size)
	{
		model["buffers"].push_back({ { "uri", "data:application/octet-stream;base64," + Base64(bytes, size) }, { "byteLength", size } });
		model["bufferViews"].push_back({ { "buffer", model["buffers"].size() - 1 }, { "byteLength", size } });
		model["accessors"][accessor]["bufferView"] = model["bufferViews"].size() - 1;
	}
	struct ClearAssetCache
	{
		~ClearAssetCache() { AssetManager::Clear(); }
	};
	Entity AddAnimator(Scene& scene, const std::string& name = "Rig")
	{
		auto entity = scene.CreateEntity(name);
		entity.AddComponent<AnimatorComponent>().Mesh = FixturePath().string();
		return entity;
	}
}

TEST_SUITE("Animation")
{
	TEST_CASE("Hierarchy finalization rejects invalid parents and cycles for asset builders")
	{
		MeshSource source;
		source.Nodes.resize(2);
		source.Nodes[0].Parent = 1;
		REQUIRE(source.FinalizeHierarchy());
		CHECK(source.NodeOrder == std::vector<uint32_t>{ 1, 0 });
		source.Nodes[1].Parent = 0;
		CHECK_FALSE(source.FinalizeHierarchy());
		source.Nodes[1].Parent = 2;
		CHECK_FALSE(source.FinalizeHierarchy());
	}
	TEST_CASE("Skin import preserves hierarchy, inverse binds, normalized influences and named clips")
	{
		auto source = LoadFixture();
		REQUIRE(source->Skins.size() == 1);
		REQUIRE(source->Skins[0].Joints.size() == 2);
		CHECK(source->Skins[0].Joints[0].Node == 2);
		CHECK(source->Skins[0].Joints[1].Node == 0);
		CHECK(source->Skins[0].Joints[1].InverseBind[3].y == -1);
		CHECK(source->NodeOrder == std::vector<uint32_t>{ 1, 2, 0 });
		REQUIRE(source->Animations.size() == 3);
		CHECK(source->Animations[0].Name == "Bend");
		CHECK(source->Animations[0].Duration == 2);
		CHECK(source->Vertices[2].Weights == glm::vec4(.5f, .5f, 0, 0));
		Scene scene;
		auto root = MeshImporter::InstantiateHierarchy(scene, *source, "Imported", {});
		CHECK(root.HasComponent<AnimatorComponent>());
		CHECK_FALSE(root.HasComponent<MeshComponent>());
		CHECK(root.GetComponent<AnimatorComponent>().Mesh == source->Key);
	}

	TEST_CASE("Rest pose and nonidentity mesh node obey glTF joint space")
	{
		auto source = LoadFixture();
		std::vector<glm::mat4> globals;
		EvaluateAnimation(*source, nullptr, 0, globals);
		CHECK(globals[1][3].x == 7);
		auto palette = BuildSkinPalette(source->Skins[0], globals);
		CHECK(palette[0] == glm::mat4(1));
		CHECK(palette[1] == glm::mat4(1));
		const auto bounds = SkinBounds(*source, source->Submeshes[0], palette);
		CHECK(bounds.Min == glm::vec3(-.25f, 0, 0));
		CHECK(bounds.Max == glm::vec3(.25f, 2, 0));
		EvaluateAnimation(*source, &source->Animations[0], 1, globals);
		palette = BuildSkinPalette(source->Skins[0], globals);
		const glm::vec3 tip = palette[1] * glm::vec4(0, 2, 0, 1);
		CHECK(glm::all(glm::epsilonEqual(tip, glm::vec3(-1, 1, 0), 1e-5f)));
		const auto bent = SkinBounds(*source, source->Submeshes[0], palette);
		CHECK(bent.Min.x == doctest::Approx(-1));
		CHECK(bent.Max.y == doctest::Approx(1.25));
	}

	TEST_CASE("Sampling handles step boundaries, quaternion slerp, cubic tangents and clamping")
	{
		auto source = LoadFixture();
		std::vector<glm::mat4> globals;
		EvaluateAnimation(*source, &source->Animations[1], .999f, globals);
		CHECK(globals[2][3].x == 0);
		EvaluateAnimation(*source, &source->Animations[1], 1, globals);
		CHECK(globals[2][3].x == 2);
		EvaluateAnimation(*source, &source->Animations[1], 20, globals);
		CHECK(globals[2][3].x == 0);
		EvaluateAnimation(*source, &source->Animations[2], .5f, globals);
		CHECK(globals[2][3].x == doctest::Approx(1.25));
		EvaluateAnimation(*source, &source->Animations[0], .5f, globals);
		CHECK(globals[0][0].x == doctest::Approx(std::sqrt(.5f)));
		// Equivalent antipodal quaternion must take the short arc.
		source->Animations[0].Tracks[0].Values[1] *= -1;
		EvaluateAnimation(*source, &source->Animations[0], .5f, globals);
		CHECK(globals[0][0].x == doctest::Approx(std::sqrt(.5f)));
	}

	TEST_CASE("Khronos Fox imports all clips and yields finite independently sampled skin palettes")
	{
		std::string error;
		const auto path = std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/Fox/Fox.glb";
		auto source = MeshImporter::LoadGltf(path, path.string(), error);
		REQUIRE_MESSAGE(source, error);
		REQUIRE(source->Animations.size() == 3);
		REQUIRE(source->Skins.size() == 1);
		CHECK(source->Animations[0].Name == "Survey");
		CHECK(source->Animations[1].Name == "Walk");
		CHECK(source->Animations[2].Name == "Run");
		for (const auto& clip : source->Animations)
		{
			CHECK(clip.Duration > 0);
			std::vector<glm::mat4> globals;
			REQUIRE(EvaluateAnimation(*source, &clip, clip.Duration * .37f, globals));
			const auto palette = BuildSkinPalette(source->Skins[0], globals);
			REQUIRE(palette.size() == source->Skins[0].Joints.size());
			for (const auto& matrix : palette)
				for (int c = 0; c < 4; c++)
					for (int r = 0; r < 4; r++)
						CHECK(std::isfinite(matrix[c][r]));
		}
	}

	TEST_CASE("Cubic translation uses both tangents scaled by a nonunit key interval")
	{
		auto source = LoadFixture();
		auto& clip = source->Animations[2];
		auto& track = clip.Tracks[0];
		track.Times = { 2, 6 };
		track.Values = { glm::vec4(0), { 3, 0, 0, 0 }, { 2, 0, 0, 0 }, { -1, 0, 0, 0 }, { 7, 0, 0, 0 }, glm::vec4(0) };
		std::vector<glm::mat4> globals;
		REQUIRE(EvaluateAnimation(*source, &clip, 3, globals));
		CHECK(globals[2][3].x == doctest::Approx(4.9375));
		REQUIRE(EvaluateAnimation(*source, &clip, 5, globals));
		CHECK(globals[2][3].x == doctest::Approx(7.3125));
	}

	TEST_CASE("Local pose blending preserves hierarchy and crossfades advance both clips without interruption jumps")
	{
		Scene scene;
		auto entity = AddAnimator(scene);
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.Play(entity, "Step"));
		REQUIRE(system.Seek(entity, .75f));
		REQUIRE(system.CrossFade(entity, "Cubic", 1));
		CHECK((*system.GetPose(entity))[2][3].x == 0);
		system.Update(.5f);
		CHECK(system.GetTime(entity) == .5f);
		CHECK((*system.GetPose(entity))[2][3].x == doctest::Approx(1.625));
		const auto before = *system.GetPose(entity);
		REQUIRE(system.CrossFade(entity, "Bend", 1));
		const auto after = *system.GetPose(entity);
		for (size_t i = 0; i < before.size(); i++)
			for (int c = 0; c < 4; c++)
				CHECK(glm::all(glm::epsilonEqual(before[i][c], after[i][c], 1e-5f)));
		system.Update(1);
		CHECK((*system.GetPose(entity))[2][3].x == 0);
		CHECK_FALSE(system.CrossFade(entity, "Bend", -1));
		CHECK_FALSE(system.Blend(entity, "Step", "Cubic", 1.1f));
		REQUIRE(system.Blend(entity, "Step", "Cubic", .25f));
		system.Update(.5f);
		CHECK((*system.GetPose(entity))[2][3].x == doctest::Approx(.3125));
		REQUIRE(system.Blend(entity, "Step", "Cubic", .75f));
		CHECK(system.GetTime(entity) == .5f);
		CHECK((*system.GetPose(entity))[2][3].x == doctest::Approx(.9375));
		system.Pause(entity, true);
		system.Update(1);
		CHECK(system.GetTime(entity) == .5f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Malformed and unsupported animation data fails import explicitly")
	{
		TempDirectory directory("BadAnimation");
		auto data = FixtureJson();
		std::string expected;
		SUBCASE("wrong joint component type")
		{
			data["accessors"][3]["componentType"] = 5120;
			expected = "invalid joint/weight accessor";
		}
		SUBCASE("missing weights")
		{
			data["meshes"][0]["primitives"][0]["attributes"].erase("WEIGHTS_0");
			expected = "invalid joint/weight accessor";
		}
		SUBCASE("joint outside skin")
		{
			data["skins"][0]["joints"] = { 2 };
			data["skins"][0].erase("inverseBindMatrices");
			expected = "vertex joint index exceeds skin joint count";
		}
		SUBCASE("duplicate joint")
		{
			data["skins"][0]["joints"] = { 2, 2 };
			expected = "duplicate skin joint";
		}
		SUBCASE("wrong inverse bind type")
		{
			data["accessors"][6]["type"] = "VEC4";
			expected = "invalid inverse bind matrices";
		}
		SUBCASE("wrong output count")
		{
			data["accessors"][8]["count"] = 2;
			// cgltf rejects count mismatches before the Basalt animation importer runs.
			expected = "validation failed";
		}
		SUBCASE("unbounded zero-initialized times")
		{
			const auto animation = data["animations"][0];
			data["animations"] = { animation };
			data["accessors"][7].erase("bufferView");
			data["accessors"][7]["count"] = 1000000000;
			data["accessors"][8].erase("bufferView");
			data["accessors"][8]["count"] = 1000000000;
			expected = "decoded animation budget exceeded";
		}
		SUBCASE("wrong time type")
		{
			const uint16_t times[] = { 0, 1, 2 };
			data["buffers"].push_back({ { "uri", "data:application/octet-stream;base64," + Base64(times, sizeof(times)) }, { "byteLength", sizeof(times) } });
			data["bufferViews"].push_back({ { "buffer", data["buffers"].size() - 1 }, { "byteLength", sizeof(times) } });
			data["accessors"][7]["bufferView"] = data["bufferViews"].size() - 1;
			data["accessors"][7]["componentType"] = 5123;
			expected = "invalid animation times";
		}
		SUBCASE("duplicate target")
		{
			data["animations"][0]["channels"].push_back(data["animations"][0]["channels"][0]);
			expected = "duplicate animation target";
		}
		SUBCASE("matrix animated node")
		{
			data["nodes"][0]["matrix"] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
			expected = "animated node must use TRS";
		}
		SUBCASE("cyclic hierarchy")
		{
			data.erase("scenes");
			data.erase("scene");
			data["nodes"][0]["children"] = { 2 };
			// Parent cycles are likewise rejected at the cgltf validation boundary.
			expected = "validation failed";
		}
		std::string error;
		const auto path = directory.GetPath() / "Bad.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		CHECK_FALSE(MeshImporter::LoadGltf(path, "Bad.gltf", error));
		REQUIRE_FALSE(expected.empty());
		CHECK_MESSAGE(error.find(expected) != std::string::npos, error);
	}

	TEST_CASE("Inverse bind accessors may be shared prefixes and absent binds default to identity")
	{
		TempDirectory directory("BindAccessors");
		auto data = FixtureJson();
		SUBCASE("absent matrices")
		{
			data["skins"][0].erase("inverseBindMatrices");
		}
		SUBCASE("shared larger accessor")
		{
			data["accessors"][6].erase("bufferView");
			data["accessors"][6]["count"] = 3;
			std::vector<float> matrices(48, 0);
			for (size_t i = 0; i < 3; i++)
				for (size_t c = 0; c < 4; c++)
					matrices[i * 16 + c * 5] = 1;
			const std::string bytes(reinterpret_cast<const char*>(matrices.data()), matrices.size() * sizeof(float));
			REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Matrices.bin", bytes));
			data["buffers"].push_back({ { "uri", "Matrices.bin" }, { "byteLength", bytes.size() } });
			data["bufferViews"].push_back({ { "buffer", 1 }, { "byteLength", bytes.size() } });
			data["accessors"][6]["bufferView"] = data["bufferViews"].size() - 1;
		}
		const auto path = directory.GetPath() / "Model.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		std::string error;
		const auto source = MeshImporter::LoadGltf(path, path.string(), error);
		REQUIRE_MESSAGE(source, error);
		REQUIRE(source->Skins[0].Joints.size() == 2);
		CHECK(source->Skins[0].Joints[1].InverseBind == glm::mat4(1));
	}

	TEST_CASE("Malformed skin values transforms and allocation limits report specific causes")
	{
		TempDirectory directory("AnimationRejections");
		auto data = FixtureJson();
		std::string expected;
		SUBCASE("zero-sum weights")
		{
			const std::vector<float> weights(24, 0);
			ReplaceAccessorBytes(data, 4, weights.data(), weights.size() * sizeof(float));
			expected = "positive finite sum";
		}
		SUBCASE("negative weights")
		{
			std::vector<float> weights(24, 0);
			weights[0] = -1;
			ReplaceAccessorBytes(data, 4, weights.data(), weights.size() * sizeof(float));
			expected = "finite and nonnegative";
		}
		SUBCASE("non-affine inverse bind")
		{
			glm::mat4 matrices[] = { glm::mat4(1), glm::mat4(1) };
			matrices[0][3].w = 2;
			ReplaceAccessorBytes(data, 6, matrices, sizeof(matrices));
			expected = "inverse bind matrix must be affine";
		}
		SUBCASE("too few inverse binds")
		{
			data["accessors"][6]["count"] = 1;
			expected = "invalid inverse bind matrices";
		}
		SUBCASE("sheared node matrix")
		{
			data["nodes"][1].erase("translation");
			data["nodes"][1]["matrix"] = { 1, 0, 0, 0, .5, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
			expected = "node matrix cannot be decomposed";
		}
		SUBCASE("zero-length animation rotation")
		{
			const float rotations[] = { 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1 };
			ReplaceAccessorBytes(data, 8, rotations, sizeof(rotations));
			expected = "invalid animation quaternion";
		}
		SUBCASE("non-increasing times")
		{
			const float times[] = { 0, 0, 2 };
			ReplaceAccessorBytes(data, 7, times, sizeof(times));
			expected = "nonnegative and strictly increasing";
		}
		SUBCASE("joint limit")
		{
			data["skins"][0].erase("inverseBindMatrices");
			data["skins"][0]["joints"] = std::vector<int>(65537, 0);
			expected = "skin must contain 1..65536 joints";
		}
		SUBCASE("skin limit")
		{
			const auto skin = data["skins"][0];
			data["skins"] = std::vector<nlohmann::json>(4097, skin);
			expected = "exceeds model limits";
		}
		SUBCASE("clip limit")
		{
			const auto clip = data["animations"][0];
			data["animations"] = std::vector<nlohmann::json>(4097, clip);
			expected = "exceeds model limits";
		}
		SUBCASE("animated node limit")
		{
			while (data["nodes"].size() <= 65536)
				data["nodes"].push_back(nlohmann::json::object());
			expected = "exceeds model limits";
		}
		const auto path = directory.GetPath() / "Bad.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		std::string error;
		CHECK_FALSE(MeshImporter::LoadGltf(path, path.string(), error));
		CHECK_MESSAGE(error.find(expected) != std::string::npos, error);
	}

	TEST_CASE("Unnamed and duplicate clip names are stable public selection keys")
	{
		TempDirectory directory("AnimationNames");
		auto data = FixtureJson();
		data["animations"][0].erase("name");
		data["animations"][1]["name"] = "Same";
		data["animations"][2]["name"] = "Same";
		const auto path = directory.GetPath() / "Names.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		Scene scene;
		auto entity = AddAnimator(scene);
		entity.GetComponent<AnimatorComponent>().Mesh = path.string();
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		CHECK(system.GetClips(entity) == std::vector<std::string>{ "Animation0", "Same", "Same_1" });
		REQUIRE(system.Play(entity, "Same_1"));
		REQUIRE(system.Seek(entity, .5f));
		CHECK((*system.GetPose(entity))[2][3].x == doctest::Approx(1.25));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Unsupported animation channels and extra influences preserve static MeshComponent imports")
	{
		TempDirectory directory("StaticCompatibility");
		auto data = FixtureJson();
		std::string warning;
		SUBCASE("morph-weight channel")
		{
			data["animations"][0]["channels"][0]["target"] = { { "node", 1 }, { "path", "weights" } };
			data["meshes"][0]["primitives"][0]["targets"] = { { { "POSITION", 0 } } };
			data["accessors"][8]["type"] = "SCALAR";
			warning = "skipping unsupported morph-weight";
		}
		SUBCASE("pointer channel without a node")
		{
			data["animations"][0]["channels"][0]["target"].erase("node");
			warning = "skipping unsupported pointer";
		}
		SUBCASE("extra influences")
		{
			data["meshes"][0]["primitives"][0]["attributes"]["JOINTS_1"] = 3;
			data["meshes"][0]["primitives"][0]["attributes"]["WEIGHTS_1"] = 4;
			warning = "retaining static geometry only";
		}
		SUBCASE("large static hierarchy")
		{
			data.erase("animations");
			data.erase("skins");
			data["nodes"][1].erase("skin");
			while (data["nodes"].size() <= 65536)
				data["nodes"].push_back(nlohmann::json::object());
		}
		const auto path = directory.GetPath() / "Model.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		ClearAssetCache cacheGuard;
		const auto since = Log::GetHistory().GetTotalCount();
		Scene scene;
		auto entity = scene.CreateEntity("Static");
		entity.AddComponent<MeshComponent>().Mesh = path.string();
		const auto source = AssetManager::GetMesh(entity.GetComponent<MeshComponent>().Mesh);
		REQUIRE_MESSAGE(source, AssetManager::GetError(path.string()));
		CHECK(source->Submeshes.size() == 1);
		CHECK(source->Vertices.size() == 6);
		if (!warning.empty())
			CHECK(CountLogMessages(since, warning) > 0);
		if (source->AnimationUnsupported)
		{
			const auto root = MeshImporter::InstantiateHierarchy(scene, *source, "Imported", {});
			CHECK_FALSE(root.HasComponent<AnimatorComponent>());
			entity.AddComponent<AnimatorComponent>().Mesh = path.string();
			scene.OnRuntimeStart();
			CHECK_FALSE(scene.GetAnimationSystem()->Play(entity, "Bend"));
			scene.OnRuntimeStop();
		}
	}

	TEST_CASE("Normalized integer rotation accessors import and interpolate")
	{
		TempDirectory directory("QuantizedRotations");
		auto data = FixtureJson();
		SUBCASE("signed short")
		{
			const int16_t values[] = { 0, 0, 0, 32767, 0, 0, 23170, 23170, 0, 0, 0, 32767 };
			ReplaceAccessorBytes(data, 8, values, sizeof(values));
			data["accessors"][8]["componentType"] = 5122;
		}
		SUBCASE("unsigned short")
		{
			const uint16_t values[] = { 0, 0, 0, 65535, 0, 0, 46340, 46340, 0, 0, 0, 65535 };
			ReplaceAccessorBytes(data, 8, values, sizeof(values));
			data["accessors"][8]["componentType"] = 5123;
		}
		SUBCASE("signed byte")
		{
			const int8_t values[] = { 0, 0, 0, 127, 0, 0, 90, 90, 0, 0, 0, 127 };
			ReplaceAccessorBytes(data, 8, values, sizeof(values));
			data["accessors"][8]["componentType"] = 5120;
		}
		SUBCASE("unsigned byte")
		{
			const uint8_t values[] = { 0, 0, 0, 255, 0, 0, 180, 180, 0, 0, 0, 255 };
			ReplaceAccessorBytes(data, 8, values, sizeof(values));
			data["accessors"][8]["componentType"] = 5121;
		}
		data["accessors"][8]["normalized"] = true;
		const auto path = directory.GetPath() / "Model.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		std::string error;
		const auto source = MeshImporter::LoadGltf(path, path.string(), error);
		REQUIRE_MESSAGE(source, error);
		std::vector<glm::mat4> globals;
		REQUIRE(EvaluateAnimation(*source, &source->Animations[0], .5f, globals));
		CHECK(globals[0][0].x == doctest::Approx(std::sqrt(.5f)));
		REQUIRE(EvaluateAnimation(*source, &source->Animations[0], 1, globals));
		CHECK(globals[0][0].y == doctest::Approx(1));
	}

	TEST_CASE("Zero-scale node matrices preserve authored transforms in animated and static imports")
	{
		TempDirectory directory("ZeroScaleMatrix");
		for (bool animated : { false, true })
			for (const auto& scale : { glm::vec3(0, 2, 3), glm::vec3(2, 0, 3), glm::vec3(2, 3, 0), glm::vec3(0, 0, 3), glm::vec3(0, 2, 0), glm::vec3(2, 0, 0), glm::vec3(0), glm::vec3(-2, 3, 4), glm::vec3(1e-21f) })
			{
				INFO("animated=" << animated << " scale=" << scale.x << "," << scale.y << "," << scale.z);
				auto data = FixtureJson();
				const auto matrix = Math::ComposeTransform({ 4, 5, 6 }, glm::quat(glm::radians(glm::vec3(30, 50, 80))), scale);
				nlohmann::json values = nlohmann::json::array();
				for (int c = 0; c < 4; c++)
					for (int r = 0; r < 4; r++)
						values.push_back(matrix[c][r]);
				data["nodes"][1].erase("translation");
				data["nodes"][1]["matrix"] = values;
				if (!animated)
				{
					data.erase("animations");
					data.erase("skins");
					data["nodes"][1].erase("skin");
				}
				const auto path = directory.GetPath() / "Model.gltf";
				REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
				std::string error;
				const auto source = MeshImporter::LoadGltf(path, path.string(), error);
				REQUIRE_MESSAGE(source, error);
				std::vector<glm::mat4> globals;
				REQUIRE(EvaluateAnimation(*source, nullptr, 0, globals));
				for (int c = 0; c < 4; c++)
					CHECK(globals[1][c] == matrix[c]);
				if (!animated)
				{
					Scene scene;
					MeshImporter::InstantiateHierarchy(scene, *source, "Model", {});
					const auto mesh = scene.FindEntityByName("Mesh");
					REQUIRE(mesh);
					const auto world = scene.GetWorldTransform(mesh);
					for (int c = 0; c < 4; c++)
						CHECK(glm::all(glm::epsilonEqual(world[c], matrix[c], 1e-5f)));
				}
			}
	}

	TEST_CASE("Normalized byte and short weights and sparse weight accessors preserve influences")
	{
		TempDirectory directory("WeightAccessors");
		auto data = FixtureJson();
		std::string bytes;
		auto& accessor = data["accessors"][4];
		SUBCASE("normalized bytes")
		{
			const uint8_t weights[] = { 255, 0, 0, 0, 255, 0, 0, 0, 128, 128, 0, 0, 128, 128, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0 };
			bytes.assign(reinterpret_cast<const char*>(weights), sizeof(weights));
			accessor["componentType"] = 5121;
			accessor["normalized"] = true;
		}
		SUBCASE("normalized shorts")
		{
			const uint16_t weights[] = { 65535, 0, 0, 0, 65535, 0, 0, 0, 32768, 32768, 0, 0, 32768, 32768, 0, 0, 0, 65535, 0, 0, 0, 65535, 0, 0 };
			bytes.assign(reinterpret_cast<const char*>(weights), sizeof(weights));
			accessor["componentType"] = 5123;
			accessor["normalized"] = true;
		}
		SUBCASE("sparse without base buffer")
		{
			const uint8_t indices[] = { 0, 1, 2, 3, 4, 5, 0, 0 };
			const float weights[] = { 1, 0, 0, 0, 1, 0, 0, 0, .5f, .5f, 0, 0, .5f, .5f, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0 };
			bytes.assign(reinterpret_cast<const char*>(indices), sizeof(indices));
			bytes.append(reinterpret_cast<const char*>(weights), sizeof(weights));
			accessor.erase("bufferView");
			const auto view = data["bufferViews"].size();
			accessor["sparse"] = { { "count", 6 }, { "indices", { { "bufferView", view }, { "componentType", 5121 } } }, { "values", { { "bufferView", view }, { "byteOffset", 8 } } } };
		}
		data["buffers"].push_back({ { "uri", "Weights.bin" }, { "byteLength", bytes.size() } });
		data["bufferViews"].push_back({ { "buffer", 1 }, { "byteLength", bytes.size() } });
		if (!accessor.contains("sparse"))
			accessor["bufferView"] = data["bufferViews"].size() - 1;
		REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Weights.bin", bytes));
		const auto path = directory.GetPath() / "Model.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		std::string error;
		const auto source = MeshImporter::LoadGltf(path, path.string(), error);
		REQUIRE_MESSAGE(source, error);
		CHECK(source->Vertices[2].Weights == glm::vec4(.5f, .5f, 0, 0));
		CHECK(source->Vertices[4].Weights == glm::vec4(0, 1, 0, 0));
	}

	TEST_CASE("Implicit inverse binds consume the aggregate decoded animation budget")
	{
		TempDirectory directory("SkinBudget");
		auto data = FixtureJson();
		data.erase("animations");
		data["skins"][0].erase("inverseBindMatrices");
		while (data["nodes"].size() < 4096)
			data["nodes"].push_back(nlohmann::json::object());
		data["skins"][0]["joints"] = nlohmann::json::array();
		for (int i = 0; i < 4096; i++)
			data["skins"][0]["joints"].push_back(i);
		const auto skin = data["skins"][0];
		while (data["skins"].size() < 257)
			data["skins"].push_back(skin);
		const auto path = directory.GetPath() / "Model.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		std::string error;
		CHECK_FALSE(MeshImporter::LoadGltf(path, path.string(), error));
		CHECK(error.find("budget") != std::string::npos);
	}

	TEST_CASE("Overflowing animation poses fall back to a finite rest pose and recover")
	{
		ClearAssetCache cacheGuard;
		Scene scene;
		auto entity = AddAnimator(scene);
		scene.OnRuntimeStart();
		auto source = AssetManager::GetMesh(FixturePath().string());
		REQUIRE(source);
		AnimationTrack scale;
		scale.Node = 2;
		scale.Path = AnimationPath::Scale;
		scale.Times = { 0 };
		scale.Values = { glm::vec4(std::numeric_limits<float>::max()) };
		source->Animations[0].Tracks.push_back(scale);
		scale.Node = 0;
		source->Animations[0].Tracks.push_back(scale);
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.Seek(entity, 1));
		CHECK((*system.GetPose(entity))[2][0].x == 1);
		source->Animations[0].Tracks.pop_back();
		source->Animations[0].Tracks.pop_back();
		REQUIRE(system.Seek(entity, 1));
		CHECK((*system.GetPose(entity))[0][0].x == doctest::Approx(0).epsilon(1e-5));
		scene.OnRuntimeStop();
		AssetManager::Reload(FixturePath().string());
	}

	TEST_CASE("Repeated mesh nodes cannot amplify a shared skin beyond the model work budget")
	{
		TempDirectory directory("SkinReferences");
		auto data = FixtureJson();
		data.erase("animations");
		data["skins"][0].erase("inverseBindMatrices");
		while (data["nodes"].size() < 4096)
			data["nodes"].push_back(nlohmann::json::object());
		data["skins"][0]["joints"] = nlohmann::json::array();
		for (int i = 0; i < 4096; i++)
			data["skins"][0]["joints"].push_back(i);
		int meshes = 16;
		SUBCASE("exactly at the work budget") {}
		SUBCASE("one mesh above the work budget")
		{
			meshes = 17;
		}
		const auto meshNode = data["nodes"][1];
		for (int i = 1; i < meshes; i++)
		{
			data["scenes"][0]["nodes"].push_back(data["nodes"].size());
			data["nodes"].push_back(meshNode);
		}
		const auto path = directory.GetPath() / "Repeated.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		std::string error;
		const auto source = MeshImporter::LoadGltf(path, path.string(), error);
		if (meshes == 16)
			REQUIRE_MESSAGE(source, error);
		else
		{
			CHECK_FALSE(source);
			CHECK(error.find("skin joint references across active mesh nodes") != std::string::npos);
		}
	}

	TEST_CASE("Unknown crossfade and blend clips are logged without changing playback")
	{
		Scene scene;
		auto entity = AddAnimator(scene);
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.Seek(entity, .75f));
		const auto before = *system.GetPose(entity);
		const auto since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(system.CrossFade(entity, "MissingFade", 1));
		CHECK_FALSE(system.Blend(entity, "MissingFirst", "Bend", .5f));
		CHECK_FALSE(system.Blend(entity, "Bend", "MissingSecond", .5f));
		CHECK(CountLogMessages(since, "crossfade clip 'MissingFade' not found") == 1);
		CHECK(CountLogMessages(since, "blend clip 'MissingFirst' not found") == 1);
		CHECK(CountLogMessages(since, "blend clip 'MissingSecond' not found") == 1);
		CHECK(system.GetTime(entity) == .75f);
		CHECK(system.IsPlaying(entity));
		CHECK(*system.GetPose(entity) == before);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Playback clip indices survive asset clip storage reallocation")
	{
		ClearAssetCache cacheGuard;
		Scene scene;
		auto entity = AddAnimator(scene);
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.Play(entity, "Step"));
		REQUIRE(system.Seek(entity, .75f));
		REQUIRE(system.CrossFade(entity, "Bend", 1));
		auto source = AssetManager::GetMesh(FixturePath().string());
		REQUIRE(source);
		source->Animations.reserve(source->Animations.capacity() + 1);
		system.Update(.5f);
		std::vector<glm::mat4> expected;
		auto target = SampleAnimation(*source, &source->Animations[0], .5f);
		BlendAnimationPose(target, SampleAnimation(*source, &source->Animations[1], 1.25f), .5f);
		REQUIRE(ComposeAnimationPose(*source, target, expected));
		CHECK(*system.GetPose(entity) == expected);
		CHECK(system.GetTime(entity) == .5f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Invalid playback numbers log reasons without changing the visible pose")
	{
		Scene scene;
		auto entity = AddAnimator(scene);
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.Seek(entity, .75f));
		const auto before = *system.GetPose(entity);
		const auto since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(system.CrossFade(entity, "Bend", -1));
		CHECK_FALSE(system.Blend(entity, "Step", "Bend", 1.0001f));
		CHECK_FALSE(system.Seek(entity, std::numeric_limits<float>::quiet_NaN()));
		CHECK(CountLogMessages(since, "seconds must be finite and nonnegative") == 2);
		CHECK(CountLogMessages(since, "blend weight must be finite and in [0, 1]") == 1);
		CHECK(system.GetTime(entity) == .75f);
		CHECK(*system.GetPose(entity) == before);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Playback is independent and handles pause, seek, looping, speed and completion")
	{
		Scene scene;
		auto a = AddAnimator(scene);
		auto b = AddAnimator(scene, "Other");
		b.GetComponent<AnimatorComponent>().PlayOnStart = false;
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		CHECK(system.IsPlaying(a));
		CHECK_FALSE(system.IsPlaying(b));
		system.Update(.5f);
		CHECK(system.GetTime(a) == .5f);
		CHECK(system.GetTime(b) == 0);
		system.Pause(a, true);
		system.Update(1);
		CHECK(system.GetTime(a) == .5f);
		CHECK(system.Seek(a, 1));
		CHECK_FALSE(system.Seek(a, std::numeric_limits<float>::infinity()));
		CHECK(system.GetTime(a) == 1);
		system.Pause(a, false);
		a.GetComponent<AnimatorComponent>().Speed = 2;
		system.Update(.75f);
		CHECK(system.GetTime(a) == .5f);
		a.GetComponent<AnimatorComponent>().Looping = false;
		system.Update(1);
		CHECK(system.GetTime(a) == 2);
		CHECK_FALSE(system.IsPlaying(a));
		CHECK_FALSE(system.Play(a, "Missing"));
		CHECK(system.GetTime(a) == 2);
		CHECK(system.Play(a, "Step"));
		system.Stop(a);
		CHECK(system.GetTime(a) == 0);
		CHECK((*system.GetPose(a))[2][3].x == 0);
		scene.OnRuntimeStop();
		CHECK(scene.GetAnimationSystem() == nullptr);
	}

	TEST_CASE("Animation follows scene pause and resets on removal, reload and play copies")
	{
		Scene scene;
		auto entity = AddAnimator(scene);
		scene.OnSimulationStart();
		scene.OnUpdate(.25f);
		CHECK(scene.GetAnimationSystem()->GetTime(entity) == .25f);
		scene.SetPaused(true);
		scene.OnUpdate(.25f);
		CHECK(scene.GetAnimationSystem()->GetTime(entity) == .25f);
		scene.Step();
		scene.OnUpdate(.25f);
		CHECK(scene.GetAnimationSystem()->GetTime(entity) == .5f);
		entity.RemoveComponent<AnimatorComponent>();
		entity.AddComponent<AnimatorComponent>().Mesh = FixturePath().string();
		CHECK(scene.GetAnimationSystem()->GetTime(entity) == 0);
		scene.GetAnimationSystem()->Seek(entity, 1);
		AssetManager::Reload(FixturePath().string());
		CHECK(scene.GetAnimationSystem()->GetTime(entity) == 0);
		const auto uuid = entity.GetUUID();
		scene.DestroyEntity(entity);
		CHECK_FALSE(scene.GetEntityByUUID(uuid));
		auto replacement = scene.CreateEntityWithUUID(uuid, "Replacement");
		replacement.AddComponent<AnimatorComponent>().Mesh = FixturePath().string();
		CHECK(scene.GetAnimationSystem()->GetTime(replacement) == 0);
		scene.OnSimulationStop();
		auto original = CreateRef<Scene>();
		auto rig = AddAnimator(*original);
		auto copy = Scene::Copy(original);
		copy->OnRuntimeStart();
		const auto copiedRig = copy->GetEntityByUUID(rig.GetUUID());
		REQUIRE(copiedRig.HasComponent<AnimatorComponent>());
		CHECK(copy->GetAnimationSystem()->GetTime(copiedRig) == 0);
		copy->OnUpdate(.5f);
		CHECK(original->GetAnimationSystem() == nullptr);
		CHECK(copy->GetAnimationSystem()->GetTime(copiedRig) == .5f);
		CHECK((*copy->GetAnimationSystem()->GetPose(copiedRig))[0][0].x == doctest::Approx(std::sqrt(.5f)));
		copy->OnRuntimeStop();
		copy->OnRuntimeStart();
		CHECK(copy->GetAnimationSystem()->GetTime(copiedRig) == 0);
		copy->OnRuntimeStop();
	}

	TEST_CASE("Stop restores authored rest transforms rather than the first animation frame")
	{
		TempDirectory directory("AnimationStop");
		auto data = FixtureJson();
		data["nodes"][2]["translation"] = { 3, 0, 0 };
		const auto path = directory.GetPath() / "Model.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		Scene scene;
		auto entity = AddAnimator(scene);
		entity.GetComponent<AnimatorComponent>().Mesh = path.string();
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.Play(entity, "Step"));
		CHECK((*system.GetPose(entity))[2][3].x == 0);
		REQUIRE(system.Seek(entity, 1));
		CHECK((*system.GetPose(entity))[2][3].x == 2);
		system.Stop(entity);
		CHECK_FALSE(system.IsPlaying(entity));
		CHECK(system.GetTime(entity) == 0);
		CHECK((*system.GetPose(entity))[2][3].x == 3);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Configuration changes replace playback and missing or clipless assets recover")
	{
		TempDirectory directory("AnimationConfiguration");
		auto data = FixtureJson();
		data.erase("animations");
		const auto clipless = directory.GetPath() / "Clipless.gltf";
		REQUIRE(FileSystem::WriteTextFile(clipless, data.dump()));
		Scene scene;
		auto entity = AddAnimator(scene);
		auto& component = entity.GetComponent<AnimatorComponent>();
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.Seek(entity, 1));
		component.Clip = "Step";
		CHECK(system.GetTime(entity) == 0);
		REQUIRE(system.Seek(entity, 1));
		CHECK((*system.GetPose(entity))[2][3].x == 2);
		component.Mesh = clipless.string();
		component.Clip.clear();
		CHECK(system.GetClips(entity).empty());
		CHECK_FALSE(system.IsPlaying(entity));
		CHECK_FALSE(system.Play(entity, ""));
		CHECK_FALSE(system.Seek(entity, 1));
		CHECK(system.GetDuration(entity) == 0);
		REQUIRE(system.GetPose(entity));
		CHECK((*system.GetPose(entity))[2][3].x == 0);
		component.Mesh.clear();
		const auto since = Log::GetHistory().GetTotalCount();
		CHECK(system.GetPose(entity) == nullptr);
		CHECK(system.GetPose(entity) == nullptr);
		CHECK(CountLogMessages(since, "Mesh is empty") == 1);
		component.Mesh = (directory.GetPath() / "Missing.gltf").string();
		CHECK(system.GetPose(entity) == nullptr);
		CHECK_FALSE(system.IsPlaying(entity));
		CHECK_FALSE(system.Play(entity, "Bend"));
		CHECK(system.GetClips(entity).empty());
		component.Mesh = FixturePath().string();
		component.PlayOnStart = false;
		CHECK(system.GetTime(entity) == 0);
		CHECK_FALSE(system.IsPlaying(entity));
		REQUIRE(system.Play(entity, "Bend"));
		REQUIRE(system.Seek(entity, 1));
		CHECK((*system.GetPose(entity))[0][0].x == doctest::Approx(0).epsilon(1e-5));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Persistent animator playback survives Lua scene loads while new animators start fresh")
	{
		TempProject project("AnimationSceneLoad");
		REQUIRE(project.IsValid());
		const auto mesh = project.WriteFile("Assets/Models/Rig.gltf", FixtureJson().dump());
		Scene destination;
		auto fresh = AddAnimator(destination, "Fresh");
		fresh.GetComponent<AnimatorComponent>().Mesh = mesh;
		std::string error;
		REQUIRE(SceneSerializer::SaveScene(destination, project.GetDirectory() / "Assets/Scenes/Next.bscene", error));
		Scene scene;
		auto entity = AddAnimator(scene);
		entity.GetComponent<AnimatorComponent>().Mesh = mesh;
		scene.OnRuntimeStart();
		REQUIRE(scene.SetPersistent(entity));
		REQUIRE(scene.GetAnimationSystem()->Play(entity, "Step"));
		REQUIRE(scene.GetAnimationSystem()->Seek(entity, .75f));
		CHECK(Eval(scene, "Scene.Load('Assets/Scenes/Next.bscene')") == "");
		scene.OnUpdate(.5f);
		CHECK(scene.GetPath() == "Assets/Scenes/Next.bscene");
		REQUIRE(scene.GetEntityByUUID(entity.GetUUID()));
		CHECK(scene.GetAnimationSystem()->GetTime(entity) == 1.25f);
		CHECK((*scene.GetAnimationSystem()->GetPose(entity))[2][3].x == 2);
		auto loaded = scene.FindEntityByName("Fresh");
		REQUIRE(loaded);
		CHECK(scene.GetAnimationSystem()->GetTime(loaded) == 0);
		scene.OnUpdate(.25f);
		CHECK(scene.GetAnimationSystem()->GetTime(entity) == 1.5f);
		CHECK(scene.GetAnimationSystem()->GetTime(loaded) == .25f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Animated model bounds include the current skin pose and parent world transform")
	{
		Scene scene;
		auto parent = scene.CreateEntity("Parent");
		parent.GetComponent<TransformComponent>().Translation = { 10, 20, 30 };
		auto entity = AddAnimator(scene);
		entity.GetComponent<TransformComponent>().Scale = { 2, 3, 4 };
		REQUIRE(scene.SetParent(entity, parent, false));
		const auto rest = AnimatedModelBounds(entity);
		CHECK(rest.Min == glm::vec3(9.5f, 20, 30));
		CHECK(rest.Max == glm::vec3(10.5f, 26, 30));
		scene.OnRuntimeStart();
		REQUIRE(scene.GetAnimationSystem()->Seek(entity, 1));
		const auto bent = AnimatedModelBounds(entity);
		CHECK(glm::all(glm::epsilonEqual(bent.Min, glm::vec3(8, 20, 30), 1e-5f)));
		CHECK(glm::all(glm::epsilonEqual(bent.Max, glm::vec3(10.5f, 23.75f, 30), 1e-5f)));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Unskinned animated mesh nodes import as an Animator and move their world bounds")
	{
		TempDirectory directory("RigidAnimation");
		auto data = FixtureJson();
		data.erase("skins");
		data["nodes"][1].erase("skin");
		const auto step = data["animations"][1];
		data["animations"] = { step };
		data["animations"][0]["channels"][0]["target"]["node"] = 1;
		const auto path = directory.GetPath() / "Rigid.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		auto source = AssetManager::GetMesh(path.string());
		REQUIRE(source);
		CHECK(source->Skins.empty());
		Scene scene;
		auto entity = MeshImporter::InstantiateHierarchy(scene, *source, "Rigid", {});
		REQUIRE(entity.HasComponent<AnimatorComponent>());
		entity.GetComponent<TransformComponent>().Translation = { 10, 0, 0 };
		const auto rest = AnimatedModelBounds(entity);
		CHECK(rest.Min == glm::vec3(16.75f, 0, 0));
		CHECK(rest.Max == glm::vec3(17.25f, 2, 0));
		scene.OnRuntimeStart();
		REQUIRE(scene.GetAnimationSystem()->Seek(entity, 1));
		const auto moved = AnimatedModelBounds(entity);
		CHECK(moved.Min == glm::vec3(11.75f, 0, 0));
		CHECK(moved.Max == glm::vec3(12.25f, 2, 0));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Lua transition arguments control the expected clips timing and blend weight")
	{
		Scene scene;
		auto entity = AddAnimator(scene);
		scene.OnRuntimeStart();
		CHECK(Eval(scene, "local e=Scene.FindEntityByName('Rig'); assert(e:PlayAnimation('Step')); assert(e:SetAnimationTime(.75)); assert(e:CrossFadeAnimation('Cubic', 1))") == "");
		scene.GetAnimationSystem()->Update(.5f);
		CHECK((*scene.GetAnimationSystem()->GetPose(entity))[2][3].x == doctest::Approx(1.625));
		CHECK(Eval(scene, "assert(Scene.FindEntityByName('Rig'):BlendAnimations('Step', 'Cubic', .25))") == "true");
		scene.GetAnimationSystem()->Update(.5f);
		CHECK((*scene.GetAnimationSystem()->GetPose(entity))[2][3].x == doctest::Approx(.3125));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Animation authoring diagnostics detect missing clips and conflicting mesh components in Edit mode")
	{
		Scene scene;
		auto entity = AddAnimator(scene);
		CHECK(GetAnimationWarnings(entity).empty());
		entity.GetComponent<AnimatorComponent>().Clip = "Renamed";
		auto warnings = GetAnimationWarnings(entity);
		REQUIRE(warnings.size() == 1);
		CHECK(warnings[0].find("Renamed") != std::string::npos);
		entity.AddComponent<MeshComponent>().Mesh = FixturePath().string();
		warnings = GetAnimationWarnings(entity);
		CHECK(warnings.size() == 3);
		entity.RemoveComponent<AnimatorComponent>();
		warnings = GetAnimationWarnings(entity);
		REQUIRE(warnings.size() == 1);
		CHECK(warnings[0].find("undeformed") != std::string::npos);
	}

	TEST_CASE("Animator registry validates atomically and Lua controls all playback operations")
	{
		Scene scene;
		auto entity = AddAnimator(scene);
		std::string error;
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Animator", { { "Speed", -1 }, { "Clip", "Bad" } }, error));
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Animator", { { "Speed", MaxAnimatorSpeed + 1 } }, error));
		CHECK(entity.GetComponent<AnimatorComponent>().Speed == 1);
		CHECK(entity.GetComponent<AnimatorComponent>().Clip.empty());
		scene.OnRuntimeStart();
		CHECK(Eval(scene, R"(
			local e = Scene.FindEntityByName('Rig')
			assert(#e:GetAnimationClips() == 3 and e:GetAnimationDuration() == 2)
			assert(e:IsAnimationPlaying() and e:GetAnimationTime() == 0)
			e:PauseAnimation(); assert(not e:IsAnimationPlaying())
			assert(not pcall(function() e:PauseAnimation(1) end))
			assert(e:SetAnimationTime(1)); assert(e:GetAnimationTime() == 1)
			assert(not e:SetAnimationTime(-1))
			e:PauseAnimation(false); assert(e:IsAnimationPlaying())
			e:StopAnimation(); assert(not e:IsAnimationPlaying() and e:GetAnimationTime() == 0)
			assert(e:PlayAnimation('Step')); assert(not e:PlayAnimation('Unknown'))
			assert(e:SetAnimationTime(1))
			for _, clip in ipairs({5, true, {}, function() end}) do
				local ok, err = pcall(function() e:PlayAnimation(clip) end)
				assert(not ok and tostring(err):find('clip must be a string or nil'))
				assert(e:GetAnimationTime() == 1)
			end
			assert(e:PlayAnimation(nil) and e:GetAnimationTime() == 0)
			assert(e:PlayAnimation())
			assert(e:CrossFadeAnimation('Step', .5))
			assert(e:BlendAnimations('Step', 'Bend', .5))
			local plain = Scene.CreateEntity('Plain')
			assert(not pcall(function() plain:PlayAnimation() end))
			return 'ok'
		)") == "ok");
		scene.OnRuntimeStop();
	}
}
