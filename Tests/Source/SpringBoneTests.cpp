#include <doctest/doctest.h>

#include "AnimationTestUtils.h"
#include "TestUtils.h"

#include <Basalt/Animation/AnimatedModel.h>
#include <Basalt/Animation/AnimationPose.h>
#include <Basalt/Animation/AnimationSystem.h>
#include <Basalt/Animation/SpringBones.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Math/Math.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scripting/ScriptEngine.h>

#include <glm/gtc/epsilon.hpp>

using namespace Basalt;
using namespace BasaltTest;
using json = nlohmann::json;

using namespace AnimationTest;

namespace {
	bool Near(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-4f)
	{
		return glm::all(glm::epsilonEqual(a, b, epsilon));
	}

	// A straight chain of `count` points spaced `length` apart along `direction` from `head`, at rest.
	struct Strand
	{
		Strand(const glm::vec3& head, const glm::vec3& direction, size_t count, float length = 0.3f)
		{
			for (size_t i = 0; i <= count; i++)
				Animated.push_back(head + direction * (length * static_cast<float>(i)));
			for (size_t i = 1; i <= count; i++)
				Points.push_back({ Animated[i], Animated[i] });
		}
		// Runs `frames` frames of `seconds` each with the animated chain held still.
		void Run(int frames, float seconds, SpringSimulationRequest request)
		{
			request.Animated = Animated;
			request.PreviousAnimated = Animated;
			request.Seconds = seconds;
			for (int i = 0; i < frames; i++)
				SimulateSpringChain(request, Points);
		}
		float Length(size_t point) const
		{
			const glm::vec3 head = point == 0 ? Animated[0] : Points[point - 1].Position;
			return glm::length(Points[point].Position - head);
		}
		std::vector<glm::vec3> Animated;
		std::vector<SpringPoint> Points;
	};

	constexpr glm::vec3 Gravity(0, -9.81f, 0);

	SpringChain RightArm(float stiffness = 0, float gravityFactor = 1)
	{
		SpringChain chain;
		chain.Name = "RightArm";
		chain.Joint = "RightUpperArm";
		chain.Stiffness = stiffness;
		chain.GravityFactor = gravityFactor;
		return chain;
	}
	// The Biped (arms straight out along X) with spring chains; its Animator holds the rest pose of "Lean".
	Entity AddBiped(Scene& scene, const std::vector<SpringChain>& chains)
	{
		auto entity = scene.CreateEntity("Biped");
		auto& animator = entity.AddComponent<AnimatorComponent>();
		animator.Mesh = BipedPath().string();
		animator.Clip = "Lean";
		animator.PlayOnStart = false;
		entity.AddComponent<SpringBoneComponent>().Chains = chains;
		return entity;
	}
	glm::vec3 BoneWorld(Scene& scene, Entity entity, const std::string& joint)
	{
		glm::mat4 bone(1.0f);
		std::string error;
		REQUIRE_MESSAGE(scene.GetAnimationSystem()->GetBoneTransform(entity, joint, bone, error), error);
		return glm::vec3(scene.GetWorldTransform(entity) * bone[3]);
	}
	void Run(Scene& scene, int frames, float seconds = Step)
	{
		for (int i = 0; i < frames; i++)
			scene.GetAnimationSystem()->Update(seconds);
	}
	// The right arm's two bone lengths in world space.
	std::pair<float, float> ArmLengths(Scene& scene, Entity biped)
	{
		const glm::vec3 shoulder = BoneWorld(scene, biped, "RightUpperArm"), elbow = BoneWorld(scene, biped, "RightForeArm"), hand = BoneWorld(scene, biped, "RightHand");
		return { glm::length(elbow - shoulder), glm::length(hand - elbow) };
	}
}

