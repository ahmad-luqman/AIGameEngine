#include <doctest/doctest.h>

#include "TestUtils.h"

#include <Basalt/Animation/AnimatedModel.h>
#include <Basalt/Animation/AnimationPose.h>
#include <Basalt/Animation/AnimationSystem.h>
#include <Basalt/Animation/BlendSpace.h>
#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/MeshImporter.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/SceneEventListener.h>
#include <Basalt/Scene/SceneSerializer.h>
#include <Basalt/Scripting/ScriptEngine.h>

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <fstream>
#include <limits>

using namespace Basalt;
using namespace BasaltTest;
using json = nlohmann::json;

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
			data["animations"] = nlohmann::json::array({ animation });
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
		data["animations"] = nlohmann::json::array({ step });
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

namespace {
	using Names = std::vector<std::string>;
	using Weights = std::vector<std::pair<std::string, float>>;

	// "Name@Clip" for each event the system collected since the last call.
	Names TakeEventNames(AnimationSystem& system)
	{
		Names names;
		for (const AnimationEventRecord& record : system.TakeEvents())
			names.push_back(record.Name + "@" + record.Clip);
		return names;
	}

	// TwoJoint with Step lasting 4 s (keys at 0, 2 and 4) while Bend and Cubic last 2 s.
	std::filesystem::path WriteSlowStepFixture(const TempDirectory& directory)
	{
		auto model = FixtureJson();
		auto& sampler = model["animations"][1]["samplers"][0];
		auto accessor = model["accessors"][sampler["input"].get<size_t>()];
		accessor["max"] = { 4 };
		model["accessors"].push_back(accessor);
		const size_t index = model["accessors"].size() - 1;
		const float times[] = { 0, 2, 4 };
		ReplaceAccessorBytes(model, index, times, sizeof(times));
		sampler["input"] = index;
		const auto path = directory.GetPath() / "SlowStep.gltf";
		std::ofstream(path) << model.dump();
		return path;
	}

	bool NearMatrix(const glm::mat4& a, const glm::mat4& b, float epsilon = 1e-4f)
	{
		for (int c = 0; c < 4; c++)
			if (!glm::all(glm::epsilonEqual(a[c], b[c], epsilon)))
				return false;
		return true;
	}

	bool NearPose(const std::vector<glm::mat4>& a, const std::vector<glm::mat4>& b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); i++)
			if (!NearMatrix(a[i], b[i]))
				return false;
		return true;
	}

	std::vector<float> BlendWeights(const AnimationBlendSpace& space, glm::vec2 parameter)
	{
		std::vector<float> weights;
		ComputeBlendSpaceWeights(space, parameter, weights);
		return weights;
	}

	AnimationBlendSpace Space1D(const std::string& name, std::vector<AnimationBlendSample> samples)
	{
		return { name, AnimationBlendSpaceType::OneD, std::move(samples) };
	}

	class RecordingListener final : public SceneEventListener
	{
	public:
		void OnContact(Scene&, ContactEventType, Entity, Entity, const std::optional<ContactInfo>&) override {}
		void OnScriptError(Scene&, const std::string&) override {}
		void OnAnimationEvent(Scene&, Entity entity, const std::string& name, const std::string& clip) override { Events.push_back(entity.GetName() + ":" + name + "@" + clip); }
		Names Events;
	};
}

