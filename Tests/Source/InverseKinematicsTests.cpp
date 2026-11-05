#include <doctest/doctest.h>

#include "AnimationTestUtils.h"
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
#include <Basalt/Scene/SceneSerializer.h>
#include <Basalt/Scripting/ScriptEngine.h>

#include <glm/gtc/epsilon.hpp>
#include <random>

using namespace Basalt;
using namespace BasaltTest;
using json = nlohmann::json;

using namespace AnimationTest;

namespace {
	glm::vec3 Position(const std::vector<glm::mat4>& globals, uint32_t node)
	{
		return glm::vec3(globals[node][3]);
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

	bool TwoBone(Rig& rig, uint32_t upper, uint32_t middle, uint32_t end, const glm::vec3& target, const glm::vec3& pole = glm::vec3(0), const std::optional<glm::quat>& rotation = std::nullopt, float weight = 1)
	{
		const TwoBoneIKRequest request{ .Upper = upper, .Middle = middle, .End = end, .Target = target, .Pole = glm::length(pole) > 0 ? std::optional<glm::vec3>(pole) : std::nullopt, .EndRotation = rotation, .Weight = weight };
		return SolveTwoBoneIK(*rig.Source, rig.Pose, rig.Globals, request) == IKSolveResult::Solved;
	}
	bool LookAt(Rig& rig, const std::vector<uint32_t>& joints, const glm::vec3& axis, const glm::vec3& target, float maxAngle, float weight = 1)
	{
		return SolveLookAtIK(*rig.Source, rig.Pose, rig.Globals, { .Joints = joints, .Axis = axis, .Target = target, .MaxAngleRadians = maxAngle, .Weight = weight }) == IKSolveResult::Solved;
	}

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
	glm::quat BoneWorldRotation(Scene& scene, Entity entity, const std::string& joint)
	{
		return GlobalRotation(BoneWorldTransform(scene, entity, joint));
	}
	// The FootLock state of a chain that exists.
	bool Locked(AnimationSystem& system, Entity entity, const std::string& chain)
	{
		bool locked = false;
		REQUIRE(system.GetIKFootLock(entity, chain, locked));
		return locked;
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
		Rig rig(LoadModel(BipedPath()));
		const auto [upper, middle, end] = LeftLeg(*rig.Source);
		const glm::vec3 a = Position(rig.Globals, upper), b = Position(rig.Globals, middle), c = Position(rig.Globals, end);
		const float thigh = glm::length(b - a), shin = glm::length(c - b);

		const glm::vec3 target(0.1f, 0.3f, 0.25f);
		REQUIRE(TwoBone(rig, upper, middle, end, target, glm::vec3(0), std::nullopt, 1));
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
		REQUIRE(TwoBone(far, upper, middle, end, below, glm::vec3(0), std::nullopt, 1));
		const glm::vec3 reached = Position(far.Globals, end);
		CHECK(glm::length(reached - a) == doctest::Approx(thigh + shin).epsilon(1e-3));
		CHECK(Degrees(reached - a, below - a) < 0.01f);
	}

	TEST_CASE("Two-bone IK weights poles and end rotations")
	{
		const auto source = LoadModel(BipedPath());
		const auto [upper, middle, end] = LeftLeg(*source);
		const glm::vec3 target(0.1f, 0.3f, 0.25f);

		Rig animated(source);
		Rig none(source);
		CHECK_FALSE(TwoBone(none, upper, middle, end, target, glm::vec3(0), std::nullopt, 0));
		CHECK(NearPose(none.Globals, animated.Globals));
		Rig half(source);
		REQUIRE(TwoBone(half, upper, middle, end, target, glm::vec3(0), std::nullopt, 0.5f));
		// Half weight turns each joint half as far as the full solve (from the identity rest rotations).
		Rig full(source);
		REQUIRE(TwoBone(full, upper, middle, end, target));
		for (uint32_t joint : { upper, middle })
			CHECK(glm::angle(half.Pose.Nodes[joint].Rotation) == doctest::Approx(glm::angle(full.Pose.Nodes[joint].Rotation) / 2).epsilon(1e-3));
		const float start = glm::length(Position(animated.Globals, end) - target);
		const float between = glm::length(Position(half.Globals, end) - target);
		CHECK(between > 0.3f * start);
		CHECK(between < 0.7f * start);

		// A pole behind the leg turns the knee backward.
		Rig back(source);
		REQUIRE(TwoBone(back, upper, middle, end, target, glm::vec3(0, 0, -1), std::nullopt, 1));
		CHECK(Near(Position(back.Globals, end), target));
		CHECK(Position(back.Globals, middle).z < 0.15f);
		CHECK(Position(back.Globals, middle).z < Position(back.Globals, upper).z + glm::dot(target - Position(back.Globals, upper), glm::vec3(0, 0, 1)) * 0.5f);

		// A straight chain (the arm) bends toward the pole; without one, toward +Z.
		const uint32_t shoulder = Node(*source, "LeftUpperArm"), elbow = Node(*source, "LeftForeArm"), hand = Node(*source, "LeftHand");
		const glm::vec3 reach(0.55f, 1.55f, 0.0f);
		Rig front(source);
		REQUIRE(TwoBone(front, shoulder, elbow, hand, reach, glm::vec3(0), std::nullopt, 1));
		CHECK(Near(Position(front.Globals, hand), reach));
		CHECK(Position(front.Globals, elbow).z > 0.1f);
		Rig down(source);
		REQUIRE(TwoBone(down, shoulder, elbow, hand, reach, glm::vec3(0, -1, 0), std::nullopt, 1));
		CHECK(Near(Position(down.Globals, hand), reach));
		CHECK(Position(down.Globals, elbow).y < 1.45f);

		// The end joint takes the given model-space rotation.
		Rig turned(source);
		const glm::quat rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
		REQUIRE(TwoBone(turned, upper, middle, end, target, glm::vec3(0), rotation, 1));
		CHECK(NearRotation(GlobalRotation(turned.Globals[end]), rotation));
	}

	TEST_CASE("Look-at IK aims the axis shares the turn along the chain and limits the angle")
	{
		const auto source = LoadModel(BipedPath());
		const uint32_t head = Node(*source, "Head"), chest = Node(*source, "Chest"), spine = Node(*source, "Spine");
		const glm::vec3 axis(0, 0, 1);
		const glm::vec3 target(2.0f, 1.7f, 0.0f);

		Rig single(source);
		REQUIRE(LookAt(single, { head }, axis, target, glm::pi<float>(), 1));
		const auto aim = [&](const Rig& rig) { return glm::vec3(rig.Globals[head] * glm::vec4(axis, 0)); };
		CHECK(Degrees(aim(single), target - Position(single.Globals, head)) < 0.01f);

		Rig limited(source);
		REQUIRE(LookAt(limited, { head }, axis, target, glm::radians(30.0f), 1));
		CHECK(Degrees(aim(limited), axis) == doctest::Approx(30.0f).epsilon(1e-3));

		Rig chain(source);
		REQUIRE(LookAt(chain, { head, chest, spine }, axis, target, glm::pi<float>(), 1));
		CHECK(Degrees(aim(chain), target - Position(chain.Globals, head)) < 0.05f);
		// Each joint turns about a third of the 90 degrees.
		for (uint32_t joint : { spine, chest, head })
		{
			const float turn = glm::degrees(glm::angle(chain.Pose.Nodes[joint].Rotation));
			CHECK(turn == doctest::Approx(30.0f).epsilon(0.1));
		}

		Rig half(source);
		REQUIRE(LookAt(half, { head }, axis, target, glm::pi<float>(), 0.5f));
		CHECK(Degrees(aim(half), axis) == doctest::Approx(45.0f).epsilon(1e-3));
		// A target at the joint has no direction.
		Rig at(source);
		CHECK_FALSE(LookAt(at, { head }, axis, Position(at.Globals, head), glm::pi<float>(), 1));
	}

	TEST_CASE("IK solves the Khronos Fox's rotated and scaled joints")
	{
		const auto source = LoadModel(std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/Fox/Fox.glb");
		Rig rig(source);
		const uint32_t upper = Node(*source, "b_LeftLeg01_015"), middle = Node(*source, "b_LeftLeg02_016"), end = Node(*source, "b_LeftFoot01_017");
		const glm::vec3 a = Position(rig.Globals, upper), b = Position(rig.Globals, middle), c = Position(rig.Globals, end);
		const float thigh = glm::length(b - a), shin = glm::length(c - b);
		// Lift the foot to 80% of the hip-to-foot distance, swung 20 degrees about the bend axis.
		const glm::vec3 bend = glm::normalize(glm::cross(c - a, b - a));
		const glm::vec3 target = a + glm::angleAxis(glm::radians(20.0f), bend) * ((c - a) * 0.8f);
		REQUIRE(TwoBone(rig, upper, middle, end, target, glm::vec3(0), std::nullopt, 1));
		const float scale = thigh + shin;
		CHECK(glm::length(Position(rig.Globals, end) - target) < 1e-4f * scale);
		CHECK(glm::length(Position(rig.Globals, middle) - a) == doctest::Approx(thigh).epsilon(1e-3));
		CHECK(glm::length(Position(rig.Globals, end) - Position(rig.Globals, middle)) == doctest::Approx(shin).epsilon(1e-3));

		const uint32_t head = Node(*source, "b_Head_05"), neck = Node(*source, "b_Neck_04");
		const glm::vec3 axis(0, 1, 0);
		const glm::vec3 headPosition = Position(rig.Globals, head);
		const glm::vec3 look = headPosition + glm::vec3(50, 20, 40);
		REQUIRE(LookAt(rig, { head, neck }, axis, look, glm::pi<float>(), 1));
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
		CHECK(NearPose(*system.GetPose(biped), first));

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
		CHECK_FALSE(system.SetIKTarget(biped, "LeftArm", glm::vec3(std::numeric_limits<float>::infinity()), std::nullopt));
		CHECK_FALSE(system.SetIKTarget(biped, "Missing", target, std::nullopt));
		CHECK_FALSE(system.SetIKTarget(biped, "Missing", target, std::nullopt));
		CHECK_FALSE(system.GetIKWeight(biped, "Missing").has_value());
		CHECK_FALSE(system.ClearIKTarget(biped, "Missing"));
		// Once per operation: SetIKTarget, GetIKWeight and ClearIKTarget.
		CHECK(CountLogMessages(since, "no chain named 'Missing'") == 3);
		CHECK(CountLogMessages(since, "target position must be finite") == 1);
		// Value-free: both bad weight calls are one problem.
		CHECK(CountLogMessages(since, "the weight must be between 0 and 1") == 1);
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
		// The pelvis is checked whether or not a foot needs it.
		CHECK(CountLogMessages(since, "Pelvis joint 'Nowhere' not found") == 1);
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
		// The biped's own collider and its children's (a hitbox around the shins) are not ground.
		biped.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		biped.AddComponent<BoxColliderComponent>().HalfExtents = { 0.4f, 0.05f, 0.4f };
		auto hitbox = scene.CreateEntity("Hitbox");
		scene.SetParent(hitbox, biped, false);
		hitbox.GetTransform().Translation = { 0, 0.3f, 0 };
		hitbox.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		hitbox.AddComponent<BoxColliderComponent>().HalfExtents = { 0.4f, 0.05f, 0.4f };
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
		// The foot also locks while it stands, and never in the air.
		auto lockedFoot = foot;
		lockedFoot.FootLock = true;
		for (bool airborne : { false, true })
		{
			CAPTURE(airborne);
			Scene scene;
			auto biped = AddBiped(scene, { lockedFoot });
			// The rest leg is straight: reaching lower needs the pelvis.
			biped.GetComponent<InverseKinematicsComponent>().Pelvis = "Hips";
			// A thin character standing (or floating, without gravity) on a pillar between its feet, which are
			// over a floor 0.1 m lower.
			biped.GetTransform().Translation = { 0, airborne ? 0.3f : 0.05f, 0 };
			biped.AddComponent<CharacterControllerComponent>().GravityFactor = airborne ? 0.0f : 1.0f;
			auto& capsule = biped.AddComponent<CapsuleColliderComponent>();
			capsule.Radius = 0.06f;
			capsule.Offset = { 0, 0.57f, 0 };
			AddBox(scene, "Pillar", { 0, -0.5f, 0 }, { 0.07f, 0.5f, 0.07f });
			AddBox(scene, "Floor", { 0, -0.6f, 0 }, { 5, 0.5f, 5 });
			scene.OnSimulationStart();
			for (int i = 0; i < 60; i++)
				scene.OnUpdate(Step);
			REQUIRE(scene.GetPhysicsWorld()->IsCharacterGrounded(biped) == !airborne);
			const glm::vec3 origin = scene.GetWorldTransform(biped)[3];
			// On the ground the foot reaches the floor 0.1 m below the origin; in the air it keeps the animation.
			CHECK(BoneWorld(scene, biped, "LeftFoot").y - origin.y == doctest::Approx(airborne ? 0.05f : -0.05f).epsilon(1e-2));
			CHECK(Locked(*scene.GetAnimationSystem(), biped, "LeftFoot") == !airborne);
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
		auto& extra = biped.GetComponent<InverseKinematicsComponent>().Chains;
		auto poleOnLook = Chain("PoleLook", IKChainType::LookAt, "LeftHand");
		poleOnLook.Pole = { 0, 0, 1 };
		poleOnLook.MaxAngle = 0;
		auto lengthOnArm = Chain("Arm2", IKChainType::TwoBone, "LeftHand");
		lengthOnArm.ChainLength = 2;
		extra.push_back(poleOnLook);
		extra.push_back(lengthOnArm);
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
		const auto has = [&](const std::string& text) { return std::ranges::any_of(warnings, [&](const std::string& warning) { return warning.starts_with(text); }); };
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
		CHECK(has("InverseKinematics chain 'PoleLook': Pole only applies to TwoBone and Fabrik chains"));
		CHECK(has("InverseKinematics chain 'PoleLook': MaxAngle 0 never turns the chain"));
		CHECK(has("InverseKinematics chain 'Arm2': ChainLength only applies to LookAt and Fabrik chains"));
		CHECK(has("InverseKinematics chains 'Arm' and 'PoleLook' both drive joint 'LeftHand'"));
		CHECK(has("InverseKinematics chains 'Arm' and 'Arm2' both drive joint 'LeftHand'"));
		CHECK(warnings.size() == 16);
		// A pelvis that is not above a ground leg cannot help it reach.
		auto& ik = biped.GetComponent<InverseKinematicsComponent>();
		ik.Chains = { Chain("Foot", IKChainType::TwoBone, "LeftFoot") };
		ik.Chains[0].Ground = true;
		ik.Pelvis = "Spine";
		CHECK(GetAnimationWarnings(biped) == std::vector<std::string>{ "Animator layer 'Upper': clip 'Missing' not found", "Animator layer 'Upper': ReferenceClip only applies to Additive layers",
																	   "Animator layer 'Upper': mask joint 'Wing' not found", "Animator layer 'Upper': a mask entry has no Joint and is ignored", "Animator layer 'Breath': reference clip 'Gone' not found",
																	   "InverseKinematics Pelvis joint 'Spine' is not above the leg starting at 'LeftUpperLeg', so lowering it does not help that foot reach" });

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
			{ { { "Chains", { { { "ChainLength", 0 } } } } }, "Chains[0].ChainLength 0 must be between 1 and 16" },
			{ { { "Chains", { { { "ChainLength", 17 } } } } }, "Chains[0].ChainLength 17 must be between 1 and 16" },
			{ { { "Chains", { { { "Axis", { 0, 0, 0 } } } } } }, "Chains[0].Axis must be finite and nonzero" },
			{ { { "Chains", { { { "MaxAngle", 181 } } } } }, "Chains[0].MaxAngle 181 must be between 0 and 180 degrees" },
			{ { { "Chains", { { { "Weight", -0.5 } } } } }, "Chains[0].Weight -0.5 must be between 0 and 1" },
			{ { { "Chains", { { { "Type", "LookAt" }, { "Ground", true } } } } }, "Chains[0]: only TwoBone and Fabrik chains can be Ground chains" },
			{ { { "Chains", { { { "Type", "LookAt" }, { "FootLock", true } } } } }, "Chains[0]: only TwoBone and Fabrik chains can lock their feet" },
			{ { { "Chains", { { { "Type", "CCD" } } } } }, "Type" },
			{ { { "Target", "Head" } }, "Target" },
			{ { { "LockSpeed", -1 } }, "LockSpeed -1 must be between 0 and 100" },
			{ { { "LockHeight", 101 } }, "LockHeight 101 must be between 0 and 100" },
			{ { { "LockDistance", -0.5 } }, "LockDistance -0.5 must be between 0 and 100" },
			{ { { "RayHeight", -1 } }, "RayHeight -1 must be between 0 and 100" },
			{ { { "RayDepth", 101 } }, "RayDepth 101 must be between 0 and 100" },
			{ { { "Smoothing", -0.1 } }, "Smoothing -0.1 must be between 0 and 100" },
			{ { { "RayHeight", 0 }, { "RayDepth", 0 }, { "Chains", { { { "Joint", "Foot" }, { "Ground", true } } } } }, "RayHeight + RayDepth must be positive for Ground chains" },
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
		CHECK(run("return tostring(e:SetIKTarget('LeftArm', Vec3(0.4, 1.3, 0.3), Quat.AngleAxis(math.pi / 2, Vec3(0, 1, 0))))") == "true");
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

	TEST_CASE("IK reaches world targets on rotated scaled parented and mirrored entities")
	{
		ClearAssetCache clear;
		for (const glm::vec3 scale : { glm::vec3(2.0f), glm::vec3(-1, 1, 1) })
		{
			CAPTURE(scale);
			Scene scene;
			auto carrier = scene.CreateEntity("Carrier");
			carrier.GetTransform().Translation = { 3, 0, -2 };
			carrier.GetTransform().Rotation = AxisAngle({ 0, 1, 0 }, 30);
			auto biped = AddBiped(scene, { Chain("LeftArm", IKChainType::TwoBone, "LeftHand") });
			scene.SetParent(biped, carrier, false);
			biped.GetTransform().Translation = { 1, 0, 0 };
			biped.GetTransform().Rotation = AxisAngle({ 0, 1, 0 }, 90);
			biped.GetTransform().Scale = scale;
			auto handle = scene.CreateEntity("Handle");
			scene.OnRuntimeStart();
			auto& system = *scene.GetAnimationSystem();

			// Within reach of the shoulder in world units (the arm is 0.6 m long at scale 1).
			const glm::vec3 shoulder = BoneWorld(scene, biped, "LeftUpperArm");
			const float reach = 0.6f * std::abs(scale.y);
			const glm::vec3 target = shoulder + glm::normalize(glm::vec3(0.3f, -0.6f, 0.5f)) * (0.7f * reach);
			const glm::quat rotation = glm::normalize(glm::quat(0.8f, 0.2f, -0.4f, 0.3f));
			REQUIRE(system.SetIKTarget(biped, "LeftArm", target, rotation));
			system.Update(Step);
			CHECK(Near(BoneWorld(scene, biped, "LeftHand"), target, 1e-3f * reach));
			CHECK(NearRotation(BoneWorldRotation(scene, biped, "LeftHand"), rotation, 1e-3f));

			// An entity target with its rotation.
			handle.GetTransform().Translation = target;
			handle.GetTransform().Rotation = AxisAngle(glm::normalize(glm::vec3(1, 2, 3)), 70);
			REQUIRE(system.SetIKTarget(biped, "LeftArm", handle, true));
			system.Update(Step);
			CHECK(Near(BoneWorld(scene, biped, "LeftHand"), target, 1e-3f * reach));
			CHECK(NearRotation(BoneWorldRotation(scene, biped, "LeftHand"), GlobalRotation(scene.GetWorldTransform(handle)), 1e-3f));
			scene.OnRuntimeStop();
		}
	}

	TEST_CASE("Two-bone IK solves under a mirrored parent joint")
	{
		const auto source = LoadBiped();
		const auto [upper, middle, end] = LeftLeg(*source);
		for (const glm::vec3 mirror : { glm::vec3(-1, 1, 1), glm::vec3(1, 1, -1) })
		{
			CAPTURE(mirror);
			Rig rig(source);
			rig.Pose.Nodes[Node(*source, "Hips")].Scale = mirror;
			REQUIRE(ComposeAnimationPose(*source, rig.Pose, rig.Globals));
			const glm::vec3 a = Position(rig.Globals, upper), c = Position(rig.Globals, end);
			// Off the leg's plane, so the swing turns about more than the knee's axis.
			const glm::vec3 target = a + AxisAngle({ 0, 0, 1 }, 25) * AxisAngle({ 1, 0, 0 }, 30) * ((c - a) * 0.8f);
			const glm::quat rotation = AxisAngle({ 0, 1, 0 }, 45);
			REQUIRE(TwoBone(rig, upper, middle, end, target, glm::vec3(0), rotation));
			CHECK(Near(Position(rig.Globals, end), target, 1e-4f));
			CHECK(NearRotation(GlobalRotation(rig.Globals[end]), rotation));
		}
	}

	TEST_CASE("Two-bone IK handles targets at the root, poles along the chain and very unequal bones")
	{
		const auto source = LoadBiped();
		const auto [upper, middle, end] = LeftLeg(*source);
		// A target on the hip folds the leg as far as it goes.
		Rig folded(source);
		REQUIRE(TwoBone(folded, upper, middle, end, Position(folded.Globals, upper)));
		CHECK(std::ranges::all_of(folded.Globals, [](const glm::mat4& global) { return Math::IsFinite(global); }));
		CHECK(glm::length(Position(folded.Globals, end) - Position(folded.Globals, upper)) < 0.01f);
		// A pole along the straight arm falls back to another bend direction.
		const uint32_t shoulder = Node(*source, "LeftUpperArm"), elbow = Node(*source, "LeftForeArm"), hand = Node(*source, "LeftHand");
		Rig arm(source);
		REQUIRE(TwoBone(arm, shoulder, elbow, hand, glm::vec3(0.55f, 1.55f, 0), glm::vec3(1, 0, 0)));
		CHECK(Near(Position(arm.Globals, hand), glm::vec3(0.55f, 1.55f, 0)));

		// A 1e-5 lower bone against a 1 m upper one: the reach limits cross, and must not trip std::clamp.
		MeshSource tiny;
		tiny.Nodes.resize(3);
		tiny.Nodes[1].Parent = 0;
		tiny.Nodes[1].Translation = { 0, -1, 0 };
		tiny.Nodes[2].Parent = 1;
		tiny.Nodes[2].Translation = { 0, -1e-5f, 1e-6f };
		REQUIRE(tiny.FinalizeHierarchy());
		AnimationLocalPose pose = SampleAnimation(tiny, nullptr, 0);
		std::vector<glm::mat4> globals;
		REQUIRE(ComposeAnimationPose(tiny, pose, globals));
		CHECK(SolveTwoBoneIK(tiny, pose, globals, { .Upper = 0, .Middle = 1, .End = 2, .Target = { 0, -0.5f, 0 } }) == IKSolveResult::Solved);
		CHECK(std::ranges::all_of(globals, [](const glm::mat4& global) { return Math::IsFinite(global); }));
		// Zero-length bones cannot be solved.
		tiny.Nodes[2].Translation = glm::vec3(0);
		pose = SampleAnimation(tiny, nullptr, 0);
		REQUIRE(ComposeAnimationPose(tiny, pose, globals));
		CHECK(SolveTwoBoneIK(tiny, pose, globals, { .Upper = 0, .Middle = 1, .End = 2, .Target = { 0, -0.5f, 0 } }) == IKSolveResult::Degenerate);
		CHECK(SolveTwoBoneIK(tiny, pose, globals, { .Upper = 0, .Middle = 1, .End = 2, .Target = glm::vec3(NAN) }) == IKSolveResult::NonFinite);
	}

	TEST_CASE("Joints with fixed glTF matrices are refused and reported")
	{
		ClearAssetCache clear;
		const auto source = LoadBiped();
		Rig rig(source);
		CHECK(SolveTwoBoneIK(*source, rig.Pose, rig.Globals, { .Upper = Node(*source, "LeftForeArm"), .Middle = Node(*source, "LeftHand"), .End = Node(*source, "Prop"), .Target = glm::vec3(1) }) == IKSolveResult::FixedMatrix);
		const std::vector<uint32_t> prop{ Node(*source, "Prop") };
		CHECK(SolveLookAtIK(*source, rig.Pose, rig.Globals, { .Joints = prop, .Target = glm::vec3(1) }) == IKSolveResult::FixedMatrix);

		Scene scene;
		auto biped = AddBiped(scene, { Chain("Prop", IKChainType::LookAt, "Prop"), Chain("Foot", IKChainType::TwoBone, "LeftFoot") });
		auto& ik = biped.GetComponent<InverseKinematicsComponent>();
		ik.Pelvis = "Prop";
		ik.Chains[1].Ground = true;
		const auto warnings = GetAnimationWarnings(biped);
		CHECK(std::ranges::count(warnings, std::string("InverseKinematics chain 'Prop': joint 'Prop' has a fixed matrix and cannot be turned")) == 1);
		CHECK(std::ranges::count(warnings, std::string("InverseKinematics Pelvis joint 'Prop' has a fixed matrix and cannot move")) == 1);
		const auto since = Log::GetHistory().GetTotalCount();
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.SetIKTarget(biped, "Prop", glm::vec3(1), std::nullopt));
		for (int i = 0; i < 3; i++)
			system.Update(Step);
		CHECK(CountLogMessages(since, "chain 'Prop': joint 'Prop' has a fixed matrix") == 1);
		CHECK(CountLogMessages(since, "Pelvis joint 'Prop' has a fixed matrix and cannot move") == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Foot placement uses a parent's character controller and stands on a parent platform")
	{
		ClearAssetCache clear;
		auto foot = Chain("LeftFoot", IKChainType::TwoBone, "LeftFoot");
		foot.Ground = true;
		for (bool airborne : { false, true })
		{
			CAPTURE(airborne);
			Scene scene;
			// The controller is on the parent; the model, with its IK, is a child at the parent's origin.
			auto character = scene.CreateEntity("Character");
			character.GetTransform().Translation = { 0, airborne ? 0.3f : 0.05f, 0 };
			character.AddComponent<CharacterControllerComponent>().GravityFactor = airborne ? 0.0f : 1.0f;
			// Wide enough to stand under the feet: the rays must skip it.
			auto& capsule = character.AddComponent<CapsuleColliderComponent>();
			capsule.Radius = 0.15f;
			capsule.Offset = { 0, 0.66f, 0 };
			auto biped = AddBiped(scene, { foot });
			auto& ik = biped.GetComponent<InverseKinematicsComponent>();
			ik.Pelvis = "Hips";
			// Rays from above the capsule would land on its top without the hierarchy filter.
			ik.RayHeight = 1.5f;
			scene.SetParent(biped, character, false);
			AddBox(scene, "Pillar", { 0, -0.5f, 0 }, { 0.07f, 0.5f, 0.07f });
			AddBox(scene, "Floor", { 0, -0.6f, 0 }, { 5, 0.5f, 5 });
			scene.OnSimulationStart();
			for (int i = 0; i < 60; i++)
				scene.OnUpdate(Step);
			REQUIRE(scene.GetPhysicsWorld()->IsCharacterGrounded(character) == !airborne);
			const glm::vec3 origin = scene.GetWorldTransform(biped)[3];
			CHECK(BoneWorld(scene, biped, "LeftFoot").y - origin.y == doctest::Approx(airborne ? 0.05f : -0.05f).epsilon(1e-2));
			scene.OnSimulationStop();
		}
		{
			// A platform the character is parented to is ground.
			Scene scene;
			auto platform = AddBox(scene, "Platform", { 0, -0.5f, 0 }, { 2, 0.5f, 2 });
			auto biped = AddBiped(scene, { foot });
			biped.GetComponent<InverseKinematicsComponent>().Pelvis = "Hips";
			scene.SetParent(biped, platform, false);
			biped.GetTransform().Translation = { 0, 0.6f, 0 };
			scene.OnSimulationStart();
			scene.OnUpdate(Step);
			CHECK(BoneWorld(scene, biped, "LeftFoot").y == doctest::Approx(0.05f).epsilon(1e-3));
			scene.OnSimulationStop();
		}
	}

	TEST_CASE("Attachments follow solved hands and OnLateUpdate targets apply the same frame")
	{
		ClearAssetCache clear;
		TempProject project("IKLateUpdate");
		const std::string script = project.WriteFile("Assets/Scripts/Mover.lua", R"(
			local Mover = {}
			function Mover:OnLateUpdate() self.Entity.Translation = Vec3(0.35, 1.2, 0.2) end
			return Mover
		)");
		Scene scene;
		auto biped = AddBiped(scene, { Chain("LeftArm", IKChainType::TwoBone, "LeftHand") });
		auto handle = scene.CreateEntity("Handle");
		handle.GetTransform().Translation = { 0.4f, 1.3f, 0.3f };
		handle.AddComponent<ScriptComponent>().Script = script;
		auto sword = scene.CreateEntity("Sword");
		auto& attachment = sword.AddComponent<BoneAttachmentComponent>();
		attachment.Target = biped.GetUUID();
		attachment.Joint = "LeftHand";
		scene.OnRuntimeStart();
		REQUIRE(scene.GetAnimationSystem()->SetIKTarget(biped, "LeftArm", handle, false));
		scene.OnUpdate(Step);
		// The script moved the handle after physics; IK and then the attachment saw it in the same frame.
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), glm::vec3(0.35f, 1.2f, 0.2f), 1e-3f));
		CHECK(Near(glm::vec3(scene.GetWorldTransform(sword)[3]), glm::vec3(0.35f, 1.2f, 0.2f), 1e-3f));
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		// An invalid step skips IK but still moves attachments with their rigs.
		biped.GetTransform().Translation = { 1, 0, 0 };
		scene.GetAnimationSystem()->LateUpdate(std::numeric_limits<float>::quiet_NaN());
		CHECK(Near(glm::vec3(scene.GetWorldTransform(sword)[3]), BoneWorld(scene, biped, "LeftHand"), 1e-4f));
		CHECK(BoneWorld(scene, biped, "LeftHand").x > 1.0f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("IK solves from the layered pose")
	{
		ClearAssetCache clear;
		const auto source = LoadBiped();
		Scene scene;
		auto biped = AddBiped(scene, { Chain("LeftArm", IKChainType::TwoBone, "LeftHand") });
		auto& animator = biped.GetComponent<AnimatorComponent>();
		AnimationLayer breathe;
		breathe.Name = "Breathe";
		breathe.Clip = "Breathe";
		breathe.Blend = AnimationLayerBlend::Additive;
		animator.Layers = { breathe };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const glm::vec3 target(0.4f, 1.3f, 0.3f);
		REQUIRE(system.SetIKTarget(biped, "LeftArm", target, std::nullopt));
		system.Update(0.5f);
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), target, 1e-3f));
		// The chest keeps the layer's 10 degree breath under the solved arm.
		const auto& globals = *system.GetPose(biped);
		const glm::quat chest = glm::inverse(GlobalRotation(globals[Node(*source, "Spine")])) * GlobalRotation(globals[Node(*source, "Chest")]);
		CHECK(NearRotation(chest, AxisAngle({ 1, 0, 0 }, 10)));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Script targets replace ground placement and partly weighted feet lower the pelvis partly")
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
		ik.Smoothing = 0;
		// Floor 0.2 m below the origin under both feet.
		biped.GetTransform().Translation = { 0, 0.2f, 0 };
		AddBox(scene, "Floor", { 0, -0.5f, 0 }, { 5, 0.5f, 5 });
		scene.OnSimulationStart();
		scene.OnUpdate(Step);
		auto& system = *scene.GetAnimationSystem();
		CHECK(BoneWorld(scene, biped, "Hips").y == doctest::Approx(1.0f).epsilon(1e-3));

		// The right foot follows a script target; only the left one still lowers the pelvis.
		const glm::vec3 lifted(-0.1f, 0.6f, 0.3f);
		REQUIRE(system.SetIKTarget(biped, "RightFoot", lifted, std::nullopt));
		REQUIRE(system.SetIKWeight(biped, "LeftFoot", 0.5f, 0));
		scene.OnUpdate(Step);
		CHECK(Near(BoneWorld(scene, biped, "RightFoot"), lifted, 1e-3f));
		CHECK(BoneWorld(scene, biped, "Hips").y == doctest::Approx(1.1f).epsilon(1e-3));

		// Cleared, the foot snaps back onto the ground.
		REQUIRE(system.ClearIKTarget(biped, "RightFoot"));
		REQUIRE(system.SetIKWeight(biped, "LeftFoot", 1, 0));
		scene.OnUpdate(Step);
		CHECK(BoneWorld(scene, biped, "RightFoot").y == doctest::Approx(0.05f).epsilon(1e-3));
		scene.OnSimulationStop();
	}

	TEST_CASE("Foot placement ignores ground out of reach and Smoothing 0 snaps")
	{
		ClearAssetCache clear;
		Scene scene;
		auto left = Chain("LeftFoot", IKChainType::TwoBone, "LeftFoot");
		left.Ground = true;
		auto biped = AddBiped(scene, { left });
		auto& ik = biped.GetComponent<InverseKinematicsComponent>();
		ik.Pelvis = "Hips";
		ik.Smoothing = 0;
		// 0.6 m below the origin: beyond RayDepth.
		auto floor = AddBox(scene, "Floor", { 0, -1.1f, 0 }, { 5, 0.5f, 5 });
		scene.OnSimulationStart();
		scene.OnUpdate(Step);
		CHECK(BoneWorld(scene, biped, "LeftFoot").y == doctest::Approx(0.05f).epsilon(1e-3));
		// Raised into reach, the foot follows at once.
		floor.GetTransform().Translation = { 0, -0.8f, 0 };
		scene.OnUpdate(Step);
		scene.OnUpdate(Step);
		CHECK(BoneWorld(scene, biped, "LeftFoot").y == doctest::Approx(-0.25f).epsilon(1e-3));
		scene.OnSimulationStop();
	}

	TEST_CASE("IK targets survive playback resets and end with the entity")
	{
		ClearAssetCache clear;
		Scene scene;
		auto biped = AddBiped(scene, { Chain("LeftArm", IKChainType::TwoBone, "LeftHand") });
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const glm::vec3 target(0.4f, 1.3f, 0.3f);
		REQUIRE(system.SetIKTarget(biped, "LeftArm", target, std::nullopt));
		REQUIRE(system.SetIKWeight(biped, "LeftArm", 0.5f, 0));
		// Changing the Clip resets the playback, not the IK state.
		biped.GetComponent<AnimatorComponent>().Clip = "Wave";
		system.Update(Step);
		CHECK(system.GetIKWeight(biped, "LeftArm") == doctest::Approx(0.5f));
		REQUIRE(system.SetIKWeight(biped, "LeftArm", 1, 0));
		system.Update(Step);
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), target, 1e-3f));
		// Destroying the entity ends everything, and the next update is fine.
		scene.DestroyEntity(biped);
		system.Update(Step);
		scene.OnRuntimeStop();
	}

	TEST_CASE("IK reports unplayable Animators zero-scale entities and LookAt rotations")
	{
		ClearAssetCache clear;
		Scene scene;
		auto biped = AddBiped(scene, { Chain("Look", IKChainType::LookAt, "Head") });
		auto broken = scene.CreateEntity("Broken");
		broken.AddComponent<AnimatorComponent>().Mesh = "Missing.gltf";
		broken.AddComponent<InverseKinematicsComponent>();
		const auto since = Log::GetHistory().GetTotalCount();
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		system.Update(Step);
		system.Update(Step);
		CHECK(CountLogMessages(since, "InverseKinematics 'Broken': not solved: model 'Missing.gltf' could not be loaded") == 1);

		CHECK_FALSE(system.SetIKTarget(biped, "Look", glm::vec3(1), glm::quat(1, 0, 0, 0)));
		CHECK_FALSE(system.SetIKTarget(biped, "Look", broken, true));
		CHECK(CountLogMessages(since, "chain 'Look' is a LookAt chain, which takes no rotation") == 1);

		REQUIRE(system.SetIKTarget(biped, "Look", glm::vec3(1, 1.7f, 1), std::nullopt));
		// A zero scale (a spawn effect) has nothing to bend and says nothing; a non-finite transform is a problem.
		biped.GetTransform().Scale = glm::vec3(0);
		system.Update(Step);
		CHECK(CountLogMessages(since, "is not invertible") == 0);
		biped.GetTransform().Scale = glm::vec3(1);
		biped.GetTransform().Translation = glm::vec3(std::numeric_limits<float>::quiet_NaN());
		system.Update(Step);
		system.Update(Step);
		CHECK(CountLogMessages(since, "the entity's transform is not invertible") == 1);
		// A non-finite step skips the solve but still updates attachments.
		system.LateUpdate(std::numeric_limits<float>::quiet_NaN());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Lua IK rotations reach the solver and matchRotation false keeps the hand's local rotation")
	{
		ClearAssetCache clear;
		Scene scene;
		auto biped = AddBiped(scene, { Chain("LeftArm", IKChainType::TwoBone, "LeftHand") });
		auto handle = scene.CreateEntity("Handle");
		handle.GetTransform().Translation = { 0.4f, 1.3f, 0.3f };
		handle.GetTransform().Rotation = AxisAngle({ 0, 0, 1 }, 50);
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const glm::quat rotation = AxisAngle({ 0, 1, 0 }, 90);
		REQUIRE(Eval(scene, "local e = Scene.FindEntityByName('Biped'); return tostring(e:SetIKTarget('LeftArm', Vec3(0.4, 1.3, 0.3), Quat.AngleAxis(math.pi / 2, Vec3(0, 1, 0))))") == "true");
		system.Update(Step);
		CHECK(NearRotation(BoneWorldRotation(scene, biped, "LeftHand"), rotation, 1e-3f));
		REQUIRE(Eval(scene, "local e = Scene.FindEntityByName('Biped'); return tostring(e:SetIKTarget('LeftArm', Scene.FindEntityByName('Handle'), true))") == "true");
		system.Update(Step);
		CHECK(NearRotation(BoneWorldRotation(scene, biped, "LeftHand"), handle.GetTransform().Rotation, 1e-3f));
		REQUIRE(Eval(scene, "local e = Scene.FindEntityByName('Biped'); return tostring(e:SetIKTarget('LeftArm', Scene.FindEntityByName('Handle'), false))") == "true");
		system.Update(Step);
		// Without a rotation the hand keeps its animated local rotation and turns with the forearm.
		const auto source = AssetManager::GetMesh(BipedPath().string());
		const auto& globals = *system.GetPose(biped);
		const glm::quat local = glm::inverse(GlobalRotation(globals[Node(*source, "LeftForeArm")])) * GlobalRotation(globals[Node(*source, "LeftHand")]);
		CHECK(NearRotation(local, glm::quat(1, 0, 0, 0)));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Foot placement and the pelvis work in world units on scaled rigs and under a turned scaled armature")
	{
		ClearAssetCache clear;
		auto left = Chain("LeftFoot", IKChainType::TwoBone, "LeftFoot");
		left.Ground = true;
		auto right = Chain("RightFoot", IKChainType::TwoBone, "RightFoot");
		right.Ground = true;
		{
			// The Armature variant poses the same skeleton, turned 90 degrees about Y.
			const auto plain = LoadBiped();
			const auto nested = LoadModel(std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/BipedArmature.gltf");
			const Rig a(plain), b(nested);
			const glm::vec3 foot = Position(a.Globals, Node(*plain, "LeftFoot"));
			CHECK(Near(Position(b.Globals, Node(*nested, "LeftFoot")), glm::vec3(foot.z, foot.y, -foot.x)));
		}
		for (bool nested : { false, true })
		{
			CAPTURE(nested);
			Scene scene;
			auto biped = AddBiped(scene, { left, right });
			auto& ik = biped.GetComponent<InverseKinematicsComponent>();
			ik.Pelvis = "Hips";
			ik.Smoothing = 0;
			if (nested)
				biped.GetComponent<AnimatorComponent>().Mesh = (std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/BipedArmature.gltf").string();
			else
				biped.GetTransform().Scale = glm::vec3(2);
			const float scale = nested ? 1.0f : 2.0f;
			biped.GetTransform().Translation = { 0, 0.2f, 0 };
			AddBox(scene, "Floor", { 0, -0.5f, 0 }, { 5, 0.5f, 5 });
			scene.OnSimulationStart();
			scene.OnUpdate(Step);
			// Both feet reach the floor 0.2 m below the origin, the pelvis drops 0.2 m in world units.
			CHECK(BoneWorld(scene, biped, "LeftFoot").y == doctest::Approx(0.05f * scale).epsilon(1e-3));
			CHECK(BoneWorld(scene, biped, "RightFoot").y == doctest::Approx(0.05f * scale).epsilon(1e-3));
			CHECK(BoneWorld(scene, biped, "Hips").y == doctest::Approx(0.2f + scale * 1.0f - 0.2f).epsilon(1e-3));
			scene.OnSimulationStop();
		}
	}

	TEST_CASE("Feet rise onto steps without raising the pelvis, and cleared targets snap back with smoothing on")
	{
		ClearAssetCache clear;
		Scene scene;
		auto left = Chain("LeftFoot", IKChainType::TwoBone, "LeftFoot");
		left.Ground = true;
		auto biped = AddBiped(scene, { left });
		auto& ik = biped.GetComponent<InverseKinematicsComponent>();
		ik.Pelvis = "Hips";
		ik.Smoothing = 0.1f;
		AddBox(scene, "Floor", { 0, -0.5f, 0 }, { 5, 0.5f, 5 });
		auto step = AddBox(scene, "Step", { 0.1f, 0.1f, 0 }, { 0.05f, 0.1f, 0.2f });
		scene.OnSimulationStart();
		scene.OnUpdate(Step);
		CHECK(BoneWorld(scene, biped, "LeftFoot").y == doctest::Approx(0.25f).epsilon(1e-3));
		CHECK(BoneWorld(scene, biped, "Hips").y == doctest::Approx(1.0f).epsilon(1e-3));

		// While a script holds the foot, the step goes; cleared, the foot snaps to the floor instead of easing.
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.SetIKTarget(biped, "LeftFoot", glm::vec3(0.1f, 0.3f, 0.2f), std::nullopt));
		scene.DestroyEntity(step);
		scene.OnUpdate(Step);
		REQUIRE(system.ClearIKTarget(biped, "LeftFoot"));
		scene.OnUpdate(Step);
		CHECK(BoneWorld(scene, biped, "LeftFoot").y == doctest::Approx(0.05f).epsilon(1e-3));
		scene.OnSimulationStop();
	}

	TEST_CASE("Component poles look-at chains and axes reach the solver")
	{
		ClearAssetCache clear;
		const auto source = LoadBiped();
		Scene scene;
		auto leg = Chain("Leg", IKChainType::TwoBone, "LeftFoot");
		leg.Pole = { 0, 0, -1 };
		auto look = Chain("Look", IKChainType::LookAt, "Head");
		look.ChainLength = 3;
		look.Axis = { 1, 0, 0 };
		look.MaxAngle = 180;
		auto biped = AddBiped(scene, { leg, look });
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const glm::vec3 foot(0.1f, 0.3f, 0.25f);
		REQUIRE(system.SetIKTarget(biped, "Leg", foot, std::nullopt));
		// The head's +X (its left) turns toward a point in front: 90 degrees about Y, shared by head, chest and spine.
		const glm::vec3 ahead(0, 1.7f, 5);
		REQUIRE(system.SetIKTarget(biped, "Look", ahead, std::nullopt));
		system.Update(Step);
		CHECK(Near(BoneWorld(scene, biped, "LeftFoot"), foot, 1e-3f));
		CHECK(BoneWorld(scene, biped, "LeftLowerLeg").z < BoneWorld(scene, biped, "LeftUpperLeg").z);
		const glm::vec3 aim = BoneWorldRotation(scene, biped, "Head") * glm::vec3(1, 0, 0);
		CHECK(Degrees(aim, ahead - BoneWorld(scene, biped, "Head")) < 0.05f);
		const auto& globals = *system.GetPose(biped);
		const glm::quat spine = glm::inverse(GlobalRotation(globals[Node(*source, "Hips")])) * GlobalRotation(globals[Node(*source, "Spine")]);
		// Lean's 20 degree roll plus about a third of the turn.
		CHECK(glm::degrees(glm::angle(glm::inverse(AxisAngle({ 0, 0, 1 }, 20)) * spine)) > 20);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Several rigs share the solver and chains keep their state by name")
	{
		ClearAssetCache clear;
		Scene scene;
		auto first = AddBiped(scene, { Chain("LeftArm", IKChainType::TwoBone, "LeftHand"), Chain("RightArm", IKChainType::TwoBone, "RightHand") });
		auto second = AddBiped(scene, { Chain("LeftArm", IKChainType::TwoBone, "LeftHand") });
		second.GetTransform().Translation = { 3, 0, 0 };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.SetIKTarget(first, "LeftArm", glm::vec3(0.4f, 1.3f, 0.3f), std::nullopt));
		REQUIRE(system.SetIKTarget(first, "RightArm", glm::vec3(-0.4f, 1.2f, 0.3f), std::nullopt));
		REQUIRE(system.SetIKTarget(second, "LeftArm", glm::vec3(3.3f, 1.2f, 0.3f), std::nullopt));
		REQUIRE(system.SetIKWeight(first, "RightArm", 0.5f, 0));
		system.Update(Step);
		CHECK(Near(BoneWorld(scene, first, "LeftHand"), glm::vec3(0.4f, 1.3f, 0.3f), 1e-3f));
		CHECK(Near(BoneWorld(scene, second, "LeftHand"), glm::vec3(3.3f, 1.2f, 0.3f), 1e-3f));
		// Reordered chains keep their targets and weights; a renamed chain starts fresh.
		auto& chains = first.GetComponent<InverseKinematicsComponent>().Chains;
		std::swap(chains[0], chains[1]);
		system.Update(Step);
		CHECK(system.GetIKWeight(first, "RightArm") == doctest::Approx(0.5f));
		CHECK(Near(BoneWorld(scene, first, "LeftHand"), glm::vec3(0.4f, 1.3f, 0.3f), 1e-3f));
		chains[1].Name = "Hand";
		system.Update(Step);
		CHECK(system.GetIKWeight(first, "Hand") == doctest::Approx(1.0f));
		// Without a target the renamed chain idles: the hand is back at its animated place.
		CHECK_FALSE(Near(BoneWorld(scene, first, "LeftHand"), glm::vec3(0.4f, 1.3f, 0.3f), 1e-2f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Settings C++ writes past validation are reported once and handled")
	{
		ClearAssetCache clear;
		const auto source = LoadBiped();
		Scene scene;
		// Ground on a LookAt chain would otherwise aim it at the world origin.
		auto look = Chain("Look", IKChainType::LookAt, "Head");
		look.Ground = true;
		auto arm = Chain("Arm", IKChainType::TwoBone, "LeftHand");
		arm.Weight = 2;
		arm.Pole = glm::vec3(NAN);
		auto biped = AddBiped(scene, { look, arm });
		auto& ik = biped.GetComponent<InverseKinematicsComponent>();
		ik.RayHeight = -1;
		biped.GetTransform().Translation = { 4, 0, 4 };
		const auto since = Log::GetHistory().GetTotalCount();
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.SetIKTarget(biped, "Arm", glm::vec3(4.4f, 1.3f, 4.3f), std::nullopt));
		for (int i = 0; i < 3; i++)
			system.Update(Step);
		CHECK(CountLogMessages(since, "chain 'Look': Ground only applies to TwoBone and Fabrik chains") == 1);
		CHECK(CountLogMessages(since, "chain 'Arm': Pole is not finite") == 1);
		CHECK(CountLogMessages(since, "RayHeight must be between 0 and 100; clamping") == 1);
		CHECK(CountLogMessages(since, "chain 'Arm' Weight must be between 0 and 1; clamping") == 1);
		// The look-at idles (head as animated); the arm solves at full weight with the animated bend.
		const auto& globals = *system.GetPose(biped);
		const glm::quat head = glm::inverse(GlobalRotation(globals[Node(*source, "Chest")])) * GlobalRotation(globals[Node(*source, "Head")]);
		CHECK(NearRotation(head, glm::quat(1, 0, 0, 0)));
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), glm::vec3(4.4f, 1.3f, 4.3f), 1e-3f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Look-at IK aims under a mirrored parent joint")
	{
		const auto source = LoadBiped();
		Rig rig(source);
		rig.Pose.Nodes[Node(*source, "Hips")].Scale = { 1, 1, -1 };
		REQUIRE(ComposeAnimationPose(*source, rig.Pose, rig.Globals));
		const uint32_t head = Node(*source, "Head");
		const glm::vec3 target = Position(rig.Globals, head) + glm::vec3(1, 0.5f, 2);
		REQUIRE(LookAt(rig, { head, Node(*source, "Chest") }, { 0, 0, 1 }, target, glm::pi<float>()));
		CHECK(Degrees(glm::vec3(rig.Globals[head] * glm::vec4(0, 0, 1, 0)), target - Position(rig.Globals, head)) < 0.05f);
	}

	TEST_CASE("A settled ground foot is solved again when an earlier chain turned it")
	{
		ClearAssetCache clear;
		Scene scene;
		// An earlier look-at turns the foot itself; the later ground chain on flat ground then puts it back.
		auto look = Chain("FootLook", IKChainType::LookAt, "LeftFoot");
		look.MaxAngle = 90;
		auto ground = Chain("LeftFoot", IKChainType::TwoBone, "LeftFoot");
		ground.Ground = true;
		auto biped = AddBiped(scene, { look, ground });
		AddBox(scene, "Floor", { 0, -0.5f, 0 }, { 5, 0.5f, 5 });
		scene.OnSimulationStart();
		REQUIRE(scene.GetAnimationSystem()->SetIKTarget(biped, "FootLook", glm::vec3(5, 0.05f, 0), std::nullopt));
		scene.OnUpdate(Step);
		// The animated foot is level (identity rest rotations, Lean only turns the spine).
		CHECK(NearRotation(BoneWorldRotation(scene, biped, "LeftFoot"), glm::quat(1, 0, 0, 0), 1e-3f));
		scene.OnSimulationStop();
	}

	TEST_CASE("Feet ease back to the animation when a grounded character takes off")
	{
		ClearAssetCache clear;
		auto foot = Chain("LeftFoot", IKChainType::TwoBone, "LeftFoot");
		foot.Ground = true;
		Scene scene;
		auto biped = AddBiped(scene, { foot });
		auto& ik = biped.GetComponent<InverseKinematicsComponent>();
		ik.Pelvis = "Hips";
		ik.Smoothing = 0.1f;
		// Standing on a pillar between its feet, which reach a floor 0.1 m lower.
		biped.GetTransform().Translation = { 0, 0.05f, 0 };
		biped.AddComponent<CharacterControllerComponent>();
		auto& capsule = biped.AddComponent<CapsuleColliderComponent>();
		capsule.Radius = 0.06f;
		capsule.Offset = { 0, 0.57f, 0 };
		AddBox(scene, "Pillar", { 0, -0.5f, 0 }, { 0.07f, 0.5f, 0.07f });
		AddBox(scene, "Floor", { 0, -0.6f, 0 }, { 5, 0.5f, 5 });
		scene.OnSimulationStart();
		for (int i = 0; i < 60; i++)
			scene.OnUpdate(Step);
		const auto lift = [&] { return BoneWorld(scene, biped, "LeftFoot").y - scene.GetWorldTransform(biped)[3].y; };
		REQUIRE(lift() == doctest::Approx(-0.05f).epsilon(1e-2));

		// Jump: the foot does not pop back at takeoff but eases (Smoothing) to its animated height.
		scene.GetPhysicsWorld()->MoveCharacter(biped, { 0, 4, 0 });
		for (int i = 0; i < 60 && scene.GetPhysicsWorld()->IsCharacterGrounded(biped); i++)
			scene.OnUpdate(Step);
		scene.GetPhysicsWorld()->MoveCharacter(biped, glm::vec3(0));
		scene.OnUpdate(Step);
		REQUIRE_FALSE(scene.GetPhysicsWorld()->IsCharacterGrounded(biped));
		const float takeoff = lift();
		CHECK(takeoff < 0.04f);
		CHECK(takeoff > -0.05f);
		for (int i = 0; i < 30 && !scene.GetPhysicsWorld()->IsCharacterGrounded(biped); i++)
			scene.OnUpdate(Step);
		CHECK(lift() == doctest::Approx(0.05f).epsilon(2e-2));
		scene.OnSimulationStop();
	}
}

namespace {
	bool Fabrik(Rig& rig, const std::vector<uint32_t>& joints, const glm::vec3& target, const glm::vec3& pole = glm::vec3(0), const std::optional<glm::quat>& rotation = std::nullopt, float weight = 1)
	{
		const FabrikIKRequest request{ .Joints = joints, .Target = target, .Pole = glm::length(pole) > 0 ? std::optional<glm::vec3>(pole) : std::nullopt, .EndRotation = rotation, .Weight = weight };
		return SolveFabrikIK(*rig.Source, rig.Pose, rig.Globals, request) == IKSolveResult::Solved;
	}
	// The left arm from the chest: three bones, straight along +X at rest.
	std::vector<uint32_t> LeftArmChain(const MeshSource& source)
	{
		return { Node(source, "Chest"), Node(source, "LeftUpperArm"), Node(source, "LeftForeArm"), Node(source, "LeftHand") };
	}
	std::vector<float> BoneLengths(const std::vector<glm::mat4>& globals, const std::vector<uint32_t>& joints)
	{
		std::vector<float> lengths;
		for (size_t i = 0; i + 1 < joints.size(); i++)
			lengths.push_back(glm::length(Position(globals, joints[i + 1]) - Position(globals, joints[i])));
		return lengths;
	}
}

TEST_SUITE("Animation")
{
	TEST_CASE("FABRIK reaches targets keeps bone lengths and stretches toward targets out of reach")
	{
		Rig rig(LoadModel(BipedPath()));
		const auto joints = LeftArmChain(*rig.Source);
		const auto lengths = BoneLengths(rig.Globals, joints);
		const glm::vec3 root = Position(rig.Globals, joints.front());
		const float reach = lengths[0] + lengths[1] + lengths[2];

		// A bent arm reaching forward and down.
		const glm::vec3 target(0.4f, 1.1f, 0.35f);
		REQUIRE(Fabrik(rig, joints, target));
		CHECK(Near(Position(rig.Globals, joints.back()), target, 1e-3f));
		CHECK(Near(Position(rig.Globals, joints.front()), root));
		const auto solved = BoneLengths(rig.Globals, joints);
		for (size_t i = 0; i < lengths.size(); i++)
			CHECK(solved[i] == doctest::Approx(lengths[i]).epsilon(1e-4));

		// Out of reach: straight toward the target, the hand at the chain's full reach.
		Rig far(rig.Source);
		const glm::vec3 away(3.0f, 2.0f, 1.0f);
		REQUIRE(Fabrik(far, joints, away));
		const glm::vec3 hand = Position(far.Globals, joints.back());
		CHECK(glm::length(hand - root) == doctest::Approx(reach).epsilon(1e-4));
		CHECK(Degrees(hand - root, away - root) < 0.05f);
		CHECK(Degrees(Position(far.Globals, joints[2]) - root, away - root) < 0.05f);
	}

	TEST_CASE("FABRIK bends straight chains toward the pole and blends weights and end rotations")
	{
		const auto source = LoadModel(BipedPath());
		const auto joints = LeftArmChain(*source);
		Rig animated(source);
		// The rest spine is straight up from the hips to the head; a target along that line, closer than its reach,
		// needs a bend.
		const std::vector<uint32_t> spine{ Node(*source, "Hips"), Node(*source, "Spine"), Node(*source, "Chest"), Node(*source, "Head") };
		const glm::vec3 inward = Position(animated.Globals, spine.front()) + glm::vec3(0, 0.5f, 0);
		Rig forward(source);
		REQUIRE(Fabrik(forward, spine, inward));
		CHECK(Near(Position(forward.Globals, spine.back()), inward, 1e-3f));
		// Without a pole the spine starts bending toward +Z.
		CHECK(Position(forward.Globals, spine[2]).z > 0.01f);
		Rig backward(source);
		REQUIRE(Fabrik(backward, spine, inward, glm::vec3(0, 0, -1)));
		CHECK(Near(Position(backward.Globals, spine.back()), inward, 1e-3f));
		CHECK(Position(backward.Globals, spine[2]).z < -0.01f);
		// A bent chain keeps its plane: the arm bends in the plane its shoulder offset makes.
		Rig planar(source);
		const glm::vec3 inline_ = Position(animated.Globals, joints.front()) + glm::vec3(0.5f, 0.1f, 0);
		REQUIRE(Fabrik(planar, joints, inline_));
		CHECK(Near(Position(planar.Globals, joints.back()), inline_, 1e-3f));
		CHECK(std::abs(Position(planar.Globals, joints[2]).z) < 1e-4f);

		// Weight 0 changes nothing; half weight lands between the animated and the solved hand.
		const glm::vec3 target(0.4f, 1.1f, 0.35f);
		Rig none(source);
		CHECK(SolveFabrikIK(*source, none.Pose, none.Globals, { .Joints = joints, .Target = target, .Weight = 0 }) == IKSolveResult::Unchanged);
		CHECK(NearPose(none.Globals, animated.Globals));
		Rig half(source);
		REQUIRE(Fabrik(half, joints, target, glm::vec3(0), std::nullopt, 0.5f));
		const float halfway = glm::length(Position(half.Globals, joints.back()) - target);
		CHECK(halfway > 0.05f);
		CHECK(halfway < glm::length(Position(animated.Globals, joints.back()) - target));

		// An end rotation turns the hand itself.
		Rig turned(source);
		const glm::quat rotation = glm::angleAxis(glm::radians(40.0f), glm::vec3(0, 1, 0));
		REQUIRE(Fabrik(turned, joints, target, glm::vec3(0), rotation));
		CHECK(NearRotation(GlobalRotation(turned.Globals[joints.back()]), rotation, 1e-3f));

		// Non-finite requests leave the pose alone; zero-length chains cannot bend.
		Rig bad(source);
		CHECK(SolveFabrikIK(*source, bad.Pose, bad.Globals, { .Joints = joints, .Target = glm::vec3(std::numeric_limits<float>::quiet_NaN()) }) == IKSolveResult::NonFinite);
		CHECK(NearPose(bad.Globals, animated.Globals));
		Rig squashed(source);
		for (uint32_t joint : joints)
			squashed.Pose.Nodes[joint].Scale = glm::vec3(joint == joints.front() ? 1.0f : 0.0f);
		REQUIRE(ComposeAnimationPose(*source, squashed.Pose, squashed.Globals));
		squashed.Pose.Nodes[joints.front()].Scale = glm::vec3(0.0f);
		REQUIRE(ComposeAnimationPose(*source, squashed.Pose, squashed.Globals));
		CHECK(SolveFabrikIK(*source, squashed.Pose, squashed.Globals, { .Joints = joints, .Target = target }) == IKSolveResult::Degenerate);
	}

	TEST_CASE("FABRIK solves under mirrored parents and handles edge cases")
	{
		const auto source = LoadBiped();
		const auto joints = LeftArmChain(*source);
		for (const glm::vec3 mirror : { glm::vec3(-1, 1, 1), glm::vec3(1, 1, -1) })
		{
			CAPTURE(mirror);
			Rig rig(source);
			rig.Pose.Nodes[Node(*source, "Hips")].Scale = mirror;
			REQUIRE(ComposeAnimationPose(*source, rig.Pose, rig.Globals));
			const auto lengths = BoneLengths(rig.Globals, joints);
			const glm::vec3 root = Position(rig.Globals, joints.front());
			const glm::vec3 target = root + glm::vec3(mirror.x * 0.3f, -0.25f, mirror.z * 0.3f);
			const glm::quat rotation = AxisAngle({ 0, 1, 0 }, 45);
			REQUIRE(Fabrik(rig, joints, target, glm::vec3(0), rotation));
			CHECK(Near(Position(rig.Globals, joints.back()), target, 1e-3f));
			CHECK(NearRotation(GlobalRotation(rig.Globals[joints.back()]), rotation, 1e-3f));
			const auto solved = BoneLengths(rig.Globals, joints);
			for (size_t i = 0; i < lengths.size(); i++)
				CHECK(solved[i] == doctest::Approx(lengths[i]).epsilon(1e-4));
		}

		const std::vector<uint32_t> spine{ Node(*source, "Hips"), Node(*source, "Spine"), Node(*source, "Chest"), Node(*source, "Head") };
		Rig animated(source);
		const glm::vec3 hips = Position(animated.Globals, spine.front());
		// A straight chain whose end is already within tolerance of its target (just inside its reach) is left alone: no
		// nudge kinks it.
		Rig there(source);
		REQUIRE(Fabrik(there, spine, Position(animated.Globals, spine.back()) - glm::vec3(0, 1e-5f, 0)));
		CHECK(NearPose(there.Globals, animated.Globals));
		// A pole along the chain falls back to another side and still reaches.
		Rig along(source);
		REQUIRE(Fabrik(along, spine, hips + glm::vec3(0, 0.5f, 0), glm::vec3(0, 1, 0)));
		CHECK(Near(Position(along.Globals, spine.back()), hips + glm::vec3(0, 0.5f, 0), 1e-3f));
		// A target on the root folds the chain onto it without breaking bone lengths.
		Rig folded(source);
		CHECK(SolveFabrikIK(*source, folded.Pose, folded.Globals, { .Joints = spine, .Target = hips }) == IKSolveResult::Solved);
		const auto restLengths = BoneLengths(animated.Globals, spine);
		const auto foldedLengths = BoneLengths(folded.Globals, spine);
		for (size_t i = 0; i < restLengths.size(); i++)
			CHECK(foldedLengths[i] == doctest::Approx(restLengths[i]).epsilon(1e-4));
		// One bone (ChainLength 1) turns toward the target like an aim.
		Rig single(source);
		const std::vector<uint32_t> forearm{ Node(*source, "LeftForeArm"), Node(*source, "LeftHand") };
		const glm::vec3 elbow = Position(single.Globals, forearm.front());
		REQUIRE(Fabrik(single, forearm, elbow + glm::vec3(0, -1, 0)));
		CHECK(Degrees(Position(single.Globals, forearm.back()) - elbow, glm::vec3(0, -1, 0)) < 0.05f);
		// An end rotation on a fixed-matrix joint (the held prop) cannot apply.
		Rig prop(source);
		const std::vector<uint32_t> toProp{ Node(*source, "LeftForeArm"), Node(*source, "LeftHand"), Node(*source, "Prop") };
		CHECK(SolveFabrikIK(*source, prop.Pose, prop.Globals, { .Joints = toProp, .Target = elbow, .EndRotation = glm::quat(1, 0, 0, 0) }) == IKSolveResult::FixedMatrix);
	}

	TEST_CASE("Fabrik chains resolve ChainLength bones and solve scripted and ground targets")
	{
		const auto source = LoadModel(BipedPath());
		std::vector<uint32_t> joints;
		auto arm = Chain("Arm", IKChainType::Fabrik, "LeftHand");
		arm.ChainLength = 3;
		CHECK(ResolveIKChain(*source, arm, joints).empty());
		CHECK(joints == LeftArmChain(*source));
		arm.ChainLength = 6;
		CHECK(ResolveIKChain(*source, arm, joints) == "joint 'LeftHand' has fewer than 6 joints above it");
		CHECK(joints.empty());

		ClearAssetCache clear;
		Scene scene;
		arm.ChainLength = 3;
		auto leg = Chain("LeftFoot", IKChainType::Fabrik, "LeftFoot");
		leg.ChainLength = 2;
		leg.Ground = true;
		auto biped = AddBiped(scene, { arm, leg });
		biped.GetComponent<InverseKinematicsComponent>().Pelvis = "Hips";
		biped.GetTransform().Translation = { 0, 0.2f, 0 };
		AddBox(scene, "Floor", { 0, -0.5f, 0 }, { 5, 0.5f, 5 });
		scene.OnSimulationStart();
		auto& system = *scene.GetAnimationSystem();
		const glm::vec3 target(0.4f, 1.3f, 0.35f);
		REQUIRE(system.SetIKTarget(biped, "Arm", target, std::nullopt));
		scene.OnUpdate(Step);
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), target, 1e-3f));
		// The foot reaches down 0.2 m to the floor (the pelvis drops with it), its ankle 0.05 m above it, like a two-bone
		// ground chain.
		CHECK(BoneWorld(scene, biped, "LeftFoot").y == doctest::Approx(0.05f).epsilon(1e-3));
		CHECK(BoneWorld(scene, biped, "LeftFoot").x == doctest::Approx(0.1f).epsilon(1e-3));
		scene.OnSimulationStop();
	}

	TEST_CASE("FollowTarget chains follow the component's Target until a script target replaces it")
	{
		ClearAssetCache clear;
		Scene scene;
		auto look = Chain("Look", IKChainType::LookAt, "Head");
		look.FollowTarget = true;
		look.MaxAngle = 180;
		auto arm = Chain("LeftArm", IKChainType::TwoBone, "LeftHand");
		arm.FollowTarget = true;
		auto biped = AddBiped(scene, { look, arm });
		auto ball = scene.CreateEntity("Ball");
		ball.GetTransform().Translation = { 0.3f, 1.2f, 0.3f };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const glm::vec3 animated = BoneWorld(scene, biped, "LeftHand");
		// Read every frame, so a Target set while playing applies at the next solve.
		biped.GetComponent<InverseKinematicsComponent>().Target = ball.GetUUID();
		const auto headAim = [&] { return glm::vec3(BoneWorldTransform(scene, biped, "Head") * glm::vec4(0, 0, 1, 0)); };
		system.Update(Step);
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), glm::vec3(0.3f, 1.2f, 0.3f), 1e-3f));
		CHECK(Degrees(headAim(), glm::vec3(0.3f, 1.2f, 0.3f) - BoneWorld(scene, biped, "Head")) < 0.1f);
		// Moving the target moves the chains with it.
		ball.GetTransform().Translation = { 0.35f, 1.2f, 0.2f };
		system.Update(Step);
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), glm::vec3(0.35f, 1.2f, 0.2f), 1e-3f));

		// A script target wins; clearing it follows the Target again.
		const glm::vec3 scripted(0.4f, 1.3f, 0.3f);
		REQUIRE(system.SetIKTarget(biped, "LeftArm", scripted, std::nullopt));
		system.Update(Step);
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), scripted, 1e-3f));
		REQUIRE(system.ClearIKTarget(biped, "LeftArm"));
		system.Update(Step);
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), glm::vec3(0.35f, 1.2f, 0.2f), 1e-3f));

		// Without FollowTarget a chain ignores the Target; a destroyed Target idles the chains and is logged once.
		biped.GetComponent<InverseKinematicsComponent>().Chains[1].FollowTarget = false;
		system.Update(Step);
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), animated));
		const auto since = Log::GetHistory().GetTotalCount();
		scene.DestroyEntity(ball);
		system.Update(Step);
		system.Update(Step);
		CHECK(CountLogMessages(since, "the Target entity no longer exists") == 1);
		CHECK(Degrees(headAim(), glm::vec3(0, 0, 1)) < 0.01f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Foot locks hold planted joints in place until the animation carries them away")
	{
		ClearAssetCache clear;
		Scene scene;
		// Wave half a second in holds the right arm raised and bent: the hand stands in for a planted foot that IK can
		// still reach as the body moves under it.
		auto hand = Chain("RightHand", IKChainType::TwoBone, "RightHand");
		hand.FootLock = true;
		auto biped = AddBiped(scene, { hand, Chain("LeftHand", IKChainType::TwoBone, "LeftHand") });
		biped.GetComponent<AnimatorComponent>().Clip = "Wave";
		auto& ik = biped.GetComponent<InverseKinematicsComponent>();
		ik.Smoothing = 0;
		ik.LockDistance = 0.1f;
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.Seek(biped, 0.5f));
		// The first solve has no speed yet; standing still then locks the hand where it stands.
		system.Update(Step);
		CHECK(!Locked(system, biped, "RightHand"));
		system.Update(Step);
		REQUIRE(Locked(system, biped, "RightHand"));
		// A chain without FootLock never locks.
		CHECK(!Locked(system, biped, "LeftHand"));
		const glm::vec3 planted = BoneWorld(scene, biped, "RightHand");

		// The body moving under it leaves the hand where it was planted...
		for (int i = 1; i <= 4; i++)
		{
			biped.GetTransform().Translation.x = 0.02f * static_cast<float>(i);
			system.Update(Step);
			CHECK(Near(BoneWorld(scene, biped, "RightHand"), planted, 1e-3f));
		}
		// ...while its height follows the animation (and the body): a lock holds the position across the up axis only.
		biped.GetTransform().Translation.y = -0.05f;
		system.Update(Step);
		CHECK(Locked(system, biped, "RightHand"));
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), planted - glm::vec3(0, 0.05f, 0), 1e-3f));
		// A crouch (the body dropping further, straight down) only lowers the hand: the lock measures its distance across the
		// up axis.
		biped.GetTransform().Translation.y = -0.15f;
		system.Update(Step);
		CHECK(Locked(system, biped, "RightHand"));
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), planted - glm::vec3(0, 0.15f, 0), 1e-3f));
		biped.GetTransform().Translation.y = -0.05f;
		system.Update(Step);
		// Once the animated hand is LockDistance away it lets go at once, and while it moves faster than LockSpeed (it
		// moved 0.04 m in a 60th of a second here) it does not lock again.
		biped.GetTransform().Translation.x = 0.12f;
		system.Update(Step);
		CHECK(!Locked(system, biped, "RightHand"));
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), planted + glm::vec3(0.12f, -0.05f, 0), 1e-3f));
		// Standing still locks it again, where it now stands.
		system.Update(Step);
		CHECK(Locked(system, biped, "RightHand"));

		// A script target releases the lock; the lock only engages again once the target is cleared.
		REQUIRE(system.SetIKTarget(biped, "RightHand", BoneWorld(scene, biped, "RightHand") + glm::vec3(0, -0.05f, 0), std::nullopt));
		system.Update(Step);
		CHECK(!Locked(system, biped, "RightHand"));
		REQUIRE(system.ClearIKTarget(biped, "RightHand"));
		system.Update(Step);
		system.Update(Step);
		CHECK(Locked(system, biped, "RightHand"));
		// A target entity that is gone by the next solve still releases the lock, and so does a zero-scale frame.
		auto doomed = scene.CreateEntity("Doomed");
		REQUIRE(system.SetIKTarget(biped, "RightHand", doomed, false));
		scene.DestroyEntity(doomed);
		system.Update(Step);
		CHECK(!Locked(system, biped, "RightHand"));
		REQUIRE(system.ClearIKTarget(biped, "RightHand"));
		system.Update(Step);
		system.Update(Step);
		REQUIRE(Locked(system, biped, "RightHand"));
		biped.GetTransform().Scale = glm::vec3(0);
		system.Update(Step);
		CHECK(!Locked(system, biped, "RightHand"));
		biped.GetTransform().Scale = glm::vec3(1);
		system.Update(Step);
		system.Update(Step);
		REQUIRE(Locked(system, biped, "RightHand"));
		// So does a chain that stops resolving.
		ik.Chains[0].Joint = "Missing";
		system.Update(Step);
		CHECK(!Locked(system, biped, "RightHand"));
		ik.Chains[0].Joint = "RightHand";
		system.Update(Step);
		system.Update(Step);
		// A chain faded to zero weight has no influence and plants nothing; restoring the weight starts afresh.
		REQUIRE(Locked(system, biped, "RightHand"));
		REQUIRE(system.SetIKWeight(biped, "RightHand", 0, 0));
		system.Update(Step);
		CHECK(!Locked(system, biped, "RightHand"));
		REQUIRE(system.SetIKWeight(biped, "RightHand", 1, 0));
		system.Update(Step);
		system.Update(Step);
		CHECK(Locked(system, biped, "RightHand"));
		// Turning FootLock off releases it too, and turning it back on starts afresh where the hand stands.
		ik.Chains[0].FootLock = false;
		system.Update(Step);
		CHECK(!Locked(system, biped, "RightHand"));
		biped.GetTransform().Translation.x = 0;
		const glm::vec3 moved = BoneWorld(scene, biped, "RightHand");
		ik.Chains[0].FootLock = true;
		system.Update(Step);
		system.Update(Step);
		CHECK(Locked(system, biped, "RightHand"));
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), moved, 1e-3f));

		bool missing = true;
		CHECK_FALSE(system.GetIKFootLock(biped, "Missing", missing));
		CHECK(missing);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Foot locks release when the animation lifts the joint and ease back with Smoothing")
	{
		ClearAssetCache clear;
		Scene scene;
		// Wave raises the right arm: the hand stands in for a foot the animation lifts.
		auto hand = Chain("RightHand", IKChainType::TwoBone, "RightHand");
		hand.FootLock = true;
		auto biped = AddBiped(scene, { hand });
		biped.GetComponent<AnimatorComponent>().Clip = "Wave";
		auto& ik = biped.GetComponent<InverseKinematicsComponent>();
		ik.Smoothing = 0.1f;
		ik.LockHeight = 0.05f;
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		system.Update(Step);
		system.Update(Step);
		// Locking eases in over Smoothing, and a held pose keeps the hand where it locked.
		for (int i = 0; i < 60; i++)
			system.Update(Step);
		REQUIRE(Locked(system, biped, "RightHand"));
		const glm::vec3 planted = BoneWorld(scene, biped, "RightHand");

		// Seeking 0.1 s into the wave lifts the hand about 7 cm: past LockHeight, so the lock releases and eases out.
		REQUIRE(system.Seek(biped, 0.1f));
		system.Update(Step);
		CHECK(!Locked(system, biped, "RightHand"));
		const glm::vec3 easing = BoneWorld(scene, biped, "RightHand");
		CHECK(glm::length(easing - planted) > 1e-3f);
		system.Update(1.0f);
		const glm::vec3 lifted = BoneWorld(scene, biped, "RightHand");
		CHECK(glm::length(lifted - planted) > glm::length(easing - planted));
		CHECK(lifted.y - planted.y > ik.LockHeight);

		// A lock the body is carried away from lets go without easing: the hand is back on its animation that frame.
		system.Update(Step);
		REQUIRE(Locked(system, biped, "RightHand"));
		for (int i = 0; i < 60; i++)
			system.Update(Step);
		biped.GetTransform().Translation.x = 1.0f;
		system.Update(Step);
		CHECK(!Locked(system, biped, "RightHand"));
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), lifted + glm::vec3(1, 0, 0), 1e-3f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Foot locks hold ground feet, release on unsolved frames and in the air")
	{
		ClearAssetCache clear;
		Scene scene;
		auto left = Chain("LeftFoot", IKChainType::Fabrik, "LeftFoot");
		left.ChainLength = 2;
		left.Ground = true;
		left.FootLock = true;
		auto biped = AddBiped(scene, { left });
		auto& ik = biped.GetComponent<InverseKinematicsComponent>();
		ik.Smoothing = 0;
		ik.Pelvis = "Hips";
		biped.GetTransform().Translation = { 0, 0.1f, 0 };
		AddBox(scene, "Floor", { 0, -0.5f, 0 }, { 5, 0.5f, 5 });
		scene.OnSimulationStart();
		auto& system = *scene.GetAnimationSystem();
		scene.OnUpdate(Step);
		scene.OnUpdate(Step);
		REQUIRE(Locked(system, biped, "LeftFoot"));
		// The foot stands on the floor (its ankle 0.05 m up) and stays planted while the body sways over it.
		const glm::vec3 planted = BoneWorld(scene, biped, "LeftFoot");
		CHECK(planted.y == doctest::Approx(0.05f).epsilon(1e-3));
		biped.GetTransform().Translation.z = -0.05f;
		scene.OnUpdate(Step);
		CHECK(Near(BoneWorld(scene, biped, "LeftFoot"), planted, 2e-3f));
		// An Animator that cannot play solves nothing, so no foot stays planted.
		biped.GetComponent<AnimatorComponent>().Mesh = "Missing.gltf";
		scene.OnUpdate(Step);
		CHECK(!Locked(system, biped, "LeftFoot"));
		scene.OnSimulationStop();
	}

	TEST_CASE("The edit preview solves IK toward the Target entity, layers and spring bones")
	{
		ClearAssetCache cacheGuard;
		Scene scene;
		auto arm = Chain("LeftArm", IKChainType::TwoBone, "LeftHand");
		arm.FollowTarget = true;
		auto biped = AddBiped(scene, { arm });
		auto& animator = biped.GetComponent<AnimatorComponent>();
		animator.Clip = "Walk";
		animator.Preview = true;
		AnimationLayer wave;
		wave.Name = "Wave";
		wave.Clip = "Wave";
		animator.Layers = { wave };
		auto ball = scene.CreateEntity("Ball");
		ball.GetTransform().Translation = { 0.3f, 1.2f, 0.3f };
		biped.GetComponent<InverseKinematicsComponent>().Target = ball.GetUUID();
		SpringChain hair;
		hair.Joint = "RightUpperArm";
		biped.AddComponent<SpringBoneComponent>().Chains = { hair };
		// A rig the preview does not play stays silent: its IK is not solved and not reported either.
		auto still = AddBiped(scene, { Chain("LeftArm", IKChainType::TwoBone, "LeftHand") });
		still.GetTransform().Translation.x = 3;
		const auto since = Log::GetHistory().GetTotalCount();
		const json before = SceneSerializer::SerializeScene(scene);
		scene.UpdateAnimationPreview(Step);
		AnimationSystem& preview = *scene.GetPoseAnimationSystem();
		const auto source = AssetManager::GetMesh(animator.Mesh);
		const auto handWorld = [&] {
			glm::mat4 hand;
			std::string error;
			REQUIRE(preview.GetBoneTransform(biped, "LeftHand", hand, error));
			return glm::vec3(scene.GetWorldTransform(biped) * hand[3]);
		};
		CHECK(Near(handWorld(), glm::vec3(0.3f, 1.2f, 0.3f), 1e-3f));
		// Dragging the target in the editor moves the hand at the next preview update.
		ball.GetTransform().Translation = { 0.35f, 1.2f, 0.2f };
		scene.UpdateAnimationPreview(Step);
		CHECK(Near(handWorld(), glm::vec3(0.35f, 1.2f, 0.2f), 1e-3f));
		// The layer plays (Wave raises the right arm, -90 degrees half a second in): exactly the clip's while the spring
		// chain on the arm has no weight, and swung behind once it has.
		auto& springs = biped.GetComponent<SpringBoneComponent>();
		springs.Chains[0].Weight = 0;
		scene.UpdateAnimationPreview(0.5f - 2 * Step);
		const auto& pose = *preview.GetPose(biped);
		const uint32_t upper = Node(*source, "RightUpperArm");
		const auto upperLocal = [&] { return glm::quat_cast(glm::mat3(glm::inverse(pose[source->Nodes[upper].Parent]) * pose[upper])); };
		CHECK(NearRotation(upperLocal(), glm::angleAxis(glm::radians(-90.0f), glm::vec3(0, 0, 1)), 1e-3f));
		springs.Chains[0].Weight = 1;
		scene.UpdateAnimationPreview(0);
		CHECK_FALSE(NearRotation(upperLocal(), glm::angleAxis(glm::radians(-90.0f), glm::vec3(0, 0, 1)), 1e-3f));
		CHECK(Math::IsFinite(pose[upper]));
		CHECK(CountLogMessages(since, "not solved") == 0);
		CHECK(preview.GetPose(still) == nullptr);
		// Only the ball and the spring weight moved: the preview wrote nothing.
		ball.GetTransform().Translation = { 0.3f, 1.2f, 0.3f };
		CHECK(SceneSerializer::SerializeScene(scene) == before);

		// The scene view draws the previewed pose: the skin palette follows it, while the other rig draws its rest pose.
		const auto palettes = [](Entity entity) {
			std::vector<glm::mat4> drawn;
			ForEachAnimatedMesh(entity, [&](const Ref<MeshSource>&, const MeshNode&, const glm::mat4&, const std::vector<glm::mat4>& palette, std::span<const float>) { drawn = palette; });
			return drawn;
		};
		std::vector<glm::mat4> rest;
		REQUIRE(EvaluateAnimation(*source, nullptr, 0, rest));
		const auto restPalette = BuildSkinPalette(source->Skins[0], rest);
		CHECK_FALSE(NearPose(palettes(biped), restPalette));
		CHECK(NearPose(palettes(biped), BuildSkinPalette(source->Skins[0], pose)));
		CHECK(NearPose(palettes(still), restPalette));
		scene.ResetAnimationPreview();
		CHECK(NearPose(palettes(biped), restPalette));
	}
}

