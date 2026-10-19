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
#include <Basalt/Scripting/ScriptEngine.h>

#include <glm/gtc/epsilon.hpp>

using namespace Basalt;
using namespace BasaltTest;
using json = nlohmann::json;

using namespace AnimationTest;

namespace {
	glm::vec3 Position(const std::vector<glm::mat4>& globals, uint32_t node)
	{
		return glm::vec3(globals[node][3]);
	}
	float Degrees(const glm::vec3& a, const glm::vec3& b)
	{
		return glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0f, 1.0f)));
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
		CHECK(has("InverseKinematics chain 'PoleLook': Pole only applies to TwoBone chains"));
		CHECK(has("InverseKinematics chain 'PoleLook': MaxAngle 0 never turns the chain"));
		CHECK(has("InverseKinematics chain 'Arm2': ChainLength only applies to LookAt chains"));
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
		CHECK(CountLogMessages(since, "chain 'Look': Ground only applies to TwoBone chains") == 1);
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