TEST_SUITE("Animation")
{
	TEST_CASE("Spring chains resolve along first children down to a tip")
	{
		const auto source = LoadBiped();
		std::vector<uint32_t> joints;
		SpringChain chain = RightArm();
		CHECK(ResolveSpringChain(*source, chain, joints).empty());
		CHECK(joints == std::vector<uint32_t>{ Node(*source, "RightUpperArm"), Node(*source, "RightForeArm"), Node(*source, "RightHand") });
		chain.ChainLength = 1;
		CHECK(ResolveSpringChain(*source, chain, joints).empty());
		CHECK(joints == std::vector<uint32_t>{ Node(*source, "RightUpperArm"), Node(*source, "RightForeArm") });
		chain.ChainLength = 3;
		CHECK(ResolveSpringChain(*source, chain, joints) == "joint 'RightUpperArm' has fewer than 3 joints in a line below it");
		CHECK(joints.empty());
		chain.ChainLength = 0;
		chain.Joint = "RightHand";
		CHECK(ResolveSpringChain(*source, chain, joints) == "joint 'RightHand' has no child to swing toward");
		chain.Joint = "Nope";
		CHECK(ResolveSpringChain(*source, chain, joints).find("joint 'Nope' not found") == 0);
		chain.Joint.clear();
		CHECK(ResolveSpringChain(*source, chain, joints) == "Joint is empty");
		// The first child is followed: Chest's first child is Head, a leaf.
		chain.Joint = "Chest";
		CHECK(ResolveSpringChain(*source, chain, joints).empty());
		CHECK(joints == std::vector<uint32_t>{ Node(*source, "Chest"), Node(*source, "Head") });

		// A fixed-matrix joint cannot turn, but it can be the tip.
		MeshSource fixed;
		fixed.Nodes.resize(3);
		fixed.Nodes[0].Name = "Root";
		fixed.Nodes[1].Name = "Fixed";
		fixed.Nodes[1].Parent = 0;
		fixed.Nodes[1].HasMatrix = true;
		fixed.Nodes[2].Name = "Tip";
		fixed.Nodes[2].Parent = 1;
		REQUIRE(fixed.FinalizeHierarchy());
		chain.Joint = "Root";
		chain.ChainLength = 1;
		CHECK(ResolveSpringChain(fixed, chain, joints).empty());
		chain.ChainLength = 2;
		CHECK(ResolveSpringChain(fixed, chain, joints) == "joint 'Fixed' has a fixed matrix and cannot be turned");
	}

	TEST_CASE("Spring points hang under gravity keep their lengths and settle on their stiffness")
	{
		// No forces: a chain at rest stays exactly where it is.
		Strand still({ 0, 2, 0 }, { 1, 0, 0 }, 3);
		still.Run(120, Step, { .Stiffness = 40, .Damping = 4 });
		for (size_t i = 0; i < 3; i++)
			CHECK(still.Points[i].Position == still.Animated[i + 1]);

		// Without stiffness gravity hangs it straight down from the head, every bone keeping its length.
		Strand hanging({ 0, 2, 0 }, { 1, 0, 0 }, 3);
		hanging.Run(600, Step, { .Stiffness = 0, .Damping = 4, .Acceleration = Gravity });
		for (size_t i = 0; i < 3; i++)
		{
			CAPTURE(i);
			CHECK(hanging.Length(i) == doctest::Approx(0.3f).epsilon(1e-4));
			CHECK(Near(hanging.Points[i].Position, glm::vec3(0, 2 - 0.3f * static_cast<float>(i + 1), 0), 2e-3f));
		}

		// Stiffness holds it partway: stiffer droops less.
		float previousDrop = 1;
		for (float stiffness : { 20.0f, 100.0f, 1000.0f })
		{
			CAPTURE(stiffness);
			Strand held({ 0, 2, 0 }, { 1, 0, 0 }, 3);
			held.Run(600, Step, { .Stiffness = stiffness, .Damping = 4, .Acceleration = Gravity });
			const float drop = 2 - held.Points[2].Position.y;
			CHECK(drop > 0);
			CHECK(drop < previousDrop);
			CHECK(held.Length(2) == doctest::Approx(0.3f).epsilon(1e-4));
			previousDrop = drop;
		}
		CHECK(previousDrop < 0.1f);
	}

	TEST_CASE("Spring points stay outside sphere and capsule colliders")
	{
		// A sphere under the first bone, and a capsule across where the tip would hang.
		const SpringCollisionShape sphere{ .A = { 0.2f, 1.6f, 0 }, .B = { 0.2f, 1.6f, 0 }, .Radius = 0.15f };
		const SpringCollisionShape capsule{ .A = { -1, 1.45f, 0.05f }, .B = { 1, 1.45f, 0.05f }, .Radius = 0.1f };
		for (const auto& [shape, name] : { std::pair{ sphere, "sphere" }, std::pair{ capsule, "capsule" } })
		{
			CAPTURE(name);
			Strand strand({ 0, 2, 0 }, { 1, 0, 0 }, 3);
			const std::array shapes{ shape };
			strand.Run(600, Step, { .Colliders = shapes, .Stiffness = 0, .Damping = 4, .Radius = 0.02f, .Acceleration = Gravity });
			for (size_t i = 0; i < 3; i++)
			{
				CAPTURE(i);
				const glm::vec3 axis = shape.B - shape.A;
				const glm::vec3 p = strand.Points[i].Position;
				const float t = glm::dot(axis, axis) > 0 ? std::clamp(glm::dot(p - shape.A, axis) / glm::dot(axis, axis), 0.0f, 1.0f) : 0.0f;
				CHECK(glm::length(p - (shape.A + axis * t)) >= shape.Radius + 0.02f - 2e-3f);
				CHECK(strand.Length(i) == doctest::Approx(0.3f).epsilon(1e-4));
			}
			// The collider held the chain off the straight hang below the head.
			CHECK(glm::length(glm::vec2(strand.Points[2].Position.x, strand.Points[2].Position.z)) > 0.05f);
		}
	}

	TEST_CASE("Spring points lag behind a moving chain and do not depend much on the frame rate")
	{
		// The head moves along +Z at 2 m/s: the tip trails behind it, then springs back once it stops.
		const auto move = [](float seconds, int frames) {
			Strand strand({ 0, 2, 0 }, { 1, 0, 0 }, 3);
			std::vector<glm::vec3> previous = strand.Animated;
			for (int i = 0; i < frames; i++)
			{
				std::vector<glm::vec3> animated = previous;
				for (glm::vec3& position : animated)
					position.z += 2 * seconds;
				SimulateSpringChain({ .Animated = animated, .PreviousAnimated = previous, .Stiffness = 60, .Damping = 4, .Seconds = seconds }, strand.Points);
				previous = std::move(animated);
			}
			strand.Animated = std::move(previous);
			return strand;
		};
		Strand at60 = move(1.0f / 60, 30);
		const float lag = at60.Animated[3].z - at60.Points[2].Position.z;
		CHECK(lag > 0.05f);
		CHECK(at60.Length(2) == doctest::Approx(0.3f).epsilon(1e-4));
		// Thirty frames per second for the same half second: substeps keep the result close.
		Strand at30 = move(1.0f / 30, 15);
		CHECK(std::abs((at30.Animated[3].z - at30.Points[2].Position.z) - lag) < 0.01f);

		at60.Run(600, Step, { .Stiffness = 60, .Damping = 4 });
		CHECK(Near(at60.Points[2].Position, at60.Animated[3], 1e-3f));

		// A long hitch simulates at most MaxSpringSteps substeps, so even the stiffest spring stays finite and stable.
		Strand stiff({ 0, 2, 0 }, { 1, 0, 0 }, 3);
		stiff.Points[2].Position += glm::vec3(0, -0.2f, 0);
		stiff.Run(20, 1.0f, { .Stiffness = 10000, .Damping = 0, .Acceleration = Gravity });
		for (const SpringPoint& point : stiff.Points)
			CHECK(Math::IsFinite(point.Position));
		CHECK(stiff.Length(2) == doctest::Approx(0.3f).epsilon(1e-4));
	}

	TEST_CASE("Spring bones swing an Animator's joints after LateUpdate")
	{
		ClearAssetCache clear;
		Scene scene;
		auto biped = AddBiped(scene, { RightArm() });
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const glm::vec3 shoulder = BoneWorld(scene, biped, "RightUpperArm");
		// The clip leans the spine, so the arm points out and a little down.
		const glm::vec3 animated = BoneWorld(scene, biped, "RightHand");
		REQUIRE(glm::length(animated - shoulder) == doctest::Approx(0.6f));
		REQUIRE(animated.x < shoulder.x - 0.5f);
		const glm::vec3 leftAnimated = BoneWorld(scene, biped, "LeftHand");

		// Without stiffness the arm falls and hangs from the shoulder.
		Run(scene, 300);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), shoulder + glm::vec3(0, -0.6f, 0), 5e-3f));
		CHECK(Near(BoneWorld(scene, biped, "RightUpperArm"), shoulder));
		auto [upper, lower] = ArmLengths(scene, biped);
		CHECK(upper == doctest::Approx(0.3f).epsilon(1e-4));
		CHECK(lower == doctest::Approx(0.3f).epsilon(1e-4));
		const glm::vec3 hanging = BoneWorld(scene, biped, "RightHand");

		// Re-evaluating the base after the pass (Play from an event callback) keeps the swung arm.
		REQUIRE(system.Play(biped, "Lean"));
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), hanging));

		// The state is kept by chain name: a chain added in front does not restart this one, and starts on the animated
		// pose itself.
		auto& component = biped.GetComponent<SpringBoneComponent>();
		SpringChain left = RightArm();
		left.Name = "LeftArm";
		left.Joint = "LeftUpperArm";
		component.Chains.insert(component.Chains.begin(), left);
		Run(scene, 1);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), hanging, 5e-3f));
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), leftAnimated));

		// Weight blends toward the animated pose.
		component.Chains[1].Weight = 0;
		Run(scene, 1);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), animated));
		component.Chains[1].Weight = 0.5f;
		Run(scene, 1);
		const glm::vec3 half = BoneWorld(scene, biped, "RightHand");
		CHECK(half.y < animated.y - 0.1f);
		CHECK(half.y > hanging.y + 0.1f);

		// A different Joint restarts the chain on the animated pose.
		component.Chains[1].Weight = 1;
		component.Chains[1].Joint = "RightForeArm";
		Run(scene, 1);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), animated));

		// Removing the component restores the animation alone.
		Run(scene, 60);
		CHECK(BoneWorld(scene, biped, "RightHand").y < animated.y - 0.1f);
		biped.RemoveComponent<SpringBoneComponent>();
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), animated));
		Run(scene, 1);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), animated));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Spring bones trail a moving entity, follow wind and reset on teleports")
	{
		ClearAssetCache clear;
		Scene scene;
		auto biped = AddBiped(scene, { RightArm(100, 0) });
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const glm::vec3 rest = BoneWorld(scene, biped, "RightHand") - BoneWorld(scene, biped, "RightUpperArm");

		// Walking forward (+Z) at 2 m/s: the hand trails behind; standing still again it springs back.
		for (int i = 0; i < 30; i++)
		{
			biped.GetTransform().Translation.z += 2 * Step;
			Run(scene, 1);
		}
		const glm::vec3 shoulder = BoneWorld(scene, biped, "RightUpperArm");
		CHECK(BoneWorld(scene, biped, "RightHand").z < shoulder.z - 0.02f);
		Run(scene, 600);
		CHECK(Near(BoneWorld(scene, biped, "RightHand") - BoneWorld(scene, biped, "RightUpperArm"), rest, 1e-3f));

		// Wind pushes it along the wind.
		biped.GetComponent<SpringBoneComponent>().Wind = { 0, 0, 30 };
		Run(scene, 300);
		CHECK(BoneWorld(scene, biped, "RightHand").z > shoulder.z + 0.05f);
		auto [upper, lower] = ArmLengths(scene, biped);
		CHECK(upper == doctest::Approx(0.3f).epsilon(1e-4));
		CHECK(lower == doctest::Approx(0.3f).epsilon(1e-4));

		// A teleport restarts the chain on the animated pose instead of swinging it across the jump.
		biped.GetTransform().Translation += glm::vec3(50, 0, 0);
		Run(scene, 1);
		CHECK(Near(BoneWorld(scene, biped, "RightHand") - BoneWorld(scene, biped, "RightUpperArm"), rest));

		// So does ResetSpringBones, from Lua.
		Run(scene, 60);
		REQUIRE(BoneWorld(scene, biped, "RightHand").z > BoneWorld(scene, biped, "RightUpperArm").z + 0.05f);
		CHECK(Eval(scene, "Scene.FindEntityByName('Biped'):ResetSpringBones(); return 'ok'") == "ok");
		Run(scene, 1);
		CHECK(Near(BoneWorld(scene, biped, "RightHand") - BoneWorld(scene, biped, "RightUpperArm"), rest));
		CHECK(system.ResetSpringBones(biped));
		CHECK_FALSE(system.ResetSpringBones(scene.CreateEntity("Plain")));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Spring bones collide with joint colliders swing from the IK pose and handle mirrored entities")
	{
		ClearAssetCache clear;
		{
			// A sphere on the chest where the elbow would hang (straight below the shoulder): the elbow comes to rest on its
			// side instead.
			Scene scene;
			auto biped = AddBiped(scene, { RightArm() });
			scene.OnRuntimeStart();
			const glm::vec3 shoulder = BoneWorld(scene, biped, "RightUpperArm");
			const glm::vec3 center = shoulder + glm::vec3(0, -0.3f, 0);
			glm::mat4 chest(1.0f);
			std::string error;
			REQUIRE(scene.GetAnimationSystem()->GetBoneTransform(biped, "Chest", chest, error));
			biped.GetComponent<SpringBoneComponent>().Colliders = { { .Joint = "Chest", .Offset = glm::vec3(glm::inverse(chest) * glm::vec4(center, 1)), .Radius = 0.1f } };
			Run(scene, 300);
			const glm::vec3 elbow = BoneWorld(scene, biped, "RightForeArm");
			CHECK(glm::length(elbow - center) >= 0.12f - 2e-3f);
			CHECK(glm::length(BoneWorld(scene, biped, "RightHand") - center) >= 0.12f - 2e-3f);
			CHECK(glm::length(elbow - shoulder) == doctest::Approx(0.3f).epsilon(1e-4));
			scene.OnRuntimeStop();
		}
		{
			// IK aims the upper arm forward; a stiff, well-damped spring keeps the arm where IK put it, not where the clip
			// does.
			Scene scene;
			SpringChain stiff = RightArm(10000, 0);
			stiff.Damping = 40;
			auto biped = AddBiped(scene, { stiff });
			IKChain aim;
			aim.Name = "Aim";
			aim.Type = IKChainType::LookAt;
			aim.Joint = "RightUpperArm";
			aim.Axis = { -1, 0, 0 };
			aim.MaxAngle = 180;
			biped.AddComponent<InverseKinematicsComponent>().Chains = { aim };
			scene.OnRuntimeStart();
			const glm::vec3 shoulder = BoneWorld(scene, biped, "RightUpperArm");
			const glm::vec3 clip = BoneWorld(scene, biped, "RightHand") - shoulder;
			REQUIRE(scene.GetAnimationSystem()->SetIKTarget(biped, "Aim", shoulder + glm::vec3(0, 0, 1), std::nullopt));
			Run(scene, 120);
			CHECK(Near(BoneWorld(scene, biped, "RightHand"), shoulder + glm::vec3(0, 0, 0.6f), 1e-2f));
			// Removing the IK component swings the arm back to the clip.
			biped.RemoveComponent<InverseKinematicsComponent>();
			Run(scene, 120);
			CHECK(Near(BoneWorld(scene, biped, "RightHand"), shoulder + clip, 1e-2f));
			scene.OnRuntimeStop();
		}
		for (const glm::vec3 scale : { glm::vec3(2.0f), glm::vec3(-1, 1, 1) })
		{
			// Scaled and mirrored entities hang the arm down in world space with world-scaled bones.
			CAPTURE(scale);
			Scene scene;
			auto biped = AddBiped(scene, { RightArm() });
			biped.GetTransform().Rotation = AxisAngle({ 0, 1, 0 }, 30);
			biped.GetTransform().Scale = scale;
			scene.OnRuntimeStart();
			Run(scene, 300);
			const float length = 0.6f * std::abs(scale.y);
			const glm::vec3 shoulder = BoneWorld(scene, biped, "RightUpperArm");
			CHECK(Near(BoneWorld(scene, biped, "RightHand"), shoulder + glm::vec3(0, -length, 0), 1e-2f * length));
			scene.OnRuntimeStop();
		}
	}

	TEST_CASE("SpringBone validation warnings and problems name the field")
	{
		ClearAssetCache clear;
		Scene scene;
		auto entity = scene.CreateEntity("Rig");
		std::string error;
		const std::pair<json, const char*> cases[] = {
			{ { { "Chains", { { { "Name", "" } } } } }, "Chains[0].Name must not be empty" },
			{ { { "Chains", { { { "Name", "A" } }, { { "Name", "A" } } } } }, "Chains[1].Name 'A' is already used by Chains[0]" },
			{ { { "Chains", { { { "ChainLength", 17 } } } } }, "Chains[0].ChainLength 17 must be between 0 and 16" },
			{ { { "Chains", { { { "Stiffness", -1 } } } } }, "Chains[0].Stiffness -1 must be between 0 and 10000" },
			{ { { "Chains", { { { "Damping", 1001 } } } } }, "Chains[0].Damping 1001 must be between 0 and 1000" },
			{ { { "Chains", { { { "GravityFactor", 101 } } } } }, "Chains[0].GravityFactor 101 must be between -100 and 100" },
			{ { { "Chains", { { { "Radius", -0.5 } } } } }, "Chains[0].Radius -0.5 must be between 0 and 100" },
			{ { { "Chains", { { { "Weight", 2 } } } } }, "Chains[0].Weight 2 must be between 0 and 1" },
			{ { { "Colliders", { { { "Radius", -1 } } } } }, "Colliders[0].Radius -1 must be between 0 and 100" },
			{ { { "Colliders", { { { "HalfHeight", 101 } } } } }, "Colliders[0].HalfHeight 101 must be between 0 and 100" },
			{ { { "Colliders", { { { "Shape", "Box" } } } } }, "Shape" },
		};
		for (const auto& [data, expected] : cases)
		{
			CAPTURE(data.dump());
			CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "SpringBone", data, error));
			CHECK_MESSAGE(error.find(expected) != std::string::npos, error);
		}
		CHECK_FALSE(entity.HasComponent<SpringBoneComponent>());
		REQUIRE(ComponentRegistry::AddOrPatch(entity, "SpringBone", { { "Wind", { 1, 0, 0 } }, { "Chains", { { { "Name", "Hair" }, { "Joint", "Head" }, { "Stiffness", 12 } } } }, { "Colliders", { { { "Joint", "Head" }, { "HalfHeight", 0.1 } } } } }, error));
		const json written = ComponentRegistry::Find("SpringBone")->Serialize(entity);
		CHECK(written["Chains"][0]["Stiffness"] == 12.0f);
		CHECK(written["Colliders"][0]["HalfHeight"] == 0.1f);
		CHECK(written["Wind"] == json::array({ 1.0f, 0.0f, 0.0f }));
		CHECK(GetAnimationWarnings(entity) == std::vector<std::string>{ "SpringBone needs an Animator on the same entity" });

		auto biped = AddBiped(scene, { RightArm() });
		auto& component = biped.GetComponent<SpringBoneComponent>();
		SpringChain empty, missing = RightArm(), leaf = RightArm(), overlap = RightArm();
		empty.Name = "Empty";
		missing.Name = "Missing";
		missing.Joint = "Nope";
		leaf.Name = "Leaf";
		leaf.Joint = "RightHand";
		overlap.Name = "Overlap";
		overlap.Joint = "RightForeArm";
		component.Chains.insert(component.Chains.end(), { empty, missing, leaf, overlap });
		component.Colliders = { { .Joint = "Nowhere" }, { .Joint = "" } };
		const auto warnings = GetAnimationWarnings(biped);
		const auto has = [&](const std::string& text) { return std::ranges::any_of(warnings, [&](const std::string& warning) { return warning.find(text) != std::string::npos; }); };
		CAPTURE(warnings);
		CHECK(has("SpringBone chain 'Empty': Joint is empty"));
		CHECK(has("SpringBone chain 'Missing': joint 'Nope' not found"));
		CHECK(has("SpringBone chain 'Leaf': joint 'RightHand' has no child to swing toward"));
		CHECK(has("SpringBone chains 'RightArm' and 'Overlap' both turn joint 'RightForeArm'; the later one wins"));
		CHECK(has("SpringBone Colliders[0]: joint 'Nowhere' not found"));
		CHECK(warnings.size() == 5);

		// At runtime each problem is logged once, and the valid chain still swings.
		scene.CreateEntity("Plain");
		const auto since = Log::GetHistory().GetTotalCount();
		scene.OnRuntimeStart();
		const glm::vec3 animated = BoneWorld(scene, biped, "RightHand");
		component.Chains[0].Damping = std::numeric_limits<float>::infinity();
		Run(scene, 30);
		CHECK(CountLogMessages(since, "chain 'Missing': joint 'Nope' not found") == 1);
		CHECK(CountLogMessages(since, "chain 'Leaf': joint 'RightHand' has no child") == 1);
		CHECK(CountLogMessages(since, "Colliders[0]: joint 'Nowhere' not found") == 1);
		CHECK(CountLogMessages(since, "chain 'RightArm': Damping must be between 0 and 1000; clamping") == 1);
		CHECK(CountLogMessages(since, "SpringBone 'Rig': not solved") == 1);
		CHECK(BoneWorld(scene, biped, "RightHand").y < animated.y - 0.05f);

		const std::pair<const char*, const char*> errors[] = {
			{ "Scene.FindEntityByName('Plain'):ResetSpringBones()", "ResetSpringBones needs the SpringBone component on 'Plain'" },
		};
		CheckLuaErrors(*scene.GetScriptEngine(), errors);
		scene.OnRuntimeStop();
	}
}
