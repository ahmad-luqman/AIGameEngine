#include <doctest/doctest.h>

#include "AnimationTestUtils.h"
#include "TestUtils.h"

#include <Breccia/Animation/AnimatedModel.h>
#include <Breccia/Animation/AnimationPose.h>
#include <Breccia/Animation/AnimationSystem.h>
#include <Breccia/Animation/InverseKinematics.h>
#include <Breccia/Animation/SpringBones.h>
#include <Breccia/Core/Log.h>
#include <Breccia/Math/Math.h>
#include <Breccia/Physics/PhysicsWorld.h>
#include <Breccia/Physics/StaticPhysicsShapes.h>
#include <Breccia/Project/ProjectValidator.h>
#include <Breccia/Scene/ComponentRegistry.h>
#include <Breccia/Scene/Entity.h>
#include <Breccia/Scene/SceneSerializer.h>
#include <Breccia/Scripting/ScriptEngine.h>

#include <nlohmann/json.hpp>

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <tuple>

using namespace Breccia;
using namespace BrecciaTest;
using json = nlohmann::json;

using namespace AnimationTest;

namespace {
	// A straight chain of `count` points spaced `length` apart along `direction` from `head`, at rest.
	struct Strand
	{
		Strand(const glm::vec3& head, const glm::vec3& direction, size_t count, float length = 0.3f)
		{
			for (size_t i = 0; i <= count; i++)
				Animated.push_back(head + direction * (length * static_cast<float>(i)));
			for (size_t i = 1; i <= count; i++)
				Points.push_back({ .Position = Animated[i] });
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
		// Moves the whole animated chain by `velocity` for `frames` frames of `seconds`.
		void Move(const glm::vec3& velocity, int frames, float seconds, SpringSimulationRequest request)
		{
			for (int i = 0; i < frames; i++)
			{
				std::vector<glm::vec3> moved = Animated;
				for (glm::vec3& position : moved)
					position += velocity * seconds;
				request.Animated = moved;
				request.PreviousAnimated = Animated;
				request.Seconds = seconds;
				SimulateSpringChain(request, Points);
				Animated = std::move(moved);
			}
		}
		float Length(size_t point) const
		{
			const glm::vec3 head = point == 0 ? Animated[0] : Points[point - 1].Position;
			return glm::length(Points[point].Position - head);
		}
		// How far the tip trails behind its animated position along `direction`.
		float Lag(const glm::vec3& direction) const { return glm::dot(Animated.back() - Points.back().Position, direction); }
		std::vector<glm::vec3> Animated;
		std::vector<SpringPoint> Points;
	};

	constexpr glm::vec3 Gravity(0, -9.81f, 0);

	float Distance(const SpringCollisionShape& shape, const glm::vec3& point)
	{
		const glm::vec3 axis = shape.B - shape.A;
		const float t = glm::dot(axis, axis) > 0 ? std::clamp(glm::dot(point - shape.A, axis) / glm::dot(axis, axis), 0.0f, 1.0f) : 0.0f;
		return glm::length(point - (shape.A + axis * t));
	}

	SpringChain RightArm(float stiffness = 0, float gravityFactor = 1)
	{
		SpringChain chain;
		chain.Name = "RightArm";
		chain.Joint = "RightUpperArm";
		chain.Stiffness = stiffness;
		chain.GravityFactor = gravityFactor;
		return chain;
	}
	// The Biped (arms straight out along X, the spine leaning 20 degrees) with spring chains; its Animator holds the
	// first frame of "Lean".
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
	// The right hand relative to the right shoulder, in world space.
	glm::vec3 ArmReach(Scene& scene, Entity biped)
	{
		return BoneWorld(scene, biped, "RightHand") - BoneWorld(scene, biped, "RightUpperArm");
	}
	// Both right arm bones keep their 0.3 m (times the entity's scale).
	void CheckArmLengths(Scene& scene, Entity biped, float scale = 1)
	{
		const glm::vec3 shoulder = BoneWorld(scene, biped, "RightUpperArm"), elbow = BoneWorld(scene, biped, "RightForeArm"), hand = BoneWorld(scene, biped, "RightHand");
		CHECK(glm::length(elbow - shoulder) == doctest::Approx(0.3f * scale).epsilon(1e-4));
		CHECK(glm::length(hand - elbow) == doctest::Approx(0.3f * scale).epsilon(1e-4));
	}
	// glTF text of a straight line of `count` joints named J0.. (J0 at the origin, each `spacing` above the one before),
	// beside a one-triangle mesh (models need geometry to load).
	std::string LineModel(size_t count, float spacing)
	{
		nlohmann::json nodes = nlohmann::json::array({ { { "name", "Mesh" }, { "mesh", 0 } } });
		for (size_t i = 0; i < count; i++)
		{
			nlohmann::json node = { { "name", "J" + std::to_string(i) }, { "translation", { 0, i == 0 ? 0.0f : spacing, 0 } } };
			if (i + 1 < count)
				node["children"] = { i + 2 };
			nodes.push_back(node);
		}
		// Three positions: (0, 0, 0), (0.1, 0, 0) and (0, 0.1, 0).
		const nlohmann::json triangle = {
			{ "buffers", { { { "byteLength", 36 }, { "uri", "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAzczMPQAAAAAAAAAAAAAAAM3MzD0AAAAA" } } } },
			{ "bufferViews", { { { "buffer", 0 }, { "byteLength", 36 } } } },
			{ "accessors", { { { "bufferView", 0 }, { "componentType", 5126 }, { "count", 3 }, { "type", "VEC3" }, { "min", { 0, 0, 0 } }, { "max", { 0.1, 0.1, 0 } } } } },
			{ "meshes", { { { "primitives", { { { "attributes", { { "POSITION", 0 } } } } } } } } },
		};
		nlohmann::json model = { { "asset", { { "version", "2.0" } } }, { "nodes", nodes }, { "scenes", { { { "nodes", { 0, 1 } } } } }, { "scene", 0 } };
		model.update(triangle);
		return model.dump();
	}
	// A three-node line: Root, Middle (`middleHeight` above it, optionally a fixed matrix) and Tip 0.5 m above that.
	MeshSource Line(bool fixedMiddle = false, float middleHeight = 0.5f)
	{
		MeshSource line;
		line.Nodes.resize(3);
		line.Nodes[0].Name = "Root";
		line.Nodes[1].Name = "Middle";
		line.Nodes[1].Parent = 0;
		line.Nodes[1].Translation = { 0, middleHeight, 0 };
		line.Nodes[1].HasMatrix = fixedMiddle;
		line.Nodes[1].Matrix = glm::translate(glm::mat4(1), line.Nodes[1].Translation);
		line.Nodes[2].Name = "Tip";
		line.Nodes[2].Parent = 1;
		line.Nodes[2].Translation = { 0, 0.5f, 0 };
		REQUIRE(line.FinalizeHierarchy());
		return line;
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
		CHECK(ResolveSpringChain(*source, chain, joints) == "joint 'RightUpperArm' needs 3 joints and a tip in a line from it");
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
		CHECK(ChildCount(*source, Node(*source, "Chest")) == 3);
		CHECK(FirstChild(*source, Node(*source, "Head")) == -1);

		// A fixed-matrix joint cannot turn, but it can be the tip.
		const MeshSource fixed = Line(true);
		chain.Joint = "Root";
		chain.ChainLength = 1;
		CHECK(ResolveSpringChain(fixed, chain, joints).empty());
		chain.ChainLength = 2;
		CHECK(ResolveSpringChain(fixed, chain, joints) == "joint 'Middle' has a fixed matrix and cannot be turned");
	}

	TEST_CASE("Spring points hang under gravity, keep their lengths and settle on their stiffness")
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

		// Zero-length bones (end joints on their parents) stay on their heads, finite.
		Strand folded({ 0, 2, 0 }, { 1, 0, 0 }, 2, 0);
		folded.Run(60, Step, { .Stiffness = 40, .Damping = 4, .Acceleration = Gravity });
		for (const SpringPoint& point : folded.Points)
			CHECK(point.Position == glm::vec3(0, 2, 0));
	}

