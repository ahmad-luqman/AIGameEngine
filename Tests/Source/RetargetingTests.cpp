#include <doctest/doctest.h>

#include "AnimationTestUtils.h"
#include "AutomationTestUtils.h"
#include "TestUtils.h"

#include <Basalt/Animation/AnimatedModel.h>
#include <Basalt/Animation/Retargeting.h>
#include <Basalt/Core/JsonUtils.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Math/Math.h>
#include <Basalt/Project/ProjectValidator.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/SceneSerializer.h>

#include <nlohmann/json.hpp>

#include <fstream>

using namespace Basalt;
using namespace BasaltTest;
using AnimationTest::ClearAssetCache;
using AnimationTest::Node;

namespace {
	std::filesystem::path RigPath(const char* name)
	{
		return std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation" / name;
	}

	const AnimationClip& Clip(const MeshSource& source, const std::string& name)
	{
		const auto clip = std::ranges::find(source.Animations, name, &AnimationClip::Name);
		REQUIRE_MESSAGE(clip != source.Animations.end(), name);
		return *clip;
	}

	glm::vec3 Position(const std::vector<glm::mat4>& globals, uint32_t node)
	{
		return glm::vec3(globals[node][3]);
	}

	glm::quat Rotation(const std::vector<glm::mat4>& globals, uint32_t node)
	{
		glm::vec3 translation, scale;
		glm::quat rotation;
		REQUIRE(Math::DecomposeAffineTransform(globals[node], translation, rotation, scale));
		return glm::normalize(rotation);
	}

	bool NearRotation(const glm::quat& a, const glm::quat& b, float epsilon = 1e-4f)
	{
		return std::abs(std::abs(glm::dot(a, b)) - 1.0f) < epsilon;
	}

	// Mapped bones (joint to mapped child) in the target and source.
	const std::pair<const char*, const char*> Bones[][2] = {
		{ { "upperarm_l", "lowerarm_l" }, { "mixamorig:LeftArm", "mixamorig:LeftForeArm" } },
		{ { "lowerarm_l", "hand_l" }, { "mixamorig:LeftForeArm", "mixamorig:LeftHand" } },
		{ { "thigh_l", "calf_l" }, { "mixamorig:LeftUpLeg", "mixamorig:LeftLeg" } },
		{ { "thigh_r", "calf_r" }, { "mixamorig:RightUpLeg", "mixamorig:RightLeg" } },
		{ { "spine_01", "spine_02" }, { "mixamorig:Spine", "mixamorig:Spine1" } },
		{ { "neck_01", "head" }, { "mixamorig:Neck", "mixamorig:Head" } },
	};
}

