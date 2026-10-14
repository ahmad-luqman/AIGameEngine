#include <doctest/doctest.h>

#include "TestUtils.h"

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
	Entity AddAnimator(Scene& scene, const std::string& name = "Rig")
	{
		auto entity = scene.CreateEntity(name);
		entity.AddComponent<AnimatorComponent>().Mesh = FixturePath().string();
		return entity;
	}
}

TEST_SUITE("Animation")
{
	TEST_CASE("Skin import preserves hierarchy, inverse binds, normalized influences and named clips")
	{
		auto source = LoadFixture();
		REQUIRE(source->Skins.size() == 1);
		CHECK(source->Skins[0].Joints == std::vector<uint32_t>{ 2, 0 });
		CHECK(source->Skins[0].InverseBindMatrices[1][3].y == -1);
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
			EvaluateAnimation(*source, &clip, clip.Duration * .37f, globals);
			for (const auto& matrix : BuildSkinPalette(source->Skins[0], globals))
				for (int c = 0; c < 4; c++)
					for (int r = 0; r < 4; r++)
						CHECK(std::isfinite(matrix[c][r]));
		}
	}

	TEST_CASE("Local pose blending preserves hierarchy and crossfades advance both clips without interruption jumps")
	{
		Scene scene;
		auto entity = AddAnimator(scene);
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.Play(entity, "Step"));
		REQUIRE(system.Seek(entity, 1));
		REQUIRE(system.CrossFade(entity, "Cubic", 1));
		CHECK((*system.GetPose(entity))[2][3].x == 2);
		system.Update(.5f);
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
		SUBCASE("wrong joint component type")
		{
			data["accessors"][3]["componentType"] = 5120;
		}
		SUBCASE("missing weights")
		{
			data["meshes"][0]["primitives"][0]["attributes"].erase("WEIGHTS_0");
		}
		SUBCASE("unsupported influence set")
		{
			data["meshes"][0]["primitives"][0]["attributes"]["JOINTS_1"] = 3;
		}
		SUBCASE("joint outside skin")
		{
			data["skins"][0]["joints"] = { 2 };
			data["skins"][0].erase("inverseBindMatrices");
		}
		SUBCASE("duplicate joint")
		{
			data["skins"][0]["joints"] = { 2, 2 };
		}
		SUBCASE("wrong inverse bind type")
		{
			data["accessors"][6]["type"] = "VEC4";
		}
		SUBCASE("wrong output count")
		{
			data["accessors"][8]["count"] = 2;
		}
		SUBCASE("unbounded zero-initialized times")
		{
			data["accessors"][7].erase("bufferView");
			data["accessors"][7]["count"] = 1000000000;
			data["accessors"][8].erase("bufferView");
			data["accessors"][8]["count"] = 1000000000;
		}
		SUBCASE("wrong time type")
		{
			data["accessors"][7]["componentType"] = 5123;
		}
		SUBCASE("duplicate target")
		{
			data["animations"][0]["channels"].push_back(data["animations"][0]["channels"][0]);
		}
		SUBCASE("morph channel")
		{
			data["animations"][0]["channels"][0]["target"]["path"] = "weights";
		}
		SUBCASE("matrix animated node")
		{
			data["nodes"][0]["matrix"] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
		}
		SUBCASE("cyclic hierarchy")
		{
			data.erase("scenes");
			data.erase("scene");
			data["nodes"][0]["children"] = { 2 };
		}
		std::string error;
		const auto path = directory.GetPath() / "Bad.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		CHECK_FALSE(MeshImporter::LoadGltf(path, "Bad.gltf", error));
		CHECK_FALSE(error.empty());
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
		REQUIRE(source->Skins[0].InverseBindMatrices.size() == 2);
		CHECK(source->Skins[0].InverseBindMatrices[1] == glm::mat4(1));
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
		scene.DestroyEntity(entity);
		scene.OnSimulationStop();
		auto original = CreateRef<Scene>();
		auto rig = AddAnimator(*original);
		auto copy = Scene::Copy(original);
		copy->OnRuntimeStart();
		copy->OnUpdate(.5f);
		CHECK(original->GetAnimationSystem() == nullptr);
		CHECK(copy->GetEntityByUUID(rig.GetUUID()).HasComponent<AnimatorComponent>());
		copy->OnRuntimeStop();
	}

	TEST_CASE("Animator registry validates atomically and Lua controls all playback operations")
	{
		Scene scene;
		auto entity = AddAnimator(scene);
		std::string error;
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Animator", { { "Speed", -1 }, { "Clip", "Bad" } }, error));
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