TEST_SUITE("Animation")
{
	TEST_CASE("Authoring warnings cover layer graphs mask assets and IK targets and foot locks")
	{
		ClearAssetCache clear;
		TempProject project("FollowUpWarnings");
		Scene scene;
		const std::string graph = project.WriteFile("Assets/Animations/Upper.banimgraph", json{ { "InitialState", "A" }, { "States", json::array({ { { "Name", "A" }, { "Clip", "Gone" } }, { { "Name", "B" }, { "BlendSpace", "Nowhere" } } }) } }.dump());
		const std::string mask = project.WriteFile("Assets/Masks/Upper.banimmask", R"({ "Joints": [ { "Joint": "Chest" }, { "Joint": "Wing" } ] })");
		const std::string broken = project.WriteFile("Assets/Masks/Broken.banimmask", "{}");
		auto follow = Chain("Look", IKChainType::LookAt, "Head");
		follow.FollowTarget = true;
		auto lock = Chain("Foot", IKChainType::Fabrik, "LeftFoot");
		lock.ChainLength = 2;
		lock.FootLock = true;
		auto twoBone = Chain("Foot2", IKChainType::TwoBone, "LeftFoot");
		auto biped = AddBiped(scene, { follow, lock, twoBone });
		auto& ik = biped.GetComponent<InverseKinematicsComponent>();
		ik.LockSpeed = 0;
		AnimationLayer upper;
		upper.Name = "Upper";
		upper.Clip = "Wave";
		upper.StateMachine = graph;
		upper.Blend = AnimationLayerBlend::Additive;
		upper.Mask = { { "Chest", 1.0f } };
		upper.MaskAsset = mask;
		upper.Looping = false;
		AnimationLayer missing;
		missing.Name = "Missing";
		missing.StateMachine = "Assets/Animations/Missing.banimgraph";
		missing.MaskAsset = broken;
		biped.GetComponent<AnimatorComponent>().Layers = { upper, missing };
		const auto warnings = GetAnimationWarnings(biped);
		const auto has = [&](const std::string& text) { return std::ranges::any_of(warnings, [&](const std::string& warning) { return warning.starts_with(text); }); };
		CAPTURE(warnings);
		CHECK(has("Animator layer 'Upper': Clip is unused; the StateMachine replaces it"));
		CHECK(has("Animator layer 'Upper': an Additive layer with a StateMachine needs a ReferenceClip"));
		CHECK(has("Animator layer 'Upper' state 'A': clip 'Gone' not found"));
		CHECK(has("Animator layer 'Upper' state 'B': blend space 'Nowhere' not found"));
		CHECK(has("Animator layer 'Upper': Mask is unused; the MaskAsset replaces it"));
		CHECK(has("Animator layer 'Upper': mask joint 'Wing' not found"));
		CHECK(has("Animator layer 'Missing': state machine could not be loaded"));
		CHECK(has("Animator layer 'Missing': mask could not be loaded, so the layer shows nothing"));
		CHECK(has("InverseKinematics chain 'Look': FollowTarget has no effect: the component has no Target"));
		CHECK(has("InverseKinematics chain 'Foot': FootLock never locks: LockSpeed is 0"));
		CHECK(has("Animator layer 'Upper': Looping is unused; each state of the StateMachine has its own"));
		// A Fabrik chain drives its end joint like a TwoBone one.
		CHECK(has("InverseKinematics chains 'Foot' and 'Foot2' both drive joint 'LeftFoot'"));
		CHECK(warnings.size() == 12);

		// A Target is reported when it is gone or nothing follows it.
		auto ball = scene.CreateEntity("Ball");
		ik.Target = ball.GetUUID();
		ik.Chains = { twoBone };
		CHECK(std::ranges::count(GetAnimationWarnings(biped), std::string("InverseKinematics Target has no effect: no chain has FollowTarget")) == 1);
		// FootLock cannot lock a foot that reaches for the Target.
		auto both = Chain("Both", IKChainType::TwoBone, "RightFoot");
		both.FollowTarget = true;
		both.FootLock = true;
		ik.Chains.push_back(both);
		CHECK(std::ranges::count(GetAnimationWarnings(biped), std::string("InverseKinematics chain 'Both': FootLock has no effect while FollowTarget reaches for the Target")) == 1);
		scene.DestroyEntity(ball);
		CHECK(std::ranges::count(GetAnimationWarnings(biped), std::string("InverseKinematics Target entity not found")) == 1);
		// A powered ragdoll without torque.
		auto& ragdoll = biped.AddComponent<RagdollComponent>();
		ragdoll.MotorStrength = 1;
		ragdoll.MotorTorque = 0;
		CHECK(std::ranges::count(GetAnimationWarnings(biped), std::string("Ragdoll MotorStrength has no effect: MotorTorque is 0")) == 1);
	}
}