	TEST_CASE("Spring points stay outside sphere and capsule colliders")
	{
		// A sphere under the first bone, and a capsule across where the middle point would hang.
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
				CHECK(Distance(shape, strand.Points[i].Position) >= shape.Radius + 0.02f - 1e-4f);
				CHECK(strand.Length(i) == doctest::Approx(0.3f).epsilon(1e-4));
			}
			// The collider held the chain off the straight hang below the head.
			CHECK(glm::length(glm::vec2(strand.Points[2].Position.x, strand.Points[2].Position.z)) > 0.05f);
		}

		// A sphere just inside the reach of a bone: one push out moves the point off its bone length, and putting it back
		// on its length alone would move it in again. The constraints iterate, so it ends outside at its length.
		const SpringCollisionShape near{ .A = { 0.25f, 1.75f, 0 }, .B = { 0.25f, 1.75f, 0 }, .Radius = 0.12f };
		Strand single({ 0, 2, 0 }, { 1, 0, 0 }, 1);
		const std::array nearShapes{ near };
		single.Run(600, Step, { .Colliders = nearShapes, .Stiffness = 0, .Damping = 4, .Acceleration = Gravity });
		CHECK(Distance(near, single.Points[0].Position) >= near.Radius - 1e-4f);
		CHECK(single.Length(0) == doctest::Approx(0.3f).epsilon(1e-4));

		// A point exactly on a sphere's center or a capsule's axis still leaves it, across its bone, at its length. A capsule
		// along the bone (the last) covers the whole bone, so nowhere at its length is outside it; the point stays finite.
		const std::pair<SpringCollisionShape, bool> centers[] = { { { .A = { 0.3f, 2, 0 }, .B = { 0.3f, 2, 0 }, .Radius = 0.05f }, true },
																  { { .A = { 0.3f, 1.8f, 0 }, .B = { 0.3f, 2.2f, 0 }, .Radius = 0.05f }, true },
																  { { .A = { 0, 2, 0 }, .B = { 1, 2, 0 }, .Radius = 0.05f }, false } };
		for (const auto& [shape, outside] : centers)
		{
			Strand centered({ 0, 2, 0 }, { 1, 0, 0 }, 1);
			const std::array centerShapes{ shape };
			centered.Run(1, Step, { .Colliders = centerShapes });
			const glm::vec3 point = centered.Points[0].Position;
			CAPTURE(point);
			CHECK(Math::IsFinite(point));
			CHECK(centered.Length(0) == doctest::Approx(0.3f).epsilon(1e-4));
			if (outside)
				CHECK(Distance(shape, point) >= shape.Radius - 2e-3f);
		}
		// Across the bone: the vertical capsule pushes the point out sideways, not along its bone or the capsule.
		Strand across({ 0, 2, 0 }, { 1, 0, 0 }, 1);
		const std::array verticalShape{ SpringCollisionShape{ .A = { 0.3f, 1.8f, 0 }, .B = { 0.3f, 2.2f, 0 }, .Radius = 0.05f } };
		across.Run(1, Step, { .Colliders = verticalShape });
		CHECK(std::abs(across.Points[0].Position.z) > 0.04f);
		CHECK(std::abs(across.Points[0].Position.y - 2) < 1e-3f);
	}

	TEST_CASE("Spring points swing at most MaxAngle from the animated shape, and colliders and the world win over it")
	{
		const auto angle = [](const glm::vec3& a, const glm::vec3& b) { return glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0f, 1.0f))); };
		// A horizontal strand falls under gravity: free, it hangs straight down; limited, each bone stops MaxAngle away from
		// where the animated shape (turned with the bones before it) points it.
		Strand free({ 0, 2, 0 }, { 1, 0, 0 }, 3);
		free.Run(600, Step, { .Stiffness = 0, .Damping = 4, .Acceleration = Gravity });
		CHECK(angle(free.Points[0].Position - free.Animated[0], { 1, 0, 0 }) == doctest::Approx(90).epsilon(0.01));
		Strand limited({ 0, 2, 0 }, { 1, 0, 0 }, 3);
		limited.Run(600, Step, { .Stiffness = 0, .Damping = 4, .MaxAngle = glm::radians(30.0f), .Acceleration = Gravity });
		glm::vec3 previous(1, 0, 0), head = limited.Animated[0];
		for (size_t i = 0; i < 3; i++)
		{
			CAPTURE(i);
			const glm::vec3 bone = limited.Points[i].Position - head;
			// Gravity holds every bone against its limit.
			CHECK(angle(bone, previous) == doctest::Approx(30).epsilon(0.01));
			CHECK(limited.Length(i) == doctest::Approx(0.3f).epsilon(1e-4));
			previous = bone;
			head = limited.Points[i].Position;
		}
		// MaxAngle 0 keeps the animated shape.
		Strand rigid({ 0, 2, 0 }, { 1, 0, 0 }, 3);
		rigid.Run(120, Step, { .Stiffness = 0, .Damping = 4, .MaxAngle = 0, .Acceleration = Gravity });
		for (size_t i = 0; i < 3; i++)
			CHECK(Near(rigid.Points[i].Position, rigid.Animated[i + 1], 1e-4f));

		// A point straight behind its bone (against the axis, which may lie along either fallback direction) still ends up
		// MaxAngle from it, finite.
		for (const glm::vec3 axis : { glm::vec3(0, 1, 0), glm::vec3(1, 0, 0) })
		{
			CAPTURE(axis);
			Strand behind({ 0, 2, 0 }, axis, 1);
			behind.Points[0].Position = behind.Animated[0] - axis * 0.3f;
			// One substep: a second would carry the swing's speed further in.
			behind.Run(1, 1.0f / SpringStepRate, { .Stiffness = 0, .MaxAngle = glm::radians(30.0f) });
			CHECK(Math::IsFinite(behind.Points[0].Position));
			CHECK(angle(behind.Points[0].Position - behind.Animated[0], axis) == doctest::Approx(30).epsilon(0.01));
		}

		// A collider on the rigid strand's line still pushes its point off the line.
		const SpringCollisionShape sphere{ .A = { 0.3f, 2.02f, 0 }, .B = { 0.3f, 2.02f, 0 }, .Radius = 0.05f };
		const std::array shapes{ sphere };
		Strand pushed({ 0, 2, 0 }, { 1, 0, 0 }, 1);
		pushed.Run(10, Step, { .Colliders = shapes, .Stiffness = 0, .Damping = 4, .MaxAngle = 0, .Acceleration = Gravity });
		CHECK(Distance(sphere, pushed.Points[0].Position) >= sphere.Radius - 1e-4f);
		CHECK(pushed.Length(0) == doctest::Approx(0.3f).epsilon(1e-4));

		// The world (physics bodies) too: a floor 0.2 m below the head holds the free strand up, at its bone lengths.
		int calls = 0;
		const auto floor = [&](const glm::vec3& point) -> std::optional<glm::vec3> {
			calls++;
			if (point.y >= 1.8f)
				return std::nullopt;
			return glm::vec3(point.x, 1.8f, point.z);
		};
		Strand resting({ 0, 2, 0 }, { 1, 0, 0 }, 3);
		resting.Run(600, Step, { .World = floor, .Stiffness = 0, .Damping = 4, .Acceleration = Gravity });
		CHECK(calls > 0);
		for (size_t i = 0; i < 3; i++)
		{
			CAPTURE(i);
			CHECK(resting.Points[i].Position.y >= 1.8f - 1e-3f);
			CHECK(resting.Length(i) == doctest::Approx(0.3f).epsilon(1e-4));
		}
		CHECK(resting.Points[2].Position.y < 1.85f);
	}

	TEST_CASE("Spring points lag behind a moving chain the same at any frame rate and through hitches")
	{
		// The head moves along +Z at 2 m/s: the tip trails behind it, then springs back once it stops.
		const SpringSimulationRequest springy{ .Stiffness = 60, .Damping = 4 };
		Strand at60({ 0, 2, 0 }, { 1, 0, 0 }, 3);
		at60.Move({ 0, 0, 2 }, 30, 1.0f / 60, springy);
		const float lag = at60.Lag({ 0, 0, 1 });
		CHECK(lag > 0.05f);
		CHECK(at60.Length(2) == doctest::Approx(0.3f).epsilon(1e-4));
		// The same half second at 144 frames per second (substeps of 1/144 s instead of 1/120 s), and at uneven frame
		// times, ends close by.
		Strand at144({ 0, 2, 0 }, { 1, 0, 0 }, 3);
		at144.Move({ 0, 0, 2 }, 72, 1.0f / 144, springy);
		CHECK(at144.Lag({ 0, 0, 1 }) == doctest::Approx(lag).epsilon(0.03));
		Strand uneven({ 0, 2, 0 }, { 1, 0, 0 }, 3);
		for (int i = 0; i < 10; i++)
		{
			uneven.Move({ 0, 0, 2 }, 1, 0.01f, springy);
			uneven.Move({ 0, 0, 2 }, 1, 0.04f, springy);
		}
		CHECK(uneven.Lag({ 0, 0, 1 }) == doctest::Approx(lag).epsilon(0.03));

		at60.Run(600, Step, springy);
		CHECK(Near(at60.Points[2].Position, at60.Animated[3], 1e-3f));

		// A half-second hitch while the chain moves 2 m: the points move along with it for the part that is not
		// simulated, so they do not whip across the gap.
		Strand hitched({ 0, 2, 0 }, { 1, 0, 0 }, 3);
		hitched.Move({ 0, 0, 4 }, 1, 0.5f, springy);
		CHECK(hitched.Lag({ 0, 0, 1 }) < 0.3f);
		for (const SpringPoint& point : hitched.Points)
			CHECK(glm::length(point.Velocity) < 10.0f);

		// Even the stiffest spring stays finite and stable at the longest substep.
		Strand stiff({ 0, 2, 0 }, { 1, 0, 0 }, 3);
		stiff.Points[2].Position += glm::vec3(0, -0.2f, 0);
		stiff.Run(20, 1.0f, { .Stiffness = SpringChain::MaxStiffness, .Damping = 0, .Acceleration = Gravity });
		for (const SpringPoint& point : stiff.Points)
			CHECK(Math::IsFinite(point.Position));
		CHECK(stiff.Length(2) == doctest::Approx(0.3f).epsilon(1e-4));
	}

	TEST_CASE("Aim chain IK turns each joint toward its target and blends by weight")
	{
		MeshSource line = Line();
		AnimationLocalPose pose = SampleAnimation(line, nullptr, 0);
		std::vector<glm::mat4> globals;
		REQUIRE(ComposeAnimationPose(line, pose, globals));
		const std::vector<uint32_t> joints{ 0, 1, 2 };
		const auto position = [&](uint32_t node) { return glm::vec3(globals[node][3]); };

		// Weight 0.25 turns each joint a quarter of the way: the root's 90 degree turn toward +X becomes 22.5.
		const std::vector<glm::vec3> side{ { 0.5f, 0, 0 }, { 1, 0, 0 } };
		CHECK(SolveAimChainIK(line, pose, globals, { .Joints = joints, .Targets = side, .Weight = 0.25f }) == IKSolveResult::Solved);
		CHECK(Degrees(position(1), { 0, 1, 0 }) == doctest::Approx(22.5f).epsilon(1e-3));
		CHECK(glm::length(position(1)) == doctest::Approx(0.5f));
		pose = SampleAnimation(line, nullptr, 0);
		REQUIRE(ComposeAnimationPose(line, pose, globals));
		CHECK(SolveAimChainIK(line, pose, globals, { .Joints = joints, .Targets = side, .Weight = 1 }) == IKSolveResult::Solved);
		CHECK(Near(position(1), { 0.5f, 0, 0 }));
		CHECK(Near(position(2), { 1, 0, 0 }));

		// Zero weight changes nothing; a non-finite target fails and leaves the pose as it was.
		const auto before = globals;
		CHECK(SolveAimChainIK(line, pose, globals, { .Joints = joints, .Targets = side, .Weight = 0 }) == IKSolveResult::Unchanged);
		const std::vector<glm::vec3> broken{ { 0.5f, 0, 0 }, glm::vec3(std::numeric_limits<float>::quiet_NaN()) };
		CHECK(SolveAimChainIK(line, pose, globals, { .Joints = joints, .Targets = broken }) == IKSolveResult::NonFinite);
		CHECK(NearPose(globals, before));

		// A fixed-matrix joint cannot turn; zero-length bones have nothing to turn.
		const MeshSource fixed = Line(true);
		AnimationLocalPose fixedPose = SampleAnimation(fixed, nullptr, 0);
		REQUIRE(ComposeAnimationPose(fixed, fixedPose, globals));
		CHECK(SolveAimChainIK(fixed, fixedPose, globals, { .Joints = joints, .Targets = side }) == IKSolveResult::FixedMatrix);
		const MeshSource flat = Line(false, 0);
		AnimationLocalPose flatPose = SampleAnimation(flat, nullptr, 0);
		REQUIRE(ComposeAnimationPose(flat, flatPose, globals));
		const std::vector<uint32_t> root{ 0, 1 };
		const std::vector<glm::vec3> one{ { 1, 0, 0 } };
		CHECK(SolveAimChainIK(flat, flatPose, globals, { .Joints = root, .Targets = one }) == IKSolveResult::Degenerate);
	}

	TEST_CASE("Spring bones swing an Animator's joints in LateUpdate")
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
		RunAnimation(scene, 300);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), shoulder + glm::vec3(0, -0.6f, 0), 5e-3f));
		CHECK(Near(BoneWorld(scene, biped, "RightUpperArm"), shoulder));
		CheckArmLengths(scene, biped);
		const glm::vec3 hanging = BoneWorld(scene, biped, "RightHand");

		// Re-evaluating the base after the pass (Play from an event callback) keeps the swung arm.
		REQUIRE(system.Play(biped, "Lean"));
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), hanging));

		// The state is kept by chain name: a chain added in front does not restart this one, and starts on the animated
		// pose itself. Removing it again leaves this one alone too.
		auto& component = biped.GetComponent<SpringBoneComponent>();
		SpringChain left = RightArm();
		left.Name = "LeftArm";
		left.Joint = "LeftUpperArm";
		component.Chains.insert(component.Chains.begin(), left);
		RunAnimation(scene, 1);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), hanging, 5e-3f));
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), leftAnimated));
		component.Chains.erase(component.Chains.begin());
		RunAnimation(scene, 1);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), hanging, 5e-3f));
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), leftAnimated));
		// Duplicate names written from C++ past Validate: each state goes to one chain, so the copy starts afresh.
		component.Chains.push_back(component.Chains[0]);
		component.Chains[1].Joint = "LeftUpperArm";
		RunAnimation(scene, 1);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), hanging, 5e-3f));
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), leftAnimated));
		component.Chains.pop_back();

		// Weight 0 shows the animated pose while the simulation carries on: back at weight 1 the arm still hangs.
		component.Chains[0].Weight = 0;
		RunAnimation(scene, 1);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), animated));
		component.Chains[0].Weight = 1;
		RunAnimation(scene, 1);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), hanging, 5e-3f));

		// A different ChainLength restarts the chain on the animated pose; so does an Animator that could not play.
		component.Chains[0].ChainLength = 1;
		RunAnimation(scene, 1);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), animated));
		component.Chains[0].ChainLength = 0;
		RunAnimation(scene, 60);
		REQUIRE(BoneWorld(scene, biped, "RightHand").y < animated.y - 0.1f);
		auto& animator = biped.GetComponent<AnimatorComponent>();
		const std::string mesh = animator.Mesh;
		animator.Mesh.clear();
		RunAnimation(scene, 1);
		animator.Mesh = mesh;
		RunAnimation(scene, 1);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), animated));

		// A different Joint restarts the chain on the animated pose.
		component.Chains[0].Joint = "RightForeArm";
		RunAnimation(scene, 1);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), animated));

		// Removing the component restores the animation alone.
		RunAnimation(scene, 60);
		CHECK(BoneWorld(scene, biped, "RightHand").y < animated.y - 0.1f);
		biped.RemoveComponent<SpringBoneComponent>();
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), animated));
		RunAnimation(scene, 1);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), animated));

		// Destroying a swinging entity drops its state with it.
		biped.AddComponent<SpringBoneComponent>().Chains = { RightArm() };
		RunAnimation(scene, 10);
		scene.DestroyEntity(biped);
		RunAnimation(scene, 2);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Spring bones trail a moving entity, follow wind and reset on teleports")
	{
		ClearAssetCache clear;
		Scene scene;
		auto biped = AddBiped(scene, { RightArm(100, 0) });
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const glm::vec3 rest = ArmReach(scene, biped);

		// Walking forward (+Z) at 2 m/s: the hand trails behind; standing still again it springs back.
		for (int i = 0; i < 30; i++)
		{
			biped.GetTransform().Translation.z += 2 * Step;
			RunAnimation(scene, 1);
		}
		CHECK(ArmReach(scene, biped).z < -0.02f);
		RunAnimation(scene, 600);
		CHECK(Near(ArmReach(scene, biped), rest, 1e-3f));

		// Wind pushes it along the wind.
		biped.GetComponent<SpringBoneComponent>().Wind = { 0, 0, 30 };
		RunAnimation(scene, 300);
		CHECK(ArmReach(scene, biped).z > 0.05f);
		CheckArmLengths(scene, biped);

		// A teleport (against the wind, so a swing could not look like the animated pose) restarts the chain on the
		// animated pose instead of swinging it across the jump.
		biped.GetTransform().Translation += glm::vec3(0, 0, -50);
		RunAnimation(scene, 1);
		CHECK(Near(ArmReach(scene, biped), rest));

		// So does ResetSpringBones, from Lua.
		RunAnimation(scene, 60);
		REQUIRE(ArmReach(scene, biped).z > 0.05f);
		CHECK(Eval(scene, "Scene.FindEntityByName('Biped'):ResetSpringBones(); return 'ok'") == "ok");
		RunAnimation(scene, 1);
		CHECK(Near(ArmReach(scene, biped), rest));
		CHECK(system.ResetSpringBones(biped));
		CHECK_FALSE(system.ResetSpringBones(scene.CreateEntity("Plain")));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Spring bones collide with joint colliders and swing from the IK pose")
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
			const glm::mat4 chest = BoneWorldTransform(scene, biped, "Chest");
			biped.GetComponent<SpringBoneComponent>().Colliders = { { .Joint = "Chest", .Offset = glm::vec3(glm::inverse(chest) * glm::vec4(center, 1)), .Radius = 0.1f } };
			RunAnimation(scene, 300);
			const glm::vec3 elbow = BoneWorld(scene, biped, "RightForeArm");
			CHECK(glm::length(elbow - center) >= 0.12f - 1e-3f);
			CHECK(glm::length(BoneWorld(scene, biped, "RightHand") - center) >= 0.12f - 1e-3f);
			CheckArmLengths(scene, biped);
			scene.OnRuntimeStop();
		}
		{
			// IK aims the upper arm forward; a stiff, well-damped spring keeps the arm where IK put it, not where the clip
			// does.
			Scene scene;
			SpringChain stiff = RightArm(SpringChain::MaxStiffness, 0);
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
			auto& system = *scene.GetAnimationSystem();
			const glm::vec3 shoulder = BoneWorld(scene, biped, "RightUpperArm");
			const glm::vec3 clip = ArmReach(scene, biped);
			REQUIRE(system.SetIKTarget(biped, "Aim", shoulder + glm::vec3(0, 0, 1), std::nullopt));
			RunAnimation(scene, 120);
			CHECK(Near(BoneWorld(scene, biped, "RightHand"), shoulder + glm::vec3(0, 0, 0.6f), 1e-2f));

			// A loose spring on the IK-aimed arm hangs; the spring overlay goes on top of the IK one, so re-evaluating the
			// pose (Play from an event callback) keeps the hanging arm rather than IK's forward one.
			biped.GetComponent<SpringBoneComponent>().Chains[0] = RightArm();
			RunAnimation(scene, 300);
			const glm::vec3 hanging = BoneWorld(scene, biped, "RightHand");
			CHECK(Near(hanging, shoulder + glm::vec3(0, -0.6f, 0), 1e-2f));
			REQUIRE(system.Play(biped, "Lean"));
			CHECK(Near(BoneWorld(scene, biped, "RightHand"), hanging, 1e-3f));

			// Removing the IK component drops both solutions at once (the springs were solved on IK's pose); the springs
			// solve again from the clip.
			biped.GetComponent<SpringBoneComponent>().Chains[0] = stiff;
			RunAnimation(scene, 120);
			biped.RemoveComponent<InverseKinematicsComponent>();
			CHECK(Near(ArmReach(scene, biped), clip, 1e-3f));
			RunAnimation(scene, 120);
			CHECK(Near(BoneWorld(scene, biped, "RightHand"), shoulder + clip, 1e-2f));
			scene.OnRuntimeStop();
		}
	}

	TEST_CASE("Spring bones scale with scaled and mirrored entities and their colliders")
	{
		ClearAssetCache clear;
		for (const glm::vec3 scale : { glm::vec3(2.0f), glm::vec3(-1, 1, 1) })
		{
			// The arm hangs down in world space with world-scaled bones.
			CAPTURE(scale);
			Scene scene;
			auto biped = AddBiped(scene, { RightArm() });
			biped.GetTransform().Rotation = AxisAngle({ 0, 1, 0 }, 30);
			biped.GetTransform().Scale = scale;
			scene.OnRuntimeStart();
			RunAnimation(scene, 300);
			const float length = 0.6f * std::abs(scale.y);
			const glm::vec3 shoulder = BoneWorld(scene, biped, "RightUpperArm");
			CHECK(Near(BoneWorld(scene, biped, "RightHand"), shoulder + glm::vec3(0, -length, 0), 1e-2f * length));
			CheckArmLengths(scene, biped, std::abs(scale.y));
			scene.OnRuntimeStop();
		}

		// A capsule in the entity's space (no Joint) along its Y axis under the shoulder, on an entity scaled 2: the
		// collider and the chain's point radius both scale, and the arm stays outside the doubled capsule.
		Scene scene;
		auto biped = AddBiped(scene, { RightArm() });
		biped.GetTransform().Scale = glm::vec3(2);
		scene.OnRuntimeStart();
		const glm::vec3 shoulder = BoneWorld(scene, biped, "RightUpperArm");
		const glm::vec3 below = glm::vec3(glm::inverse(scene.GetWorldTransform(biped)) * glm::vec4(shoulder + glm::vec3(0, -0.6f, 0), 1));
		biped.GetComponent<SpringBoneComponent>().Colliders = { { .Offset = below, .Radius = 0.1f, .HalfHeight = 0.2f } };
		RunAnimation(scene, 300);
		const SpringCollisionShape capsule{ .A = shoulder + glm::vec3(0, -1.0f, 0), .B = shoulder + glm::vec3(0, -0.2f, 0), .Radius = 0.2f };
		for (const char* joint : { "RightForeArm", "RightHand" })
		{
			CAPTURE(joint);
			CHECK(Distance(capsule, BoneWorld(scene, biped, joint)) >= (0.1f + 0.02f) * 2 - 2e-3f);
		}
		CheckArmLengths(scene, biped, 2);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Spring chains collide only with the colliders they name")
	{
		ClearAssetCache clear;
		Scene scene;
		auto biped = AddBiped(scene, { RightArm() });
		scene.OnRuntimeStart();
		const glm::vec3 shoulder = BoneWorld(scene, biped, "RightUpperArm");
		const glm::mat4 model = glm::inverse(scene.GetWorldTransform(biped));
		// Two spheres below the shoulder where the hanging arm would pass, one named Belly, one unnamed.
		auto& component = biped.GetComponent<SpringBoneComponent>();
		const glm::vec3 below = glm::vec3(model * glm::vec4(shoulder + glm::vec3(0, -0.3f, 0), 1));
		component.Colliders = { { .Name = "Belly", .Offset = below, .Radius = 0.1f }, { .Offset = below + glm::vec3(0, -0.15f, 0), .Radius = 0.05f } };
		const glm::vec3 hanging = shoulder + glm::vec3(0, -0.6f, 0);
		const auto hangsStraight = [&] { return Near(BoneWorld(scene, biped, "RightHand"), hanging, 5e-3f); };
		// Naming another collider leaves both out of the way: the arm hangs through them.
		component.Colliders.push_back({ .Name = "Head", .Offset = below + glm::vec3(0, 2, 0), .Radius = 0.1f });
		component.Chains[0].Colliders = { { .Name = "Head" } };
		RunAnimation(scene, 300);
		CHECK(hangsStraight());
		// Naming Belly holds the arm off it, while the unnamed one is still ignored: the hand swings into it.
		component.Chains[0].Colliders = { { .Name = "Belly" } };
		RunAnimation(scene, 300);
		CHECK_FALSE(hangsStraight());
		const glm::vec3 elbow = BoneWorld(scene, biped, "RightForeArm");
		CHECK(glm::length(elbow - (shoulder + glm::vec3(0, -0.3f, 0))) >= 0.1f + component.Chains[0].Radius - 1e-3f);
		// The unnamed sphere plays no part: without it the arm rests exactly the same.
		const glm::vec3 held = BoneWorld(scene, biped, "RightHand");
		const auto named = component.Colliders;
		component.Colliders.erase(component.Colliders.begin() + 1);
		RunAnimation(scene, 60);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), held, 1e-4f));
		component.Colliders = named;
		// A collider that cannot resolve (an unknown joint) drops out without shifting which colliders the names pick.
		component.Colliders.insert(component.Colliders.begin(), { .Name = "Broken", .Joint = "Nope", .Radius = 0.1f });
		RunAnimation(scene, 60);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), held, 1e-4f));
		component.Colliders = named;
		// Entries without a Name are the inspector's unnamed additions: they name nothing (so they pick none of the unnamed
		// colliders), and a list of only those means every collider, so Belly still holds the arm.
		component.Chains[0].Colliders = { { .Name = "" } };
		RunAnimation(scene, 60);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), held, 1e-4f));
		CHECK(GetAnimationWarnings(biped) == std::vector<std::string>{ "SpringBone chain 'RightArm': Colliders[0] has no Name, so it names no collider (a list without names keeps every collider)" });
		// Next to a name, an unnamed entry still picks no unnamed collider: naming only the far Head, the arm hangs
		// straight, its hand inside the unnamed sphere moved to where the hand hangs.
		component.Colliders[1].Offset = glm::vec3(model * glm::vec4(hanging, 1));
		component.Chains[0].Colliders = { { .Name = "" }, { .Name = "Head" } };
		RunAnimation(scene, 300);
		CHECK(hangsStraight());
		// Once a chain lists colliders, an unnamed collider is one no list can pick: the inspector says so.
		const auto listed = GetAnimationWarnings(biped);
		CHECK(std::ranges::count(listed, std::string("SpringBone Colliders[1] has no Name, so chains that list colliders cannot pick it")) == 1);
		CHECK(std::ranges::count(listed, std::string("SpringBone chain 'RightArm': Colliders[0] has no Name, so it names no collider (a list without names keeps every collider)")) == 1);
		component.Colliders = named;
		component.Chains[0].Colliders = { { .Name = "Belly" } };
		// Without a list the chain collides with every collider.
		component.Chains[0].Colliders.clear();
		RunAnimation(scene, 300);
		CHECK_FALSE(hangsStraight());
		// Several colliders may share a name, which then names all of them.
		component.Colliders[1].Name = "Belly";
		component.Chains[0].Colliders = { { .Name = "Belly" } };
		RunAnimation(scene, 300);
		CHECK_FALSE(hangsStraight());

		// An unnamed entry among named colliders is no missing name: neither the inspector nor the log reports one.
		const auto beforeUnnamed = Log::GetHistory().GetTotalCount();
		component.Chains[0].Colliders = { { .Name = "" }, { .Name = "Belly" } };
		CHECK(GetAnimationWarnings(biped) == std::vector<std::string>{ "SpringBone chain 'RightArm': Colliders[0] has no Name, so it names no collider (a list without names keeps every collider)" });
		RunAnimation(scene, 10);
		CHECK(CountLogMessages(beforeUnnamed, "no collider is named") == 0);
		// A name that matches no collider is an authoring warning and a runtime problem, logged once.
		component.Chains[0].Colliders = { { .Name = "Belly" }, { .Name = "Gone" } };
		CHECK(GetAnimationWarnings(biped) == std::vector<std::string>{ "SpringBone chain 'RightArm': Colliders: no collider is named 'Gone'" });
		const auto since = Log::GetHistory().GetTotalCount();
		RunAnimation(scene, 10);
		CHECK(CountLogMessages(since, "chain 'RightArm': Colliders: no collider is named 'Gone'") == 1);
		CHECK_FALSE(hangsStraight());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Spring chains with CollidePhysics stay outside physics bodies other than their character's")
	{
		ClearAssetCache clear;
		// Who owns the boxes around the arm: nobody (no boxes), the character (a controller on the biped's parent, a box
		// under that controller and one under the biped), or another entity (the same boxes, unparented).
		enum class Boxes
		{
			None,
			Character,
			Foreign
		};
		struct Result
		{
			glm::vec3 Hand, Elbow, Shoulder;
			float ShelfTop = 0;
		};
		const auto run = [](bool collide, Boxes boxes) {
			Scene scene;
			auto chain = RightArm();
			chain.CollidePhysics = collide;
			auto biped = AddBiped(scene, { chain });
			auto ground = scene.CreateEntity("Ground");
			ground.GetTransform().Translation = { 0, -0.5f, 0 };
			ground.AddComponent<RigidBodyComponent>();
			ground.AddComponent<BoxColliderComponent>().HalfExtents = { 5, 0.5f, 5 };
			const auto addBox = [&](const char* name, Entity parent, const glm::vec3& center) {
				auto box = scene.CreateEntity(name);
				if (parent)
					scene.SetParent(box, parent, false);
				box.GetTransform().Translation = center;
				box.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
				box.AddComponent<BoxColliderComponent>().HalfExtents = { 0.04f, 0.04f, 0.04f };
			};
			if (boxes != Boxes::None)
			{
				// The character holding the biped, its slim capsule clear of everything (so nothing shoves the character), and
				// boxes where the elbow and the hand come to rest on the shelf.
				auto character = scene.CreateEntity("Character");
				scene.SetParent(biped, character, false);
				character.AddComponent<CharacterControllerComponent>().GravityFactor = 0;
				auto& capsule = character.AddComponent<CapsuleColliderComponent>();
				capsule.Offset = { 0.6f, 1.0f, 0 };
				capsule.Radius = 0.05f;
				capsule.HalfHeight = 0.3f;
				const bool owned = boxes == Boxes::Character;
				addBox("Sibling", owned ? character : Entity(), { -0.6f, 1.17f, 0 });
				addBox("Child", owned ? biped : Entity(), { -0.31f, 1.2f, 0 });
			}
			scene.OnSimulationStart();
			scene.OnUpdate(Step);
			const glm::vec3 shoulder = BoneWorld(scene, biped, "RightUpperArm");
			// A shelf 0.35 m below the shoulder, and a trigger in the way that does not collide.
			auto shelf = scene.CreateEntity("Shelf");
			shelf.GetTransform().Translation = shoulder + glm::vec3(0, -0.4f, 0);
			shelf.AddComponent<RigidBodyComponent>();
			shelf.AddComponent<BoxColliderComponent>().HalfExtents = { 0.5f, 0.05f, 0.5f };
			auto trigger = scene.CreateEntity("Trigger");
			trigger.GetTransform().Translation = shoulder + glm::vec3(0, -0.2f, 0);
			trigger.AddComponent<RigidBodyComponent>().IsTrigger = true;
			trigger.AddComponent<BoxColliderComponent>().HalfExtents = { 0.5f, 0.05f, 0.5f };
			for (int i = 0; i < 300; i++)
				scene.OnUpdate(Step);
			Result result{ BoneWorld(scene, biped, "RightHand"), BoneWorld(scene, biped, "RightForeArm"), BoneWorld(scene, biped, "RightUpperArm"), scene.GetWorldTransform(shelf)[3].y + 0.05f };
			scene.OnSimulationStop();
			return result;
		};
		// Off: the arm hangs straight through the shelf.
		const Result off = run(false, Boxes::None);
		CHECK(Near(off.Hand, off.Shoulder + glm::vec3(0, -0.6f, 0), 5e-3f));
		// On: the hand comes to rest on the shelf's top (plus the point radius), not on the trigger above it.
		const Result on = run(true, Boxes::None);
		CHECK(on.Hand.y == doctest::Approx(on.ShelfTop + 0.02f).epsilon(0.004));
		CHECK(on.Elbow.y > on.ShelfTop);
		CHECK(glm::length(on.Hand - on.Elbow) == doctest::Approx(0.3f).epsilon(1e-3));
		// The character's own bodies (a box under its controller beside the biped, one under the biped) never push the arm:
		// it rests exactly as without them.
		const Result owned = run(true, Boxes::Character);
		CHECK(Near(owned.Hand - owned.Shoulder, on.Hand - on.Shoulder, 2e-3f));
		CHECK(Near(owned.Elbow - owned.Shoulder, on.Elbow - on.Shoulder, 2e-3f));
		// The same boxes owned by another entity do.
		const Result foreign = run(true, Boxes::Foreign);
		CHECK_FALSE(Near(foreign.Elbow - foreign.Shoulder, on.Elbow - on.Shoulder, 2e-3f));
		CHECK_FALSE(Near(foreign.Hand - foreign.Shoulder, on.Hand - on.Shoulder, 2e-3f));
	}

	TEST_CASE("Spring chains read the nearest physics bodies and warn once when more are near")
	{
		ClearAssetCache clear;
		Scene scene;
		auto chain = RightArm();
		chain.CollidePhysics = true;
		auto biped = AddBiped(scene, { chain });
		scene.OnSimulationStart();
		scene.OnUpdate(Step);
		const glm::vec3 shoulder = BoneWorld(scene, biped, "RightUpperArm");
		// More small bodies than a chain reads, within its reach but beside the arm's plane, so none touches it.
		std::vector<Entity> pebbles;
		for (uint32_t i = 0; i < PhysicsShapeSnapshot::MaxShapes + 8; i++)
		{
			auto pebble = pebbles.emplace_back(scene.CreateEntity("Pebble"));
			const uint32_t column = i % 10, row = i / 10;
			pebble.GetTransform().Translation = shoulder + glm::vec3(-0.05f * static_cast<float>(column), -0.05f * static_cast<float>(row), 0.3f);
			pebble.AddComponent<RigidBodyComponent>();
			pebble.AddComponent<BoxColliderComponent>().HalfExtents = { 0.01f, 0.01f, 0.01f };
		}
		const auto since = Log::GetHistory().GetTotalCount();
		const auto run = [&](int frames) {
			for (int i = 0; i < frames; i++)
				scene.OnUpdate(Step);
		};
		run(100);
		// The count drops below the limit and rises past it again: still one warning.
		for (size_t i = 0; i < 20; i++)
			pebbles[i].GetTransform().Translation.z += 50;
		run(50);
		for (size_t i = 0; i < 20; i++)
			pebbles[i].GetTransform().Translation.z -= 50;
		run(150);
		CHECK(CountLogMessages(since, "SpringBone 'Biped': chain 'RightArm': more than 32 physics bodies are within its reach; it collides with the nearest 32") == 1);
		// Out of the way, the arm still hangs straight.
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), BoneWorld(scene, biped, "RightUpperArm") + glm::vec3(0, -0.6f, 0), 5e-3f));
		scene.OnSimulationStop();
	}

	TEST_CASE("Spheres leave a body against its penetration axis, else away from its centre, else up")
	{
		CHECK(Near(PushOutDirection({ 0, 0, -2 }, { 1, 1, 1 }, { 0, 0, 0 }), glm::vec3(0, 0, 1)));
		// No axis (Jolt never reports one, but a penetration must still move the sphere).
		CHECK(Near(PushOutDirection(glm::vec3(0), { 3, 0, 4 }, { 0, 0, 0 }), glm::vec3(0.6f, 0, 0.8f)));
		CHECK(Near(PushOutDirection(glm::vec3(std::numeric_limits<float>::quiet_NaN()), { 3, 0, 4 }, { 0, 0, 0 }), glm::vec3(0.6f, 0, 0.8f)));
		CHECK(Near(PushOutDirection(glm::vec3(0), { 1, 2, 3 }, { 1, 2, 3 }), glm::vec3(0, 1, 0)));
	}

	TEST_CASE("Physics shape snapshots gather the nearest bodies and push spheres out of them")
	{
		const auto addBox = [](Scene& owner, const std::string& name, const glm::vec3& center, bool trigger = false) {
			auto box = owner.CreateEntity(name);
			box.GetTransform().Translation = center;
			box.AddComponent<RigidBodyComponent>().IsTrigger = trigger;
			box.AddComponent<BoxColliderComponent>().HalfExtents = { 1, 1, 1 };
			return box;
		};
		Scene scene;
		auto floor = addBox(scene, "Floor", { 0, -1, 0 });
		addBox(scene, "Sensor", { 0, 3, 0 }, true);
		scene.OnSimulationStart();
		scene.OnUpdate(Step);
		const PhysicsWorld& physics = *scene.GetPhysicsWorld();
		PhysicsShapeSnapshot snapshot = physics.CollectShapes({ -0.5f, -0.5f, -0.5f }, { 0.5f, 3, 0.5f }, 0.1f);
		REQUIRE_FALSE(snapshot.Empty());
		CHECK_FALSE(snapshot.Truncated());
		// A sphere 0.05 m into the floor leaves upward to touch it; one clear of it (or inside the trigger) stays.
		const auto out = snapshot.PushOut({ 0.2f, 0.05f, 0 });
		REQUIRE(out);
		CHECK(Near(*out, glm::vec3(0.2f, 0.1f, 0), 1e-3f));
		CHECK_FALSE(snapshot.PushOut({ 0, 0.2f, 0 }));
		CHECK_FALSE(snapshot.PushOut({ 0, 2.5f, 0 }));
		CHECK_FALSE(snapshot.PushOut({ std::numeric_limits<float>::quiet_NaN(), 0, 0 }));
		// Triggers join when the filter asks; `accept` and the filter leave bodies out.
		CHECK(physics.CollectShapes({ -0.5f, 2.5f, -0.5f }, { 0.5f, 3, 0.5f }, 0.1f, { .IncludeTriggers = true }).PushOut({ 0, 2.5f, 0 }));
		CHECK(physics.CollectShapes({ -0.5f, -0.5f, -0.5f }, { 0.5f, 3, 0.5f }, 0.1f, {}, [&](UUID id) { return id != floor.GetUUID(); }).Empty());
		// `accept` runs with no body locked, so it may query the world itself.
		const auto queried = physics.CollectShapes({ -0.5f, -0.5f, -0.5f }, { 0.5f, 3, 0.5f }, 0.1f, {}, [&](UUID id) { return !physics.OverlapSphere({ 0, -1, 0 }, 0.5f).empty() && id == floor.GetUUID(); });
		CHECK(queried.PushOut({ 0.2f, 0.05f, 0 }));
		// The radius queried with, at least MinRadius.
		CHECK(snapshot.GetRadius() == 0.1f);
		CHECK(physics.CollectShapes({ -0.5f, -0.5f, -0.5f }, { 0.5f, 3, 0.5f }, 0).GetRadius() == PhysicsShapeSnapshot::MinRadius);
		CHECK(physics.CollectShapes({ 50, 50, 50 }, { 51, 51, 51 }, 0.2f).GetRadius() == 0.2f);
		CHECK(PhysicsShapeSnapshot().GetRadius() == 0);
		CHECK(physics.CollectShapes({ -0.5f, -0.5f, -0.5f }, { 0.5f, 3, 0.5f }, 0.1f, { .IgnoreEntity = floor.GetUUID() }).Empty());
		CHECK(physics.CollectShapes({ -0.5f, -0.5f, -0.5f }, { 0.5f, 3, 0.5f }, 0.1f, { .LayerMask = 0 }).Empty());
		// Bad input gathers nothing.
		CHECK(physics.CollectShapes({ 1, 1, 1 }, { -1, -1, -1 }, 0.1f).Empty());
		CHECK(physics.CollectShapes({ -1, -1, -1 }, { 1, std::numeric_limits<float>::infinity(), 1 }, 0.1f).Empty());
		CHECK(physics.CollectShapes({ -1, -1, -1 }, { 1, 1, 1 }, -1).Empty());
		CHECK(PhysicsShapeSnapshot().Empty());
		CHECK_FALSE(PhysicsShapeSnapshot().PushOut({ 0, 0, 0 }));
		scene.OnSimulationStop();

		// Past MaxShapes, the nearest are kept.
		Scene crowded;
		for (uint32_t i = 0; i < PhysicsShapeSnapshot::MaxShapes + 4; i++)
			addBox(crowded, "Box", { 3.0f * static_cast<float>(i), 0, 0 });
		crowded.OnSimulationStart();
		crowded.OnUpdate(Step);
		// Centered on the first box, so the last four are the farthest.
		const float span = 3.0f * (PhysicsShapeSnapshot::MaxShapes + 4);
		const PhysicsShapeSnapshot many = crowded.GetPhysicsWorld()->CollectShapes({ -span, -2, -2 }, { span, 2, 2 }, 0.1f);
		CHECK(many.Truncated());
		CHECK(many.PushOut({ 0, 0.95f, 0 }));
		CHECK_FALSE(many.PushOut({ 3.0f * static_cast<float>(PhysicsShapeSnapshot::MaxShapes + 3), 0.95f, 0 }));
		crowded.OnSimulationStop();
	}

	TEST_CASE("The edit preview swings the spring bones of previewed Animators and writes nothing")
	{
		ClearAssetCache clear;
		Scene scene;
		auto chain = RightArm();
		chain.CollidePhysics = true;
		auto biped = AddBiped(scene, { chain });
		biped.GetComponent<AnimatorComponent>().Preview = true;
		// A rig the preview does not play keeps its rest pose, quietly, whatever its springs.
		auto still = AddBiped(scene, { RightArm() });
		still.GetTransform().Translation.x = 3;
		still.GetComponent<SpringBoneComponent>().Chains[0].Joint = "Nope";
		// Bodies that move in Play wait for it: a dynamic shelf in the way changes nothing.
		auto shelf = scene.CreateEntity("Shelf");
		shelf.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		shelf.AddComponent<BoxColliderComponent>().HalfExtents = { 2, 0.05f, 2 };
		const auto since = Log::GetHistory().GetTotalCount();
		scene.UpdateAnimationPreview(Step);
		AnimationSystem& preview = *scene.GetPoseAnimationSystem();
		const auto handAndShoulder = [&](Entity entity) {
			glm::mat4 hand, shoulder;
			std::string error;
			REQUIRE(preview.GetBoneTransform(entity, "RightHand", hand, error));
			REQUIRE(preview.GetBoneTransform(entity, "RightUpperArm", shoulder, error));
			return std::pair{ glm::vec3(scene.GetWorldTransform(entity) * hand[3]), glm::vec3(scene.GetWorldTransform(entity) * shoulder[3]) };
		};
		shelf.GetTransform().Translation = handAndShoulder(biped).second + glm::vec3(0, -0.4f, 0);
		const json before = SceneSerializer::SerializeScene(scene);
		for (int i = 0; i < 300; i++)
			scene.UpdateAnimationPreview(Step);
		const auto [hand, shoulder] = handAndShoulder(biped);
		CHECK(Near(hand, shoulder + glm::vec3(0, -0.6f, 0), 5e-3f));
		CHECK(preview.GetPose(still) == nullptr);
		CHECK(CountLogMessages(since, "SpringBone") == 0);
		CHECK(SceneSerializer::SerializeScene(scene) == before);
		// Moving the entity in the editor swings the chain.
		biped.GetTransform().Translation.z += 0.2f;
		scene.UpdateAnimationPreview(Step);
		const auto [movedHand, movedShoulder] = handAndShoulder(biped);
		CHECK(movedHand.z < movedShoulder.z - 0.01f);
		scene.ResetAnimationPreview();
	}

	TEST_CASE("Swept points stop at what is too thin to catch them where they end")
	{
		// One bone hanging from the origin, flung sideways along its arc into a capsule 2 mm thick that crosses the arc
		// along a radius (so the point cannot slide around it): fast enough to pass it within one substep.
		const glm::vec3 radial(0.5f, -0.866f, 0);
		const SpringCollisionShape rod{ .A = radial * 0.5f, .B = radial * 1.5f, .Radius = 0.001f };
		const auto fling = [&](bool continuous) {
			Strand strand({ 0, 0, 0 }, { 0, -1, 0 }, 1, 1.0f);
			strand.Run(60, 1.0f / 60.0f, { .Colliders = std::span(&rod, 1), .Continuous = continuous, .Acceleration = { 400, 0, 0 } });
			return strand;
		};
		// Without the sweep the point never ends a substep near the rod.
		CHECK(fling(false).Points[0].Position.x > 0.6f);
		const Strand held = fling(true);
		CHECK(held.Points[0].Position.x < 0.5f);
		CHECK(held.Points[0].Position.x > 0.45f);
		CHECK(held.Length(0) == doctest::Approx(1).epsilon(0.01));

		// A point inside a collider moves out of it freely.
		const SpringCollisionShape around{ .A = { 0, -1, 0 }, .B = { 0, -1, 0 }, .Radius = 0.2f };
		Strand inside({ 0, 0, 0 }, { 0, -1, 0 }, 1, 1.0f);
		inside.Run(30, 1.0f / 60.0f, { .Colliders = std::span(&around, 1), .Continuous = true, .Acceleration = { 400, 0, 0 } });
		CHECK(inside.Points[0].Position.x > 0.3f);
	}

	TEST_CASE("A swept point slides along what stops it and lets go of what its chain left behind")
	{
		// A long capsule under a bone whose head moves along it, its point dragged across the top under strong gravity:
		// at each contact the move along the surface stays, so the point keeps up with its head.
		const SpringCollisionShape rail{ .A = { -50, -1.4f, 0 }, .B = { 50, -1.4f, 0 }, .Radius = 0.5f };
		Strand strand({ 0, 0, 0 }, { -1, 0, 0 }, 1, 1.0f);
		strand.Move({ 2, 0, 0 }, 120, 1.0f / 60.0f, { .Colliders = std::span(&rail, 1), .Continuous = true, .Acceleration = { 0, -400, 0 } });
		const glm::vec3 dragged = strand.Points[0].Position;
		CHECK(dragged.y == doctest::Approx(-0.9f).epsilon(0.01));
		CHECK(dragged.x == doctest::Approx(4 - 0.436f).epsilon(0.03));
		CHECK(strand.Length(0) == doctest::Approx(1).epsilon(0.02));

		// A chain whose head walks through a wall, its point behind: stopped at first, held past half a bone it comes along.
		// The wall is a plane at z = 0.5 that stops anything reaching it from behind.
		const auto wall = [](const glm::vec3& from, const glm::vec3& to) -> std::optional<SpringSweepHit> {
			if (from.z >= 0.5f || to.z < 0.5f)
				return std::nullopt;
			return SpringSweepHit{ .Fraction = (0.5f - from.z) / (to.z - from.z), .Normal = { 0, 0, -1 } };
		};
		Strand trailing({ 0, 0, 0 }, { 0, 0, -1 }, 1, 0.2f);
		SpringSimulationRequest request{ .Continuous = true, .WorldSweep = wall, .Stiffness = 100 };
		// Its head a little past the wall, the point is held on the near side.
		trailing.Move({ 0, 0, 2 }, 23, 1.0f / 60.0f, request);
		CHECK(trailing.Points[0].Position.z == doctest::Approx(0.5f).scale(0).epsilon(1e-3));
		CHECK(trailing.Points[0].Position.z < 0.5f);
		trailing.Move({ 0, 0, 2 }, 37, 1.0f / 60.0f, request);
		CHECK(trailing.Points[0].Position.z > 1.5f);
		CHECK(trailing.Length(0) == doctest::Approx(0.2f).epsilon(0.05));
	}

	TEST_CASE("Snapshots sweep spheres into bodies but let them out")
	{
		Scene scene;
		auto wall = scene.CreateEntity("Wall");
		wall.AddComponent<RigidBodyComponent>();
		wall.AddComponent<BoxColliderComponent>().HalfExtents = { 2, 2, 0.01f };
		scene.OnSimulationStart();
		const PhysicsShapeSnapshot snapshot = scene.GetPhysicsWorld()->CollectShapes({ -1, -1, -1 }, { 1, 1, 1 }, 0.1f);
		REQUIRE_FALSE(snapshot.Empty());
		const auto hit = snapshot.Sweep({ 0, 0, -1 }, { 0, 0, 1 });
		REQUIRE(hit);
		CHECK(hit->Fraction == doctest::Approx((1 - 0.11f) / 2).epsilon(1e-3));
		CHECK(Near(hit->Normal, glm::vec3(0, 0, -1)));
		CHECK(snapshot.Sweep({ 0, 0, 1 }, { 0, 0, -1 })->Normal.z == doctest::Approx(1));
		// Beside it, short of it and away from it.
		CHECK_FALSE(snapshot.Sweep({ 3, 0, -1 }, { 3, 0, 1 }));
		CHECK_FALSE(snapshot.Sweep({ 0, 0, -1 }, { 0, 0, -0.5f }));
		CHECK_FALSE(snapshot.Sweep({ 0, 0, -0.5f }, { 0, 0, -1 }));
		// Starting inside: out is free, deeper stops at once with the way out as the normal.
		CHECK_FALSE(snapshot.Sweep({ 0, 0, -0.05f }, { 0, 0, -1 }));
		const auto deeper = snapshot.Sweep({ 0, 0, -0.05f }, { 0, 0, 1 });
		REQUIRE(deeper);
		CHECK(deeper->Fraction == 0);
		CHECK(Near(deeper->Normal, glm::vec3(0, 0, -1)));
		// Deep inside a thick body the normal is still the shortest way out, not against the move.
		auto block = scene.CreateEntity("Block");
		block.GetTransform().Translation = { 20, 0, 0 };
		block.AddComponent<RigidBodyComponent>();
		block.AddComponent<BoxColliderComponent>().HalfExtents = { 2, 2, 2 };
		scene.OnUpdate(Step);
		const PhysicsShapeSnapshot inBlock = scene.GetPhysicsWorld()->CollectShapes({ 18, -2, -2 }, { 22, 2, 2 }, 0.1f);
		const auto inner = inBlock.Sweep({ 20, 0, -1.8f }, { 21, 0, 0 });
		REQUIRE(inner);
		CHECK(Near(inner->Normal, glm::vec3(0, 0, -1)));
		// Nothing moves, or nothing is known.
		CHECK_FALSE(snapshot.Sweep({ 0, 0, -1 }, { 0, 0, -1 }));
		CHECK_FALSE(snapshot.Sweep({ 0, 0, -1 }, { 0, 0, std::numeric_limits<float>::infinity() }));
		CHECK_FALSE(PhysicsShapeSnapshot().Sweep({ 0, 0, -1 }, { 0, 0, 1 }));
		scene.OnSimulationStop();
	}

	TEST_CASE("Continuous spring chains stop at physics walls thinner than one substep's move")
	{
		const auto run = [](bool continuous) {
			ClearAssetCache clear;
			Scene scene;
			auto chain = RightArm();
			chain.CollidePhysics = true;
			chain.Continuous = continuous;
			auto biped = AddBiped(scene, { chain });
			biped.GetComponent<SpringBoneComponent>().Wind = { 0, 0, 300 };
			scene.GetPhysicsSettings().Gravity = glm::vec3(0, -1, 0);
			scene.OnSimulationStart();
			scene.OnUpdate(Step);
			const glm::vec3 shoulder = BoneWorld(scene, biped, "RightUpperArm");
			// 2 mm thick, across the way the wind swings the arm, below the shoulder's height.
			auto wall = scene.CreateEntity("Wall");
			wall.GetTransform().Translation = shoulder + glm::vec3(0, 0, 0.3f);
			wall.AddComponent<RigidBodyComponent>();
			wall.AddComponent<BoxColliderComponent>().HalfExtents = { 2, 2, 0.001f };
			for (int i = 0; i < 90; i++)
				scene.OnUpdate(Step);
			const glm::vec3 hand = BoneWorld(scene, biped, "RightHand"), elbow = BoneWorld(scene, biped, "RightForeArm");
			const float wallZ = shoulder.z + 0.3f;
			scene.OnSimulationStop();
			return std::pair{ hand.z - wallZ, elbow.z - wallZ };
		};
		const auto [passedHand, passedElbow] = run(false);
		CHECK((passedHand > 0 || passedElbow > 0));
		const auto [hand, elbow] = run(true);
		CHECK(hand < 0);
		CHECK(elbow < 0);
	}

	TEST_CASE("Spring chains collide through the project's physics layers")
	{
		enum class Mode
		{
			Game,
			Preview
		};
		// Where the arm's hand rests above a shelf on `shelfLayer`, for a chain on `chainLayer`, relative to the shelf's top
		// (negative: through it).
		const auto rest = [](const std::string& chainLayer, const std::string& shelfLayer, Mode mode, std::vector<std::string>* warnings = nullptr) {
			ClearAssetCache clear;
			Scene scene;
			PhysicsLayers layers;
			std::string error;
			REQUIRE(layers.Add("Hair", error));
			REQUIRE(layers.Add("Props", error));
			layers.SetCollides(*layers.Find("Hair"), *layers.Find("Props"), false);
			scene.SetPhysicsLayers(layers);
			auto chain = RightArm();
			chain.CollidePhysics = true;
			chain.Layer = chainLayer;
			auto biped = AddBiped(scene, { chain });
			biped.GetComponent<AnimatorComponent>().Preview = true;
			// The shoulder never moves (the pose holds still), so the shelf can be placed before anything runs.
			const glm::vec3 shoulder(-0.2f, 1.45f, 0);
			auto shelf = scene.CreateEntity("Shelf");
			shelf.GetTransform().Translation = shoulder + glm::vec3(0, -0.4f, 0);
			auto& body = shelf.AddComponent<RigidBodyComponent>();
			body.Layer = shelfLayer;
			shelf.AddComponent<BoxColliderComponent>().HalfExtents = { 3, 0.05f, 3 };
			if (warnings)
				*warnings = GetAnimationWarnings(biped);
			float top = 0;
			if (mode == Mode::Game)
			{
				scene.OnSimulationStart();
				scene.OnUpdate(Step);
				shelf.GetTransform().Translation = BoneWorld(scene, biped, "RightUpperArm") + glm::vec3(0, -0.4f, 0);
				for (int i = 0; i < 300; i++)
					scene.OnUpdate(Step);
				top = BoneWorld(scene, biped, "RightHand").y - (shelf.GetTransform().Translation.y + 0.05f);
				scene.OnSimulationStop();
			}
			else
			{
				scene.UpdateAnimationPreview(Step);
				glm::mat4 joint;
				REQUIRE(scene.GetPoseAnimationSystem()->GetBoneTransform(biped, "RightUpperArm", joint, error));
				shelf.GetTransform().Translation = glm::vec3(joint[3]) + glm::vec3(0, -0.4f, 0);
				for (int i = 0; i < 300; i++)
					scene.UpdateAnimationPreview(Step);
				REQUIRE(scene.GetPoseAnimationSystem()->GetBoneTransform(biped, "RightHand", joint, error));
				top = joint[3].y - (shelf.GetTransform().Translation.y + 0.05f);
				scene.ResetAnimationPreview();
			}
			return top;
		};
		for (const Mode mode : { Mode::Game, Mode::Preview })
		{
			CAPTURE(static_cast<int>(mode));
			// Every layer without a chain layer; with one, what the matrix says.
			CHECK(rest("", "Props", mode) > 0);
			CHECK(rest("Hair", "Default", mode) > 0);
			CHECK(rest("Props", "Props", mode) > 0);
			CHECK(rest("Hair", "Props", mode) < -0.1f);
		}
		// A name the project does not define is Default, in the log and the inspector.
		const auto since = Log::GetHistory().GetTotalCount();
		std::vector<std::string> warnings;
		CHECK(rest("Nope", "Props", Mode::Game, &warnings) > 0);
		CHECK(CountLogMessages(since, "the physics layer 'Nope' is not defined; it counts as Default") == 1);
		CHECK(std::ranges::count(warnings, std::string("SpringBone chain 'RightArm': the physics layer 'Nope' is not defined; it counts as Default")) == 1);

		// Said once, however often the chain restarts, and also while a ragdoll has it. A layer that meets nothing says so too.
		Scene scene;
		PhysicsLayers lonely;
		std::string error;
		REQUIRE(lonely.Add("Lonely", error));
		lonely.SetCollides(0, *lonely.Find("Lonely"), false);
		lonely.SetCollides(*lonely.Find("Lonely"), *lonely.Find("Lonely"), false);
		scene.SetPhysicsLayers(lonely);
		auto chain = RightArm();
		chain.CollidePhysics = true;
		chain.Layer = "Nope";
		auto biped = AddBiped(scene, { chain });
		const auto logged = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		scene.OnUpdate(Step);
		biped.GetTransform().Translation.x += 50;
		scene.OnUpdate(Step);
		scene.OnUpdate(Step);
		scene.GetAnimationSystem()->ResetSpringBones(biped);
		scene.OnUpdate(Step);
		scene.OnUpdate(Step);
		CHECK(CountLogMessages(logged, "the physics layer 'Nope' is not defined; it counts as Default") == 1);
		biped.GetComponent<SpringBoneComponent>().Chains[0].Layer = "Lonely";
		scene.OnUpdate(Step);
		CHECK(CountLogMessages(logged, "the physics layer 'Lonely' collides with no layer") == 1);
		scene.OnSimulationStop();
	}

	TEST_CASE("Static shapes keep up with the project's layers")
	{
		ClearAssetCache clear;
		Scene scene;
		PhysicsLayers layers;
		std::string error;
		REQUIRE(layers.Add("A", error));
		REQUIRE(layers.Add("B", error));
		scene.SetPhysicsLayers(layers);
		auto box = scene.CreateEntity("Box");
		box.AddComponent<RigidBodyComponent>().Layer = "B";
		box.AddComponent<BoxColliderComponent>();
		StaticPhysicsShapes shapes(scene);
		const auto seenOn = [&](const char* layer) {
			const auto index = shapes.GetLayers().Find(layer);
			return index && !shapes.CollectShapes(glm::vec3(-1), glm::vec3(1), 0.1f, { .LayerMask = 1u << *index }).Empty();
		};
		CHECK(seenOn("B"));
		// Removing a layer moves the others down: the body stays on its name's layer.
		REQUIRE(layers.Remove(*layers.Find("A"), error));
		scene.SetPhysicsLayers(layers);
		shapes.Refresh();
		CHECK(seenOn("B"));
		// Its name gone, it is on Default, and said so; defined again, it is back.
		const auto since = Log::GetHistory().GetTotalCount();
		REQUIRE(layers.Remove(*layers.Find("B"), error));
		scene.SetPhysicsLayers(layers);
		shapes.Refresh();
		CHECK(seenOn("Default"));
		CHECK(CountLogMessages(since, "unknown physics layer 'B', using Default") == 1);
		REQUIRE(layers.Add("C", error));
		REQUIRE(layers.Add("B", error));
		scene.SetPhysicsLayers(layers);
		shapes.Refresh();
		CHECK(seenOn("B"));
		CHECK_FALSE(seenOn("Default"));
	}

	TEST_CASE("Static shapes drop a mesh that went away")
	{
		ClearAssetCache clear;
		TempDirectory directory("StaticShapesMesh");
		const auto path = directory.GetPath() / "Box.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, ReadJson(BipedPath()).dump()));
		Scene scene;
		auto body = scene.CreateEntity("Body");
		body.AddComponent<RigidBodyComponent>();
		auto& mesh = body.AddComponent<MeshColliderComponent>();
		mesh.Mesh = path.string();
		mesh.Convex = true;
		StaticPhysicsShapes shapes(scene);
		REQUIRE_FALSE(shapes.CollectShapes(glm::vec3(-2), glm::vec3(2), 0.1f).Empty());
		// The asset is gone from the cache and the disk: the shape must not outlive it silently.
		std::filesystem::remove(path);
		AssetManager::Clear();
		PhysicsWorld::ClearMeshShapeCache();
		const auto since = Log::GetHistory().GetTotalCount();
		shapes.Refresh();
		CHECK(shapes.CollectShapes(glm::vec3(-2), glm::vec3(2), 0.1f).Empty());
		CHECK(CountLogMessages(since, "MeshCollider ignored") == 1);
	}

	TEST_CASE("Spring chain layers are warned about and validated like body layers")
	{
		ClearAssetCache clear;
		Scene scene;
		auto chain = RightArm();
		chain.Layer = "Nope";
		auto biped = AddBiped(scene, { chain });
		// Without CollidePhysics the layer does nothing, which is the warning.
		CHECK(std::ranges::count(GetAnimationWarnings(biped), std::string("SpringBone chain 'RightArm': Layer has no effect without CollidePhysics")) == 1);
		const auto unknown = FindUnknownPhysicsLayers(scene, PhysicsLayers());
		REQUIRE(unknown.size() == 1);
		CHECK(unknown[0].Entity == biped.GetUUID());
		CHECK(unknown[0].Layer == "Nope");
		biped.GetComponent<SpringBoneComponent>().Chains[0].Layer.clear();
		CHECK(FindUnknownPhysicsLayers(scene, PhysicsLayers()).empty());
	}

	TEST_CASE("Static shapes read the edit scene as a world would and follow its edits")
	{
		ClearAssetCache clear;
		Scene scene;
		const auto add = [&](const char* name, const glm::vec3& at, RigidBodyType type, bool trigger = false) {
			auto entity = scene.CreateEntity(name);
			entity.GetTransform().Translation = at;
			auto& body = entity.AddComponent<RigidBodyComponent>();
			body.Type = type;
			body.IsTrigger = trigger;
			entity.AddComponent<BoxColliderComponent>();
			return entity;
		};
		auto wall = add("Wall", { 0, 0, 0 }, RigidBodyType::Static);
		add("Dynamic", { 5, 0, 0 }, RigidBodyType::Dynamic);
		add("Kinematic", { 10, 0, 0 }, RigidBodyType::Kinematic);
		add("Trigger", { 15, 0, 0 }, RigidBodyType::Static, true);
		add("Character", { 20, 0, 0 }, RigidBodyType::Static).AddComponent<CharacterControllerComponent>();
		add("Scaled", { 25, 0, 0 }, RigidBodyType::Static).GetTransform().Scale = glm::vec3(2);
		auto hull = scene.CreateEntity("Hull");
		hull.GetTransform().Translation = { 30, 0, 0 };
		hull.AddComponent<RigidBodyComponent>();
		auto& mesh = hull.AddComponent<MeshColliderComponent>();
		mesh.Mesh = "builtin://Cube";
		mesh.Convex = true;
		const json before = SceneSerializer::SerializeScene(scene);
		StaticPhysicsShapes shapes(scene);
		shapes.Refresh();
		CHECK(SceneSerializer::SerializeScene(scene) == before);
		const auto pushed = [&](const glm::vec3& point) {
			return shapes.CollectShapes(point - glm::vec3(1), point + glm::vec3(1), 0.1f).PushOut(point).has_value();
		};
		CHECK(pushed({ 0, 0.5f, 0 }));
		CHECK_FALSE(pushed({ 5, 0.5f, 0 }));
		CHECK_FALSE(pushed({ 10, 0.5f, 0 }));
		CHECK_FALSE(pushed({ 15, 0.5f, 0 }));
		CHECK_FALSE(pushed({ 20, 0.5f, 0 }));
		// World scale reaches the shape, and a mesh collider is the same cooked shape as in Play.
		CHECK(pushed({ 25, 0.9f, 0 }));
		CHECK(pushed({ 30, 0.3f, 0 }));
		// A filter and `accept` work as in a world, and what they drop is not there at all.
		CHECK(shapes.CollectShapes({ -1, -1, -1 }, { 1, 1, 1 }, 0.1f, { .LayerMask = 0 }).Empty());
		CHECK(shapes.CollectShapes({ -1, -1, -1 }, { 1, 1, 1 }, 0.1f, { .IgnoreEntity = wall.GetUUID() }).Empty());
		CHECK(shapes.CollectShapes({ -1, -1, -1 }, { 1, 1, 1 }, 0.1f, {}, [&](UUID id) { return id != wall.GetUUID(); }).Empty());
		CHECK(shapes.CollectShapes({ 1, 1, 1 }, { -1, -1, -1 }, 0.1f).Empty());
		CHECK(shapes.CollectShapes({ -1, -1, -1 }, { 1, 1, std::numeric_limits<float>::infinity() }, 0.1f).Empty());
		CHECK(shapes.CollectShapes({ -1, -1, -1 }, { 1, 1, 1 }, -1).Empty());
		CHECK(shapes.CollectShapes({ -1, -1, -1 }, { 1, 1, 1 }, 0).GetRadius() == PhysicsShapeSnapshot::MinRadius);

		// Edits count after the next Refresh: a move, a bigger collider, a removed body.
		wall.GetTransform().Translation.z = 3;
		CHECK(pushed({ 0, 0.5f, 0 }));
		shapes.Refresh();
		CHECK_FALSE(pushed({ 0, 0.5f, 0 }));
		CHECK(pushed({ 0, 0.5f, 3 }));
		wall.GetComponent<BoxColliderComponent>().HalfExtents = glm::vec3(2);
		shapes.Refresh();
		CHECK(pushed({ 0, 1.5f, 3 }));
		wall.GetTransform().Scale = glm::vec3(2);
		shapes.Refresh();
		CHECK(pushed({ 0, 3.5f, 3 }));
		scene.DestroyEntity(wall);
		shapes.Refresh();
		CHECK_FALSE(pushed({ 0, 1.5f, 3 }));
		// Bodies whose type changes come and go.
		auto dynamic = scene.FindEntityByName("Dynamic");
		dynamic.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		shapes.Refresh();
		CHECK(pushed({ 5, 0.5f, 0 }));

		// A transform that cannot place a body leaves it out, said once, and it returns with its collider when it can.
		const auto since = Log::GetHistory().GetTotalCount();
		dynamic.GetTransform().Scale = glm::vec3(0);
		for (int i = 0; i < 3; i++)
			shapes.Refresh();
		CHECK_FALSE(pushed({ 5, 0.5f, 0 }));
		CHECK(CountLogMessages(since, "degenerate transform; the preview leaves its body out") == 1);
		dynamic.GetTransform().Scale = glm::vec3(1);
		shapes.Refresh();
		CHECK(pushed({ 5, 0.5f, 0 }));
		// A layer the project does not define is Default, as in Play.
		dynamic.GetComponent<RigidBodyComponent>().Layer = "Nope";
		shapes.Refresh();
		CHECK(CountLogMessages(since, "unknown physics layer 'Nope', using Default") == 1);
		CHECK(shapes.CollectShapes({ 4, -1, -1 }, { 6, 1, 1 }, 0.1f, { .LayerMask = 1 }).PushOut({ 5, 0.5f, 0 }).has_value());
	}

	TEST_CASE("Static shapes answer as the world does for the same scene")
	{
		ClearAssetCache clear;
		Scene scene;
		const auto add = [&](const glm::vec3& at, const glm::quat& rotation, const glm::vec3& scale) {
			auto entity = scene.CreateEntity("Body");
			entity.GetTransform().Translation = at;
			entity.GetTransform().Rotation = rotation;
			entity.GetTransform().Scale = scale;
			entity.AddComponent<RigidBodyComponent>();
			return entity;
		};
		// Offset colliders put the center of mass away from the origin; a compound of two kinds, a rotated parent, a scale.
		const glm::quat turned = glm::angleAxis(0.6f, glm::normalize(glm::vec3(1, 2, 3)));
		auto offset = add({ 0, 0, 0 }, turned, glm::vec3(1));
		offset.AddComponent<BoxColliderComponent>().Offset = { 1, 0, 0 };
		auto compound = add({ 3, 0, 0 }, turned, glm::vec3(1));
		compound.AddComponent<SphereColliderComponent>().Offset = { 0, 1, 0 };
		compound.AddComponent<BoxColliderComponent>().HalfExtents = { 0.4f, 0.4f, 0.4f };
		auto child = add({ 0, 2, 0 }, glm::quat(1, 0, 0, 0), glm::vec3(1));
		child.AddComponent<CapsuleColliderComponent>();
		scene.SetParent(child, compound, false);
		auto scaled = add({ -3, 0, 0 }, turned, glm::vec3(1.5f));
		scaled.AddComponent<MeshColliderComponent>().Mesh = "builtin://Cube";
		StaticPhysicsShapes shapes(scene);
		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		const glm::vec3 min(-6, -4, -4), max(7, 5, 4);
		const PhysicsShapeSnapshot world = physics.CollectShapes(min, max, 0.1f);
		const PhysicsShapeSnapshot edit = shapes.CollectShapes(min, max, 0.1f);
		for (int x = 0; x <= 26; x++)
			for (int y = 0; y <= 18; y++)
				for (int z = 0; z <= 16; z++)
				{
					const glm::vec3 point = min + 0.5f * glm::vec3(x, y, z);
					CAPTURE(point);
					const auto expected = world.PushOut(point);
					const auto actual = edit.PushOut(point);
					REQUIRE(expected.has_value() == actual.has_value());
					if (expected)
						CHECK(Near(*expected, *actual, 1e-4f));
					const auto expectedHit = world.Sweep(point, point + glm::vec3(1.3f, 0.7f, -0.4f));
					const auto actualHit = edit.Sweep(point, point + glm::vec3(1.3f, 0.7f, -0.4f));
					REQUIRE(expectedHit.has_value() == actualHit.has_value());
					if (expectedHit)
						CHECK(expectedHit->Fraction == doctest::Approx(actualHit->Fraction).epsilon(1e-4));
					// Rays start inside bodies too; both report them the same way, and hit each body once.
					const glm::vec3 ray = glm::normalize(glm::vec3(0.8f, -1, 0.3f));
					const auto worldHits = physics.RaycastAll(point, ray, 6);
					const auto editHits = shapes.RaycastAll(point, ray, 6);
					REQUIRE(worldHits.size() == editHits.size());
					for (size_t i = 0; i < worldHits.size(); i++)
					{
						CHECK(worldHits[i].EntityID == editHits[i].EntityID);
						CHECK(worldHits[i].Distance == doctest::Approx(editHits[i].Distance).epsilon(1e-4));
						CHECK(Near(worldHits[i].Point, editHits[i].Point, 1e-3f));
						CHECK(Near(worldHits[i].Normal, editHits[i].Normal, 1e-3f));
					}
				}
		// Filters and arguments read the same too.
		const glm::vec3 above(3, 4, 0), down(0, -1, 0);
		const auto hitsOf = [&](const PhysicsQueryFilter& filter) {
			std::vector<UUID> inWorld, inEdit;
			for (const RaycastHit& hit : physics.RaycastAll(above, down, 9, filter))
				inWorld.push_back(hit.EntityID);
			for (const RaycastHit& hit : shapes.RaycastAll(above, down, 9, filter))
				inEdit.push_back(hit.EntityID);
			CHECK(inWorld == inEdit);
			return inEdit;
		};
		REQUIRE_FALSE(hitsOf({}).empty());
		const UUID first = hitsOf({}).front();
		const std::vector<UUID> others = hitsOf({ .IgnoreEntity = first });
		CHECK(std::ranges::find(others, first) == others.end());
		CHECK(hitsOf({ .LayerMask = 0 }).empty());
		CHECK(shapes.RaycastAll(above, glm::vec3(0), 9).empty());
		CHECK(shapes.RaycastAll(above, down, 0).empty());
		CHECK(shapes.RaycastAll(glm::vec3(std::numeric_limits<float>::quiet_NaN()), down, 9).empty());
		scene.OnSimulationStop();
	}

	TEST_CASE("Static shapes follow a collider's mesh and stop at a triangle mesh's front faces")
	{
		ClearAssetCache clear;
		Scene scene;
		auto floor = scene.CreateEntity("Floor");
		floor.AddComponent<RigidBodyComponent>();
		floor.AddComponent<MeshComponent>().Mesh = "builtin://Plane";
		floor.AddComponent<MeshColliderComponent>();
		auto cube = scene.CreateEntity("Cube");
		cube.GetTransform().Translation = { 5, 0, 0 };
		cube.AddComponent<RigidBodyComponent>();
		cube.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
		cube.AddComponent<MeshColliderComponent>().Convex = true;
		StaticPhysicsShapes shapes(scene);
		const auto collect = [&](const glm::vec3& at) { return shapes.CollectShapes(at - glm::vec3(2), at + glm::vec3(2), 0.1f); };
		// A plane's triangles face up: a sphere comes down on it, and up through it.
		CHECK(collect({ 0, 1, 0 }).Sweep({ 0, 1, 0 }, { 0, -1, 0 }).has_value());
		CHECK_FALSE(collect({ 0, -1, 0 }).Sweep({ 0, -1, 0 }, { 0, 1, 0 }).has_value());
		// The borrowed mesh is the entity's MeshComponent: a change reaches the shape.
		CHECK(collect({ 5, 0.45f, 0 }).PushOut({ 5, 0.45f, 0 }).has_value());
		cube.GetComponent<MeshComponent>().Mesh = "builtin://Sphere";
		shapes.Refresh();
		CHECK(collect({ 5, 0.45f, 0.4f }).PushOut({ 5, 0.45f, 0.4f }).has_value() == false);
	}

	TEST_CASE("Spring chain warnings say what a chain's settings cannot do")
	{
		ClearAssetCache clear;
		Scene scene;
		PhysicsLayers layers;
		std::string error;
		REQUIRE(layers.Add("Nothing", error));
		for (uint32_t other = 0; other < layers.GetCount(); other++)
			layers.SetCollides(*layers.Find("Nothing"), other, false);
		scene.SetPhysicsLayers(layers);
		auto chain = RightArm();
		chain.Continuous = true;
		auto biped = AddBiped(scene, { chain });
		const auto warned = [&](const std::string& text) { return std::ranges::count(GetAnimationWarnings(biped), text); };
		const std::string nothing = "SpringBone chain 'RightArm': Continuous has no effect: there is nothing to sweep against";
		CHECK(warned(nothing) == 1);
		auto& component = biped.GetComponent<SpringBoneComponent>();
		component.Colliders.push_back({});
		CHECK(warned(nothing) == 0);
		component.Colliders.clear();
		auto& config = component.Chains[0];
		config.CollidePhysics = true;
		CHECK(warned(nothing) == 0);
		config.Layer = "Nothing";
		CHECK(warned("SpringBone chain 'RightArm': the physics layer 'Nothing' collides with no layer") == 1);
		config.Layer = "Default";
		CHECK(warned("SpringBone chain 'RightArm': the physics layer 'Default' collides with no layer") == 0);
	}

	TEST_CASE("A swept chain in the edit preview stops at thin static bodies")
	{
		const auto run = [](bool continuous) {
			ClearAssetCache clear;
			Scene scene;
			auto chain = RightArm();
			chain.CollidePhysics = true;
			chain.Continuous = continuous;
			auto biped = AddBiped(scene, { chain });
			biped.GetComponent<AnimatorComponent>().Preview = true;
			biped.GetComponent<SpringBoneComponent>().Wind = { 0, 0, 300 };
			scene.GetPhysicsSettings().Gravity = glm::vec3(0, -1, 0);
			scene.UpdateAnimationPreview(Step);
			glm::mat4 joint;
			std::string error;
			REQUIRE(scene.GetPoseAnimationSystem()->GetBoneTransform(biped, "RightUpperArm", joint, error));
			const float wallZ = joint[3].z + 0.3f;
			auto wall = scene.CreateEntity("Wall");
			wall.GetTransform().Translation = { 0, 1, wallZ };
			wall.AddComponent<RigidBodyComponent>();
			wall.AddComponent<BoxColliderComponent>().HalfExtents = { 3, 3, 0.001f };
			for (int i = 0; i < 90; i++)
				scene.UpdateAnimationPreview(Step);
			REQUIRE(scene.GetPoseAnimationSystem()->GetBoneTransform(biped, "RightHand", joint, error));
			scene.ResetAnimationPreview();
			return joint[3].z - wallZ;
		};
		CHECK(run(false) > 0);
		CHECK(run(true) < 0);
	}

	TEST_CASE("The edit preview's chains rest on static bodies and skip the character's own")
	{
		ClearAssetCache clear;
		Scene scene;
		auto chain = RightArm();
		chain.CollidePhysics = true;
		auto character = scene.CreateEntity("Character");
		character.AddComponent<CharacterControllerComponent>();
		auto biped = AddBiped(scene, { chain });
		scene.SetParent(biped, character, false);
		biped.GetComponent<AnimatorComponent>().Preview = true;
		scene.UpdateAnimationPreview(Step);
		AnimationSystem& preview = *scene.GetPoseAnimationSystem();
		const auto bone = [&](const char* name) {
			glm::mat4 joint;
			std::string error;
			REQUIRE(preview.GetBoneTransform(biped, name, joint, error));
			return glm::vec3(joint[3]);
		};
		const glm::vec3 shoulder = bone("RightUpperArm");
		const auto addShelf = [&](const char* name, Entity parent) {
			auto shelf = scene.CreateEntity(name);
			if (parent)
				scene.SetParent(shelf, parent, false);
			shelf.GetTransform().Translation = shoulder + glm::vec3(0, -0.4f, 0);
			shelf.AddComponent<RigidBodyComponent>();
			shelf.AddComponent<BoxColliderComponent>().HalfExtents = { 3, 0.05f, 3 };
			return shelf;
		};
		// The controller above the Animator owns what is under it, in a preview too.
		auto owned = addShelf("Owned", character);
		for (int i = 0; i < 300; i++)
			scene.UpdateAnimationPreview(Step);
		CHECK(Near(bone("RightHand"), shoulder + glm::vec3(0, -0.6f, 0), 5e-3f));
		scene.DestroyEntity(owned);
		const json before = SceneSerializer::SerializeScene(scene);
		auto foreign = addShelf("Foreign", {});
		const json placed = SceneSerializer::SerializeScene(scene);
		CHECK(before != placed);
		// Reversed gravity lifts the arm above the shelf, and normal gravity lands it there.
		biped.GetComponent<SpringBoneComponent>().Chains[0].GravityFactor = -1;
		for (int i = 0; i < 300; i++)
			scene.UpdateAnimationPreview(Step);
		biped.GetComponent<SpringBoneComponent>().Chains[0].GravityFactor = 1;
		for (int i = 0; i < 300; i++)
			scene.UpdateAnimationPreview(Step);
		const float top = foreign.GetTransform().Translation.y + 0.05f;
		CHECK(bone("RightHand").y == doctest::Approx(top + 0.02f).epsilon(0.01));
		// Nothing physical came into being, and the scene is as the editor left it.
		CHECK(scene.GetPhysicsWorld() == nullptr);
		biped.GetComponent<SpringBoneComponent>().Chains[0].GravityFactor = 1;
		CHECK(SceneSerializer::SerializeScene(scene) == placed);
		scene.ResetAnimationPreview();
	}

	TEST_CASE("SpringBone validation, warnings and runtime problems name their cause once")
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
			{ { { "Chains", { { { "MaxAngle", -1 } } } } }, "Chains[0].MaxAngle -1 must be between 0 and 180" },
			{ { { "Chains", { { { "MaxAngle", 181 } } } } }, "Chains[0].MaxAngle 181 must be between 0 and 180" },
			{ { { "Chains", { { { "Colliders", json::array({ { { "Name", 3 } } }) } } } } }, "field 'Chains[0].Colliders[0].Name' must be a string" },
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
		SpringChain empty, missing = RightArm(), leaf = RightArm(), overlap = RightArm(), branching = RightArm();
		empty.Name = "Empty";
		missing.Name = "Missing";
		missing.Joint = "Nope";
		leaf.Name = "Leaf";
		leaf.Joint = "RightHand";
		overlap.Name = "Overlap";
		overlap.Joint = "RightForeArm";
		branching.Name = "Branching";
		branching.Joint = "Chest";
		component.Chains.insert(component.Chains.end(), { empty, missing, leaf, overlap, branching });
		component.Colliders = { { .Joint = "Nowhere" }, { .Joint = "" } };
		const auto warnings = GetAnimationWarnings(biped);
		const auto has = [&](const std::string& text) { return std::ranges::any_of(warnings, [&](const std::string& warning) { return warning.find(text) != std::string::npos; }); };
		CAPTURE(warnings);
		CHECK(has("SpringBone chain 'Empty': Joint is empty"));
		CHECK(has("SpringBone chain 'Missing': joint 'Nope' not found"));
		CHECK(has("SpringBone chain 'Leaf': joint 'RightHand' has no child to swing toward"));
		CHECK(has("SpringBone chains 'RightArm' and 'Overlap' both turn joint 'RightForeArm'; the later one wins"));
		CHECK(has("SpringBone chain 'Branching': joint 'Chest' has 3 children; the chain follows the first, 'Head'"));
		CHECK(has("SpringBone Colliders[0]: joint 'Nowhere' not found"));
		CHECK(warnings.size() == 6);

		// At runtime each problem is logged once, and the valid chain still swings (with its Damping, written as infinity
		// from C++, clamped to 1000).
		scene.CreateEntity("Plain");
		const auto since = Log::GetHistory().GetTotalCount();
		scene.OnRuntimeStart();
		const glm::vec3 animated = BoneWorld(scene, biped, "RightHand");
		component.Chains[0].Damping = std::numeric_limits<float>::infinity();
		component.Chains[0].MaxAngle = std::numeric_limits<float>::infinity();
		RunAnimation(scene, 30);
		CHECK(CountLogMessages(since, "MaxAngle must be between 0 and 180; clamping") == 1);
		CHECK(CountLogMessages(since, "chain 'Missing': joint 'Nope' not found") == 1);
		CHECK(CountLogMessages(since, "chain 'Leaf': joint 'RightHand' has no child") == 1);
		CHECK(CountLogMessages(since, "Colliders[0]: joint 'Nowhere' not found") == 1);
		CHECK(CountLogMessages(since, "chain 'RightArm': Damping must be between 0 and 1000; clamping") == 1);
		CHECK(CountLogMessages(since, "SpringBone 'Rig': not solved") == 1);
		CHECK(glm::length(BoneWorld(scene, biped, "RightHand") - animated) > 0.05f);
		CheckArmLengths(scene, biped);

		const std::pair<const char*, const char*> errors[] = {
			{ "Scene.FindEntityByName('Plain'):ResetSpringBones()", "ResetSpringBones needs the SpringBone component on 'Plain'" },
		};
		CheckLuaErrors(*scene.GetScriptEngine(), errors);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Spring bones skip zero-scale entities quietly and ignore non-finite wind and gravity once")
	{
		ClearAssetCache clear;
		Scene scene;
		auto biped = AddBiped(scene, { RightArm() });
		scene.OnRuntimeStart();
		const glm::vec3 animated = ArmReach(scene, biped);

		// Growing in from a zero scale: the arm shows the animation, nothing is logged, and it swings from the animated
		// pose once the entity has a size again.
		const auto quiet = Log::GetHistory().GetTotalCount();
		biped.GetTransform().Scale = glm::vec3(0);
		RunAnimation(scene, 5);
		CHECK(CountLogMessages(quiet, "SpringBone 'Biped'") == 0);
		biped.GetTransform().Scale = glm::vec3(1);
		RunAnimation(scene, 1);
		CHECK(Near(ArmReach(scene, biped), animated));
		RunAnimation(scene, 60);
		CHECK(ArmReach(scene, biped).y < animated.y - 0.1f);

		// Non-finite wind and gravity, both written from C++ (Lua's Physics.SetGravity rejects them), are ignored, each
		// logged once, and the bones stay finite.
		const auto since = Log::GetHistory().GetTotalCount();
		biped.GetComponent<SpringBoneComponent>().Wind = glm::vec3(std::numeric_limits<float>::quiet_NaN(), 0, 0);
		scene.GetPhysicsWorld()->SetGravity(glm::vec3(0, std::numeric_limits<float>::infinity(), 0));
		RunAnimation(scene, 10);
		CHECK(CountLogMessages(since, "Wind is not finite; ignoring it") == 1);
		CHECK(CountLogMessages(since, "the physics gravity is not finite; ignoring it") == 1);
		CHECK(Math::IsFinite(BoneWorld(scene, biped, "RightHand")));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Spring colliders and chains resolve again when their joints or the model change")
	{
		ClearAssetCache clear;
		Scene scene;
		const std::string armature = (std::filesystem::path(BRECCIA_TEST_DATA_DIR) / "Animation/BipedArmature.gltf").string();
		auto biped = AddBiped(scene, { RightArm() });
		// The other model's animated pose, without springs.
		auto reference = scene.CreateEntity("Reference");
		auto& referenceAnimator = reference.AddComponent<AnimatorComponent>();
		referenceAnimator.Mesh = armature;
		referenceAnimator.Clip = "Lean";
		referenceAnimator.PlayOnStart = false;
		scene.OnRuntimeStart();
		const glm::vec3 shoulder = BoneWorld(scene, biped, "RightUpperArm");
		const glm::vec3 center = shoulder + glm::vec3(0, -0.3f, 0);
		const glm::vec3 offset = glm::vec3(glm::inverse(BoneWorldTransform(scene, biped, "Chest")) * glm::vec4(center, 1));
		auto& springs = biped.GetComponent<SpringBoneComponent>();
		springs.Colliders = { { .Joint = "Chest", .Offset = offset, .Radius = 0.1f } };
		RunAnimation(scene, 300);
		REQUIRE(glm::length(BoneWorld(scene, biped, "RightForeArm") - center) >= 0.12f - 1e-3f);

		// Renaming the collider's joint resolves it again: unknown, the arm hangs straight through where it was.
		const auto since = Log::GetHistory().GetTotalCount();
		springs.Colliders[0].Joint = "Nowhere";
		RunAnimation(scene, 300);
		CHECK(CountLogMessages(since, "Colliders[0]: joint 'Nowhere' not found") == 1);
		CHECK(Near(BoneWorld(scene, biped, "RightForeArm"), center, 5e-3f));
		springs.Colliders[0].Joint = "Chest";
		RunAnimation(scene, 300);
		CHECK(glm::length(BoneWorld(scene, biped, "RightForeArm") - center) >= 0.12f - 1e-3f);
		// A collider added after the others resolved.
		springs.Colliders.push_back({ .Joint = "Head", .Radius = 0.05f });
		RunAnimation(scene, 1);
		CHECK(Math::IsFinite(BoneWorld(scene, biped, "RightHand")));

		// Another model with the same joints at other indices: the chain and the collider resolve on it, the chain
		// restarts on its animated pose, and no other joint swings.
		biped.GetComponent<AnimatorComponent>().Mesh = armature;
		RunAnimation(scene, 1);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), BoneWorld(scene, reference, "RightHand")));
		CHECK(Near(BoneWorld(scene, biped, "LeftHand"), BoneWorld(scene, reference, "LeftHand")));
		RunAnimation(scene, 300);
		const glm::vec3 armatureShoulder = BoneWorld(scene, biped, "RightUpperArm");
		const glm::vec3 armatureCenter = glm::vec3(BoneWorldTransform(scene, biped, "Chest") * glm::vec4(offset, 1));
		const float scale = glm::length(BoneWorld(scene, reference, "RightForeArm") - BoneWorld(scene, reference, "RightUpperArm")) / 0.3f;
		CHECK(glm::length(BoneWorld(scene, biped, "RightForeArm") - armatureCenter) >= (0.1f + 0.02f) * scale - 1e-3f);
		CHECK(glm::length(BoneWorld(scene, biped, "RightForeArm") - armatureShoulder) == doctest::Approx(0.3f * scale).epsilon(1e-3));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Spring chains stop at MaxChainLength joints and warn about zero-length bones")
	{
		// A 20-joint line: a ChainLength 0 chain takes the first 16 joints and the 17th as its tip, and simulates them.
		MeshSource line;
		line.Nodes.resize(SpringChain::MaxChainLength + 4);
		for (size_t i = 0; i < line.Nodes.size(); i++)
		{
			line.Nodes[i].Name = "J" + std::to_string(i);
			line.Nodes[i].Parent = static_cast<int32_t>(i) - 1;
			line.Nodes[i].Translation = i == 0 ? glm::vec3(0) : glm::vec3(0, 0.1f, 0);
		}
		REQUIRE(line.FinalizeHierarchy());
		SpringChain chain;
		chain.Joint = "J0";
		std::vector<uint32_t> joints;
		CHECK(ResolveSpringChain(line, chain, joints).empty());
		CHECK(joints.size() == SpringChain::MaxChainLength + 1);
		CHECK(FirstChild(line, joints.back()) >= 0);
		Strand strand({ 0, 0, 0 }, { 1, 0, 0 }, SpringChain::MaxChainLength, 0.1f);
		strand.Run(60, Step, { .Stiffness = 10, .Damping = 4, .Acceleration = Gravity });
		for (const SpringPoint& point : strand.Points)
			CHECK(Math::IsFinite(point.Position));

		// The same lines as models: the inspector warns that the chain stops early, and about bones of zero length.
		ClearAssetCache clear;
		TempProject project("SpringLines");
		Scene scene;
		const auto add = [&](const char* name, size_t count, float spacing) {
			auto entity = scene.CreateEntity(name);
			auto& animator = entity.AddComponent<AnimatorComponent>();
			animator.Mesh = project.WriteFile(fmt::format("Assets/Models/{}.gltf", name), LineModel(count, spacing));
			SpringChain springChain;
			springChain.Joint = "J0";
			entity.AddComponent<SpringBoneComponent>().Chains = { springChain };
			return GetAnimationWarnings(entity);
		};
		CHECK(add("Long", SpringChain::MaxChainLength + 4, 0.1f) == std::vector<std::string>{ "SpringBone chain 'Chain': the joints go on past the first 16; the rest follow the animation" });
		CHECK(add("Flat", 3, 0) == std::vector<std::string>{ "SpringBone chain 'Chain': its bones have zero length in the model's rest pose, so nothing can swing" });
		CHECK(add("Fine", 3, 0.1f).empty());
	}

	TEST_CASE("Spring settings written as NaN or infinity from C++ fall back to their defaults or bounds")
	{
		ClearAssetCache clear;
		Scene scene;
		SpringChain chain = RightArm();
		auto biped = AddBiped(scene, { chain });
		scene.OnRuntimeStart();
		const glm::vec3 shoulder = BoneWorld(scene, biped, "RightUpperArm");
		const auto since = Log::GetHistory().GetTotalCount();
		// A NaN GravityFactor is the default 1: the arm hangs down (the lower bound, -100, would fling it up).
		auto& config = biped.GetComponent<SpringBoneComponent>().Chains[0];
		config.GravityFactor = std::numeric_limits<float>::quiet_NaN();
		RunAnimation(scene, 300);
		CHECK(CountLogMessages(since, "chain 'RightArm': GravityFactor is not a number; using 1") == 1);
		CHECK(Near(BoneWorld(scene, biped, "RightHand"), shoulder + glm::vec3(0, -0.6f, 0), 1e-2f));
		// An infinite Damping is its upper bound (an infinite one would read as 0 if it fell to the lower bound).
		config.GravityFactor = 1;
		config.Damping = std::numeric_limits<float>::infinity();
		RunAnimation(scene, 2);
		CHECK(CountLogMessages(since, "chain 'RightArm': Damping must be between 0 and 1000; clamping") == 1);

		// Gravity and wind finite on their own but overflowing together are ignored, logged once.
		config.GravityFactor = 1;
		config.Damping = 4;
		biped.GetComponent<SpringBoneComponent>().Wind = { 3e38f, 0, 0 };
		scene.GetPhysicsWorld()->SetGravity({ 3e38f, 0, 0 });
		const auto overflow = Log::GetHistory().GetTotalCount();
		RunAnimation(scene, 30);
		CHECK(CountLogMessages(overflow, "gravity times GravityFactor plus Wind is not finite; ignoring it") == 1);
		CHECK(Math::IsFinite(BoneWorld(scene, biped, "RightHand")));
		scene.OnRuntimeStop();
	}
}

TEST_SUITE("Animation")
{
	TEST_CASE("An edit ray sees a body only through a mask that holds its layer")
	{
		Scene scene;
		PhysicsLayers layers;
		std::string error;
		REQUIRE(layers.Add("A", error));
		REQUIRE(layers.Add("B", error));
		scene.SetPhysicsLayers(layers);
		auto box = scene.CreateEntity("Box");
		box.AddComponent<RigidBodyComponent>().Layer = "B";
		box.AddComponent<BoxColliderComponent>();
		StaticPhysicsShapes shapes(scene);
		const uint32_t own = 1u << *shapes.GetLayers().Find("B"), other = 1u << *shapes.GetLayers().Find("A");
		const auto hits = [&](uint32_t mask) { return shapes.RaycastAll(glm::vec3(0, 5, 0), glm::vec3(0, -1, 0), 10, { .LayerMask = mask }).size(); };
		CHECK(hits(own) == 1);
		CHECK(hits(other) == 0);
	}
}
