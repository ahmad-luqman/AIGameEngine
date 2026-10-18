#include <doctest/doctest.h>

#include "TestUtils.h"

#include <Basalt/Animation/AnimatedModel.h>
#include <Basalt/Animation/AnimationPose.h>
#include <Basalt/Animation/AnimationSystem.h>
#include <Basalt/Animation/InverseKinematics.h>
#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/MeshImporter.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Math/Math.h>
#include <Basalt/Physics/PhysicsWorld.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scripting/ScriptEngine.h>

#include <glm/gtc/epsilon.hpp>

using namespace Basalt;
using namespace BasaltTest;
using json = nlohmann::json;

namespace {
	std::filesystem::path BipedPath()
	{
		return std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/Biped.gltf";
	}
	Ref<MeshSource> Load(const std::filesystem::path& path)
	{
		std::string error;
		auto source = MeshImporter::LoadGltf(path, path.string(), error);
		REQUIRE_MESSAGE(source, error);
		return source;
	}
	struct ClearAssetCache
	{
		~ClearAssetCache() { AssetManager::Clear(); }
	};
	uint32_t Node(const MeshSource& source, const std::string& name)
	{
		const auto node = std::ranges::find(source.Nodes, name, &MeshNode::Name);
		REQUIRE(node != source.Nodes.end());
		return static_cast<uint32_t>(node - source.Nodes.begin());
	}
	glm::vec3 Position(const std::vector<glm::mat4>& globals, uint32_t node)
	{
		return glm::vec3(globals[node][3]);
	}
	bool Near(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-4f)
	{
		return glm::all(glm::epsilonEqual(a, b, epsilon));
	}
	bool NearRotation(const glm::quat& a, const glm::quat& b, float epsilon = 1e-4f)
	{
		return std::abs(std::abs(glm::dot(a, b)) - 1.0f) < epsilon;
	}
	float Degrees(const glm::vec3& a, const glm::vec3& b)
	{
		return glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0f, 1.0f)));
	}
	bool SamePose(const std::vector<glm::mat4>& a, const std::vector<glm::mat4>& b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); i++)
			for (int c = 0; c < 4; c++)
				if (!glm::all(glm::epsilonEqual(a[i][c], b[i][c], 1e-5f)))
					return false;
		return true;
	}

	// The rest pose of `source` and its globals.
	struct Rig
	{
		explicit Rig(Ref<MeshSource> source)
			: Source(std::move(source))
			, Pose(SampleAnimation(*Source, nullptr, 0))
		{
			REQUIRE(ComposeAnimationPose(*Source, Pose, Globals));
		}
		Ref<MeshSource> Source;
		AnimationLocalPose Pose;
		std::vector<glm::mat4> Globals;
	};

	struct Leg
	{
		uint32_t Upper, Middle, End;
	};
	Leg LeftLeg(const MeshSource& source)
	{
		return { Node(source, "LeftUpperLeg"), Node(source, "LeftLowerLeg"), Node(source, "LeftFoot") };
	}

	IKChain Chain(const std::string& name, IKChainType type, const std::string& joint)
	{
		IKChain chain;
		chain.Name = name;
		chain.Type = type;
		chain.Joint = joint;
		return chain;
	}
	Entity AddBiped(Scene& scene, const std::vector<IKChain>& chains)
	{
		auto entity = scene.CreateEntity("Biped");
		auto& animator = entity.AddComponent<AnimatorComponent>();
		animator.Mesh = BipedPath().string();
		// Lean turns only the spine: the legs and arms hold their rest pose, so positions are analytic.
		animator.Clip = "Lean";
		animator.PlayOnStart = false;
		entity.AddComponent<InverseKinematicsComponent>().Chains = chains;
		return entity;
	}
	glm::vec3 BoneWorld(Scene& scene, Entity entity, const std::string& joint)
	{
		glm::mat4 bone(1.0f);
		std::string error;
		REQUIRE_MESSAGE(scene.GetAnimationSystem()->GetBoneTransform(entity, joint, bone, error), error);
		return glm::vec3((scene.GetWorldTransform(entity) * bone)[3]);
	}
	glm::quat BoneWorldRotation(Scene& scene, Entity entity, const std::string& joint)
	{
		glm::mat4 bone(1.0f);
		std::string error;
		REQUIRE_MESSAGE(scene.GetAnimationSystem()->GetBoneTransform(entity, joint, bone, error), error);
		return GlobalRotation(scene.GetWorldTransform(entity) * bone);
	}
	Entity AddBox(Scene& scene, const std::string& name, const glm::vec3& center, const glm::vec3& halfExtents, const glm::vec3& degrees = glm::vec3(0))
	{
		auto box = scene.CreateEntity(name);
		box.GetTransform().Translation = center;
		box.GetTransform().Rotation = Math::QuatFromEuler(glm::radians(degrees));
		box.AddComponent<RigidBodyComponent>();
		box.AddComponent<BoxColliderComponent>().HalfExtents = halfExtents;
		return box;
	}
}