namespace {
	// The swing of `joint`'s bone away from where the rest pose points it, and its twist about the bone, in degrees: how
	// ResolveFabrikLimits's limits read a pose.
	struct JointTurn
	{
		float Swing = 0, Twist = 0;
	};
	JointTurn MeasureTurn(const Rig& rig, uint32_t joint, uint32_t child)
	{
		const MeshNode& node = rig.Source->Nodes[child];
		const glm::vec3 axis = glm::normalize(rig.Source->Nodes[joint].Scale * (node.HasMatrix ? glm::vec3(node.Matrix[3]) : node.Translation));
		const glm::quat relative = glm::normalize(glm::inverse(rig.Source->Nodes[joint].Rotation) * rig.Pose.Nodes[joint].Rotation);
		const glm::vec3 swung = relative * axis;
		const glm::quat twist = glm::normalize(glm::inverse(RotationBetween(axis, swung)) * relative);
		const float along = glm::dot(glm::vec3(twist.x, twist.y, twist.z), axis);
		return { glm::degrees(std::acos(std::clamp(glm::dot(axis, swung), -1.0f, 1.0f))), glm::degrees(2.0f * std::atan2(twist.w < 0 ? -along : along, std::abs(twist.w))) };
	}
	std::vector<uint32_t> TorsoToHand(const MeshSource& source)
	{
		return { Node(source, "Hips"), Node(source, "Spine"), Node(source, "Chest"), Node(source, "LeftUpperArm"), Node(source, "LeftForeArm"), Node(source, "LeftHand") };
	}
	std::vector<FabrikJointLimit> Limits(size_t count, float cone, float minTwist = -180, float maxTwist = 180)
	{
		return std::vector<FabrikJointLimit>(count, { .SwingAngle = glm::radians(cone), .TwistMin = glm::radians(minTwist), .TwistMax = glm::radians(maxTwist) });
	}
}