TEST_SUITE("Animation")
{
	TEST_CASE("Animation events fire once per crossing across loop wraps, clip ends, seeks, pauses and crossfades")
	{
		ClearAssetCache clear;
		Scene scene;
		auto entity = AddAnimator(scene);
		auto& events = entity.GetComponent<AnimatorComponent>().Events;
		events = { { "Bend", 0.0f, "Start" }, { "Bend", 0.5f, "Mid" }, { "Bend", 2.0f, "End" }, { "", 1.0f, "Any" }, { "Step", 0.25f, "StepOnly" }, { "Bend", 3.0f, "Never" } };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		CHECK(system.TakeEvents().empty());
		// Windows are [from, to): an event at 0 fires on the first advancing update.
		system.Update(.25f);
		CHECK(TakeEventNames(system) == Names{ "Start@Bend" });
		system.Update(.5f);
		CHECK(TakeEventNames(system) == Names{ "Mid@Bend" });
		system.Update(1);
		CHECK(TakeEventNames(system) == Names{ "Any@Bend" });
		// A wrap fires the tail including the clip's end, then the restart.
		system.Update(.5f);
		CHECK(TakeEventNames(system) == Names{ "End@Bend", "Start@Bend" });
		// A step longer than the clip fires every marker of every loop it covers, in order.
		system.Update(4);
		CHECK(TakeEventNames(system) == Names{ "Mid@Bend", "Any@Bend", "End@Bend", "Start@Bend", "Mid@Bend", "Any@Bend", "End@Bend", "Start@Bend" });
		CHECK(system.GetTime(entity) == .25f);
		// A step ending exactly on the end fires it once; the restart's markers wait for the next update.
		system.Update(1.75f);
		CHECK(TakeEventNames(system) == Names{ "Mid@Bend", "Any@Bend", "End@Bend" });
		system.Update(.25f);
		CHECK(TakeEventNames(system) == Names{ "Start@Bend" });
		// A seek skips the markers in between; one at the seek target fires on the next update.
		REQUIRE(system.Seek(entity, 1.5f));
		system.Update(.25f);
		CHECK(system.TakeEvents().empty());
		REQUIRE(system.Seek(entity, .5f));
		system.Update(.125f);
		CHECK(TakeEventNames(system) == Names{ "Mid@Bend" });
		system.Pause(entity, true);
		system.Update(1);
		CHECK(system.TakeEvents().empty());
		system.Stop(entity);
		system.Update(1);
		CHECK(system.TakeEvents().empty());
		// Speed 0 never advances, so nothing is crossed.
		entity.GetComponent<AnimatorComponent>().Speed = 0;
		REQUIRE(system.Play(entity, "Bend"));
		system.Update(1);
		CHECK(system.TakeEvents().empty());
		entity.GetComponent<AnimatorComponent>().Speed = 1;

		SUBCASE("A non-looping clip fires its end once, also after a seek past the end")
		{
			entity.GetComponent<AnimatorComponent>().Looping = false;
			REQUIRE(system.Play(entity, "Bend"));
			system.Update(1.9f);
			CHECK(TakeEventNames(system) == Names{ "Start@Bend", "Mid@Bend", "Any@Bend" });
			system.Update(1);
			CHECK(TakeEventNames(system) == Names{ "End@Bend" });
			CHECK_FALSE(system.IsPlaying(entity));
			system.Update(1);
			REQUIRE(system.Seek(entity, 5));
			system.Pause(entity, false);
			system.Update(1);
			CHECK(system.TakeEvents().empty());
		}
		SUBCASE("Markers name their clip; empty marks every clip; equal times keep their order")
		{
			events.push_back({ "Step", 0.25f, "StepOnlyToo" });
			REQUIRE(system.Play(entity, "Step"));
			system.Update(1.5f);
			CHECK(TakeEventNames(system) == Names{ "StepOnly@Step", "StepOnlyToo@Step", "Any@Step" });
		}
		SUBCASE("A crossfade fires the incoming and the outgoing clip until the fade ends")
		{
			REQUIRE(system.Play(entity, "Bend"));
			REQUIRE(system.Seek(entity, .4f));
			REQUIRE(system.CrossFade(entity, "Step", 1));
			system.Update(.3f);
			CHECK(TakeEventNames(system) == Names{ "StepOnly@Step", "Mid@Bend" });
			system.Update(.8f);
			CHECK(TakeEventNames(system) == Names{ "Any@Step", "Any@Bend" });
			// The fade finished: only the incoming clip remains (Bend's End is not one of Step's markers).
			system.Update(1.2f);
			CHECK(TakeEventNames(system) == Names{ "StepOnly@Step" });
			// A fade that ends within an update: the outgoing clip fires only up to where it ends (Bend .4..1).
			REQUIRE(system.Play(entity, "Bend"));
			REQUIRE(system.CrossFade(entity, "Step", 1));
			system.Update(.4f);
			CHECK(TakeEventNames(system) == Names{ "StepOnly@Step", "Start@Bend" });
			system.Update(1);
			CHECK(TakeEventNames(system) == Names{ "Any@Step", "Mid@Bend" });
			// A fade shorter than the update: the outgoing clip fires only for the faded part (Bend 0..0.1).
			REQUIRE(system.Play(entity, "Bend"));
			REQUIRE(system.CrossFade(entity, "Step", .1f));
			system.Update(1.5f);
			CHECK(TakeEventNames(system) == Names{ "StepOnly@Step", "Any@Step", "Start@Bend" });
		}
		SUBCASE("A two-clip blend fires each clip while it shows")
		{
			REQUIRE(system.Blend(entity, "Bend", "Step", 0));
			system.Update(.75f);
			CHECK(TakeEventNames(system) == Names{ "Start@Bend", "Mid@Bend" });
			REQUIRE(system.Blend(entity, "Bend", "Step", 1));
			system.Update(.5f);
			CHECK(TakeEventNames(system) == Names{ "Any@Step" });
			REQUIRE(system.Blend(entity, "Bend", "Step", .5f));
			system.Update(.75f);
			CHECK(TakeEventNames(system) == Names{ "End@Bend" });
		}
		SUBCASE("A flood of events is capped per frame, warned once per animation started")
		{
			const auto since = Log::GetHistory().GetTotalCount();
			REQUIRE(system.Play(entity, "Bend"));
			system.Update(1000);
			CHECK(system.TakeEvents().size() == AnimationSystem::MaxEventsPerFrame);
			system.Update(1000);
			CHECK(system.TakeEvents().size() == AnimationSystem::MaxEventsPerFrame);
			CHECK(CountLogMessages(since, "animation events in one frame") == 1);
			REQUIRE(system.Play(entity, "Bend"));
			system.Update(1000);
			CHECK(CountLogMessages(since, "animation events in one frame") == 2);
		}
		SUBCASE("Markers C++ wrote outside the validated range are ignored")
		{
			events = { { "Bend", -1.0f, "Negative" }, { "Bend", NAN, "NaN" } };
			REQUIRE(system.Play(entity, "Bend"));
			system.Update(1000);
			CHECK(system.TakeEvents().empty());
		}
		scene.OnRuntimeStop();
	}

	TEST_CASE("Animation events reach scripts after OnLateUpdate and the event listener, also in simulation")
	{
		ClearAssetCache clear;
		BasaltTest::TempProject project("AnimationEventScripts");
		const std::string script = project.WriteFile("Assets/Scripts/Rig.lua", R"(
			local Rig = {}
			function Rig:OnCreate() self.Log = "" self.Late = 0 end
			function Rig:OnLateUpdate() self.Late = self.Late + 1 end
			function Rig:OnAnimationEvent(name, clip)
				self.Log = self.Log .. name .. "@" .. clip .. ":" .. self.Late .. ";"
				-- The system is not iterating its playback while callbacks run, so this is safe.
				if name == "Mid" then self.Entity:RemoveComponent("Animator") end
			end
			return Rig
		)");
		Scene scene;
		auto entity = AddAnimator(scene);
		entity.GetComponent<AnimatorComponent>().Events = { { "", 0.0f, "Start" }, { "", 0.5f, "Mid" }, { "", 0.6f, "After" } };
		entity.AddComponent<ScriptComponent>().Script = script;
		RecordingListener listener;
		scene.SetEventListener(&listener);

		SUBCASE("Play")
		{
			scene.OnRuntimeStart();
			scene.OnUpdate(.75f);
			// All three were crossed in one frame; removing the Animator does not cancel delivered ones.
			CHECK(scene.GetScriptEngine()->GetInstanceField(entity, "Log") == "Start@Bend:1;Mid@Bend:1;After@Bend:1;");
			CHECK(listener.Events == Names{ "Rig:Start@Bend", "Rig:Mid@Bend", "Rig:After@Bend" });
			CHECK_FALSE(entity.HasComponent<AnimatorComponent>());
			scene.OnUpdate(.75f);
			CHECK(listener.Events.size() == 3);
			CHECK(scene.GetScriptEngine()->GetErrors().empty());
			scene.OnRuntimeStop();
		}
		SUBCASE("Simulate has no scripts but still reports to the listener")
		{
			scene.OnSimulationStart();
			scene.OnUpdate(.25f);
			CHECK(listener.Events == Names{ "Rig:Start@Bend" });
			scene.OnSimulationStop();
		}
		scene.SetEventListener(nullptr);
	}

	TEST_CASE("Events of an entity destroyed by an earlier event callback are not delivered")
	{
		ClearAssetCache clear;
		BasaltTest::TempProject project("AnimationEventDestroy");
		const std::string script = project.WriteFile("Assets/Scripts/Doom.lua", R"(
			local Doom = {}
			function Doom:OnAnimationEvent(name, clip)
				Delivered = (Delivered or "") .. self.Entity.Name .. ":" .. name .. ";"
				Scene.FindEntityByName(self.Entity.Name == "A" and "B" or "A"):Destroy()
				self.Entity:Destroy()
			end
			return Doom
		)");
		Scene scene;
		for (const char* name : { "A", "B" })
		{
			auto rig = AddAnimator(scene, name);
			rig.GetComponent<AnimatorComponent>().Events = { { "", 0.0f, "First" }, { "", 0.5f, "Second" } };
			rig.AddComponent<ScriptComponent>().Script = script;
		}
		RecordingListener listener;
		scene.SetEventListener(&listener);
		scene.OnRuntimeStart();
		scene.OnUpdate(.75f);
		// Whichever rig is delivered first destroys both; nothing else reaches the scripts or the listener.
		CHECK(listener.Events.size() == 1);
		const std::string delivered = Eval(scene, "return Delivered");
		CHECK((delivered == "A:First;" || delivered == "B:First;"));
		scene.OnUpdate(.75f);
		CHECK(listener.Events.size() == 1);
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
		scene.SetEventListener(nullptr);
	}

	TEST_CASE("Blend space weights are exact at samples, sum to one and clamp outside the samples")
	{
		auto sample = [](float x, float y = 0) { return AnimationBlendSample{ "", x, y, 0 }; };
		const auto space = Space1D("Line", { sample(3), sample(0), sample(1) });
		CHECK(BlendWeights(space, { -1, 7 }) == std::vector<float>{ 0, 1, 0 });
		CHECK(BlendWeights(space, { 0, 0 }) == std::vector<float>{ 0, 1, 0 });
		CHECK(BlendWeights(space, { .25f, 0 }) == std::vector<float>{ 0, .75f, .25f });
		CHECK(BlendWeights(space, { 2, 0 }) == std::vector<float>{ .5f, 0, .5f });
		CHECK(BlendWeights(space, { 9, 0 }) == std::vector<float>{ 1, 0, 0 });
		CHECK(BlendWeights(Space1D("Empty", {}), { 0, 0 }).empty());
		CHECK(BlendWeights(Space1D("One", { sample(2) }), { -5, 0 }) == std::vector<float>{ 1 });
		// Samples at one position share its weight, at the position and between it and its neighbours.
		const auto shared = Space1D("Shared", { sample(0), sample(1), sample(1) });
		CHECK(BlendWeights(shared, { 1, 0 }) == std::vector<float>{ 0, .5f, .5f });
		CHECK(BlendWeights(shared, { .5f, 0 }) == std::vector<float>{ .5f, .25f, .25f });
		CHECK(BlendWeights(shared, { 3, 0 }) == std::vector<float>{ 0, .5f, .5f });

		AnimationBlendSpace plane{ "Plane", AnimationBlendSpaceType::TwoD, { sample(0, 0), sample(1, 0), sample(0, 1), sample(-1, 0), sample(0, -1) } };
		for (size_t i = 0; i < plane.Samples.size(); i++)
		{
			const auto atSample = BlendWeights(plane, { plane.Samples[i].X, plane.Samples[i].Y });
			for (size_t j = 0; j < atSample.size(); j++)
				CHECK(atSample[j] == doctest::Approx(i == j ? 1 : 0));
		}
		const auto between = BlendWeights(plane, { .5f, 0 });
		CHECK(between[0] == doctest::Approx(.5));
		CHECK(between[1] == doctest::Approx(.5));
		for (const glm::vec2 point : { glm::vec2(.3f, .7f), glm::vec2(-2, 5), glm::vec2(.1f, -.4f), glm::vec2(40, -40) })
		{
			float total = 0;
			for (const float weight : BlendWeights(plane, point))
			{
				CHECK(weight >= 0);
				total += weight;
			}
			CHECK(total == doctest::Approx(1));
		}
		// Collinear samples in a 2D space still blend along the line; coincident ones share.
		AnimationBlendSpace line2D{ "Line2D", AnimationBlendSpaceType::TwoD, { sample(0, 0), sample(2, 0), sample(2, 0) } };
		const auto onLine = BlendWeights(line2D, { .5f, 3 });
		CHECK(onLine[0] == doctest::Approx(.75));
		CHECK(onLine[1] == doctest::Approx(.125));
		CHECK(onLine[2] == doctest::Approx(.125));
	}

	TEST_CASE("Blend spaces play their clips in step and keep the phase when the parameter moves")
	{
		ClearAssetCache clear;
		TempDirectory directory("AnimationBlendSpaces");
		const auto path = WriteSlowStepFixture(directory);
		Scene scene;
		auto entity = scene.CreateEntity("Rig");
		auto& animator = entity.AddComponent<AnimatorComponent>();
		animator.Mesh = path.string();
		animator.Events = { { "Step", 1.0f, "StepMark" }, { "Cubic", 0.5f, "CubicMark" } };
		animator.BlendSpaces = { Space1D("Walk", { { "Step", 0, 0, 1 }, { "Cubic", 1, 0, 2 } }), Space1D("Broken", { { "Nope", 0, 0, 0 }, { "Step", 1, 0, 0 } }), Space1D("Twins", { { "Step", 0, 0, 0 }, { "Step", 1, 0, 0 }, { "", 2, 0, 0 } }) };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const auto source = AssetManager::GetMesh(path.string());
		REQUIRE(source);
		REQUIRE(source->Animations[1].Duration == 4);
		REQUIRE(source->Animations[2].Duration == 2);

		REQUIRE(system.PlayBlendSpace(entity, "Walk", { .5f, 0 }));
		CHECK(system.GetBlendSpaceWeights(entity) == Weights{ { "Step", .5f }, { "Cubic", .5f } });
		// One cycle lasts the weighted durations: .5 * 4 + .5 * 2.
		CHECK(system.GetDuration(entity) == 3);
		system.Update(1.5f);
		CHECK(system.GetTime(entity) == doctest::Approx(1.5));
		// Both clips sit at half their own length: Step at 2 s, Cubic at 1 s.
		auto expected = SampleAnimation(*source, &source->Animations[2], 1);
		BlendAnimationPose(expected, SampleAnimation(*source, &source->Animations[1], 2), .5f);
		std::vector<glm::mat4> expectedGlobals;
		REQUIRE(ComposeAnimationPose(*source, expected, expectedGlobals));
		CHECK(NearPose(*system.GetPose(entity), expectedGlobals));
		// The heaviest sample alone fires (a tie goes to the first): both marks sit at phase .25.
		CHECK(TakeEventNames(system) == Names{ "StepMark@Step" });

		// Moving the parameter keeps the phase: half of the new duration.
		REQUIRE(system.SetBlendSpaceParameter(entity, { 1, 0 }));
		CHECK(system.GetTime(entity) == doctest::Approx(1));
		CHECK(system.GetBlendSpaceWeights(entity) == Weights{ { "Step", 0.0f }, { "Cubic", 1.0f } });
		REQUIRE(system.PlayBlendSpace(entity, "Walk", { .25f, 0 }));
		CHECK(system.GetTime(entity) == doctest::Approx(.5 * 3.5));
		// Seeking a space moves its phase.
		REQUIRE(system.Seek(entity, 3.5f));
		CHECK(system.GetTime(entity) == doctest::Approx(3.5));
		REQUIRE(system.SetBlendSpaceParameter(entity, { -3, 0 }));
		CHECK(system.GetDuration(entity) == 4);
		CHECK(system.GetTime(entity) == 4);

		const auto since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(system.PlayBlendSpace(entity, "Missing", { 0, 0 }));
		CHECK_FALSE(system.PlayBlendSpace(entity, "Walk", { NAN, 0 }));
		CHECK_FALSE(system.PlayBlendSpace(entity, "Walk", { 0, 0 }, -1));
		CHECK_FALSE(system.SetBlendSpaceParameter(entity, { INFINITY, 0 }));
		CHECK(CountLogMessages(since, "blend space 'Missing' not found") == 1);
		CHECK(CountLogMessages(since, "fadeSeconds must be finite and nonnegative") == 1);
		// A sample whose clip is missing gets no weight; the space fails only when nothing is left.
		CHECK_FALSE(system.PlayBlendSpace(entity, "Broken", { 0, 0 }));
		CHECK(CountLogMessages(since, "blend space 'Broken': sample clips not found: 'Nope'") == 1);
		REQUIRE(system.PlayBlendSpace(entity, "Broken", { .5f, 0 }));
		CHECK(system.GetBlendSpaceWeights(entity) == Weights{ { "Step", 1.0f } });
		// Moving onto the missing clip pauses the space (one log for the same problem); moving off resumes it.
		CHECK_FALSE(system.SetBlendSpaceParameter(entity, { 0, 0 }));
		CHECK_FALSE(system.IsPlaying(entity));
		CHECK(system.GetBlendSpaceWeights(entity).empty());
		REQUIRE(system.SetBlendSpaceParameter(entity, { 1, 0 }));
		CHECK(system.IsPlaying(entity));
		// The same problem is logged once, whether the space failed to start or went silent later.
		CHECK(CountLogMessages(since, "sample clips not found: 'Nope'") == 1);
		// Samples sharing a clip add up; an empty sample clip is the first clip (Bend).
		REQUIRE(system.PlayBlendSpace(entity, "Twins", { 1.5f, 0 }));
		CHECK(system.GetBlendSpaceWeights(entity) == Weights{ { "Step", .5f }, { "Bend", .5f } });
		REQUIRE(system.SetBlendSpaceParameter(entity, { .5f, 0 }));
		CHECK(system.GetBlendSpaceWeights(entity) == Weights{ { "Step", 1.0f }, { "Bend", 0.0f } });

		// Other playback leaves the space.
		REQUIRE(system.Play(entity, "Bend"));
		CHECK(system.GetBlendSpaceWeights(entity).empty());
		CHECK_FALSE(system.SetBlendSpaceParameter(entity, { 0, 0 }));
		REQUIRE(system.PlayBlendSpace(entity, "Walk", { 0, 0 }));
		system.Stop(entity);
		CHECK(system.GetBlendSpaceWeights(entity).empty());

		SUBCASE("A fade into a space starts from the visible pose")
		{
			REQUIRE(system.Play(entity, "Bend"));
			system.Update(.5f);
			const auto before = *system.GetPose(entity);
			REQUIRE(system.PlayBlendSpace(entity, "Walk", { 1, 0 }, 1));
			CHECK(NearPose(before, *system.GetPose(entity)));
			system.Update(1);
			std::vector<glm::mat4> cubic;
			REQUIRE(EvaluateAnimation(*source, &source->Animations[2], 1, cubic));
			CHECK(NearPose(*system.GetPose(entity), cubic));
		}
		SUBCASE("A fade out of a space starts from the visible pose and fires no space events")
		{
			REQUIRE(system.PlayBlendSpace(entity, "Walk", { .5f, 0 }));
			system.Update(1);
			system.TakeEvents();
			const auto before = *system.GetPose(entity);
			REQUIRE(system.CrossFade(entity, "Bend", 1));
			CHECK(NearPose(before, *system.GetPose(entity)));
			system.Update(.9f);
			CHECK(system.TakeEvents().empty());
			REQUIRE(system.PlayBlendSpace(entity, "Walk", { .5f, 0 }));
			REQUIRE(system.PlayBlendSpace(entity, "Twins", { 0, 0 }, 1));
			CHECK(system.GetBlendSpaceWeights(entity) == Weights{ { "Step", 1.0f }, { "Bend", 0.0f } });
		}
		SUBCASE("A non-looping space stops at the end of its cycle and replays when played again")
		{
			entity.GetComponent<AnimatorComponent>().Looping = false;
			REQUIRE(system.PlayBlendSpace(entity, "Walk", { .5f, 0 }));
			system.Update(10);
			CHECK_FALSE(system.IsPlaying(entity));
			CHECK(system.GetTime(entity) == 3);
			REQUIRE(system.PlayBlendSpace(entity, "Walk", { .5f, 0 }));
			CHECK(system.IsPlaying(entity));
			CHECK(system.GetTime(entity) == 0);
			// A paused space restarts too.
			system.Update(.5f);
			system.Pause(entity, true);
			REQUIRE(system.PlayBlendSpace(entity, "Walk", { .5f, 0 }));
			CHECK(system.IsPlaying(entity));
			CHECK(system.GetTime(entity) == 0);
		}
		SUBCASE("Removing the running space holds the pose with one error and resumes when it is back")
		{
			REQUIRE(system.PlayBlendSpace(entity, "Walk", { .5f, 0 }));
			const auto removed = Log::GetHistory().GetTotalCount();
			const auto spaces = entity.GetComponent<AnimatorComponent>().BlendSpaces;
			entity.GetComponent<AnimatorComponent>().BlendSpaces.clear();
			system.Update(.1f);
			const float held = system.GetTime(entity);
			const auto heldPose = *system.GetPose(entity);
			system.Update(.1f);
			CHECK_FALSE(system.IsPlaying(entity));
			CHECK(system.GetTime(entity) == held);
			CHECK_FALSE(system.SetBlendSpaceParameter(entity, { 0, 0 }));
			// Seeking cannot play it either: the pose holds.
			CHECK_FALSE(system.Seek(entity, .5f));
			CHECK(NearPose(*system.GetPose(entity), heldPose));
			CHECK(CountLogMessages(removed, "cannot seek: blend space 'Walk' not found") == 1);
			CHECK(CountLogMessages(removed, "Animator 'Rig': blend space 'Walk' not found") == 1);
			entity.GetComponent<AnimatorComponent>().BlendSpaces = spaces;
			system.Update(.1f);
			CHECK(system.IsPlaying(entity));
			CHECK(system.GetTime(entity) > held);
		}
		scene.OnRuntimeStop();
	}

	TEST_CASE("Three weighted blend space clips accumulate into the normalized blend")
	{
		ClearAssetCache clear;
		Scene scene;
		auto entity = AddAnimator(scene);
		entity.GetComponent<AnimatorComponent>().BlendSpaces = { { "Strafe", AnimationBlendSpaceType::TwoD, { { "Step", 0, 0, 0 }, { "Cubic", 1, 0, 0 }, { "Bend", 0, 1, 0 } } } };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.PlayBlendSpace(entity, "Strafe", { .25f, .25f }));
		system.Update(.5f);
		const auto weights = system.GetBlendSpaceWeights(entity);
		REQUIRE(weights.size() == 3);
		// Gradient bands at (.25, .25): .75 for Step and .25 for the others, normalized.
		CHECK(weights[0].second == doctest::Approx(.6));
		CHECK(weights[1].second == doctest::Approx(.2));
		CHECK(weights[2].second == doctest::Approx(.2));
		// Step and Cubic at their mutual ratio, then Bend at its share of the total.
		const auto source = AssetManager::GetMesh(FixturePath().string());
		auto pose = SampleAnimation(*source, &source->Animations[2], .5f);
		BlendAnimationPose(pose, SampleAnimation(*source, &source->Animations[1], .5f), weights[1].second / (weights[0].second + weights[1].second));
		auto bend = SampleAnimation(*source, &source->Animations[0], .5f);
		BlendAnimationPose(bend, pose, weights[2].second);
		std::vector<glm::mat4> expected;
		REQUIRE(ComposeAnimationPose(*source, bend, expected));
		CHECK(NearPose(*system.GetPose(entity), expected));
		scene.OnRuntimeStop();
	}

	TEST_CASE("An Animator Clip may name a blend space to play on start")
	{
		ClearAssetCache clear;
		Scene scene;
		auto entity = AddAnimator(scene);
		auto& animator = entity.GetComponent<AnimatorComponent>();
		animator.BlendSpaces = { { "Strafe", AnimationBlendSpaceType::TwoD, { { "Step", 0, 0, 0 }, { "Cubic", 1, 0, 0 }, { "Bend", 0, 1, 0 } } } };
		animator.Clip = "Strafe";
		CHECK(GetAnimationWarnings(entity).empty());
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		CHECK(system.IsPlaying(entity));
		CHECK(system.GetBlendSpaceWeights(entity) == Weights{ { "Step", 1.0f }, { "Cubic", 0.0f }, { "Bend", 0.0f } });
		REQUIRE(system.SetBlendSpaceParameter(entity, { 0, 1 }));
		CHECK(system.GetBlendSpaceWeights(entity)[2].second == doctest::Approx(1));
		// Lua's SetBlendSpaceParameter(x) keeps a 2D space's Y.
		CHECK(Eval(scene, "local e = Scene.FindEntityByName('Rig'); assert(e:SetBlendSpaceParameter(0)); return e:GetBlendSpaceWeights().Bend") == "1.0");
		scene.OnRuntimeStop();
	}

	TEST_CASE("A start blend space that cannot play holds the rest pose until it can")
	{
		ClearAssetCache clear;
		Scene scene;
		auto entity = AddAnimator(scene);
		auto& animator = entity.GetComponent<AnimatorComponent>();
		animator.BlendSpaces = { Space1D("Run", { { "Gone", 0, 0, 0 }, { "Lost", 1, 0, 0 }, { "Gone", 2, 0, 0 } }) };
		animator.Clip = "Run";
		const auto since = Log::GetHistory().GetTotalCount();
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		system.Update(.5f);
		CHECK(CountLogMessages(since, "blend space 'Run': sample clips not found: 'Gone', 'Lost'") == 1);
		CHECK_FALSE(system.IsPlaying(entity));
		CHECK_FALSE(system.Seek(entity, .5f));
		CHECK(system.GetBlendSpaceWeights(entity).empty());
		std::vector<glm::mat4> rest;
		REQUIRE(EvaluateAnimation(*AssetManager::GetMesh(FixturePath().string()), nullptr, 0, rest));
		CHECK(NearPose(*system.GetPose(entity), rest));
		animator.BlendSpaces[0].Samples[0].Clip = "Step";
		system.Update(.5f);
		CHECK(system.IsPlaying(entity));
		CHECK(system.GetBlendSpaceWeights(entity) == Weights{ { "Step", 1.0f } });
		CHECK(CountLogMessages(since, "sample clips not found: 'Lost'") == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A start blend space added after the Animator's Clip named it still starts")
	{
		ClearAssetCache clear;
		Scene scene;
		auto entity = AddAnimator(scene);
		auto& animator = entity.GetComponent<AnimatorComponent>();
		animator.Clip = "Later";
		auto other = AddAnimator(scene, "Other");
		other.GetComponent<AnimatorComponent>().Clip = "Later";
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		system.Update(.1f);
		CHECK_FALSE(system.IsPlaying(entity));
		// Something else played first: the late space does not take over.
		REQUIRE(system.Play(other, "Bend"));
		animator.BlendSpaces = { Space1D("Later", { { "Step", 0, 0, 0 } }) };
		other.GetComponent<AnimatorComponent>().BlendSpaces = animator.BlendSpaces;
		system.Update(.1f);
		CHECK(system.IsPlaying(entity));
		CHECK(system.GetBlendSpaceWeights(entity) == Weights{ { "Step", 1.0f } });
		CHECK(system.GetBlendSpaceWeights(other).empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Animation operations on a model that cannot animate say why, once")
	{
		ClearAssetCache clear;
		Scene scene;
		auto entity = scene.CreateEntity("Broken");
		entity.AddComponent<AnimatorComponent>().Mesh = "Missing/Model.gltf";
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const auto since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(system.Play(entity, "Walk"));
		CHECK_FALSE(system.CrossFade(entity, "Walk", 1));
		CHECK_FALSE(system.PlayBlendSpace(entity, "Run", { 0, 0 }));
		CHECK(CountLogMessages(since, "Animator 'Broken': cannot animate: model 'Missing/Model.gltf' could not be loaded") == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Animator authoring warnings name unknown clips, unreachable markers and inert settings")
	{
		ClearAssetCache clear;
		Scene scene;
		auto entity = AddAnimator(scene);
		auto& animator = entity.GetComponent<AnimatorComponent>();
		animator.Events = { { "Nope", 0.0f, "Lost" }, { "Bend", 3.0f, "Late" }, { "", 9.0f, "AnyLate" } };
		animator.BlendSpaces = { Space1D("Empty", {}), Space1D("Bend", { { "Gone", 0, 0, 0 } }) };
		animator.MatchVelocity = true;
		animator.Clip = "Bend";
		const Names expected = {
			"Animator event 'Lost': clip 'Nope' not found",
			"Animator event 'Late' at 3s is past the end of clip 'Bend' (2s) and never fires",
			"Animator event 'AnyLate' at 9s is past the end of every clip and never fires",
			"Animator blend space 'Empty' has no samples",
			"Animator blend space 'Bend' has the name of a clip; Clip selects the clip",
			"Animator blend space 'Bend': clip 'Gone' not found",
			"Animator MatchVelocity has no effect: no blend space sample has a Speed",
		};
		CHECK(GetAnimationWarnings(entity) == expected);
	}

	TEST_CASE("MatchVelocity plays a blend space as fast as the entity moves horizontally")
	{
		ClearAssetCache clear;
		Scene scene;
		auto entity = AddAnimator(scene);
		auto& animator = entity.GetComponent<AnimatorComponent>();
		animator.BlendSpaces = { Space1D("Run", { { "Step", 0, 0, 1 }, { "Cubic", 1, 0, 2 } }), Space1D("Start", { { "Bend", 0, 0, 0 }, { "Step", 1, 0, 1 } }) };
		animator.MatchVelocity = true;
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.PlayBlendSpace(entity, "Run", { 0, 0 }));
		// The clip time one .1 s update advances after the entity moved by `delta` per update for a while.
		auto advance = [&](const glm::vec3& delta) {
			for (int i = 0; i < 60; i++)
			{
				entity.GetTransform().Translation += delta;
				system.Update(.1f);
			}
			const float before = system.GetTime(entity);
			entity.GetTransform().Translation += delta;
			system.Update(.1f);
			return std::fmod(system.GetTime(entity) - before + system.GetDuration(entity), system.GetDuration(entity));
		};
		// 2.5 m/s over a sample that covers 1 m/s at speed 1: 2.5 times as fast.
		CHECK(advance({ .25f, 0, 0 }) == doctest::Approx(.25).epsilon(1e-3));
		// Falling is not walking.
		CHECK(advance({ 0, -.25f, 0 }) == doctest::Approx(0).epsilon(1e-3));
		REQUIRE(system.SetBlendSpaceParameter(entity, { 1, 0 }));
		CHECK(advance({ 0, 0, .3f }) == doctest::Approx(.15).epsilon(1e-3));
		// A teleport does not count as movement.
		entity.GetTransform().Translation.x += 500;
		system.Update(.1f);
		const float beforeTeleport = system.GetTime(entity);
		entity.GetTransform().Translation.z += .3f;
		system.Update(.1f);
		CHECK(std::fmod(system.GetTime(entity) - beforeTeleport + 2.0f, 2.0f) == doctest::Approx(.15).epsilon(1e-3));
		// Moving while paused is measured too: resuming plays at the steady rate at once.
		system.Pause(entity, true);
		for (int i = 0; i < 30; i++)
		{
			entity.GetTransform().Translation.z += .3f;
			system.Update(.1f);
		}
		system.Pause(entity, false);
		const float paused = system.GetTime(entity);
		entity.GetTransform().Translation.z += .3f;
		system.Update(.1f);
		CHECK(std::fmod(system.GetTime(entity) - paused + 2.0f, 2.0f) == doctest::Approx(.15).epsilon(1e-3));
		// Turning matching off and on starts a fresh measurement: movement while it was off is not one frame's.
		animator.MatchVelocity = false;
		for (int i = 0; i < 10; i++)
		{
			entity.GetTransform().Translation.z += .3f;
			system.Update(.1f);
		}
		animator.MatchVelocity = true;
		system.Update(.1f);
		const float reenabled = system.GetTime(entity);
		system.Update(.1f);
		CHECK(std::fmod(system.GetTime(entity) - reenabled + 2.0f, 2.0f) == doctest::Approx(0).epsilon(1e-3));
		// Samples that stand still (Speed 0) keep the plain Speed for their share: half idle, half moving at 1 m/s
		// while the entity moves at 2 m/s plays at .5 + .5 * 2.
		REQUIRE(system.PlayBlendSpace(entity, "Start", { .5f, 0 }));
		CHECK(advance({ 0, 0, .2f }) == doctest::Approx(.15).epsilon(1e-3));
		REQUIRE(system.SetBlendSpaceParameter(entity, { 0, 0 }));
		CHECK(advance({ 0, 0, .2f }) == doctest::Approx(.1).epsilon(1e-3));
		// At a high frame rate physics moves the entity only every fixed step; those moves are not teleports. 2000
		// updates per second, moving .1 every fixed step: 6 m/s over Run's 1 m/s sample.
		REQUIRE(system.PlayBlendSpace(entity, "Run", { 0, 0 }));
		const float fixedStep = scene.GetFixedTimestep();
		const int perStep = static_cast<int>(std::lround(fixedStep / .0005f));
		auto highRate = [&](int steps) {
			for (int i = 0; i < steps * perStep; i++)
			{
				if (i % perStep == 0)
					entity.GetTransform().Translation.z += .1f;
				system.Update(.0005f);
			}
		};
		highRate(30);
		const float beforeSteps = system.GetTime(entity);
		highRate(6);
		CHECK(std::fmod(system.GetTime(entity) - beforeSteps + 2.0f, 2.0f) == doctest::Approx(.6).epsilon(.05));
		animator.MatchVelocity = false;
		REQUIRE(system.PlayBlendSpace(entity, "Run", { 1, 0 }));
		CHECK(advance({ 0, 0, .3f }) == doctest::Approx(.1).epsilon(1e-3));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Bone transforms and attachments follow the posed joint in world space")
	{
		ClearAssetCache clear;
		Scene scene;
		auto rig = AddAnimator(scene);
		rig.GetTransform().Translation = { 5, 0, 0 };
		rig.GetComponent<AnimatorComponent>().Clip = "Bend";
		// Created before the second rig it is attached to, so plain entity order would move it first.
		auto hat = scene.CreateEntity("Hat");
		auto rig2 = AddAnimator(scene, "Rig2");
		rig2.AddComponent<BoneAttachmentComponent>() = { rig.GetUUID(), "Tip", { 0, .5f, 0 }, glm::quat(1, 0, 0, 0), { 1, 1, 1 } };
		const glm::quat hatRotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
		hat.AddComponent<BoneAttachmentComponent>() = { rig2.GetUUID(), "Tip", { 0, .25f, 0 }, hatRotation, { 2, 2, 2 } };
		// Target 0 attaches to the parent.
		auto child = scene.CreateChildEntity(rig, "Child");
		child.AddComponent<BoneAttachmentComponent>().Joint = "RootJoint";
		// A parent that is not the target: the joint's world transform is converted into the parent's space.
		auto holder = scene.CreateEntity("Holder");
		holder.GetTransform().Translation = { 0, 10, 0 };
		holder.GetTransform().SetRotationEuler({ 0, glm::radians(90.0f), 0 });
		auto held = scene.CreateChildEntity(holder, "Held");
		held.AddComponent<BoneAttachmentComponent>() = { rig.GetUUID(), "Tip", { 1, 0, 0 }, glm::quat(1, 0, 0, 0), { 1, 1, 1 } };
		CHECK(GetAnimationWarnings(held).empty());
		// An attachment whose parent is attached, and one whose target's parent is attached: both created before
		// the attachments they depend on.
		auto gem = scene.CreateEntity("Gem");
		auto rig3Parent = scene.CreateEntity("Rig3Parent");
		auto rig3 = scene.CreateChildEntity(rig3Parent, "Rig3");
		rig3.AddComponent<AnimatorComponent>().Mesh = FixturePath().string();
		gem.AddComponent<BoneAttachmentComponent>() = { rig3.GetUUID(), "Tip", { 0, .1f, 0 }, glm::quat(1, 0, 0, 0), { 1, 1, 1 } };
		auto ring = scene.CreateChildEntity(scene.CreateEntity("RingHolder"), "Ring");
		ring.AddComponent<BoneAttachmentComponent>() = { rig.GetUUID(), "Tip", { 0, .3f, 0 }, glm::quat(1, 0, 0, 0), { 1, 1, 1 } };
		ring.GetParent().AddComponent<BoneAttachmentComponent>() = { rig.GetUUID(), "RootJoint", { 1, 0, 0 }, glm::quat(1, 0, 0, 0), { 1, 1, 1 } };
		rig3Parent.AddComponent<BoneAttachmentComponent>() = { rig.GetUUID(), "Tip", { 0, 1, 0 }, glm::quat(1, 0, 0, 0), { 1, 1, 1 } };

		const auto since = Log::GetHistory().GetTotalCount();
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		system.Update(.5f);
		const auto& pose = *system.GetPose(rig);
		glm::mat4 tip;
		std::string error;
		REQUIRE(system.GetBoneTransform(rig, "Tip", tip, error));
		CHECK(NearMatrix(tip, pose[0]));
		CHECK_FALSE(system.GetBoneTransform(rig, "Nope", tip, error));
		CHECK(error.find("joint 'Nope' not found in '") == 0);
		CHECK_FALSE(system.GetBoneTransform(holder, "Tip", tip, error));
		CHECK(error == "it has no Animator");

		const glm::mat4 rigWorld = scene.GetWorldTransform(rig);
		CHECK(NearMatrix(scene.GetWorldTransform(rig2), rigWorld * pose[0] * glm::translate(glm::mat4(1.0f), { 0, .5f, 0 })));
		CHECK(NearMatrix(scene.GetWorldTransform(hat), scene.GetWorldTransform(rig2) * (*system.GetPose(rig2))[0] * Math::ComposeTransform({ 0, .25f, 0 }, hatRotation, glm::vec3(2))));
		CHECK(NearMatrix(child.GetTransform().GetTransform(), pose[2]));
		CHECK(NearMatrix(scene.GetWorldTransform(held), rigWorld * pose[0] * glm::translate(glm::mat4(1.0f), { 1, 0, 0 })));
		CHECK(NearMatrix(scene.GetWorldTransform(gem), scene.GetWorldTransform(rig3) * (*system.GetPose(rig3))[0] * glm::translate(glm::mat4(1.0f), { 0, .1f, 0 })));
		CHECK(NearMatrix(scene.GetWorldTransform(ring), rigWorld * pose[0] * glm::translate(glm::mat4(1.0f), { 0, .3f, 0 })));

		// Lua returns the joint's world position, rotation and scale.
		CHECK(Eval(scene, R"(
			local rig = Scene.FindEntityByName('Rig')
			local position, rotation, scale = rig:GetBoneTransform('Tip')
			local world = Scene.FindEntityByName('Rig2').WorldPosition
			local expected = position + rotation * Vec3(0, 0.5, 0)
			assert((expected - world):Length() < 1e-4, tostring(expected) .. ' vs ' .. tostring(world))
			assert((scale - Vec3(1, 1, 1)):Length() < 1e-4)
			for _ = 1, 3 do assert(rig:GetBoneTransform('Nope') == nil) end
			assert(rig:GetBoneTransform('Tip') ~= nil)
			assert(not pcall(function() Scene.FindEntityByName('Holder'):GetBoneTransform('Tip') end))
			return 'ok'
		)") == "ok");
		CHECK(CountLogMessages(since, "GetBoneTransform on 'Rig': joint 'Nope' not found") == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Bone attachment problems are reported once and leave the entity where it is")
	{
		ClearAssetCache clear;
		Scene scene;
		auto a = AddAnimator(scene, "A");
		auto b = AddAnimator(scene, "B");
		a.AddComponent<BoneAttachmentComponent>() = { b.GetUUID(), "Tip" };
		b.AddComponent<BoneAttachmentComponent>() = { a.GetUUID(), "Tip" };
		auto self = AddAnimator(scene, "Self");
		self.AddComponent<BoneAttachmentComponent>() = { self.GetUUID(), "Tip" };
		auto lost = scene.CreateEntity("Lost");
		lost.AddComponent<BoneAttachmentComponent>().Joint = "Tip";
		auto wrongJoint = scene.CreateEntity("WrongJoint");
		wrongJoint.AddComponent<BoneAttachmentComponent>() = { a.GetUUID(), "Nope" };
		auto plain = scene.CreateEntity("Plain");
		auto noAnimator = scene.CreateEntity("NoAnimator");
		noAnimator.AddComponent<BoneAttachmentComponent>() = { plain.GetUUID(), "Tip" };
		auto dynamic = scene.CreateEntity("Dynamic");
		dynamic.AddComponent<BoneAttachmentComponent>() = { a.GetUUID(), "Tip" };
		dynamic.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		// A weapon root that targets the rig it holds would move with its own target.
		auto weapon = scene.CreateEntity("Weapon");
		auto held = scene.CreateChildEntity(weapon, "HeldRig");
		held.AddComponent<AnimatorComponent>().Mesh = FixturePath().string();
		weapon.AddComponent<BoneAttachmentComponent>() = { held.GetUUID(), "Tip", { 0, 1, 0 } };
		auto emptyJoint = scene.CreateEntity("EmptyJoint");
		emptyJoint.AddComponent<BoneAttachmentComponent>().Target = a.GetUUID();
		for (auto entity : { a, b, self, lost, wrongJoint, noAnimator, weapon, emptyJoint })
			entity.GetTransform().Translation = { 3, 4, 5 };

		auto warning = [](Entity entity) {
			const auto warnings = GetAnimationWarnings(entity);
			return warnings.empty() ? std::string() : warnings.back();
		};
		CHECK(warning(self) == "BoneAttachment cannot attach an entity to itself");
		CHECK(warning(lost) == "BoneAttachment has no Target and no parent");
		CHECK(warning(wrongJoint).find("BoneAttachment joint 'Nope' not found in '") == 0);
		CHECK(warning(noAnimator) == "BoneAttachment Target 'Plain' has no Animator");
		CHECK(warning(dynamic).find("Dynamic RigidBody") != std::string::npos);
		CHECK(warning(weapon) == "BoneAttachment Target 'HeldRig' is a descendant of this entity");
		CHECK(warning(emptyJoint) == "BoneAttachment Joint is empty");
		b.GetComponent<BoneAttachmentComponent>().Target = 12345;
		CHECK(warning(b) == "BoneAttachment Target entity not found");
		b.GetComponent<BoneAttachmentComponent>().Target = a.GetUUID();
		auto broken = scene.CreateEntity("BrokenModel");
		broken.AddComponent<AnimatorComponent>().Mesh = "Missing/Model.gltf";
		noAnimator.GetComponent<BoneAttachmentComponent>().Target = broken.GetUUID();
		CHECK(warning(noAnimator).find("BoneAttachment Target 'BrokenModel' model could not be loaded: ") == 0);
		CHECK(warning(a) == "BoneAttachment Target 'B' depends on this entity's attachment (a cycle)");
		// A cycle through an attached ancestor of the target: C targets plain D, whose parent E targets C.
		auto c = AddAnimator(scene, "C");
		auto e = AddAnimator(scene, "E");
		auto d = scene.CreateChildEntity(e, "D");
		d.AddComponent<AnimatorComponent>().Mesh = FixturePath().string();
		c.AddComponent<BoneAttachmentComponent>() = { d.GetUUID(), "Tip" };
		e.AddComponent<BoneAttachmentComponent>() = { c.GetUUID(), "Tip" };
		CHECK(warning(c) == "BoneAttachment Target 'D' depends on this entity's attachment (a cycle)");
		CHECK(warning(e) == "BoneAttachment Target 'C' depends on this entity's attachment (a cycle)");
		CHECK(warning(dynamic).find("Dynamic RigidBody") != std::string::npos);

		const auto since = Log::GetHistory().GetTotalCount();
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		system.Update(.1f);
		system.Update(.1f);
		CHECK(CountLogMessages(since, "BoneAttachment 'A': its attachments form a cycle") == 1);
		CHECK(CountLogMessages(since, "BoneAttachment 'B': its attachments form a cycle") == 1);
		CHECK(CountLogMessages(since, "BoneAttachment 'Self': it cannot attach to itself") == 1);
		CHECK(CountLogMessages(since, "BoneAttachment 'Lost': no Target and no parent") == 1);
		CHECK(CountLogMessages(since, "BoneAttachment 'WrongJoint': Target 'A': joint 'Nope' not found") == 1);
		CHECK(CountLogMessages(since, "BoneAttachment 'NoAnimator': Target 'BrokenModel': model 'Missing/Model.gltf' could not be loaded") == 1);
		CHECK(CountLogMessages(since, "BoneAttachment 'Weapon': its Target 'HeldRig' is one of its descendants") == 1);
		for (auto entity : { a, b, self, lost, wrongJoint, noAnimator, weapon })
			CHECK(entity.GetTransform().Translation == glm::vec3(3, 4, 5));
		// Attachments that only depend on a cycle still follow it.
		const auto& poseA = *system.GetPose(a);
		CHECK(NearMatrix(scene.GetWorldTransform(dynamic), scene.GetWorldTransform(a) * poseA[0]));
		// A zero scale above the entity, or in its result, has no Transform: reported, and the entity stays finite.
		auto flat = scene.CreateEntity("Flat");
		flat.GetTransform().Scale = { 1, 0, 1 };
		auto squashed = scene.CreateChildEntity(flat, "Squashed");
		squashed.AddComponent<BoneAttachmentComponent>() = { a.GetUUID(), "Tip" };
		auto zeroOffset = scene.CreateEntity("ZeroOffset");
		zeroOffset.AddComponent<BoneAttachmentComponent>() = { a.GetUUID(), "Tip", { 0, 0, 0 }, glm::quat(1, 0, 0, 0), { 0, 0, 0 } };
		auto tiny = scene.CreateEntity("Tiny");
		tiny.AddComponent<BoneAttachmentComponent>() = { a.GetUUID(), "Tip", { 0, 0, 0 }, glm::quat(1, 0, 0, 0), { 1e-4f, 1e-4f, 1e-4f } };
		system.Update(.1f);
		CHECK(CountLogMessages(since, "BoneAttachment 'Squashed': its parent 'Flat' has a zero scale or a non-finite world transform") == 1);
		CHECK(CountLogMessages(since, "BoneAttachment 'ZeroOffset': its world transform would have a zero scale or non-finite values") == 1);
		CHECK(squashed.GetTransform().Translation == glm::vec3(0));
		CHECK(zeroOffset.GetTransform().Scale == glm::vec3(1));
		// A non-finite translation (a script can write one) above the attachment or on its target does not spread
		// into it.
		auto far = scene.CreateEntity("Far");
		auto follower = scene.CreateChildEntity(far, "Follower");
		follower.AddComponent<BoneAttachmentComponent>() = { a.GetUUID(), "Tip" };
		far.GetTransform().Translation.x = INFINITY;
		auto farTarget = AddAnimator(scene, "FarTarget");
		farTarget.GetTransform().Translation.y = NAN;
		auto targetFollower = scene.CreateEntity("TargetFollower");
		targetFollower.AddComponent<BoneAttachmentComponent>() = { farTarget.GetUUID(), "Tip" };
		system.Update(.1f);
		CHECK(CountLogMessages(since, "BoneAttachment 'Follower': its parent 'Far' has a zero scale or a non-finite world transform") == 1);
		CHECK(CountLogMessages(since, "BoneAttachment 'TargetFollower': its world transform would have a zero scale or non-finite values") == 1);
		CHECK(follower.GetTransform().Translation == glm::vec3(0));
		CHECK(targetFollower.GetTransform().Translation == glm::vec3(0));
		// A small but valid scale still attaches.
		CHECK(NearMatrix(scene.GetWorldTransform(tiny), scene.GetWorldTransform(a) * (*system.GetPose(a))[0] * glm::scale(glm::mat4(1.0f), glm::vec3(1e-4f)), 1e-6f));
		// Fixing a problem lets the attachment move; breaking it again reports again.
		wrongJoint.GetComponent<BoneAttachmentComponent>().Joint = "Tip";
		system.Update(.1f);
		CHECK(NearMatrix(scene.GetWorldTransform(wrongJoint), scene.GetWorldTransform(a) * (*system.GetPose(a))[0]));
		wrongJoint.GetComponent<BoneAttachmentComponent>().Joint = "Nope";
		system.Update(.1f);
		CHECK(CountLogMessages(since, "BoneAttachment 'WrongJoint': Target 'A': joint 'Nope' not found") == 2);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Animator lists and BoneAttachment validate atomically and round-trip through Lua")
	{
		ClearAssetCache clear;
		Scene scene;
		auto entity = AddAnimator(scene);
		std::string error;
		auto rejects = [&](const json& data, const std::string& expected) {
			CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Animator", data, error));
			CHECK(error == "Animator: " + expected);
		};
		rejects({ { "Events", 3 } }, "field 'Events' must be an array of objects");
		rejects({ { "Events", { 3 } } }, "field 'Events[0]' must be an object");
		rejects({ { "Events", { { { "Time", "soon" } } } } }, "field 'Events[0].Time' must be a number");
		rejects({ { "Events", { { { "Name", "" } } } } }, "Events[0].Name must not be empty");
		rejects({ { "Events", { { { "Time", -1 } } } } }, "Events[0].Time -1 must be finite and nonnegative");
		rejects({ { "BlendSpaces", { { { "Samples", { json::object(), { { "Bogus", 1 } } } } } } } }, "unknown field 'BlendSpaces[0].Samples[1].Bogus' (valid: Clip, X, Y, Speed)");
		rejects({ { "BlendSpaces", { { { "Type", "3D" } } } } }, "field 'BlendSpaces[0].Type' has invalid value '3D' (valid: 1D, 2D)");
		rejects({ { "BlendSpaces", { json::object(), json::object() } } }, "BlendSpaces[1].Name 'BlendSpace' is already used by BlendSpaces[0]");
		rejects({ { "BlendSpaces", { { { "Samples", { { { "Speed", -1 } } } } } } } }, "BlendSpaces[0].Samples[0].Speed -1 must be finite and nonnegative");
		rejects({ { "Events", json::array_t(AnimatorComponent::MaxEvents + 1, json::object()) } }, "field 'Events' has 257 elements (at most 256)");
		rejects({ { "BlendSpaces", { { { "Samples", json::array_t(AnimatorComponent::MaxBlendSamples + 1, json::object()) } } } } }, "field 'BlendSpaces[0].Samples' has 65 elements (at most 64)");
		CHECK(entity.GetComponent<AnimatorComponent>().Events.empty());
		CHECK(entity.GetComponent<AnimatorComponent>().BlendSpaces.empty());

		// Samples may share a position; missing keys keep the element defaults.
		REQUIRE(ComponentRegistry::AddOrPatch(entity, "Animator", { { "BlendSpaces", { { { "Type", "2D" }, { "Samples", { { { "X", 1 } }, { { "X", 1 }, { "Clip", "Bend" } } } } } } }, { "Events", { { { "Time", 1.5 } } } } }, error));
		const auto& animator = entity.GetComponent<AnimatorComponent>();
		REQUIRE(animator.BlendSpaces.size() == 1);
		CHECK(animator.BlendSpaces[0].Name == "BlendSpace");
		CHECK(animator.BlendSpaces[0].Samples[1].Clip == "Bend");
		CHECK(animator.Events[0].Name == "Event");
		CHECK(animator.Events[0].Time == 1.5f);
		// An empty object is an empty list (an empty Lua table cannot say which it is); other fields are kept.
		REQUIRE(ComponentRegistry::AddOrPatch(entity, "Animator", { { "Events", json::object() } }, error));
		CHECK(animator.Events.empty());
		CHECK(animator.BlendSpaces.size() == 1);

		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "BoneAttachment", { { "Translation", { 0, NAN, 0 } } }, error));
		CHECK_FALSE(entity.HasComponent<BoneAttachmentComponent>());

		scene.OnRuntimeStart();
		CHECK(Eval(scene, R"(
			local e = Scene.FindEntityByName('Rig')
			local animator = e:GetComponent('Animator')
			assert(#animator.BlendSpaces == 1 and #animator.BlendSpaces[1].Samples == 2)
			animator.Events = { { Clip = 'Bend', Time = 0.5, Name = 'Hit' } }
			animator.BlendSpaces = {}
			e:SetComponent('Animator', animator)
			animator = e:GetComponent('Animator')
			assert(animator.Events[1].Name == 'Hit' and #animator.BlendSpaces == 0)
			e:SetComponent('Animator', e:GetComponent('Animator'))
			assert(not pcall(function() e:SetComponent('Animator', { Events = { { Time = 'x' } } }) end))
			assert(not e:PlayBlendSpace('Missing', 0))
			assert(not pcall(function() e:PlayBlendSpace('Missing', 0, 'up') end))
			assert(not pcall(function() e:SetBlendSpaceParameter(0, {}) end))
			local weights = e:GetBlendSpaceWeights()
			assert(next(weights) == nil)
			return 'ok'
		)") == "ok");
		scene.OnRuntimeStop();
	}
}