TEST_SUITE("Animation")
{
	TEST_CASE("Two-bone IK reaches targets keeps bone lengths and bends in the animated plane")
	{
		Rig rig(Load(BipedPath()));
		const auto [upper, middle, end] = LeftLeg(*rig.Source);
		const glm::vec3 a = Position(rig.Globals, upper), b = Position(rig.Globals, middle), c = Position(rig.Globals, end);
		const float thigh = glm::length(b - a), shin = glm::length(c - b);

		const glm::vec3 target(0.1f, 0.3f, 0.25f);
		REQUIRE(SolveTwoBoneIK(*rig.Source, rig.Pose, rig.Globals, upper, middle, end, target, glm::vec3(0), std::nullopt, 1));
		CHECK(Near(Position(rig.Globals, end), target));
		CHECK(glm::length(Position(rig.Globals, middle) - a) == doctest::Approx(thigh));
		CHECK(glm::length(Position(rig.Globals, end) - Position(rig.Globals, middle)) == doctest::Approx(shin));
		// The knee keeps bending forward (+Z), in the plane through the hip and the target.
		const glm::vec3 knee = Position(rig.Globals, middle);
		CHECK(knee.z > 0.25f);
		CHECK(knee.x == doctest::Approx(0.1f).epsilon(1e-3));

		// Unreachable: stretched toward the target.
		Rig far(rig.Source);
		const glm::vec3 below(0.1f, -3.0f, 0.0f);
		REQUIRE(SolveTwoBoneIK(*far.Source, far.Pose, far.Globals, upper, middle, end, below, glm::vec3(0), std::nullopt, 1));
		const glm::vec3 reached = Position(far.Globals, end);
		CHECK(glm::length(reached - a) == doctest::Approx(thigh + shin).epsilon(1e-3));
		CHECK(Degrees(reached - a, below - a) < 0.01f);
	}

	TEST_CASE("Two-bone IK weights poles and end rotations")
	{
		const auto source = Load(BipedPath());
		const auto [upper, middle, end] = LeftLeg(*source);
		const glm::vec3 target(0.1f, 0.3f, 0.25f);

		Rig animated(source);
		Rig none(source);
		CHECK_FALSE(SolveTwoBoneIK(*source, none.Pose, none.Globals, upper, middle, end, target, glm::vec3(0), std::nullopt, 0));
		CHECK(SamePose(none.Globals, animated.Globals));
		Rig half(source);
		REQUIRE(SolveTwoBoneIK(*source, half.Pose, half.Globals, upper, middle, end, target, glm::vec3(0), std::nullopt, 0.5f));
		const float start = glm::length(Position(animated.Globals, end) - target);
		const float between = glm::length(Position(half.Globals, end) - target);
		CHECK(between > 0.05f);
		CHECK(between < start - 0.05f);

		// A pole behind the leg turns the knee backward.
		Rig back(source);
		REQUIRE(SolveTwoBoneIK(*source, back.Pose, back.Globals, upper, middle, end, target, glm::vec3(0, 0, -1), std::nullopt, 1));
		CHECK(Near(Position(back.Globals, end), target));
		CHECK(Position(back.Globals, middle).z < 0.15f);
		CHECK(Position(back.Globals, middle).z < Position(back.Globals, upper).z + glm::dot(target - Position(back.Globals, upper), glm::vec3(0, 0, 1)) * 0.5f);

		// A straight chain (the arm) bends toward the pole; without one, toward +Z.
		const uint32_t shoulder = Node(*source, "LeftUpperArm"), elbow = Node(*source, "LeftForeArm"), hand = Node(*source, "LeftHand");
		const glm::vec3 reach(0.55f, 1.55f, 0.0f);
		Rig front(source);
		REQUIRE(SolveTwoBoneIK(*source, front.Pose, front.Globals, shoulder, elbow, hand, reach, glm::vec3(0), std::nullopt, 1));
		CHECK(Near(Position(front.Globals, hand), reach));
		CHECK(Position(front.Globals, elbow).z > 0.1f);
		Rig down(source);
		REQUIRE(SolveTwoBoneIK(*source, down.Pose, down.Globals, shoulder, elbow, hand, reach, glm::vec3(0, -1, 0), std::nullopt, 1));
		CHECK(Near(Position(down.Globals, hand), reach));
		CHECK(Position(down.Globals, elbow).y < 1.45f);

		// The end joint takes the given model-space rotation.
		Rig turned(source);
		const glm::quat rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
		REQUIRE(SolveTwoBoneIK(*source, turned.Pose, turned.Globals, upper, middle, end, target, glm::vec3(0), rotation, 1));
		CHECK(NearRotation(GlobalRotation(turned.Globals[end]), rotation));
	}

	TEST_CASE("Look-at IK aims the axis shares the turn along the chain and limits the angle")
	{
		const auto source = Load(BipedPath());
		const uint32_t head = Node(*source, "Head"), chest = Node(*source, "Chest"), spine = Node(*source, "Spine");
		const glm::vec3 axis(0, 0, 1);
		const glm::vec3 target(2.0f, 1.7f, 0.0f);

		Rig single(source);
		REQUIRE(SolveLookAtIK(*source, single.Pose, single.Globals, { head }, axis, target, glm::pi<float>(), 1));
		const auto aim = [&](const Rig& rig) { return glm::vec3(rig.Globals[head] * glm::vec4(axis, 0)); };
		CHECK(Degrees(aim(single), target - Position(single.Globals, head)) < 0.01f);

		Rig limited(source);
		REQUIRE(SolveLookAtIK(*source, limited.Pose, limited.Globals, { head }, axis, target, glm::radians(30.0f), 1));
		CHECK(Degrees(aim(limited), axis) == doctest::Approx(30.0f).epsilon(1e-3));

		Rig chain(source);
		REQUIRE(SolveLookAtIK(*source, chain.Pose, chain.Globals, { head, chest, spine }, axis, target, glm::pi<float>(), 1));
		CHECK(Degrees(aim(chain), target - Position(chain.Globals, head)) < 0.05f);
		// Each joint turns about a third of the 90 degrees.
		for (uint32_t joint : { spine, chest, head })
		{
			const float turn = glm::degrees(glm::angle(chain.Pose.Nodes[joint].Rotation));
			CHECK(turn == doctest::Approx(30.0f).epsilon(0.1));
		}

		Rig half(source);
		REQUIRE(SolveLookAtIK(*source, half.Pose, half.Globals, { head }, axis, target, glm::pi<float>(), 0.5f));
		CHECK(Degrees(aim(half), axis) == doctest::Approx(45.0f).epsilon(1e-3));
		// A target at the joint has no direction.
		Rig at(source);
		CHECK_FALSE(SolveLookAtIK(*source, at.Pose, at.Globals, { head }, axis, Position(at.Globals, head), glm::pi<float>(), 1));
	}

	TEST_CASE("IK solves the Khronos Fox's rotated and scaled joints")
	{
		const auto source = Load(std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/Fox/Fox.glb");
		Rig rig(source);
		const uint32_t upper = Node(*source, "b_LeftLeg01_015"), middle = Node(*source, "b_LeftLeg02_016"), end = Node(*source, "b_LeftFoot01_017");
		const glm::vec3 a = Position(rig.Globals, upper), b = Position(rig.Globals, middle), c = Position(rig.Globals, end);
		const float thigh = glm::length(b - a), shin = glm::length(c - b);
		// Lift the foot to 80% of the hip-to-foot distance, swung 20 degrees about the bend axis.
		const glm::vec3 bend = glm::normalize(glm::cross(c - a, b - a));
		const glm::vec3 target = a + glm::angleAxis(glm::radians(20.0f), bend) * ((c - a) * 0.8f);
		REQUIRE(SolveTwoBoneIK(*source, rig.Pose, rig.Globals, upper, middle, end, target, glm::vec3(0), std::nullopt, 1));
		const float scale = thigh + shin;
		CHECK(glm::length(Position(rig.Globals, end) - target) < 1e-4f * scale);
		CHECK(glm::length(Position(rig.Globals, middle) - a) == doctest::Approx(thigh).epsilon(1e-3));
		CHECK(glm::length(Position(rig.Globals, end) - Position(rig.Globals, middle)) == doctest::Approx(shin).epsilon(1e-3));

		const uint32_t head = Node(*source, "b_Head_05"), neck = Node(*source, "b_Neck_04");
		const glm::vec3 axis(0, 1, 0);
		const glm::vec3 headPosition = Position(rig.Globals, head);
		const glm::vec3 look = headPosition + glm::vec3(50, 20, 40);
		REQUIRE(SolveLookAtIK(*source, rig.Pose, rig.Globals, { head, neck }, axis, look, glm::pi<float>(), 1));
		const glm::vec3 aim = glm::vec3(rig.Globals[head] * glm::vec4(axis, 0));
		CHECK(Degrees(aim, look - Position(rig.Globals, head)) < 0.05f);
	}

	TEST_CASE("Script IK targets move chains after LateUpdate and follow entities")
	{
		ClearAssetCache clear;
		Scene scene;
		auto biped = AddBiped(scene, { Chain("LeftArm", IKChainType::TwoBone, "LeftHand"), Chain("Look", IKChainType::LookAt, "Head") });
		biped.GetTransform().Translation = { 5, 0, 0 };
		auto handle = scene.CreateEntity("Handle");
		handle.GetTransform().Translation = { 5.3f, 1.2f, 0.3f };
		handle.GetTransform().Rotation = Math::QuatFromEuler(glm::radians(glm::vec3(0, 90, 0)));
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();

		const glm::vec3 animated = BoneWorld(scene, biped, "LeftHand");
		const glm::vec3 target(5.4f, 1.3f, 0.3f);
		REQUIRE(system.SetIKTarget(biped, "LeftArm", target, std::nullopt));
		// Nothing moves until the IK pass.
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), animated));
		system.Update(Step);
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), target, 1e-3f));

		// Following an entity, with its rotation.
		REQUIRE(system.SetIKTarget(biped, "LeftArm", handle, true));
		system.Update(Step);
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), glm::vec3(5.3f, 1.2f, 0.3f), 1e-3f));
		CHECK(NearRotation(BoneWorldRotation(scene, biped, "LeftHand"), GlobalRotation(scene.GetWorldTransform(handle)), 1e-3f));
		handle.GetTransform().Translation = { 5.35f, 1.2f, 0.2f };
		system.Update(Step);
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), glm::vec3(5.35f, 1.2f, 0.2f), 1e-3f));

		// Look at a position.
		REQUIRE(system.SetIKTarget(biped, "Look", glm::vec3(5, 1.7f, -3), std::nullopt));
		system.Update(Step);
		const glm::mat4 headWorld = scene.GetWorldTransform(biped) * (*system.GetPose(biped))[Node(*AssetManager::GetMesh(BipedPath().string()), "Head")];
		// The target is behind: the turn stops at MaxAngle (60 degrees by default).
		CHECK(Degrees(glm::vec3(headWorld * glm::vec4(0, 0, 1, 0)), glm::vec3(0, 0, 1)) == doctest::Approx(60.0f).epsilon(1e-3));

		// A destroyed followed entity idles the chain once; clearing returns the animated pose.
		const auto since = Log::GetHistory().GetTotalCount();
		scene.DestroyEntity(handle);
		system.Update(Step);
		system.Update(Step);
		CHECK(CountLogMessages(since, "its target entity no longer exists") == 1);
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), animated));
		REQUIRE(system.ClearIKTarget(biped, "Look"));
		system.Update(Step);
		CHECK(Degrees(glm::vec3((scene.GetWorldTransform(biped) * (*system.GetPose(biped))[Node(*AssetManager::GetMesh(BipedPath().string()), "Head")]) * glm::vec4(0, 0, 1, 0)), glm::vec3(0, 0, 1)) < 0.01f);

		// Removing the component restores the animation alone.
		REQUIRE(system.SetIKTarget(biped, "LeftArm", target, std::nullopt));
		system.Update(Step);
		biped.RemoveComponent<InverseKinematicsComponent>();
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), animated));
		scene.OnRuntimeStop();
	}

	TEST_CASE("IK weights fade solve from the animated pose and survive re-evaluation")
	{
		ClearAssetCache clear;
		Scene scene;
		auto biped = AddBiped(scene, { Chain("LeftArm", IKChainType::TwoBone, "LeftHand") });
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const glm::vec3 animated = BoneWorld(scene, biped, "LeftHand");
		const glm::vec3 target(0.4f, 1.3f, 0.3f);
		REQUIRE(system.SetIKTarget(biped, "LeftArm", target, std::nullopt));

		// The Animator is not playing, so the IK pass alone writes the pose: it must not compound frame over frame.
		system.Update(Step);
		const auto first = *system.GetPose(biped);
		for (int i = 0; i < 10; i++)
			system.Update(Step);
		CHECK(SamePose(*system.GetPose(biped), first));

		// Re-evaluating the base after the IK pass (Play from an event callback) keeps the solved arm.
		REQUIRE(system.Play(biped, "Lean"));
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), target, 1e-3f));

		REQUIRE(system.SetIKWeight(biped, "LeftArm", 0, 1));
		system.Update(0.5f);
		CHECK(system.GetIKWeight(biped, "LeftArm") == doctest::Approx(0.5f));
		system.Update(0.5f);
		CHECK(system.GetIKWeight(biped, "LeftArm") == doctest::Approx(0.0f));
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), animated));
		// A changed configured weight applies at once.
		biped.GetComponent<InverseKinematicsComponent>().Chains[0].Weight = 0.75f;
		system.Update(Step);
		CHECK(system.GetIKWeight(biped, "LeftArm") == doctest::Approx(0.75f));
		CHECK(glm::length(BoneWorld(scene, biped, "LeftHand") - target) < 0.5f * glm::length(animated - target));
		CHECK_FALSE(Near(BoneWorld(scene, biped, "LeftHand"), target, 1e-3f));

		const auto since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(system.SetIKWeight(biped, "LeftArm", 2, 0));
		CHECK_FALSE(system.SetIKWeight(biped, "LeftArm", 1, -1));
		CHECK_FALSE(system.SetIKTarget(biped, "LeftArm", glm::vec3(std::numeric_limits<float>::infinity()), std::nullopt));
		CHECK_FALSE(system.SetIKTarget(biped, "Missing", target, std::nullopt));
		CHECK_FALSE(system.SetIKTarget(biped, "Missing", target, std::nullopt));
		CHECK_FALSE(system.GetIKWeight(biped, "Missing").has_value());
		CHECK_FALSE(system.ClearIKTarget(biped, "Missing"));
		CHECK(CountLogMessages(since, "no chain named 'Missing'") == 1);
		CHECK(CountLogMessages(since, "target must be finite") == 1);
		CHECK(CountLogMessages(since, "weight must be between 0 and 1") == 2);
		auto plain = scene.CreateEntity("Plain");
		CHECK_FALSE(system.GetIKWeight(plain, "LeftArm").has_value());
		CHECK(CountLogMessages(since, "has no InverseKinematics component") == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("IK reports unresolvable chains once and leaves them out")
	{
		ClearAssetCache clear;
		Scene scene;
		auto lookTooLong = Chain("Spine", IKChainType::LookAt, "Head");
		lookTooLong.ChainLength = 8;
		auto biped = AddBiped(scene, { Chain("Missing", IKChainType::TwoBone, "Tail"), Chain("Empty", IKChainType::TwoBone, ""), Chain("Short", IKChainType::TwoBone, "Spine"), lookTooLong, Chain("LeftArm", IKChainType::TwoBone, "LeftHand") });
		biped.GetComponent<InverseKinematicsComponent>().Pelvis = "Nowhere";
		const auto since = Log::GetHistory().GetTotalCount();
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		for (const char* chain : { "Missing", "Empty", "Short", "Spine" })
			REQUIRE(system.SetIKTarget(biped, chain, glm::vec3(1, 1, 1), std::nullopt));
		REQUIRE(system.SetIKTarget(biped, "LeftArm", glm::vec3(0.4f, 1.3f, 0.3f), std::nullopt));
		for (int i = 0; i < 3; i++)
			system.Update(Step);
		CHECK(CountLogMessages(since, "chain 'Missing': joint 'Tail' not found") == 1);
		CHECK(CountLogMessages(since, "chain 'Empty': Joint is empty") == 1);
		CHECK(CountLogMessages(since, "chain 'Short': joint 'Spine' needs a parent and a grandparent") == 1);
		CHECK(CountLogMessages(since, "chain 'Spine': joint 'Head' has fewer than 7 joints above it") == 1);
		// The valid chain still solves.
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), glm::vec3(0.4f, 1.3f, 0.3f), 1e-3f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Foot placement lands feet on steps lowers the pelvis and aligns to slopes")
	{
		ClearAssetCache clear;
		Scene scene;
		auto left = Chain("LeftFoot", IKChainType::TwoBone, "LeftFoot");
		left.Ground = true;
		auto right = Chain("RightFoot", IKChainType::TwoBone, "RightFoot");
		right.Ground = true;
		auto biped = AddBiped(scene, { left, right });
		auto& ik = biped.GetComponent<InverseKinematicsComponent>();
		ik.Pelvis = "Hips";
		ik.Smoothing = 0.1f;
		// The biped stands 0.2 m up, its right foot on a step and its left foot over the floor below.
		biped.GetTransform().Translation = { 0, 0.2f, 0 };
		AddBox(scene, "Floor", { 0, -0.5f, 0 }, { 5, 0.5f, 5 });
		auto step = AddBox(scene, "Step", { -0.1f, 0.1f, 0 }, { 0.05f, 0.1f, 0.2f });
		// The biped's own collider is not ground.
		biped.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		biped.AddComponent<BoxColliderComponent>().HalfExtents = { 0.4f, 0.05f, 0.4f };
		scene.OnSimulationStart();
		scene.OnUpdate(Step);

		// The left foot reaches down 0.2 m to the floor (its ankle 0.05 m above it), the pelvis drops with it, and
		// the right foot stays on the step.
		CHECK(BoneWorld(scene, biped, "LeftFoot").y == doctest::Approx(0.05f).epsilon(1e-3));
		CHECK(BoneWorld(scene, biped, "RightFoot").y == doctest::Approx(0.25f).epsilon(1e-3));
		CHECK(BoneWorld(scene, biped, "Hips").y == doctest::Approx(1.0f).epsilon(1e-3));
		CHECK(BoneWorld(scene, biped, "LeftFoot").x == doctest::Approx(0.1f).epsilon(1e-3));

		// Offsets ease toward a change rather than snapping.
		scene.DestroyEntity(step);
		scene.OnUpdate(Step);
		scene.OnUpdate(Step);
		const float easing = BoneWorld(scene, biped, "RightFoot").y;
		CHECK(easing < 0.25f);
		CHECK(easing > 0.1f);
		for (int i = 0; i < 120; i++)
			scene.OnUpdate(Step);
		CHECK(BoneWorld(scene, biped, "RightFoot").y == doctest::Approx(0.05f).epsilon(1e-3));
		scene.OnSimulationStop();
	}

	TEST_CASE("Foot placement aligns feet to slopes and lets airborne characters keep their animation")
	{
		ClearAssetCache clear;
		auto foot = Chain("LeftFoot", IKChainType::TwoBone, "LeftFoot");
		foot.Ground = true;
		{
			Scene scene;
			auto biped = AddBiped(scene, { foot });
			// A 20 degree slope through the origin, rising toward +X.
			const float slope = glm::radians(20.0f);
			AddBox(scene, "Slope", { 0.5f * std::sin(slope), -0.5f * std::cos(slope), 0 }, { 3, 0.5f, 3 }, { 0, 0, 20 });
			scene.OnSimulationStart();
			scene.OnUpdate(Step);
			const glm::vec3 up = BoneWorldRotation(scene, biped, "LeftFoot") * glm::vec3(0, 1, 0);
			CHECK(Degrees(up, glm::vec3(-std::sin(glm::radians(20.0f)), std::cos(glm::radians(20.0f)), 0)) < 0.1f);
			// The ankle lands its animated height above the slope where the ray met it.
			CHECK(BoneWorld(scene, biped, "LeftFoot").y == doctest::Approx(0.05f + 0.1f * std::tan(glm::radians(20.0f))).epsilon(1e-2));
			scene.OnSimulationStop();
		}
		for (bool airborne : { false, true })
		{
			CAPTURE(airborne);
			Scene scene;
			auto biped = AddBiped(scene, { foot });
			// The rest leg is straight: reaching lower needs the pelvis.
			biped.GetComponent<InverseKinematicsComponent>().Pelvis = "Hips";
			// A thin character standing (or floating, without gravity) on a pillar between its feet, which are
			// over a floor 0.1 m lower.
			biped.GetTransform().Translation = { 0, airborne ? 0.3f : 0.05f, 0 };
			biped.AddComponent<CharacterControllerComponent>().GravityFactor = airborne ? 0.0f : 1.0f;
			auto& capsule = biped.AddComponent<CapsuleColliderComponent>();
			capsule.Radius = 0.04f;
			capsule.Offset = { 0, 0.55f, 0 };
			AddBox(scene, "Pillar", { 0, -0.5f, 0 }, { 0.04f, 0.5f, 0.04f });
			AddBox(scene, "Floor", { 0, -0.6f, 0 }, { 5, 0.5f, 5 });
			scene.OnSimulationStart();
			for (int i = 0; i < 60; i++)
				scene.OnUpdate(Step);
			REQUIRE(scene.GetPhysicsWorld()->IsCharacterGrounded(biped) == !airborne);
			const glm::vec3 origin = scene.GetWorldTransform(biped)[3];
			// On the ground the foot reaches the floor 0.1 m below the origin; in the air it keeps the animation.
			CHECK(BoneWorld(scene, biped, "LeftFoot").y - origin.y == doctest::Approx(airborne ? 0.05f : -0.05f).epsilon(1e-2));
			scene.OnSimulationStop();
		}
	}

	TEST_CASE("Authoring warnings name unknown layer clips mask joints and IK joints")
	{
		ClearAssetCache clear;
		Scene scene;
		auto longLook = Chain("Look", IKChainType::LookAt, "Head");
		longLook.ChainLength = 6;
		auto biped = AddBiped(scene, { Chain("Arm", IKChainType::TwoBone, "LeftHand"), Chain("Tail", IKChainType::TwoBone, "Tail"), Chain("Short", IKChainType::TwoBone, "Spine"), Chain("Blank", IKChainType::TwoBone, ""), longLook });
		biped.GetComponent<InverseKinematicsComponent>().Pelvis = "Pelvis";
		AnimationLayer layer;
		layer.Name = "Upper";
		layer.Clip = "Missing";
		layer.ReferenceClip = "Lean";
		layer.Mask = { { "Chest", 1.0f }, { "Wing", 1.0f }, { "", 1.0f } };
		AnimationLayer additive;
		additive.Name = "Breath";
		additive.Clip = "Breathe";
		additive.Blend = AnimationLayerBlend::Additive;
		additive.ReferenceClip = "Gone";
		biped.GetComponent<AnimatorComponent>().Layers = { layer, additive };
		const auto warnings = GetAnimationWarnings(biped);
		const auto has = [&](const std::string& text) { return std::ranges::find(warnings, text) != warnings.end(); };
		CAPTURE(warnings);
		CHECK(has("Animator layer 'Upper': clip 'Missing' not found"));
		CHECK(has("Animator layer 'Upper': ReferenceClip only applies to Additive layers"));
		CHECK(has("Animator layer 'Upper': mask joint 'Wing' not found"));
		CHECK(has("Animator layer 'Upper': a mask entry has no Joint and is ignored"));
		CHECK(has("Animator layer 'Breath': reference clip 'Gone' not found"));
		CHECK(has("InverseKinematics chain 'Tail': joint 'Tail' not found"));
		CHECK(has("InverseKinematics chain 'Short': joint 'Spine' needs a parent and a grandparent"));
		CHECK(has("InverseKinematics chain 'Blank': Joint is empty"));
		CHECK(has("InverseKinematics chain 'Look': joint 'Head' has fewer than 5 joints above it"));
		CHECK(has("InverseKinematics Pelvis joint 'Pelvis' not found"));
		CHECK(has("InverseKinematics Pelvis has no effect: no chain is a Ground chain"));
		CHECK(warnings.size() == 11);

		auto loose = scene.CreateEntity("Loose");
		loose.AddComponent<InverseKinematicsComponent>();
		CHECK(GetAnimationWarnings(loose) == std::vector<std::string>{ "InverseKinematics needs an Animator on the same entity" });
	}

	TEST_CASE("InverseKinematics validation names the field")
	{
		Scene scene;
		auto entity = scene.CreateEntity("Rig");
		std::string error;
		const std::pair<json, const char*> cases[] = {
			{ { { "Chains", { { { "Name", "" } } } } }, "Chains[0].Name must not be empty" },
			{ { { "Chains", { { { "Name", "A" } }, { { "Name", "A" } } } } }, "Chains[1].Name 'A' is already used by Chains[0]" },
			{ { { "Chains", { { { "ChainLength", 0 } } } } }, "Chains[0].ChainLength 0 must be between 1 and 8" },
			{ { { "Chains", { { { "ChainLength", 9 } } } } }, "Chains[0].ChainLength 9 must be between 1 and 8" },
			{ { { "Chains", { { { "Axis", { 0, 0, 0 } } } } } }, "Chains[0].Axis must be finite and nonzero" },
			{ { { "Chains", { { { "MaxAngle", 181 } } } } }, "Chains[0].MaxAngle 181 must be between 0 and 180 degrees" },
			{ { { "Chains", { { { "Weight", -0.5 } } } } }, "Chains[0].Weight -0.5 must be between 0 and 1" },
			{ { { "Chains", { { { "Type", "LookAt" }, { "Ground", true } } } } }, "Chains[0]: only TwoBone chains can be Ground chains" },
			{ { { "Chains", { { { "Type", "FABRIK" } } } } }, "Type" },
			{ { { "RayHeight", -1 } }, "RayHeight -1 must be between 0 and 100" },
			{ { { "RayDepth", 101 } }, "RayDepth 101 must be between 0 and 100" },
			{ { { "Smoothing", -0.1 } }, "Smoothing -0.1 must be between 0 and 100" },
		};
		for (const auto& [data, expected] : cases)
		{
			CAPTURE(data.dump());
			CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "InverseKinematics", data, error));
			CHECK_MESSAGE(error.find(expected) != std::string::npos, error);
		}
		CHECK_FALSE(entity.HasComponent<InverseKinematicsComponent>());
		REQUIRE(ComponentRegistry::AddOrPatch(entity, "InverseKinematics", { { "Pelvis", "Hips" }, { "Chains", { { { "Name", "Head" }, { "Type", "LookAt" }, { "Joint", "Head" }, { "ChainLength", 3 }, { "MaxAngle", 45 } } } } }, error));
		const auto& ik = entity.GetComponent<InverseKinematicsComponent>();
		CHECK(ik.Chains.at(0).Type == IKChainType::LookAt);
		CHECK(ik.Chains.at(0).ChainLength == 3);
		const json written = ComponentRegistry::Find("InverseKinematics")->Serialize(entity);
		CHECK(written["Chains"][0]["Type"] == "LookAt");
		CHECK(written["Pelvis"] == "Hips");
	}

	TEST_CASE("Lua sets IK targets and weights")
	{
		ClearAssetCache clear;
		Scene scene;
		AddBiped(scene, { Chain("LeftArm", IKChainType::TwoBone, "LeftHand") });
		scene.CreateEntity("Plain");
		scene.CreateEntity("Handle");
		scene.OnRuntimeStart();
		const char* prefix = "local e = Scene.FindEntityByName('Biped'); ";
		const auto run = [&](const std::string& code) { return Eval(scene, prefix + code); };
		CHECK(run("return tostring(e:SetIKTarget('LeftArm', Vec3(0.4, 1.3, 0.3)))") == "true");
		CHECK(run("return tostring(e:SetIKTarget('LeftArm', Vec3(0.4, 1.3, 0.3), Quat.FromEuler(Vec3(0, 90, 0))))") == "true");
		CHECK(run("return tostring(e:SetIKTarget('LeftArm', Scene.FindEntityByName('Handle'), true))") == "true");
		CHECK(run("return tostring(e:SetIKTarget('LeftArm', Scene.FindEntityByName('Handle')))") == "true");
		CHECK(run("return tostring(e:ClearIKTarget('LeftArm'))") == "true");
		CHECK(run("return tostring(e:SetIKWeight('LeftArm', 0.5))") == "true");
		CHECK(run("return tostring(e:SetIKWeight('LeftArm', 1, 0.5))") == "true");
		CHECK(run("return tostring(e:GetIKWeight('LeftArm'))") == "0.5");
		CHECK(run("return tostring(e:GetIKWeight('Missing'))") == "nil");
		CHECK(run("return tostring(e:SetIKTarget('Missing', Vec3(0, 0, 0)))") == "false");
		const std::pair<const char*, const char*> errors[] = {
			{ "Scene.FindEntityByName('Biped'):SetIKTarget('LeftArm', 5)", "target must be a Vec3 or an Entity" },
			{ "Scene.FindEntityByName('Biped'):SetIKTarget('LeftArm', Vec3(0, 0, 0), 5)", "rotation must be a Quat or nil" },
			{ "Scene.FindEntityByName('Biped'):SetIKTarget('LeftArm', Scene.FindEntityByName('Handle'), 1)", "matchRotation must be true, false or nil" },
			{ "Scene.FindEntityByName('Biped'):SetIKWeight('LeftArm', 1, 'slow')", "seconds must be a number or nil" },
			{ "Scene.FindEntityByName('Plain'):SetIKTarget('LeftArm', Vec3(0, 0, 0))", "needs the InverseKinematics component" },
			{ "Scene.FindEntityByName('Plain'):GetIKWeight('LeftArm')", "needs the InverseKinematics component" },
		};
		CheckLuaErrors(*scene.GetScriptEngine(), errors);
		scene.OnRuntimeStop();
	}
}