TEST_SUITE("Animation")
{
	TEST_CASE("Limited FABRIK stays inside each joint's cone and twist range, however far the target")
	{
		const auto source = LoadBiped();
		const std::vector<uint32_t> joints = LeftArmChain(*source);
		for (const glm::vec3& target : { glm::vec3(0.1f, 1.2f, 0.6f), glm::vec3(5, 5, 5), glm::vec3(-3, 0, 0), glm::vec3(0.2f, 1.45f, 0) })
		{
			CAPTURE(target);
			Rig rig(source);
			const glm::vec3 root = Position(rig.Globals, joints.front());
			const auto lengths = BoneLengths(rig.Globals, joints);
			const auto limits = Limits(3, 25, -10, 10);
			REQUIRE(SolveFabrikIK(*source, rig.Pose, rig.Globals, { .Joints = joints, .Target = target, .Limits = limits }) == IKSolveResult::Solved);
			for (size_t i = 0; i < 3; i++)
			{
				const JointTurn turn = MeasureTurn(rig, joints[i], joints[i + 1]);
				CHECK(turn.Swing <= doctest::Approx(25).epsilon(1e-3));
				CHECK(std::abs(turn.Twist) <= doctest::Approx(10).epsilon(1e-3));
			}
			// The limits turn joints; they never stretch bones or move the chain's root.
			CHECK(Near(Position(rig.Globals, joints.front()), root));
			const auto solved = BoneLengths(rig.Globals, joints);
			for (size_t i = 0; i < lengths.size(); i++)
				CHECK(solved[i] == doctest::Approx(lengths[i]).epsilon(1e-4));
		}

		// A limit means what it says: at 0 degrees the chain is rigid and the end stays put.
		Rig stiff(source);
		const glm::vec3 hand = Position(stiff.Globals, joints.back());
		const auto rigid = Limits(3, 0, 0, 0);
		REQUIRE(SolveFabrikIK(*source, stiff.Pose, stiff.Globals, { .Joints = joints, .Target = glm::vec3(1, 3, 2), .Limits = rigid }) == IKSolveResult::Solved);
		CHECK(Near(Position(stiff.Globals, joints.back()), hand));
	}

	TEST_CASE("Limited FABRIK measures from the rest pose, so an animated pose outside its limits comes back inside")
	{
		const auto source = LoadBiped();
		const std::vector<uint32_t> joints = LeftArmChain(*source);
		// The animation bent and twisted the elbow far outside; the target is exactly where that pose has the hand.
		Rig rig(source);
		const uint32_t elbow = joints[2];
		const glm::vec3 bone = glm::normalize(source->Nodes[joints[3]].Translation);
		rig.Pose.Nodes[elbow].Rotation = AxisAngle(glm::vec3(0, 0, 1), 70) * AxisAngle(bone, 80);
		REQUIRE(ComposeAnimationPose(*source, rig.Pose, rig.Globals));
		const JointTurn before = MeasureTurn(rig, elbow, joints[3]);
		CHECK(before.Swing == doctest::Approx(70).epsilon(1e-3));
		CHECK(before.Twist == doctest::Approx(80).epsilon(1e-3));
		const glm::vec3 target = Position(rig.Globals, joints.back());
		std::vector<FabrikJointLimit> limits = Limits(3, 180);
		limits[2] = Limits(1, 30, -20, 40)[0];
		REQUIRE(SolveFabrikIK(*source, rig.Pose, rig.Globals, { .Joints = joints, .Target = target, .Limits = limits }) == IKSolveResult::Solved);
		const JointTurn after = MeasureTurn(rig, elbow, joints[3]);
		CHECK(after.Swing <= doctest::Approx(30).epsilon(1e-3));
		CHECK(after.Twist == doctest::Approx(40).epsilon(1e-3));
		// The free joints above make up for it where they can.
		CHECK(Near(Position(rig.Globals, joints.back()), target, 2e-3f));

		// One-sided twist ranges read the sign: a pose outside them lands on the near end.
		Rig opposite(source);
		opposite.Pose.Nodes[elbow].Rotation = AxisAngle(bone, -50);
		REQUIRE(ComposeAnimationPose(*source, opposite.Pose, opposite.Globals));
		limits[2] = Limits(1, 180, 10, 60)[0];
		REQUIRE(SolveFabrikIK(*source, opposite.Pose, opposite.Globals, { .Joints = joints, .Target = Position(opposite.Globals, joints.back()), .Limits = limits }) == IKSolveResult::Solved);
		CHECK(MeasureTurn(opposite, elbow, joints[3]).Twist == doctest::Approx(10).epsilon(1e-3));
	}

	TEST_CASE("Limited FABRIK reaches targets its limits allow")
	{
		const auto source = LoadBiped();
		const std::vector<uint32_t> joints = TorsoToHand(*source);
		const Rig rest(source);
		float reach = 0;
		for (const float length : BoneLengths(rest.Globals, joints))
			reach += length;
		std::mt19937 random(11);
		const auto unit = [&] { return static_cast<float>(random()) / static_cast<float>(std::mt19937::max()); };
		for (const float cone : { 15.0f, 30.0f, 60.0f, 90.0f })
		{
			CAPTURE(cone);
			const auto limits = Limits(joints.size() - 1, cone);
			std::vector<float> errors;
			for (int i = 0; i < 200; i++)
			{
				// A pose the limits allow, so its end is a target they can reach.
				Rig posed(source);
				for (size_t j = 0; j + 1 < joints.size(); j++)
				{
					const glm::vec3 bone = glm::normalize(source->Nodes[joints[j + 1]].Translation);
					const glm::vec3 side = glm::normalize(glm::cross(bone, glm::vec3(unit(), unit(), unit()) - glm::vec3(0.5f)));
					posed.Pose.Nodes[joints[j]].Rotation = glm::angleAxis(glm::radians(cone * unit()), side);
				}
				REQUIRE(ComposeAnimationPose(*source, posed.Pose, posed.Globals));
				const glm::vec3 target = Position(posed.Globals, joints.back());
				Rig rig(source);
				REQUIRE(SolveFabrikIK(*source, rig.Pose, rig.Globals, { .Joints = joints, .Target = target, .Limits = limits }) == IKSolveResult::Solved);
				errors.push_back(glm::length(Position(rig.Globals, joints.back()) - target));
				for (size_t j = 0; j + 1 < joints.size(); j++)
					REQUIRE(MeasureTurn(rig, joints[j], joints[j + 1]).Swing <= doctest::Approx(cone).epsilon(1e-3));
			}
			std::ranges::sort(errors);
			CHECK(errors[errors.size() / 2] < 2 * FabrikTolerance * reach);
			CHECK(errors[errors.size() * 9 / 10] < 1e-3f * reach);
			CHECK(errors.back() < 0.03f * reach);
		}
	}

	TEST_CASE("Limited FABRIK keeps the end rotation exact, blends by weight and reads mirrored and boneless joints")
	{
		auto source = CreateRef<MeshSource>(*LoadBiped());
		const std::vector<uint32_t> joints = LeftArmChain(*source);
		const auto limits = Limits(3, 40);
		// The end joint is no turned joint: its target rotation stays exact.
		Rig turned(source);
		const glm::quat rotation = AxisAngle(glm::vec3(1, 0, 0), 60);
		REQUIRE(SolveFabrikIK(*source, turned.Pose, turned.Globals, { .Joints = joints, .Target = glm::vec3(0.5f, 1.2f, 0.3f), .EndRotation = rotation, .Limits = limits }) == IKSolveResult::Solved);
		CHECK(NearRotation(GlobalRotation(turned.Globals[joints.back()]), rotation));

		// Weight 0 changes nothing; half blends the solved rotations with the animated ones, inside the cone too.
		Rig none(source);
		const auto animated = none.Globals;
		CHECK(SolveFabrikIK(*source, none.Pose, none.Globals, { .Joints = joints, .Target = glm::vec3(0.5f, 1.2f, 0.3f), .Weight = 0, .Limits = limits }) == IKSolveResult::Unchanged);
		CHECK(NearPose(none.Globals, animated));
		Rig full(source), half(source);
		REQUIRE(SolveFabrikIK(*source, full.Pose, full.Globals, { .Joints = joints, .Target = glm::vec3(0.5f, 1.2f, 0.3f), .Limits = limits }) == IKSolveResult::Solved);
		REQUIRE(SolveFabrikIK(*source, half.Pose, half.Globals, { .Joints = joints, .Target = glm::vec3(0.5f, 1.2f, 0.3f), .Weight = 0.5f, .Limits = limits }) == IKSolveResult::Solved);
		CHECK(MeasureTurn(half, joints[1], joints[2]).Swing == doctest::Approx(MeasureTurn(full, joints[1], joints[2]).Swing * 0.5f).epsilon(1e-2));

		// A mirrored parent changes model space, not the limits, which live in the joint's own space.
		auto mirrored = CreateRef<MeshSource>(*source);
		mirrored->Nodes[Node(*mirrored, "Spine")].Scale = glm::vec3(-1, 1, 1);
		Rig flipped(mirrored);
		const glm::vec3 far = Position(flipped.Globals, joints.back()) + glm::vec3(0, -0.6f, 0.4f);
		REQUIRE(SolveFabrikIK(*mirrored, flipped.Pose, flipped.Globals, { .Joints = joints, .Target = far, .Limits = limits }) == IKSolveResult::Solved);
		for (size_t i = 0; i < 3; i++)
			CHECK(MeasureTurn(flipped, joints[i], joints[i + 1]).Swing <= doctest::Approx(40).epsilon(1e-3));
		CHECK(glm::length(Position(flipped.Globals, joints.back()) - far) < 0.2f);

		// A joint whose child lies on it has no bone to measure from: it stays free, and its neighbours stay limited. The free
		// joint keeps an animated turn the limits would have taken away.
		auto boneless = CreateRef<MeshSource>(*source);
		boneless->Nodes[joints[3]].Translation = glm::vec3(0);
		Rig stub(boneless);
		const glm::quat kept = AxisAngle(glm::vec3(0, 1, 0), 60);
		stub.Pose.Nodes[joints[2]].Rotation = kept;
		REQUIRE(ComposeAnimationPose(*boneless, stub.Pose, stub.Globals));
		const auto strict = Limits(3, 10, -1, 1);
		REQUIRE(SolveFabrikIK(*boneless, stub.Pose, stub.Globals, { .Joints = joints, .Target = glm::vec3(0.3f, 1.2f, 0.5f), .Limits = strict }) == IKSolveResult::Solved);
		CHECK(Math::IsFinite(stub.Globals[joints.back()]));
		CHECK(NearRotation(stub.Pose.Nodes[joints[2]].Rotation, kept));
		CHECK(MeasureTurn(stub, joints[1], joints[2]).Swing <= doctest::Approx(10).epsilon(1e-3));
	}

	TEST_CASE("Limited FABRIK reads each joint's own rest rotation, scale and child, and either sign of a rotation")
	{
		const auto biped = LoadBiped();
		const std::vector<uint32_t> joints = LeftArmChain(*biped);
		// Rest rotations that are not the identity: limits follow them, not the local rotation's own axes.
		auto turned = CreateRef<MeshSource>(*biped);
		const glm::quat rest = AxisAngle(glm::normalize(glm::vec3(1, 2, 3)), 70);
		turned->Nodes[joints[1]].Rotation = rest;
		turned->Nodes[joints[2]].Rotation = glm::inverse(rest);
		for (const glm::vec3& target : { glm::vec3(0.1f, 1.2f, 0.6f), glm::vec3(0.2f, 2, -1), glm::vec3(1, 1, 1) })
		{
			CAPTURE(target);
			Rig rig(turned);
			const auto limits = Limits(3, 20, -5, 15);
			REQUIRE(SolveFabrikIK(*turned, rig.Pose, rig.Globals, { .Joints = joints, .Target = target, .Limits = limits }) == IKSolveResult::Solved);
			for (size_t i = 0; i < 3; i++)
			{
				const JointTurn turn = MeasureTurn(rig, joints[i], joints[i + 1]);
				CHECK(turn.Swing <= doctest::Approx(20).epsilon(1e-3));
				CHECK(turn.Twist >= doctest::Approx(-5).epsilon(1e-3));
				CHECK(turn.Twist <= doctest::Approx(15).epsilon(1e-3));
			}
		}
		// A pose at its rest rotation is inside any limit, so it stays exactly there.
		Rig still(turned);
		const auto strict = Limits(3, 0, 0, 0);
		const auto before = still.Globals;
		REQUIRE(SolveFabrikIK(*turned, still.Pose, still.Globals, { .Joints = joints, .Target = glm::vec3(0, 5, 0), .Limits = strict }) == IKSolveResult::Solved);
		CHECK(NearPose(still.Globals, before));

		// -q is the same rotation as q: a twist past the half turn must not read as its opposite.
		const glm::vec3 bone = glm::normalize(biped->Nodes[joints[3]].Translation);
		for (const float sign : { 1.0f, -1.0f })
		{
			Rig rig(biped);
			rig.Pose.Nodes[joints[2]].Rotation = sign * AxisAngle(bone, 80);
			REQUIRE(ComposeAnimationPose(*biped, rig.Pose, rig.Globals));
			std::vector<FabrikJointLimit> one = Limits(3, 180);
			one[2] = Limits(1, 180, -20, 40)[0];
			REQUIRE(SolveFabrikIK(*biped, rig.Pose, rig.Globals, { .Joints = joints, .Target = Position(rig.Globals, joints.back()), .Limits = one }) == IKSolveResult::Solved);
			CHECK(MeasureTurn(rig, joints[2], joints[3]).Twist == doctest::Approx(40).epsilon(1e-3));
		}

		// A bone is where the joint's scale puts its child, not just the child's translation.
		auto stretched = CreateRef<MeshSource>(*biped);
		stretched->Nodes[joints[1]].Scale = glm::vec3(1, 4, 1);
		stretched->Nodes[joints[2]].Translation = glm::vec3(0.3f, 0.1f, 0);
		Rig scaled(stretched);
		const auto narrow = Limits(3, 10);
		REQUIRE(SolveFabrikIK(*stretched, scaled.Pose, scaled.Globals, { .Joints = joints, .Target = glm::vec3(0.1f, 2, 0.6f), .Limits = narrow }) == IKSolveResult::Solved);
		CHECK(MeasureTurn(scaled, joints[1], joints[2]).Swing <= doctest::Approx(10).epsilon(1e-3));
		CHECK(MeasureTurn(scaled, joints[1], joints[2]).Swing > 1);

		// A last joint given as a matrix still gives its parent a bone.
		const std::vector<uint32_t> toProp{ Node(*biped, "LeftForeArm"), Node(*biped, "LeftHand"), Node(*biped, "Prop") };
		Rig prop(biped);
		if (biped->Nodes[toProp[2]].Matrix[3] != glm::vec4(0, 0, 0, 1))
		{
			const auto twoBones = Limits(2, 15);
			REQUIRE(SolveFabrikIK(*biped, prop.Pose, prop.Globals, { .Joints = toProp, .Target = glm::vec3(0.5f, 1, 0.4f), .Limits = twoBones }) == IKSolveResult::Solved);
			CHECK(MeasureTurn(prop, toProp[1], toProp[2]).Swing <= doctest::Approx(15).epsilon(1e-3));
		}

		// However far a target, the result is inside the limits: an error too large to compare keeps the first pass.
		Rig far(biped);
		far.Pose.Nodes[joints[2]].Rotation = AxisAngle(glm::vec3(0, 0, 1), 70);
		REQUIRE(ComposeAnimationPose(*biped, far.Pose, far.Globals));
		const auto tight = Limits(3, 20);
		const float huge = std::numeric_limits<float>::max() / 4;
		REQUIRE(SolveFabrikIK(*biped, far.Pose, far.Globals, { .Joints = joints, .Target = glm::vec3(huge), .Limits = tight }) == IKSolveResult::Solved);
		CHECK(MeasureTurn(far, joints[2], joints[3]).Swing <= doctest::Approx(20).epsilon(1e-3));
	}

	TEST_CASE("Ground chains on flat ground still keep their limits")
	{
		ClearAssetCache clear;
		Scene scene;
		auto foot = Chain("Foot", IKChainType::Fabrik, "LeftFoot");
		foot.Ground = true;
		foot.ChainLength = 2;
		foot.SwingAngle = 2;
		auto biped = AddBiped(scene, { foot });
		biped.GetComponent<AnimatorComponent>().Clip = "Walk";
		biped.GetComponent<AnimatorComponent>().PlayOnStart = false;
		biped.GetComponent<InverseKinematicsComponent>().Smoothing = 0;
		AddBox(scene, "Floor", { 0, -0.5f, 0 }, { 5, 0.5f, 5 });
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.Seek(biped, 0.25f));
		RunAnimation(scene, 2);
		// The clip swings the upper leg 20 degrees; on flat ground the chain has nothing to reach for, but its limit holds.
		const auto source = AssetManager::GetMesh(biped.GetComponent<AnimatorComponent>().Mesh);
		const auto& pose = *system.GetPose(biped);
		const uint32_t upper = Node(*source, "LeftUpperLeg");
		const glm::vec3 axis = glm::normalize(source->Nodes[Node(*source, "LeftLowerLeg")].Translation);
		const glm::mat3 local(glm::inverse(pose[source->Nodes[upper].Parent]) * pose[upper]);
		CHECK(glm::degrees(std::acos(std::clamp(glm::dot(axis, glm::normalize(local * axis)), -1.0f, 1.0f))) <= doctest::Approx(2).epsilon(1e-2));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Fabrik chains resolve joint limits with per-joint overrides and leave free chains to the plain solver")
	{
		const auto source = LoadBiped();
		std::vector<uint32_t> joints;
		std::vector<FabrikJointLimit> limits;
		std::vector<std::string> problems;
		auto chain = Chain("Arm", IKChainType::Fabrik, "LeftHand");
		chain.ChainLength = 3;
		REQUIRE(ResolveIKChain(*source, chain, joints).empty());
		ResolveFabrikLimits(*source, chain, joints, limits, problems);
		CHECK(limits.empty());
		chain.SwingAngle = 30;
		chain.TwistMin = -10;
		chain.TwistMax = 20;
		chain.JointLimits = { { .Joint = "LeftForeArm", .SwingAngle = 5, .TwistMin = 0, .TwistMax = 1 }, { .Joint = "" }, { .Joint = "LeftHand" }, { .Joint = "RightHand" } };
		ResolveFabrikLimits(*source, chain, joints, limits, problems);
		REQUIRE(limits.size() == 3);
		CHECK(limits[0].SwingAngle == doctest::Approx(glm::radians(30.0f)));
		CHECK(limits[0].TwistMin == doctest::Approx(glm::radians(-10.0f)));
		CHECK(limits[1].TwistMax == doctest::Approx(glm::radians(20.0f)));
		CHECK(limits[2].SwingAngle == doctest::Approx(glm::radians(5.0f)));
		CHECK(limits[2].TwistMax == doctest::Approx(glm::radians(1.0f)));
		// The end joint is not turned, and an entry without a name is an inspector's addition.
		REQUIRE(problems.size() == 2);
		CHECK(problems[0] == "JointLimits: 'LeftHand' is not a joint the chain turns");
		CHECK(problems[1] == "JointLimits: 'RightHand' is not a joint the chain turns");

		// An entry leaves out nothing it names: what it does not set is free, not the chain's.
		chain.JointLimits = { { .Joint = "LeftForeArm", .SwingAngle = 5 } };
		problems.clear();
		ResolveFabrikLimits(*source, chain, joints, limits, problems);
		CHECK(problems.empty());
		CHECK(limits[2].SwingAngle == doctest::Approx(glm::radians(5.0f)));
		CHECK(limits[2].TwistMax == doctest::Approx(glm::pi<float>()));
		CHECK(limits[1].TwistMax == doctest::Approx(glm::radians(20.0f)));

		// A bad entry leaves its own joint free even while the chain's limits are good.
		chain.JointLimits = { { .Joint = "LeftForeArm", .SwingAngle = 500 } };
		ResolveFabrikLimits(*source, chain, joints, limits, problems);
		REQUIRE(problems.size() == 1);
		CHECK(limits[2].SwingAngle == doctest::Approx(glm::pi<float>()));
		CHECK(limits[1].SwingAngle == doctest::Approx(glm::radians(30.0f)));
		problems.clear();

		// Values past Validate leave their joint free, and say so.
		chain.JointLimits = { { .Joint = "LeftForeArm", .SwingAngle = std::numeric_limits<float>::quiet_NaN() } };
		chain.TwistMin = 50;
		chain.TwistMax = 20;
		problems.clear();
		ResolveFabrikLimits(*source, chain, joints, limits, problems);
		CHECK(limits.empty());
		REQUIRE(problems.size() == 2);
		CHECK(problems[0] == "SwingAngle, TwistMin or TwistMax is out of range; those joints stay free");
		CHECK(problems[1] == "JointLimits: the limits of 'LeftForeArm' are out of range; it stays free");
	}

	TEST_CASE("Joint limits reach the solve through the component, validate and warn")
	{
		ClearAssetCache clear;
		Scene scene;
		auto chain = Chain("Arm", IKChainType::Fabrik, "LeftHand");
		chain.ChainLength = 3;
		chain.SwingAngle = 20;
		auto biped = AddBiped(scene, { chain });
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const auto source = AssetManager::GetMesh(biped.GetComponent<AnimatorComponent>().Mesh);
		// How far the bone below `joint` swung from where the Biped's rest pose points it (rest rotations are identity).
		const auto swing = [&](const char* joint, const char* child) {
			const auto& pose = *system.GetPose(biped);
			const uint32_t node = Node(*source, joint);
			const glm::vec3 axis = glm::normalize(source->Nodes[Node(*source, child)].Translation);
			const glm::mat3 local = glm::mat3(glm::inverse(pose[source->Nodes[node].Parent]) * pose[node]);
			return glm::degrees(std::acos(std::clamp(glm::dot(axis, glm::normalize(local * axis)), -1.0f, 1.0f)));
		};
		REQUIRE(system.SetIKTarget(biped, "Arm", glm::vec3(0.1f, 1.1f, 0.7f), std::nullopt));
		RunAnimation(scene, 1);
		CHECK(swing("LeftForeArm", "LeftHand") <= doctest::Approx(20).epsilon(1e-2));
		CHECK(swing("LeftForeArm", "LeftHand") > 1);
		// A per-joint entry frees one joint of the chain, at once.
		biped.GetComponent<InverseKinematicsComponent>().Chains[0].JointLimits = { { .Joint = "LeftForeArm", .SwingAngle = 100 } };
		RunAnimation(scene, 1);
		CHECK(swing("LeftForeArm", "LeftHand") > 21);
		CHECK(swing("LeftUpperArm", "LeftForeArm") <= doctest::Approx(20).epsilon(1e-2));
		scene.OnRuntimeStop();

		const json limit = { { "Name", "Arm" }, { "Type", "Fabrik" }, { "Joint", "LeftHand" } };
		const std::vector<std::pair<json, std::string>> invalid = {
			{ { { "SwingAngle", -1 } }, "Chains[0].SwingAngle -1 must be between 0 and 180 degrees" },
			{ { { "SwingAngle", 181 } }, "SwingAngle 181" },
			{ { { "TwistMin", -181 } }, "twist -181 must be between -180 and 180 degrees" },
			{ { { "TwistMax", 200 } }, "twist 200" },
			{ { { "TwistMin", 20 }, { "TwistMax", 10 } }, "Chains[0].TwistMin 20 must not exceed TwistMax 10" },
			{ { { "JointLimits", { { { "Joint", "LeftForeArm" }, { "SwingAngle", 200 } } } } }, "Chains[0].JointLimits[0].SwingAngle 200" },
			{ { { "JointLimits", { { { "Joint", "LeftForeArm" } }, { { "Joint", "LeftForeArm" } } } } }, "Chains[0].JointLimits[1].Joint 'LeftForeArm' is already used by JointLimits[0]" },
		};
		Scene other;
		auto entity = other.CreateEntity("Rig");
		std::string error;
		for (const auto& [patch, message] : invalid)
		{
			json data = limit;
			data.update(patch);
			CAPTURE(data.dump());
			CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "InverseKinematics", { { "Chains", { data } } }, error));
			CHECK_MESSAGE(error.find(message) != std::string::npos, error);
		}
		// Several unnamed additions are fine, as in the inspector; the fields round-trip.
		json data = limit;
		data.update({ { "SwingAngle", 45 }, { "TwistMin", -5 }, { "TwistMax", 6 }, { "JointLimits", { { { "Joint", "" } }, { { "Joint", "" } }, { { "Joint", "LeftForeArm" }, { "SwingAngle", 7 } } } } });
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, "InverseKinematics", { { "Chains", { data } } }, error), error);
		const json written = ComponentRegistry::Find("InverseKinematics")->Serialize(entity);
		CHECK(written["Chains"][0]["SwingAngle"] == 45);
		CHECK(written["Chains"][0]["JointLimits"][2]["SwingAngle"] == 7);
		CHECK(written["Chains"][0]["JointLimits"][2]["TwistMax"] == 180);

		// A limit that holds every joint at rest is a chain that never moves.
		Scene frozen;
		auto stiff = Chain("Stiff", IKChainType::Fabrik, "LeftHand");
		stiff.SwingAngle = stiff.TwistMin = stiff.TwistMax = 0;
		const auto frozenWarnings = GetAnimationWarnings(AddBiped(frozen, { stiff }));
		CHECK(std::ranges::find(frozenWarnings, "InverseKinematics chain 'Stiff': its limits hold every joint at the rest pose, so it never reaches for a target") != frozenWarnings.end());
		// One free joint is enough for a chain to move.
		Scene moving;
		auto loose = Chain("Loose", IKChainType::Fabrik, "LeftHand");
		loose.ChainLength = 2;
		loose.SwingAngle = loose.TwistMin = loose.TwistMax = 0;
		loose.JointLimits = { { .Joint = "LeftForeArm", .SwingAngle = 10 } };
		const auto movingWarnings = GetAnimationWarnings(AddBiped(moving, { loose }));
		CHECK(std::ranges::none_of(movingWarnings, [](const std::string& line) { return line.find("Loose") != std::string::npos; }));
		// An unnamed entry is the inspector's addition: it limits nothing, on any chain type.
		auto blank = Chain("Blank", IKChainType::TwoBone, "LeftFoot");
		blank.JointLimits = { { .Joint = "" } };
		CHECK_FALSE(HasIKJointLimits(blank));

		// Warnings: limits on other chain types, and joints no chain turns.
		Scene warned;
		auto look = Chain("Look", IKChainType::LookAt, "Head");
		look.SwingAngle = 10;
		auto arm = Chain("Arm", IKChainType::Fabrik, "LeftHand");
		arm.JointLimits = { { .Joint = "Head" } };
		auto twoBone = Chain("Leg", IKChainType::TwoBone, "LeftFoot");
		twoBone.TwistMax = 10;
		auto rig = AddBiped(warned, { look, arm, twoBone });
		const auto warnings = GetAnimationWarnings(rig);
		const auto has = [&](const std::string& text) { return std::ranges::find(warnings, text) != warnings.end(); };
		CHECK(has("InverseKinematics chain 'Look': SwingAngle, TwistMin, TwistMax and JointLimits only apply to Fabrik chains"));
		CHECK(has("InverseKinematics chain 'Leg': SwingAngle, TwistMin, TwistMax and JointLimits only apply to Fabrik chains"));
		CHECK(has("InverseKinematics chain 'Arm': JointLimits: 'Head' is not a joint the chain turns"));
		// A chain the limits do apply to, and one with none, draw no such warning.
		Scene clean;
		auto fine = Chain("Fine", IKChainType::Fabrik, "LeftHand");
		fine.ChainLength = 2;
		fine.SwingAngle = 30;
		auto plain = Chain("Plain", IKChainType::TwoBone, "LeftFoot");
		const auto cleanWarnings = GetAnimationWarnings(AddBiped(clean, { fine, plain }));
		CHECK(std::ranges::none_of(cleanWarnings, [](const std::string& line) { return line.find("only apply to Fabrik") != std::string::npos || line.find("JointLimits") != std::string::npos; }));

		// The same problems reach the log once, from the solve, for chains the check cannot stop.
		const auto since = Log::GetHistory().GetTotalCount();
		warned.OnRuntimeStart();
		REQUIRE(warned.GetAnimationSystem()->SetIKTarget(rig, "Look", glm::vec3(0, 2, 2), std::nullopt));
		REQUIRE(warned.GetAnimationSystem()->SetIKTarget(rig, "Arm", glm::vec3(0.5f, 1.2f, 0.3f), std::nullopt));
		RunAnimation(warned, 1);
		RunAnimation(warned, 1);
		CHECK(CountLogMessages(since, "joint limits only apply to Fabrik chains; ignoring them") == 2);
		CHECK(CountLogMessages(since, "JointLimits: 'Head' is not a joint the chain turns") == 1);
		warned.OnRuntimeStop();
	}

	TEST_CASE("Ground chains stand on static bodies in the edit preview as they do in Play")
	{
		ClearAssetCache clear;
		Scene scene;
		auto left = Chain("LeftFoot", IKChainType::TwoBone, "LeftFoot");
		left.Ground = true;
		auto biped = AddBiped(scene, { left });
		auto& animator = biped.GetComponent<AnimatorComponent>();
		animator.Preview = true;
		auto& ik = biped.GetComponent<InverseKinematicsComponent>();
		ik.Smoothing = 0;
		ik.Pelvis = "Hips";
		// A step 0.2 high under the left foot, and one the same height the foot must not see: a dynamic body will have moved.
		auto step = AddBox(scene, "Step", { 0.1f, 0.05f, 0 }, { 0.2f, 0.1f, 0.2f });
		AddBox(scene, "Far", { 3, 0.05f, 0 }, { 0.2f, 0.1f, 0.2f });
		const json before = SceneSerializer::SerializeScene(scene);
		scene.UpdateAnimationPreview(Step);
		scene.UpdateAnimationPreview(Step);
		AnimationSystem& preview = *scene.GetPoseAnimationSystem();
		const auto footHeight = [&](AnimationSystem& system) {
			glm::mat4 bone(1.0f);
			std::string error;
			REQUIRE_MESSAGE(system.GetBoneTransform(biped, "LeftFoot", bone, error), error);
			return (scene.GetWorldTransform(biped) * bone)[3].y;
		};
		CHECK(footHeight(preview) == doctest::Approx(0.2f).epsilon(1e-3));
		// The feet end where Play puts them, on the same bodies.
		const float placed = footHeight(preview);
		CHECK(SceneSerializer::SerializeScene(scene) == before);
		CHECK(scene.GetPhysicsWorld() == nullptr);

		// A moved step follows at the next update, and a removed one lets the foot down, as dragging it in the editor does.
		step.GetTransform().Translation.y = 0.15f;
		scene.UpdateAnimationPreview(Step);
		CHECK(footHeight(preview) == doctest::Approx(0.3f).epsilon(1e-3));
		step.GetTransform().Translation.y = 0.05f;
		scene.UpdateAnimationPreview(Step);
		CHECK(footHeight(preview) == doctest::Approx(placed).epsilon(1e-3));
		// Only static bodies stand still when the game runs.
		step.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		scene.UpdateAnimationPreview(Step);
		CHECK(footHeight(preview) == doctest::Approx(0.05f).epsilon(1e-3));
		step.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		// The entity's own bodies, and those below it, are not ground (the rig would stand on itself).
		biped.AddComponent<RigidBodyComponent>();
		biped.AddComponent<BoxColliderComponent>().HalfExtents = { 1, 0.5f, 1 };
		auto ownChild = scene.CreateEntity("OwnChild");
		scene.SetParent(ownChild, biped, false);
		ownChild.AddComponent<RigidBodyComponent>();
		ownChild.AddComponent<BoxColliderComponent>().HalfExtents = { 1, 0.45f, 1 };
		scene.UpdateAnimationPreview(Step);
		CHECK(footHeight(preview) == doctest::Approx(placed).epsilon(1e-3));
		biped.RemoveComponent<BoxColliderComponent>();
		biped.RemoveComponent<RigidBodyComponent>();
		scene.DestroyEntity(ownChild);

		// So are a character controller's bodies above it, as they are in Play.
		auto controller = scene.CreateEntity("Controller");
		controller.AddComponent<CharacterControllerComponent>();
		scene.SetParent(biped, controller, false);
		auto ownBody = AddBox(scene, "OwnBody", { 0.1f, 0.2f, 0 }, { 0.3f, 0.2f, 0.3f });
		scene.SetParent(ownBody, controller, false);
		scene.UpdateAnimationPreview(Step);
		CHECK(footHeight(preview) == doctest::Approx(placed).epsilon(1e-3));
		scene.DestroyEntity(ownBody);
		scene.SetParent(biped, Entity(), false);
		scene.DestroyEntity(controller);

		// Play agrees.
		scene.OnRuntimeStart();
		RunAnimation(scene, 1);
		RunAnimation(scene, 1);
		CHECK(footHeight(*scene.GetAnimationSystem()) == doctest::Approx(placed).epsilon(1e-3));
		scene.OnRuntimeStop();
	}
}

