#include <doctest/doctest.h>

#include "AnimationTestUtils.h"
#include "AutomationTestUtils.h"
#include "TestUtils.h"

#include <Basalt/Animation/AnimatedModel.h>
#include <Basalt/Animation/AnimationClipSet.h>
#include <Basalt/Animation/Retargeting.h>
#include <Basalt/Asset/Thumbnails.h>
#include <Basalt/Core/JsonUtils.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Project/ProjectValidator.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/SceneSerializer.h>

using namespace Basalt;
using namespace BasaltTest;
using AnimationTest::ClearAssetCache;
using AnimationTest::Clip;
using AnimationTest::FixturePath;
using AnimationTest::GlobalRotation;
using AnimationTest::MutableClip;
using AnimationTest::NearRotation;
using AnimationTest::Node;

namespace {
	glm::vec3 Position(const std::vector<glm::mat4>& globals, uint32_t node)
	{
		return glm::vec3(globals[node][3]);
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

	struct Rigs
	{
		Ref<MeshSource> Target = AnimationTest::LoadModel(FixturePath("UnrealRig.gltf"));
		Ref<MeshSource> Source = AnimationTest::LoadModel(FixturePath("MixamoRig.gltf"));
	};

	AnimationClip Bake(const MeshSource& target, const MeshSource& source, const AnimationClip& clip, std::vector<std::string>* outWarnings = nullptr)
	{
		std::vector<std::string> warnings;
		const RetargetMap map = Retargeting::BuildMap(target, source, {}, warnings);
		AnimationClip baked;
		std::string error;
		REQUIRE_MESSAGE(Retargeting::BakeClip(target, source, map, clip, baked, outWarnings ? *outWarnings : warnings, error), error);
		return baked;
	}

	bool HasText(const std::vector<std::string>& lines, const std::string& text)
	{
		return std::ranges::any_of(lines, [&](const std::string& line) { return line.find(text) != std::string::npos; });
	}

	AnimatorComponent& AddHero(Entity entity, std::vector<AnimationRetargetJoint> joints = {})
	{
		auto& animator = entity.AddComponent<AnimatorComponent>();
		animator.Mesh = FixturePath("UnrealRig.gltf").string();
		animator.ClipLibraries = { { FixturePath("MixamoRig.gltf").string(), std::move(joints) } };
		animator.Clip = "Wave";
		return animator;
	}

}

TEST_SUITE("Retargeting")
{
	TEST_CASE("Humanoid bones are recognized across rig naming conventions")
	{
		const std::pair<const char*, const char*> cases[] = {
			{ "mixamorig:Hips", "Hips" },
			{ "pelvis", "Hips" },
			{ "J_Bip_C_Hips", "Hips" },
			{ "Bip001 Pelvis", "Hips" },
			{ "mixamorig:Spine", "Spine" },
			{ "spine_01", "Spine" },
			{ "mixamorig:Spine1", "Chest" },
			{ "spine_02", "Chest" },
			{ "Chest", "Chest" },
			{ "spine_03", "UpperChest" },
			{ "neck_01", "Neck" },
			{ "Armature|head", "Head" },
			// Rigify's deform spine counts from the pelvis.
			{ "DEF-spine", "Hips" },
			{ "DEF-spine.001", "Spine" },
			{ "DEF-spine.003", "UpperChest" },
			{ "DEF-spine.004", "Neck" },
			{ "DEF-spine.006", "Head" },
			{ "clavicle_l", "LeftShoulder" },
			{ "shoulder.R", "RightShoulder" },
			{ "mixamorig:LeftArm", "LeftUpperArm" },
			{ "upper_arm.L", "LeftUpperArm" },
			{ "J_Bip_R_UpperArm", "RightUpperArm" },
			{ "mixamorig1:RightForeArm", "RightLowerArm" },
			{ "lowerarm_l", "LeftLowerArm" },
			{ "Bip01 L Forearm", "LeftLowerArm" },
			{ "Bip001 R Calf", "RightLowerLeg" },
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
			// Not humanoid bones: fingers, twist bones, sided centre names, unsided limbs, props, the UE5 spine past 03.
			{ "mixamorig:LeftHandThumb1", "" },
			{ "upperarm_twist_01_l", "" },
			{ "Left_Hips", "" },
			{ "thigh", "" },
			{ "weapon_r", "" },
			{ "root", "" },
			{ "spine_05", "" },
		};
		for (const auto& [name, bone] : cases)
			CHECK_MESSAGE(Retargeting::HumanoidBone(name) == bone, name);
		CHECK(Retargeting::StripNamespace("mixamorig:Hips") == "Hips");
		CHECK(Retargeting::StripNamespace("Armature|Root:Spine") == "Spine");
		CHECK(Retargeting::StripNamespace("Hips") == "Hips");
		CHECK(std::string(ToString(RetargetMatch::JointMap)) == "joint map");
		CHECK(std::string(ToString(RetargetMatch::None)) == "none");
	}

	TEST_CASE("Skeletons map by joint map, then name, then humanoid bone, with the root scaled by leg length")
	{
		const Rigs rigs;
		const MeshSource& target = *rigs.Target;
		const MeshSource& source = *rigs.Source;
		std::vector<std::string> warnings;
		RetargetMap map = Retargeting::BuildMap(target, source, {}, warnings);
		CHECK(warnings.empty());
		const auto mapped = [&](const char* targetJoint) {
			const int32_t node = map.SourceOf[Node(target, targetJoint)];
			return node < 0 ? std::string() : source.Nodes[node].Name;
		};
		CHECK(mapped("pelvis") == "mixamorig:Hips");
		CHECK(mapped("spine_02") == "mixamorig:Spine1");
		CHECK(mapped("upperarm_l") == "mixamorig:LeftArm");
		CHECK(mapped("ball_r") == "mixamorig:RightToeBase");
		CHECK(mapped("clavicle_l").empty());
		CHECK(mapped("root").empty());
		CHECK(map.MappedCount() == 19); // Hips to head (5), two arms (6) and two legs (8).
		CHECK(map.TargetRoot == static_cast<int32_t>(Node(target, "pelvis")));
		CHECK(map.SourceRoot == static_cast<int32_t>(Node(source, "mixamorig:Hips")));
		// Hips 1.4 m above the toes against 1.0 m.
		CHECK(map.RootScale == doctest::Approx(1.4f));
		CHECK(map.Matches[Node(target, "pelvis")] == RetargetMatch::Humanoid);
		CHECK(map.Matches[Node(target, "root")] == RetargetMatch::None);
		CHECK(map.TargetRest.Rotations.size() == target.Nodes.size());
		CHECK(map.SourceRest.Positions.size() == source.Nodes.size());

		// Explicit pairs come first and take both joints. Problems with the others are reported, each its own way.
		map = Retargeting::BuildMap(target, source, { { "mixamorig:Spine1", "clavicle_l" }, { "mixamorig:Nope", "nada" }, { "mixamorig:Spine1", "clavicle_r" }, { "mixamorig:Neck", "" }, {} }, warnings);
		CHECK(mapped("clavicle_l") == "mixamorig:Spine1");
		CHECK(mapped("spine_02").empty());
		CHECK(map.Matches[Node(target, "clavicle_l")] == RetargetMatch::JointMap);
		REQUIRE(warnings.size() == 3);
		CHECK(warnings[0].find("Source 'mixamorig:Nope' is not in") != std::string::npos);
		CHECK(warnings[0].find("Target 'nada' is not in") != std::string::npos);
		CHECK(warnings[1].find("'mixamorig:Spine1' -> 'clavicle_r' is skipped") != std::string::npos);
		CHECK(warnings[2].find("needs both a Source and a Target") != std::string::npos);

		// The same skeleton maps by name, with nothing to scale.
		warnings.clear();
		const RetargetMap self = Retargeting::BuildMap(source, source, {}, warnings);
		CHECK(self.MappedCount() == source.Nodes.size());
		CHECK(self.RootScale == 1.0f);
		CHECK(self.Matches[Node(source, "mixamorig:Hips")] == RetargetMatch::Name);
		CHECK(warnings.empty());
	}

	TEST_CASE("Rigs without humanoid hips take the topmost paired node as the root and move it unscaled")
	{
		const auto twoJoint = AnimationTest::LoadModel(FixturePath("TwoJoint.gltf"));
		std::vector<std::string> warnings;
		const RetargetMap map = Retargeting::BuildMap(*twoJoint, *twoJoint, {}, warnings);
		CHECK(warnings.empty());
		CHECK(map.TargetRoot == static_cast<int32_t>(twoJoint->NodeOrder.front()));
		CHECK(map.RootScale == 1.0f);
		// Translation is baked on the root only.
		AnimationClip baked;
		std::string error;
		REQUIRE(Retargeting::BakeClip(*twoJoint, *twoJoint, map, Clip(*twoJoint, "Step"), baked, warnings, error));
		CHECK(std::ranges::none_of(baked.Tracks, [&](const AnimationTrack& track) { return track.Path == AnimationPath::Translation && static_cast<int32_t>(track.Node) != map.TargetRoot; }));

		// Humanoid hips with nothing paired below them cannot be measured, which is worth a warning.
		const Rigs rigs;
		auto legless = CreateRef<MeshSource>(*rigs.Source);
		for (MeshNode& node : legless->Nodes)
			if (node.Name.find("Leg") != std::string::npos || node.Name.find("Foot") != std::string::npos || node.Name.find("Toe") != std::string::npos)
				node.Name = "Prop" + node.Name.substr(node.Name.find(':') + 1);
		warnings.clear();
		const RetargetMap unmeasured = Retargeting::BuildMap(*rigs.Target, *legless, {}, warnings);
		CHECK(unmeasured.RootScale == 1.0f);
		CHECK(HasText(warnings, "is not above any other paired joint"));
	}

	TEST_CASE("Baked clips turn mapped joints like the source joints from their own rest frames")
	{
		const Rigs rigs;
		const MeshSource& target = *rigs.Target;
		const MeshSource& source = *rigs.Source;
		std::vector<glm::mat4> targetRest, sourceRest;
		REQUIRE(EvaluateAnimation(target, nullptr, 0, targetRest));
		REQUIRE(EvaluateAnimation(source, nullptr, 0, sourceRest));
		for (const char* name : { "Wave", "Walk", "Nod" })
		{
			CAPTURE(name);
			const AnimationClip& clip = Clip(source, name);
			const AnimationClip baked = Bake(target, source, clip);
			CHECK(baked.Name == name);
			CHECK(baked.Duration == clip.Duration);
			for (float time : { 0.0f, 0.2f, 0.5f, 0.75f, 1.0f })
			{
				CAPTURE(time);
				std::vector<glm::mat4> targetPose, sourcePose;
				REQUIRE(EvaluateAnimation(target, &baked, time, targetPose));
				REQUIRE(EvaluateAnimation(source, &clip, time, sourcePose));
				// Both rigs stand in the same rest pose, so their bones point the same way at every time.
				for (const auto& [targetBone, sourceBone] : Bones)
				{
					CAPTURE(targetBone.first);
					const glm::vec3 a = glm::normalize(Position(targetPose, Node(target, targetBone.second)) - Position(targetPose, Node(target, targetBone.first)));
					const glm::vec3 b = glm::normalize(Position(sourcePose, Node(source, sourceBone.second)) - Position(sourcePose, Node(source, sourceBone.first)));
					CHECK(glm::all(glm::epsilonEqual(a, b, 2e-3f)));
					// Each joint turns as much as its source, in model space, from its rest orientation.
					const glm::quat turn = GlobalRotation(targetPose, Node(target, targetBone.first)) * glm::inverse(GlobalRotation(targetRest, Node(target, targetBone.first)));
					const glm::quat expected = GlobalRotation(sourcePose, Node(source, sourceBone.first)) * glm::inverse(GlobalRotation(sourceRest, Node(source, sourceBone.first)));
					CHECK(NearRotation(turn, expected, 2e-4f));
				}
				// Limbs keep the target's lengths.
				CHECK(glm::distance(Position(targetPose, Node(target, "thigh_l")), Position(targetPose, Node(target, "calf_l"))) == doctest::Approx(0.65f).epsilon(1e-4));
				// The hips move by the source's displacement times the leg ratio.
				const glm::vec3 sourceMove = Position(sourcePose, Node(source, "mixamorig:Hips")) - Position(sourceRest, Node(source, "mixamorig:Hips"));
				const glm::vec3 targetMove = Position(targetPose, Node(target, "pelvis")) - Position(targetRest, Node(target, "pelvis"));
				CHECK(glm::all(glm::epsilonEqual(targetMove, sourceMove * 1.4f, 1e-4f)));
			}
		}

		// Channels follow the source: Wave moves only the arm, so layers leave the rest alone.
		const AnimationClip wave = Bake(target, source, Clip(source, "Wave"));
		const auto channels = AnimatedChannels(target, wave);
		CHECK(channels[Node(target, "upperarm_l")] == AnimatedChannel::Rotation);
		CHECK(channels[Node(target, "lowerarm_l")] == AnimatedChannel::Rotation);
		CHECK(channels[Node(target, "pelvis")] == 0);
		CHECK(channels[Node(target, "thigh_l")] == 0);
		CHECK(channels[Node(target, "clavicle_l")] == 0);
		CHECK(wave.Tracks[0].Interpolation == AnimationInterpolation::Linear);
		// Walk moves the hips, which carry the translation.
		CHECK(AnimatedChannels(target, Bake(target, source, Clip(source, "Walk")))[Node(target, "pelvis")] == AnimatedChannel::Translation);
		// The cubic Nod is sampled between its two keys.
		const AnimationClip nod = Bake(target, source, Clip(source, "Nod"));
		REQUIRE_FALSE(nod.Tracks.empty());
		CHECK(nod.Tracks[0].Times.size() > 2);
		CHECK(nod.Tracks[0].Interpolation == AnimationInterpolation::Linear);
	}

	TEST_CASE("Stepped clips stay stepped and scale tracks carry the ratio to the source's rest scale")
	{
		const Rigs rigs;
		auto data = AnimationTest::ReadJson(FixturePath("MixamoRig.gltf"));
		auto& wave = data["animations"][0];
		for (auto& sampler : wave["samplers"])
			sampler["interpolation"] = "STEP";
		// LeftArm doubles its rest scale halfway through.
		const size_t arm = Node(*rigs.Source, "mixamorig:LeftArm");
		REQUIRE_FALSE(data["nodes"][arm].contains("scale"));
		const float values[] = { 1, 1, 1, 2, 2, 2, 1, 1, 1 };
		data["buffers"].push_back({ { "byteLength", sizeof(values) }, { "uri", "data:application/octet-stream;base64," + Base64(values, sizeof(values)) } });
		data["bufferViews"].push_back({ { "buffer", data["buffers"].size() - 1 }, { "byteLength", sizeof(values) } });
		data["accessors"].push_back({ { "bufferView", data["bufferViews"].size() - 1 }, { "componentType", 5126 }, { "count", 3 }, { "type", "VEC3" } });
		wave["samplers"].push_back({ { "input", wave["samplers"][0]["input"] }, { "output", data["accessors"].size() - 1 }, { "interpolation", "STEP" } });
		wave["channels"].push_back({ { "sampler", wave["samplers"].size() - 1 }, { "target", { { "node", arm }, { "path", "scale" } } } });
		TempDirectory directory("RetargetStep");
		const auto source = AnimationTest::LoadModified(data, directory.GetPath(), "Stepped.gltf");
		const AnimationClip baked = Bake(*rigs.Target, *source, Clip(*source, "Wave"));
		CHECK(std::ranges::all_of(baked.Tracks, [](const AnimationTrack& track) { return track.Interpolation == AnimationInterpolation::Step; }));
		const AnimationLocalPose pose = SampleAnimation(*rigs.Target, &baked, 0.6f);
		CHECK(AnimationTest::Near(pose.Nodes[Node(*rigs.Target, "upperarm_l")].Scale, glm::vec3(2)));
		CHECK(AnimatedChannels(*rigs.Target, baked)[Node(*rigs.Target, "upperarm_l")] == (AnimatedChannel::Rotation | AnimatedChannel::Scale));
	}

	TEST_CASE("Clips past the bake budget and maps for other skeletons are rejected")
	{
		const Rigs rigs;
		std::vector<std::string> warnings;
		const RetargetMap map = Retargeting::BuildMap(*rigs.Target, *rigs.Source, {}, warnings);
		// A cubic clip a million seconds long would need a 30 Hz grid of 30 million samples.
		AnimationClip huge = Clip(*rigs.Source, "Nod");
		huge.Tracks[0].Times.back() = huge.Duration = 1e6f;
		std::string error;
		CHECK_FALSE(Retargeting::CheckBakeBudget(*rigs.Target, *rigs.Source, map, huge, error));
		CHECK(error.find("exceeds the decoded animation budget") != std::string::npos);
		AnimationClip baked;
		error.clear();
		CHECK_FALSE(Retargeting::BakeClip(*rigs.Target, *rigs.Source, map, huge, baked, warnings, error));
		CHECK(error.find("budget") != std::string::npos);
		// A map built for other skeletons is refused, not read out of bounds.
		const RetargetMap self = Retargeting::BuildMap(*rigs.Source, *rigs.Source, {}, warnings);
		error.clear();
		CHECK_FALSE(Retargeting::BakeClip(*rigs.Target, *rigs.Source, self, Clip(*rigs.Source, "Wave"), baked, warnings, error));
		CHECK(error == "the joint map was built for other skeletons");

		// A set leaves such a clip out with a warning and keeps the rest.
		auto library = CreateRef<MeshSource>(*rigs.Source);
		AnimationClip& nod = MutableClip(*library, "Nod");
		nod.Tracks[0].Times.back() = nod.Duration = 1e6f;
		const auto set = AnimationClipSet::Build(rigs.Target, { AnimationClipSet::Library{ .Entry = { .Mesh = "Library.gltf" }, .Source = library } });
		CHECK(set->Find("Nod") < 0);
		CHECK(set->Find("Wave") >= 0);
		REQUIRE(set->GetLibraries().size() == 1);
		CHECK(HasText(set->GetLibraries()[0].Warnings, "clip 'Nod' cannot be retargeted: the baked clip exceeds"));
	}

	TEST_CASE("Morph weight tracks retarget by node and target name, cubic tangents included")
	{
		const auto target = AnimationTest::LoadModel(FixturePath("Morph.gltf"));
		auto data = AnimationTest::ReadJson(FixturePath("Morph.gltf"));
		// The library names Face's targets Frown and Smile (no Blink), and Teeth's Grin, which the model lacks.
		data["meshes"][0]["extras"]["targetNames"] = { "Frown", "Smile" };
		data["meshes"][1]["extras"]["targetNames"] = { "Grin" };
		TempDirectory directory("RetargetMorph");
		const auto source = AnimationTest::LoadModified(data, directory.GetPath(), "Library.gltf");
		const auto face = static_cast<size_t>(target->Nodes[Node(*target, "Face")].MorphWeightOffset);
		const auto teeth = static_cast<size_t>(target->Nodes[Node(*target, "Teeth")].MorphWeightOffset);

		std::vector<std::string> warnings;
		const AnimationClip talk = Bake(*target, *source, Clip(*source, "Talk"), &warnings);
		const AnimationLocalPose pose = SampleAnimation(*target, &talk, 1.0f);
		// The library's second weight (0.5 at t = 1) drives its "Smile", the model's first target; Blink keeps the
		// node's default.
		CHECK(pose.MorphWeights[face] == doctest::Approx(0.5f));
		CHECK(pose.MorphWeights[face + 1] == doctest::Approx(0.25f));
		// Teeth's "Grin" matches nothing: its weight stays at the default, and the dropped track is reported.
		CHECK(pose.MorphWeights[teeth] == 0.0f);
		CHECK(std::ranges::none_of(talk.Tracks, [&](const AnimationTrack& track) { return track.Node == Node(*target, "Teeth"); }));
		CHECK(HasText(warnings, "the morph weights of 'Teeth' match no node and morph target name"));

		// Cubic tracks keep their keys; unmatched targets get flat tangents around their default.
		const AnimationClip cubic = Bake(*target, *source, Clip(*source, "CubicSmile"));
		const auto track = std::ranges::find(cubic.Tracks, AnimationPath::Weights, &AnimationTrack::Path);
		REQUIRE(track != cubic.Tracks.end());
		CHECK(track->Interpolation == AnimationInterpolation::CubicSpline);
		REQUIRE(track->Weights.size() == track->Times.size() * 3 * track->Width);
		for (size_t key = 0; key < track->Times.size(); key++)
		{
			CHECK(track->Weights[(key * 3 + 0) * 2 + 1] == 0.0f);
			CHECK(track->Weights[(key * 3 + 1) * 2 + 1] == 0.25f);
			CHECK(track->Weights[(key * 3 + 2) * 2 + 1] == 0.0f);
		}
		// The model's Smile follows the library's second weight of CubicSmile at any time.
		const float t = 0.25f;
		const size_t libraryFace = static_cast<size_t>(source->Nodes[Node(*source, "Face")].MorphWeightOffset);
		CHECK(SampleAnimation(*target, &cubic, t).MorphWeights[face] == doctest::Approx(SampleAnimation(*source, &Clip(*source, "CubicSmile"), t).MorphWeights[libraryFace + 1]));
	}

	TEST_CASE("Animators play clip libraries after their own clips, sharing one baked set")
	{
		ClearAssetCache cacheGuard;
		Scene scene;
		auto first = scene.CreateEntity("First");
		AddHero(first);
		auto second = scene.CreateEntity("Second");
		AddHero(second);
		const auto since = Log::GetHistory().GetTotalCount();
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		// The model's Idle hides the library's (warned once for the shared set).
		CHECK(system.GetClips(first) == std::vector<std::string>{ "Idle", "Wave", "Walk", "Nod" });
		CHECK(CountLogMessages(since, "already has clip Idle") == 1);
		CHECK(system.GetClipSetCount() == 1);
		system.Update(0.5f);

		const auto target = AssetManager::GetMesh(FixturePath("UnrealRig.gltf").string());
		const auto source = AssetManager::GetMesh(FixturePath("MixamoRig.gltf").string());
		const AnimationClip wave = Bake(*target, *source, Clip(*source, "Wave"));
		std::vector<glm::mat4> expected;
		REQUIRE(EvaluateAnimation(*target, &wave, 0.5f, expected));
		CHECK(AnimationTest::NearPose(*system.GetPose(first), expected));
		CHECK(AnimationTest::NearPose(*system.GetPose(second), expected));

		// Other clip-name consumers see library clips too.
		REQUIRE(system.CrossFade(first, "Walk", 0.25f));
		REQUIRE(system.Play(second, "Idle"));
		CHECK(system.GetDuration(first) == doctest::Approx(1.0f));

		// A changed joint map rebuilds the set and restarts playback: the clavicle now turns like the source's left arm.
		REQUIRE(system.Play(first, "Wave"));
		system.Update(0.5f);
		const uint32_t clavicle = Node(*target, "clavicle_l");
		const std::vector<glm::mat4> unmapped = { (*system.GetPose(first))[clavicle] };
		first.GetComponent<AnimatorComponent>().ClipLibraries[0].Joints = { { "mixamorig:LeftArm", "clavicle_l" } };
		system.Update(0);
		CHECK(system.GetTime(first) == 0.0f);
		CHECK(system.GetClipSetCount() == 2);
		REQUIRE(system.Play(first, "Wave"));
		system.Update(0.5f);
		CHECK_FALSE(AnimationTest::NearPose({ (*system.GetPose(first))[clavicle] }, unmapped));

		// A reloaded library is a new source: the set is built again and playback restarts.
		AssetManager::Reload(FixturePath("MixamoRig.gltf").string());
		system.Update(0.25f);
		CHECK(system.GetTime(second) == doctest::Approx(0.25f));
		CHECK(system.GetClips(second).size() == 4);

		// A missing library is reported and its clips are gone; the model's still play.
		auto broken = scene.CreateEntity("Broken");
		auto& animator = broken.AddComponent<AnimatorComponent>();
		animator.Mesh = FixturePath("UnrealRig.gltf").string();
		animator.ClipLibraries = { { "Missing/Library.gltf" }, { "" } };
		CHECK(system.GetClips(broken) == std::vector<std::string>{ "Idle" });
		CHECK(CountLogMessages(since, "clip library 'Missing/Library.gltf' adds no clips: cannot load") == 1);
		CHECK(HasText(GetAnimationWarnings(broken), "clip library 'Missing/Library.gltf' adds no clips: cannot load"));
		// Clip-not-found errors name the libraries searched.
		CHECK_FALSE(system.Play(broken, "Dance"));
		CHECK(CountLogMessages(since, "clip 'Dance' not found in '" + FixturePath("UnrealRig.gltf").string() + "' or its 1 clip library") == 1);

		// Sets no playback uses are dropped.
		scene.DestroyEntity(broken);
		scene.DestroyEntity(second);
		first.GetComponent<AnimatorComponent>().ClipLibraries.clear();
		system.Update(0);
		system.Update(0);
		CHECK(system.GetClipSetCount() == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Library clips play through layers, blend spaces, events and root motion")
	{
		ClearAssetCache cacheGuard;
		Scene scene;
		auto hero = scene.CreateEntity("Hero");
		auto& animator = AddHero(hero);
		animator.Clip = "Locomotion";
		animator.BlendSpaces = { AnimationBlendSpace{ .Name = "Locomotion", .Samples = { AnimationBlendSample{ .Clip = "Walk", .X = 0 }, AnimationBlendSample{ .Clip = "Wave", .X = 1 } } } };
		AnimationLayer layer;
		layer.Name = "Arm";
		layer.Clip = "Wave";
		layer.Mask = { AnimationMaskJoint{ .Joint = "upperarm_l" } };
		animator.Layers = { layer };
		animator.Events = { AnimationEvent{ .Clip = "Walk", .Time = 0.25f, .Name = "Step" } };
		animator.RootMotion = true;
		animator.RootJoint = "pelvis";
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const auto target = AssetManager::GetMesh(animator.Mesh);
		std::vector<glm::mat4> rest;
		REQUIRE(EvaluateAnimation(*target, nullptr, 0, rest));
		const glm::vec3 start = scene.GetWorldTransform(hero)[3];
		for (int i = 0; i < 30; i++)
			system.Update(1.0f / 60.0f);
		// The blend space plays Walk at X 0: the event fires and the hips' displacement became root motion.
		const auto events = system.TakeEvents();
		CHECK(std::ranges::any_of(events, [](const AnimationEventRecord& event) { return event.Name == "Step" && event.Clip == "Walk"; }));
		const glm::vec3 moved = glm::vec3(scene.GetWorldTransform(hero)[3]) - start;
		CHECK(glm::length(glm::vec2(moved.x, moved.z)) > 0.05f);
		// The layer turns the upper arm with the library's Wave.
		CHECK_FALSE(NearRotation(GlobalRotation(*system.GetPose(hero), Node(*target, "upperarm_l")), GlobalRotation(rest, Node(*target, "upperarm_l"))));
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
		for (const char* name : { "MixamoRig.gltf", "UnrealRig.gltf", "TwoJoint.gltf" })
		{
			const auto text = FileSystem::ReadTextFile(FixturePath(name));
			REQUIRE(text);
			REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(std::string("Assets/Models/") + name), *text));
		}
		Run(registry, session, "scene.new", { { "name", "Library" }, { "empty", true } });
		Run(registry, session, "entity.create", { { "name", "Hero" } });
		const nlohmann::json animator = { { "Mesh", "Assets/Models/UnrealRig.gltf" }, { "ClipLibraries", { { { "Mesh", "Assets/Models/MixamoRig.gltf" } } } }, { "Clip", "Wave" } };
		Run(registry, session, "component.set", { { "entity", "Hero" }, { "component", "Animator" }, { "data", animator } });
		// Agents can read the mapping without playing.
		const auto info = Run(registry, session, "animator.retarget", { { "entity", "Hero" } });
		REQUIRE(info["libraries"].size() == 1);
		const auto& mapping = info["libraries"][0];
		CHECK(mapping["index"] == 0);
		CHECK(mapping["mesh"] == "Assets/Models/MixamoRig.gltf");
		CHECK(mapping["error"].is_null());
		CHECK(std::ranges::count(mapping["joints"], nlohmann::json{ { "target", "pelvis" }, { "source", "mixamorig:Hips" }, { "match", "humanoid" } }) == 1);
		CHECK(mapping["unmatchedHumanoidBones"] == nlohmann::json{ "clavicle_l", "clavicle_r" });
		CHECK(mapping["root"]["target"] == "pelvis");
		CHECK(mapping["root"]["scale"].get<float>() == doctest::Approx(1.4f));
		CHECK(mapping["clips"] == nlohmann::json{ "Wave", "Walk", "Nod" });
		CHECK(mapping["hiddenClips"] == nlohmann::json{ "Idle" });
		CHECK(mapping["warnings"].empty());

		// Every way a library can fail is in the output.
		const nlohmann::json failingLibraries = { { { "Mesh", "" } }, { { "Mesh", "Assets/Models/Gone.gltf" } }, { { "Mesh", "Assets/Models/TwoJoint.gltf" } }, { { "Mesh", "Assets/Models/MixamoRig.gltf" }, { "Joints", { { { "Source", "mixamorig:Nope" }, { "Target", "pelvis" } } } } } };
		Run(registry, session, "component.set", { { "entity", "Hero" }, { "component", "Animator" }, { "data", { { "ClipLibraries", failingLibraries } } } });
		const auto failing = Run(registry, session, "animator.retarget", { { "entity", "Hero" } })["libraries"];
		REQUIRE(failing.size() == 3);
		CHECK(failing[0]["index"] == 1);
		CHECK(failing[0]["error"].get<std::string>().find("cannot load") == 0);
		CHECK(failing[0]["root"].is_null());
		CHECK(failing[1]["error"].get<std::string>().find("no joint matches") == 0);
		CHECK(failing[1]["clips"].empty());
		CHECK(failing[2]["warnings"][0].get<std::string>().find("Source 'mixamorig:Nope' is not in") != std::string::npos);
		CHECK(BasaltTest::RunError(registry, session, "animator.retarget", { { "entity", "Missing" } }).find("Missing") != std::string::npos);
		Run(registry, session, "entity.create", { { "name", "Plain" } });
		CHECK(BasaltTest::RunError(registry, session, "animator.retarget", { { "entity", "Plain" } }).find("has no Animator") != std::string::npos);
		Run(registry, session, "component.set", { { "entity", "Plain" }, { "component", "Animator" }, { "data", { { "Mesh", "Assets/Models/Gone.gltf" } } } });
		CHECK(BasaltTest::RunError(registry, session, "animator.retarget", { { "entity", "Plain" } }).find("cannot load") != std::string::npos);
		Run(registry, session, "entity.destroy", { { "entity", "Plain" } });
		Run(registry, session, "component.set", { { "entity", "Hero" }, { "component", "Animator" }, { "data", animator } });

		// A path-only copy, as older tools wrote scenes.
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Scenes/Legacy.bscene"), SceneSerializer::SerializeScene(*session.GetScene()).dump()));
		Run(registry, session, "scene.save", { { "path", "Assets/Scenes/Library.bscene" } });
		const auto saved = FileSystem::ReadTextFile(Project::ResolvePath("Assets/Scenes/Library.bscene"));
		REQUIRE(saved);
		const auto library = ParseJson(*saved)["Entities"][0]["Components"]["Animator"]["ClipLibraries"][0]["Mesh"];
		CHECK(library.is_object());
		CHECK(library["Path"] == "Assets/Models/MixamoRig.gltf");
		// Prefabs too, and their thumbnails depend on the library.
		std::string error;
		REQUIRE(SceneSerializer::SavePrefab(session.GetScene()->GetAllEntitiesOrdered()[0], Project::ResolvePath("Assets/Prefabs/Hero.bprefab"), error));
		ThumbnailCache thumbnails(Project::GetActive()->GetDirectory());
		const auto has = [](const std::vector<std::string>& paths, const std::string& path) { return std::ranges::find(paths, path) != paths.end(); };
		CHECK(has(thumbnails.GetDependencies("Assets/Prefabs/Hero.bprefab"), "Assets/Models/MixamoRig.gltf.meta"));

		Run(registry, session, "asset.move", { { "from", "Assets/Models/MixamoRig.gltf" }, { "to", "Assets/Clips/Mixamo.gltf" } });
		CHECK(session.GetScene()->GetAllEntitiesOrdered()[0].GetComponent<AnimatorComponent>().ClipLibraries[0].Mesh == "Assets/Clips/Mixamo.gltf");
		for (const char* scene : { "Assets/Scenes/Library.bscene", "Assets/Scenes/Legacy.bscene" })
		{
			CAPTURE(scene);
			auto loaded = SceneSerializer::LoadScene(Project::ResolvePath(scene), error);
			REQUIRE_MESSAGE(loaded, error);
			CHECK(loaded->GetAllEntitiesOrdered()[0].GetComponent<AnimatorComponent>().ClipLibraries[0].Mesh == "Assets/Clips/Mixamo.gltf");
		}
		Scene prefabScene;
		auto instance = SceneSerializer::InstantiatePrefab(prefabScene, Project::ResolvePath("Assets/Prefabs/Hero.bprefab"), "Assets/Prefabs/Hero.bprefab", {}, error);
		REQUIRE_MESSAGE(instance, error);
		CHECK(instance.GetComponent<AnimatorComponent>().ClipLibraries[0].Mesh == "Assets/Clips/Mixamo.gltf");
		CHECK(has(thumbnails.GetDependencies("Assets/Prefabs/Hero.bprefab"), "Assets/Clips/Mixamo.gltf.meta"));

		// A library that is gone is an error, like any other asset field.
		Run(registry, session, "component.set", { { "entity", "Hero" }, { "component", "Animator" }, { "data", { { "ClipLibraries", { { { "Mesh", "Assets/Clips/Gone.gltf" } } } } } } });
		Run(registry, session, "scene.save", { { "path", "Assets/Scenes/Library.bscene" } });
		const ValidationReport report = ValidateProject(*Project::GetActive());
		CHECK(std::ranges::any_of(report.GetIssues(), [](const ValidationIssue& issue) {
			return issue.Severity == ValidationSeverity::Error && issue.File == "Assets/Scenes/Library.bscene" && issue.Message.find("the missing file 'Assets/Clips/Gone.gltf'") != std::string::npos;
		}));
	}

	TEST_CASE("Clip library joints are validated per library")
	{
		Scene scene;
		auto entity = scene.CreateEntity("Hero");
		entity.AddComponent<AnimatorComponent>();
		std::string error;
		const auto set = [&](const nlohmann::json& joints) { return ComponentRegistry::AddOrPatch(entity, "Animator", { { "ClipLibraries", { { { "Mesh", "a.glb" }, { "Joints", joints } } } } }, error); };
		CHECK(set({ { { "Source", "A" }, { "Target", "pelvis" } }, { { "Source", "B" }, { "Target", "spine" } } }));
		CHECK_FALSE(set({ { { "Source", "A" }, { "Target", "pelvis" } }, { { "Source", "B" }, { "Target", "pelvis" } } }));
		CHECK(error.find("ClipLibraries[0].Joints[1].Target 'pelvis' is already used by Joints[0]") != std::string::npos);
		CHECK_FALSE(set({ { { "Source", "A" }, { "Target", "pelvis" } }, { { "Source", "A" }, { "Target", "spine" } } }));
		CHECK(error.find("Joints[1].Source 'A' is already used") != std::string::npos);
		// Unnamed entries are the inspector's additions.
		CHECK(set({ nlohmann::json::object(), nlohmann::json::object() }));
	}

	TEST_CASE("Animator warnings resolve clip names in libraries and report unmatched skeletons")
	{
		ClearAssetCache cacheGuard;
		Scene scene;
		auto entity = scene.CreateEntity("Character");
		auto& animator = entity.AddComponent<AnimatorComponent>();
		animator.Mesh = FixturePath("UnrealRig.gltf").string();
		animator.Clip = "Walk";
		const auto has = [&](const std::string& text) { return HasText(GetAnimationWarnings(entity), text); };
		CHECK(has("clip 'Walk' not found"));
		animator.ClipLibraries = { { FixturePath("MixamoRig.gltf").string() } };
		CHECK_FALSE(has("clip 'Walk' not found"));
		CHECK(has("clip 'Idle' is hidden by an earlier clip"));
		animator.ClipLibraries[0].Joints = { { "mixamorig:Nope", "pelvis" } };
		CHECK(has("Source 'mixamorig:Nope' is not in"));

		animator.ClipLibraries = { { FixturePath("TwoJoint.gltf").string() } };
		CHECK(has("no joint matches the model's skeleton"));
	}

	TEST_CASE("A library clip that cannot be retargeted leaves its name to a later library's")
	{
		const Rigs rigs;
		// Library A's Wave turns only the unpaired Body node: nothing of the model moves.
		auto data = AnimationTest::ReadJson(FixturePath("MixamoRig.gltf"));
		for (auto& channel : data["animations"][0]["channels"])
			channel["target"]["node"] = Node(*rigs.Source, "Body");
		data["animations"][0]["channels"].erase(1);
		data["animations"][0]["samplers"].erase(1);
		TempDirectory directory("RetargetShadowing");
		const auto stray = AnimationTest::LoadModified(data, directory.GetPath(), "Stray.gltf");
		std::vector<AnimationClipSet::Library> libraries = { { .Index = 0, .Entry = { .Mesh = "Stray.gltf" }, .Source = stray }, { .Index = 1, .Entry = { .Mesh = "Mixamo.gltf" }, .Source = rigs.Source } };
		// Plan and Build agree: A leaves Wave out with a warning and B supplies it.
		AnimationClipSet::Plan(*rigs.Target, libraries);
		CHECK(HasText(libraries[0].Warnings, "clip 'Wave' moves no joint paired with the model"));
		CHECK(std::ranges::find(libraries[1].Clips, std::string("Wave")) != libraries[1].Clips.end());
		// Planning again starts over rather than piling up.
		const size_t warnings = libraries[0].Warnings.size();
		AnimationClipSet::Plan(*rigs.Target, libraries);
		CHECK(libraries[0].Warnings.size() == warnings);
		const auto set = AnimationClipSet::Build(rigs.Target, libraries);
		CHECK(set->Find("Wave") >= 0);
		CHECK(set->GetLibraries()[0].Shadowed.empty() == false); // Its Walk and Nod come first in A.
		CHECK(std::ranges::find(set->GetLibraries()[1].Clips, std::string("Wave")) != set->GetLibraries()[1].Clips.end());
		CHECK(std::ranges::find(set->GetLibraries()[1].Shadowed, std::string("Wave")) == set->GetLibraries()[1].Shadowed.end());
	}

	TEST_CASE("Libraries without clips, rest poses or animatable joints are reported")
	{
		const Rigs rigs;
		auto empty = CreateRef<MeshSource>(*rigs.Source);
		empty->Animations.clear();
		std::vector<AnimationClipSet::Library> libraries = { { .Entry = { .Mesh = "Empty.gltf" }, .Source = empty } };
		AnimationClipSet::Plan(*rigs.Target, libraries);
		CHECK(libraries[0].Error.find("has no animation clips") != std::string::npos);

		// A paired joint given as a matrix cannot be animated.
		auto data = AnimationTest::ReadJson(FixturePath("UnrealRig.gltf"));
		auto& arm = data["nodes"][Node(*rigs.Target, "upperarm_l")];
		const auto translation = arm["translation"];
		arm.erase("translation");
		arm["matrix"] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, translation[0], translation[1], translation[2], 1 };
		TempDirectory directory("RetargetMatrix");
		const auto target = AnimationTest::LoadModified(data, directory.GetPath(), "Matrix.gltf");
		std::vector<std::string> warnings;
		Retargeting::BuildMap(*target, *rigs.Source, {}, warnings);
		CHECK(HasText(warnings, "joint 'upperarm_l' of '"));
		CHECK(HasText(warnings, "is given as a matrix, which cannot be animated"));
	}

	TEST_CASE("The bake budget rejects long cubic clips before sampling them")
	{
		const Rigs rigs;
		std::vector<std::string> warnings;
		const RetargetMap map = Retargeting::BuildMap(*rigs.Target, *rigs.Source, {}, warnings);
		// 5e5 s at 30 Hz is 15M samples: under the clip budget on its own, far past it for 19 paired joints.
		AnimationClip lengthy = Clip(*rigs.Source, "Nod");
		lengthy.Tracks[0].Times.back() = lengthy.Duration = 5e5f;
		std::string error;
		CHECK_FALSE(Retargeting::CheckBakeBudget(*rigs.Target, *rigs.Source, map, lengthy, error));
		CHECK(error.find("budget") != std::string::npos);
		// Ordinary clips pass without building their sample set.
		CHECK(Retargeting::CheckBakeBudget(*rigs.Target, *rigs.Source, map, Clip(*rigs.Source, "Walk"), error));
	}
}