TEST_SUITE("Retargeting")
{
	TEST_CASE("Humanoid bones are recognized across rig naming conventions")
	{
		const std::pair<const char*, const char*> cases[] = {
			{ "mixamorig:Hips", "Hips" },
			{ "pelvis", "Hips" },
			{ "J_Bip_C_Hips", "Hips" },
			{ "mixamorig:Spine", "Spine" },
			{ "spine_01", "Spine" },
			{ "mixamorig:Spine1", "Chest" },
			{ "spine_02", "Chest" },
			{ "Chest", "Chest" },
			{ "spine_03", "UpperChest" },
			{ "neck_01", "Neck" },
			{ "Armature|head", "Head" },
			{ "clavicle_l", "LeftShoulder" },
			{ "shoulder.R", "RightShoulder" },
			{ "mixamorig:LeftArm", "LeftUpperArm" },
			{ "upper_arm.L", "LeftUpperArm" },
			{ "J_Bip_R_UpperArm", "RightUpperArm" },
			{ "mixamorig1:RightForeArm", "RightLowerArm" },
			{ "lowerarm_l", "LeftLowerArm" },
			{ "Bip01 L Forearm", "LeftLowerArm" },
			{ "hand_r", "RightHand" },
			{ "mixamorig:LeftUpLeg", "LeftUpperLeg" },
			{ "thigh_l", "LeftUpperLeg" },
			{ "DEF-thigh.R", "RightUpperLeg" },
			{ "mixamorig:LeftLeg", "LeftLowerLeg" },
			{ "calf_r", "RightLowerLeg" },
			{ "shin.L", "LeftLowerLeg" },
			{ "LeftLowerLeg", "LeftLowerLeg" },
			{ "foot_l", "LeftFoot" },
			{ "mixamorig:RightToeBase", "RightToes" },
			{ "ball_l", "LeftToes" },
			// Not humanoid bones: fingers, twist bones, sided centre names, unsided limbs, props.
			{ "mixamorig:LeftHandThumb1", "" },
			{ "upperarm_twist_01_l", "" },
			{ "Left_Hips", "" },
			{ "thigh", "" },
			{ "weapon_r", "" },
			{ "root", "" },
		};
		for (const auto& [name, bone] : cases)
			CHECK_MESSAGE(Retargeting::HumanoidBone(name) == bone, name);
		CHECK(Retargeting::StripNamespace("mixamorig:Hips") == "Hips");
		CHECK(Retargeting::StripNamespace("Armature|Root:Spine") == "Spine");
		CHECK(Retargeting::StripNamespace("Hips") == "Hips");
	}

	TEST_CASE("Skeletons map by joint map, then name, then humanoid bone, with the root scaled by leg length")
	{
		const auto target = AnimationTest::LoadModel(RigPath("UnrealRig.gltf"));
		const auto source = AnimationTest::LoadModel(RigPath("MixamoRig.gltf"));
		std::vector<std::string> warnings;
		RetargetMap map = Retargeting::BuildMap(*target, *source, {}, warnings);
		CHECK(warnings.empty());
		const auto mapped = [&](const char* targetJoint) {
			const int32_t node = map.SourceOf[Node(*target, targetJoint)];
			return node < 0 ? std::string() : source->Nodes[node].Name;
		};
		CHECK(mapped("pelvis") == "mixamorig:Hips");
		CHECK(mapped("spine_02") == "mixamorig:Spine1");
		CHECK(mapped("upperarm_l") == "mixamorig:LeftArm");
		CHECK(mapped("ball_r") == "mixamorig:RightToeBase");
		CHECK(mapped("clavicle_l").empty());
		CHECK(mapped("root").empty());
		CHECK(map.MappedCount() == 19); // Hips to head (5), two arms (6) and two legs (8).
		CHECK(map.TargetRoot == static_cast<int32_t>(Node(*target, "pelvis")));
		CHECK(map.SourceRoot == static_cast<int32_t>(Node(*source, "mixamorig:Hips")));
		// Hips 1.4 m above the toes against 1.0 m.
		CHECK(map.RootScale == doctest::Approx(1.4f));
		CHECK(std::string(map.Reasons[Node(*target, "pelvis")]) == "humanoid");

		// Explicit pairs come first and take both joints; names that are missing warn.
		map = Retargeting::BuildMap(*target, *source, { { "mixamorig:Spine1", "clavicle_l" }, { "mixamorig:Nope", "head" } }, warnings);
		CHECK(mapped("clavicle_l") == "mixamorig:Spine1");
		CHECK(mapped("spine_02").empty());
		CHECK(std::string(map.Reasons[Node(*target, "clavicle_l")]) == "joint map");
		REQUIRE(warnings.size() == 1);
		CHECK(warnings[0].find("'mixamorig:Nope'") != std::string::npos);

		// The same skeleton maps by name, with nothing to scale.
		const RetargetMap self = Retargeting::BuildMap(*source, *source, {}, warnings);
		CHECK(self.MappedCount() == source->Nodes.size());
		CHECK(self.RootScale == 1.0f);
		CHECK(std::string(self.Reasons[Node(*source, "mixamorig:Hips")]) == "name");
	}

	TEST_CASE("Baked clips turn mapped joints like the source joints from their own rest frames")
	{
		const auto target = AnimationTest::LoadModel(RigPath("UnrealRig.gltf"));
		const auto source = AnimationTest::LoadModel(RigPath("MixamoRig.gltf"));
		std::vector<std::string> warnings;
		const RetargetMap map = Retargeting::BuildMap(*target, *source, {}, warnings);
		std::vector<glm::mat4> targetRest, sourceRest;
		REQUIRE(EvaluateAnimation(*target, nullptr, 0, targetRest));
		REQUIRE(EvaluateAnimation(*source, nullptr, 0, sourceRest));
		for (const char* name : { "Wave", "Walk", "Nod" })
		{
			CAPTURE(name);
			const AnimationClip& clip = Clip(*source, name);
			AnimationClip baked;
			std::string error;
			REQUIRE_MESSAGE(Retargeting::BakeClip(*target, *source, map, clip, baked, error), error);
			CHECK(baked.Name == name);
			CHECK(baked.Duration == clip.Duration);
			for (float time : { 0.0f, 0.2f, 0.5f, 0.75f, 1.0f })
			{
				CAPTURE(time);
				std::vector<glm::mat4> targetPose, sourcePose;
				REQUIRE(EvaluateAnimation(*target, &baked, time, targetPose));
				REQUIRE(EvaluateAnimation(*source, &clip, time, sourcePose));
				// Both rigs stand in the same rest pose, so their bones point the same way at every time.
				for (const auto& [targetBone, sourceBone] : Bones)
				{
					CAPTURE(targetBone.first);
					const glm::vec3 a = glm::normalize(Position(targetPose, Node(*target, targetBone.second)) - Position(targetPose, Node(*target, targetBone.first)));
					const glm::vec3 b = glm::normalize(Position(sourcePose, Node(*source, sourceBone.second)) - Position(sourcePose, Node(*source, sourceBone.first)));
					CHECK(glm::all(glm::epsilonEqual(a, b, 2e-3f)));
					// Each joint turns as much as its source, in model space, from its rest orientation.
					const glm::quat turn = Rotation(targetPose, Node(*target, targetBone.first)) * glm::inverse(Rotation(targetRest, Node(*target, targetBone.first)));
					const glm::quat expected = Rotation(sourcePose, Node(*source, sourceBone.first)) * glm::inverse(Rotation(sourceRest, Node(*source, sourceBone.first)));
					CHECK(NearRotation(turn, expected, 2e-4f));
				}
				// Limbs keep the target's lengths.
				CHECK(glm::distance(Position(targetPose, Node(*target, "thigh_l")), Position(targetPose, Node(*target, "calf_l"))) == doctest::Approx(0.65f).epsilon(1e-4));
				// The hips move by the source's displacement times the leg ratio.
				const glm::vec3 sourceMove = Position(sourcePose, Node(*source, "mixamorig:Hips")) - Position(sourceRest, Node(*source, "mixamorig:Hips"));
				const glm::vec3 targetMove = Position(targetPose, Node(*target, "pelvis")) - Position(targetRest, Node(*target, "pelvis"));
				CHECK(glm::all(glm::epsilonEqual(targetMove, sourceMove * 1.4f, 1e-4f)));
			}
		}

		// Channels follow the source: Wave moves only the arm, so layers leave the rest alone.
		AnimationClip wave;
		std::string error;
		REQUIRE(Retargeting::BakeClip(*target, *source, map, Clip(*source, "Wave"), wave, error));
		const auto channels = AnimatedChannels(*target, wave);
		CHECK(channels[Node(*target, "upperarm_l")] == AnimatedChannel::Rotation);
		CHECK(channels[Node(*target, "lowerarm_l")] == AnimatedChannel::Rotation);
		CHECK(channels[Node(*target, "pelvis")] == 0);
		CHECK(channels[Node(*target, "thigh_l")] == 0);
		CHECK(channels[Node(*target, "clavicle_l")] == 0);
		// Walk moves the hips, which carry the translation.
		AnimationClip walk;
		REQUIRE(Retargeting::BakeClip(*target, *source, map, Clip(*source, "Walk"), walk, error));
		CHECK(AnimatedChannels(*target, walk)[Node(*target, "pelvis")] == AnimatedChannel::Translation);
		// The cubic Nod is sampled between its two keys.
		AnimationClip nod;
		REQUIRE(Retargeting::BakeClip(*target, *source, map, Clip(*source, "Nod"), nod, error));
		REQUIRE_FALSE(nod.Tracks.empty());
		CHECK(nod.Tracks[0].Times.size() > 2);
		CHECK(nod.Tracks[0].Interpolation == AnimationInterpolation::Linear);
	}

	TEST_CASE("Morph weight tracks retarget by node and target name")
	{
		const auto target = AnimationTest::LoadModel(std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/Morph.gltf");
		std::ifstream stream(std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/Morph.gltf");
		auto data = nlohmann::json::parse(stream);
		// The library lists Face's targets the other way round and has a target the model lacks.
		data["meshes"][0]["extras"]["targetNames"] = { "Blink", "Smile" };
		data["meshes"][1]["extras"]["targetNames"] = { "Grin" };
		TempDirectory directory("RetargetMorph");
		const auto path = directory.GetPath() / "Library.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		const auto source = AnimationTest::LoadModel(path);
		std::vector<std::string> warnings;
		const RetargetMap map = Retargeting::BuildMap(*target, *source, {}, warnings);
		AnimationClip baked;
		std::string error;
		REQUIRE(Retargeting::BakeClip(*target, *source, map, Clip(*source, "Talk"), baked, error));
		const AnimationLocalPose pose = SampleAnimation(*target, &baked, 1.0f);
		const auto face = static_cast<size_t>(target->Nodes[Node(*target, "Face")].MorphWeightOffset);
		const auto teeth = static_cast<size_t>(target->Nodes[Node(*target, "Teeth")].MorphWeightOffset);
		// The library's first weight (1 at t = 1) drives its "Blink", the model's second target.
		CHECK(pose.MorphWeights[face] == doctest::Approx(0.5f));
		CHECK(pose.MorphWeights[face + 1] == doctest::Approx(1.0f));
		// Teeth's "Grin" matches nothing, so its weight stays at the default.
		CHECK(pose.MorphWeights[teeth] == 0.0f);
		CHECK(std::ranges::none_of(baked.Tracks, [&](const AnimationTrack& track) { return track.Node == Node(*target, "Teeth"); }));
	}

	TEST_CASE("Animators play clip libraries after their own clips, sharing one baked set")
	{
		ClearAssetCache cacheGuard;
		Scene scene;
		const auto addCharacter = [&](const char* name) {
			auto entity = scene.CreateEntity(name);
			auto& animator = entity.AddComponent<AnimatorComponent>();
			animator.Mesh = RigPath("UnrealRig.gltf").string();
			animator.ClipLibraries = { { RigPath("MixamoRig.gltf").string() } };
			animator.Clip = "Wave";
			return entity;
		};
		auto first = addCharacter("First");
		auto second = addCharacter("Second");
		const auto since = Log::GetHistory().GetTotalCount();
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		// The model's Idle hides the library's (warned once for the shared set).
		CHECK(system.GetClips(first) == std::vector<std::string>{ "Idle", "Wave", "Walk", "Nod" });
		CHECK(CountLogMessages(since, "already has clip Idle") == 1);
		system.Update(0.5f);

		const auto target = AssetManager::GetMesh(RigPath("UnrealRig.gltf").string());
		const auto source = AssetManager::GetMesh(RigPath("MixamoRig.gltf").string());
		std::vector<std::string> warnings;
		AnimationClip baked;
		std::string error;
		REQUIRE(Retargeting::BakeClip(*target, *source, Retargeting::BuildMap(*target, *source, {}, warnings), Clip(*source, "Wave"), baked, error));
		std::vector<glm::mat4> expected;
		REQUIRE(EvaluateAnimation(*target, &baked, 0.5f, expected));
		CHECK(AnimationTest::NearPose(*system.GetPose(first), expected));
		CHECK(AnimationTest::NearPose(*system.GetPose(second), expected));

		// Other clip-name consumers see library clips too.
		REQUIRE(system.CrossFade(first, "Walk", 0.25f));
		REQUIRE(system.Play(second, "Idle"));
		CHECK(system.GetDuration(first) == doctest::Approx(1.0f));

		// A changed joint map rebuilds the set and restarts playback.
		first.GetComponent<AnimatorComponent>().RetargetJoints = { { "mixamorig:Spine1", "clavicle_l" } };
		system.Update(0);
		CHECK(system.GetClips(first).size() == 4);

		// A missing library is reported and its clips are gone; the model's still play.
		auto broken = scene.CreateEntity("Broken");
		auto& animator = broken.AddComponent<AnimatorComponent>();
		animator.Mesh = RigPath("UnrealRig.gltf").string();
		animator.ClipLibraries = { { "Missing/Library.gltf" }, { "" } };
		CHECK(system.GetClips(broken) == std::vector<std::string>{ "Idle" });
		CHECK(CountLogMessages(since, "clip library 'Missing/Library.gltf' cannot load") == 1);
		const auto animatorWarnings = GetAnimationWarnings(broken);
		CHECK(std::ranges::any_of(animatorWarnings, [](const std::string& warning) { return warning.find("clip library 'Missing/Library.gltf' could not be loaded") != std::string::npos; }));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Clip library references are saved by identity, follow moves and are validated")
	{
		// List elements declare their asset fields to every asset-field consumer.
		const auto& fields = ComponentRegistry::Find("Animator")->AssetFields;
		CHECK(fields == std::vector<std::string>{ "Mesh", "StateMachine", "ClipLibraries[].Mesh" });
		TempProject project("RetargetReferences");
		ClearAssetCache cacheGuard;
		CommandRegistry registry;
		AutomationSession session;
		using BasaltTest::Run;
		for (const char* name : { "MixamoRig.gltf", "UnrealRig.gltf" })
		{
			const auto text = FileSystem::ReadTextFile(RigPath(name));
			REQUIRE(text);
			REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(std::string("Assets/Models/") + name), *text));
		}
		Run(registry, session, "scene.new", { { "name", "Library" }, { "empty", true } });
		Run(registry, session, "entity.create", { { "name", "Hero" } });
		const nlohmann::json animator = { { "Mesh", "Assets/Models/UnrealRig.gltf" }, { "ClipLibraries", { { { "Mesh", "Assets/Models/MixamoRig.gltf" } } } }, { "Clip", "Wave" } };
		Run(registry, session, "component.set", { { "entity", "Hero" }, { "component", "Animator" }, { "data", animator } });
		// A path-only copy, as older tools wrote scenes.
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Scenes/Legacy.bscene"), SceneSerializer::SerializeScene(*session.GetScene()).dump()));
		Run(registry, session, "scene.save", { { "path", "Assets/Scenes/Library.bscene" } });
		const auto saved = FileSystem::ReadTextFile(Project::ResolvePath("Assets/Scenes/Library.bscene"));
		REQUIRE(saved);
		const auto library = ParseJson(*saved)["Entities"][0]["Components"]["Animator"]["ClipLibraries"][0]["Mesh"];
		CHECK(library.is_object());
		CHECK(library["Path"] == "Assets/Models/MixamoRig.gltf");

		Run(registry, session, "asset.move", { { "from", "Assets/Models/MixamoRig.gltf" }, { "to", "Assets/Clips/Mixamo.gltf" } });
		CHECK(session.GetScene()->GetAllEntitiesOrdered()[0].GetComponent<AnimatorComponent>().ClipLibraries[0].Mesh == "Assets/Clips/Mixamo.gltf");
		std::string error;
		for (const char* scene : { "Assets/Scenes/Library.bscene", "Assets/Scenes/Legacy.bscene" })
		{
			CAPTURE(scene);
			auto loaded = SceneSerializer::LoadScene(Project::ResolvePath(scene), error);
			REQUIRE_MESSAGE(loaded, error);
			CHECK(loaded->GetAllEntitiesOrdered()[0].GetComponent<AnimatorComponent>().ClipLibraries[0].Mesh == "Assets/Clips/Mixamo.gltf");
		}

		// A library that is gone is an error, like any other asset field.
		Run(registry, session, "component.set", { { "entity", "Hero" }, { "component", "Animator" }, { "data", { { "ClipLibraries", { { { "Mesh", "Assets/Clips/Gone.gltf" } } } } } } });
		Run(registry, session, "scene.save", { { "path", "Assets/Scenes/Library.bscene" } });
		const ValidationReport report = ValidateProject(*Project::GetActive());
		CHECK(std::ranges::any_of(report.GetIssues(), [](const ValidationIssue& issue) {
			return issue.Severity == ValidationSeverity::Error && issue.File == "Assets/Scenes/Library.bscene" && issue.Message.find("the missing file 'Assets/Clips/Gone.gltf'") != std::string::npos;
		}));
	}

	TEST_CASE("Animator warnings resolve clip names in libraries and report unmatched skeletons")
	{
		ClearAssetCache cacheGuard;
		Scene scene;
		auto entity = scene.CreateEntity("Character");
		auto& animator = entity.AddComponent<AnimatorComponent>();
		animator.Mesh = RigPath("UnrealRig.gltf").string();
		animator.Clip = "Walk";
		const auto has = [&](const std::string& text) {
			const auto warnings = GetAnimationWarnings(entity);
			return std::ranges::any_of(warnings, [&](const std::string& warning) { return warning.find(text) != std::string::npos; });
		};
		CHECK(has("clip 'Walk' not found"));
		animator.ClipLibraries = { { RigPath("MixamoRig.gltf").string() } };
		CHECK_FALSE(has("clip 'Walk' not found"));
		CHECK(has("clip 'Idle' is hidden by an earlier clip"));

		animator.ClipLibraries = { { std::filesystem::path(BASALT_TEST_DATA_DIR).append("Animation/TwoJoint.gltf").string() } };
		CHECK(has("no joint matches the model's skeleton"));
		animator.ClipLibraries.clear();
		animator.RetargetJoints = { { "a", "b" } };
		CHECK(has("RetargetJoints has no effect without ClipLibraries"));
	}
}