namespace {
	float TwistAbout(const glm::quat& rotation, const glm::vec3& axis)
	{
		const glm::quat q = rotation.w < 0 ? -rotation : rotation;
		return glm::degrees(2.0f * std::atan2(glm::dot(glm::vec3(q.x, q.y, q.z), axis), q.w));
	}
	float AngleOf(const glm::quat& rotation)
	{
		return glm::degrees(2.0f * std::atan2(glm::length(glm::vec3(rotation.x, rotation.y, rotation.z)), std::abs(rotation.w)));
	}
	// A rig whose chain joints rest turned, as real skeletons do.
	Ref<MeshSource> TurnedRest(const std::vector<uint32_t>& joints)
	{
		auto source = CreateRef<MeshSource>(*LoadBiped());
		source->Nodes[joints[0]].Rotation = AxisAngle(glm::vec3(0, 1, 0), 15);
		source->Nodes[joints[1]].Rotation = AxisAngle(glm::vec3(0, 0, 1), 35);
		source->Nodes[joints[2]].Rotation = AxisAngle(glm::vec3(0, 1, 0), 50);
		return source;
	}
}

TEST_SUITE("Animation")
{
	TEST_CASE("The rest pose of a turned skeleton is inside any limits, and limits read from it")
	{
		const std::vector<uint32_t> joints = LeftArmChain(*LoadBiped());
		const auto source = TurnedRest(joints);
		const auto tight = Limits(3, 20, -10, 10);
		Rig rest(source);
		const glm::vec3 hand = Position(rest.Globals, joints.back());
		REQUIRE(SolveFabrikIK(*source, rest.Pose, rest.Globals, { .Joints = joints, .Target = hand, .Limits = tight }) == IKSolveResult::Solved);
		for (size_t i = 0; i < 3; i++)
			CHECK(NearRotation(rest.Pose.Nodes[joints[i]].Rotation, source->Nodes[joints[i]].Rotation, 1e-3f));

		// Outside the limits, measured from rest and not from identity, and pulled back to them.
		Rig bent(source);
		bent.Pose.Nodes[joints[2]].Rotation = source->Nodes[joints[2]].Rotation * AxisAngle(glm::vec3(0, 0, 1), 70);
		REQUIRE(ComposeAnimationPose(*source, bent.Pose, bent.Globals));
		REQUIRE(SolveFabrikIK(*source, bent.Pose, bent.Globals, { .Joints = joints, .Target = glm::vec3(0.4f, 1.2f, 0.4f), .Limits = tight }) == IKSolveResult::Solved);
		for (size_t i = 0; i < 3; i++)
		{
			const JointTurn turn = MeasureTurn(bent, joints[i], joints[i + 1]);
			CHECK(turn.Swing <= doctest::Approx(20).epsilon(1e-3));
			CHECK(std::abs(turn.Twist) <= doctest::Approx(10).epsilon(1e-3));
		}
	}

	TEST_CASE("A turned skeleton reaches what its limits allow as well as an upright one")
	{
		const auto biped = LoadBiped();
		const std::vector<uint32_t> joints = TorsoToHand(*biped);
		auto source = CreateRef<MeshSource>(*biped);
		for (size_t i = 1; i + 1 < joints.size(); i++)
			source->Nodes[joints[i]].Rotation = AxisAngle(glm::normalize(glm::vec3(1, static_cast<float>(i), 0.5f)), 25);
		const Rig rest(source);
		float reach = 0;
		for (const float length : BoneLengths(rest.Globals, joints))
			reach += length;
		std::mt19937 random(11);
		const auto unit = [&] { return static_cast<float>(random()) / static_cast<float>(std::mt19937::max()); };
		const auto limits = Limits(joints.size() - 1, 30);
		std::vector<float> errors;
		for (int i = 0; i < 200; i++)
		{
			Rig posed(source);
			for (size_t j = 0; j + 1 < joints.size(); j++)
			{
				const glm::vec3 bone = glm::normalize(source->Nodes[joints[j + 1]].Translation);
				const glm::vec3 side = glm::normalize(glm::cross(bone, glm::vec3(unit(), unit(), unit()) - glm::vec3(0.5f)));
				posed.Pose.Nodes[joints[j]].Rotation = source->Nodes[joints[j]].Rotation * glm::angleAxis(glm::radians(30 * unit()), side);
			}
			REQUIRE(ComposeAnimationPose(*source, posed.Pose, posed.Globals));
			const glm::vec3 target = Position(posed.Globals, joints.back());
			Rig rig(source);
			REQUIRE(SolveFabrikIK(*source, rig.Pose, rig.Globals, { .Joints = joints, .Target = target, .Limits = limits }) == IKSolveResult::Solved);
			errors.push_back(glm::length(Position(rig.Globals, joints.back()) - target));
		}
		std::ranges::sort(errors);
		CHECK(errors[100] < 2 * FabrikTolerance * reach);
		CHECK(errors[180] < 5e-3f * reach);
	}

	TEST_CASE("A pose stored as the negative quaternion reads as the same rotation")
	{
		const auto source = LoadBiped();
		const std::vector<uint32_t> joints = LeftArmChain(*source);
		const uint32_t elbow = joints[2];
		const glm::vec3 bone = glm::normalize(source->Nodes[joints[3]].Translation);
		Rig rig(source);
		rig.Pose.Nodes[elbow].Rotation = -AxisAngle(bone, 50);
		REQUIRE(ComposeAnimationPose(*source, rig.Pose, rig.Globals));
		std::vector<FabrikJointLimit> limits = Limits(3, 180);
		limits[2] = Limits(1, 180, -60, 60)[0];
		REQUIRE(SolveFabrikIK(*source, rig.Pose, rig.Globals, { .Joints = joints, .Target = Position(rig.Globals, joints.back()), .Limits = limits }) == IKSolveResult::Solved);
		CHECK(TwistAbout(rig.Pose.Nodes[elbow].Rotation, bone) == doctest::Approx(50).epsilon(1e-2));
	}

	TEST_CASE("A joint limited on one side of its twist alone is limited")
	{
		const auto source = LoadBiped();
		const std::vector<uint32_t> joints = LeftArmChain(*source);
		const uint32_t elbow = joints[2];
		const glm::vec3 bone = glm::normalize(source->Nodes[joints[3]].Translation);
		for (const auto& [start, minimum, maximum, wanted] : { std::array<float, 4>{ 80, -180, 30, 30 }, std::array<float, 4>{ -80, -30, 180, -30 } })
		{
			CAPTURE(start);
			Rig rig(source);
			rig.Pose.Nodes[elbow].Rotation = AxisAngle(bone, start);
			REQUIRE(ComposeAnimationPose(*source, rig.Pose, rig.Globals));
			std::vector<FabrikJointLimit> limits = Limits(3, 180);
			limits[2] = Limits(1, 180, minimum, maximum)[0];
			REQUIRE(SolveFabrikIK(*source, rig.Pose, rig.Globals, { .Joints = joints, .Target = Position(rig.Globals, joints.back()), .Limits = limits }) == IKSolveResult::Solved);
			CHECK(MeasureTurn(rig, elbow, joints[3]).Twist == doctest::Approx(wanted).epsilon(1e-3));
		}
	}

	TEST_CASE("The bone is where a joint's own scale puts its child")
	{
		auto source = CreateRef<MeshSource>(*LoadBiped());
		const std::vector<uint32_t> joints = LeftArmChain(*source);
		const uint32_t elbow = joints[2];
		source->Nodes[joints[3]].Translation = glm::vec3(0.1f, 0.2f, 0);
		source->Nodes[elbow].Scale = glm::vec3(1, 3, 1);
		const glm::vec3 axis = glm::normalize(source->Nodes[elbow].Scale * source->Nodes[joints[3]].Translation);
		Rig rig(source);
		rig.Pose.Nodes[elbow].Rotation = AxisAngle(axis, 50);
		REQUIRE(ComposeAnimationPose(*source, rig.Pose, rig.Globals));
		std::vector<FabrikJointLimit> limits = Limits(3, 180);
		limits[2] = Limits(1, 180, -10, 10)[0];
		REQUIRE(SolveFabrikIK(*source, rig.Pose, rig.Globals, { .Joints = joints, .Target = Position(rig.Globals, joints.back()), .Limits = limits }) == IKSolveResult::Solved);
		CHECK(AngleOf(rig.Pose.Nodes[elbow].Rotation) == doctest::Approx(10).epsilon(0.02));
		CHECK(TwistAbout(rig.Pose.Nodes[elbow].Rotation, axis) == doctest::Approx(10).epsilon(0.02));
	}

	TEST_CASE("An end joint given as a matrix still defines its parent's bone")
	{
		auto source = CreateRef<MeshSource>(*LoadBiped());
		const std::vector<uint32_t> joints = LeftArmChain(*source);
		const uint32_t elbow = joints[2];
		const glm::vec3 offset(0.1f, 0.2f, 0);
		source->Nodes[joints[3]].HasMatrix = true;
		source->Nodes[joints[3]].Matrix = glm::translate(glm::mat4(1.0f), offset);
		const glm::vec3 axis = glm::normalize(offset);
		Rig rig(source);
		rig.Pose.Nodes[elbow].Rotation = AxisAngle(axis, 50);
		REQUIRE(ComposeAnimationPose(*source, rig.Pose, rig.Globals));
		std::vector<FabrikJointLimit> limits = Limits(3, 180);
		limits[2] = Limits(1, 180, -10, 10)[0];
		REQUIRE(SolveFabrikIK(*source, rig.Pose, rig.Globals, { .Joints = joints, .Target = Position(rig.Globals, joints.back()), .Limits = limits }) == IKSolveResult::Solved);
		CHECK(AngleOf(rig.Pose.Nodes[elbow].Rotation) == doctest::Approx(10).epsilon(0.02));
		CHECK(TwistAbout(rig.Pose.Nodes[elbow].Rotation, axis) == doctest::Approx(10).epsilon(0.02));
	}

	TEST_CASE("A joint whose child lies on it keeps a turn past the limit")
	{
		auto source = CreateRef<MeshSource>(*LoadBiped());
		const std::vector<uint32_t> joints = LeftArmChain(*source);
		source->Nodes[joints[2]].Translation = glm::vec3(0);
		Rig rig(source);
		rig.Pose.Nodes[joints[1]].Rotation = AxisAngle(glm::vec3(0, 0, 1), 60);
		REQUIRE(ComposeAnimationPose(*source, rig.Pose, rig.Globals));
		const auto strict = Limits(3, 10);
		REQUIRE(SolveFabrikIK(*source, rig.Pose, rig.Globals, { .Joints = joints, .Target = Position(rig.Globals, joints.back()), .Limits = strict }) == IKSolveResult::Solved);
		CHECK(AngleOf(rig.Pose.Nodes[joints[1]].Rotation) > 30);
	}

	TEST_CASE("Bad values free only their own joint, at the edges of the ranges too")
	{
		const auto source = LoadBiped();
		std::vector<uint32_t> joints;
		std::vector<FabrikJointLimit> limits;
		std::vector<std::string> problems;
		auto chain = Chain("Arm", IKChainType::Fabrik, "LeftHand");
		chain.ChainLength = 3;
		REQUIRE(ResolveIKChain(*source, chain, joints).empty());
		chain.SwingAngle = 30;
		chain.JointLimits = { { .Joint = "LeftForeArm", .SwingAngle = std::numeric_limits<float>::quiet_NaN() } };
		ResolveFabrikLimits(*source, chain, joints, limits, problems);
		REQUIRE(limits.size() == 3);
		CHECK(limits[0].SwingAngle == doctest::Approx(glm::radians(30.0f)));
		CHECK(limits[2].SwingAngle == doctest::Approx(glm::pi<float>()));

		chain.JointLimits.clear();
		for (const auto& [cone, minimum, maximum] : { std::array<float, 3>{ 200, -180, 180 }, std::array<float, 3>{ -1, -180, 180 }, std::array<float, 3>{ 30, -200, 180 }, std::array<float, 3>{ 30, -180, 200 } })
		{
			CAPTURE(cone);
			CAPTURE(minimum);
			chain.SwingAngle = cone;
			chain.TwistMin = minimum;
			chain.TwistMax = maximum;
			problems.clear();
			ResolveFabrikLimits(*source, chain, joints, limits, problems);
			CHECK(limits.empty());
			CHECK(problems.size() == 1);
		}
	}

	TEST_CASE("Limits on a Fabrik chain are not reported as misplaced")
	{
		ClearAssetCache clear;
		Scene scene;
		auto arm = Chain("Arm", IKChainType::Fabrik, "LeftHand");
		arm.ChainLength = 3;
		arm.SwingAngle = 30;
		auto rig = AddBiped(scene, { arm });
		for (const std::string& warning : GetAnimationWarnings(rig))
			CHECK(warning.find("only apply to Fabrik chains") == std::string::npos);
	}
}
