#include <doctest/doctest.h>

#include "AnimationTestUtils.h"
#include "TestUtils.h"

#include <Basalt/Animation/AnimatedModel.h>
#include <Basalt/Animation/AnimationPose.h>
#include <Basalt/Animation/AnimationSystem.h>
#include <Basalt/Animation/InverseKinematics.h>
#include <Basalt/Animation/SpringBones.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Math/Math.h>
#include <Basalt/Physics/PhysicsWorld.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/SceneSerializer.h>
#include <Basalt/Scripting/ScriptEngine.h>

#include <nlohmann/json.hpp>

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <limits>
#include <optional>
#include <tuple>

using namespace Basalt;
using namespace BasaltTest;
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
		// Naming Belly holds the arm off it, while the unnamed one is still ignored.
		component.Chains[0].Colliders = { { .Name = "Belly" } };
		RunAnimation(scene, 300);
		CHECK_FALSE(hangsStraight());
		const glm::vec3 elbow = BoneWorld(scene, biped, "RightForeArm");
		CHECK(glm::length(elbow - (shoulder + glm::vec3(0, -0.3f, 0))) >= 0.1f + component.Chains[0].Radius - 1e-3f);
		// Without a list the chain collides with every collider.
		component.Chains[0].Colliders.clear();
		RunAnimation(scene, 300);
		CHECK_FALSE(hangsStraight());
		// Several colliders may share a name, which then names all of them.
		component.Colliders[1].Name = "Belly";
		component.Chains[0].Colliders = { { .Name = "Belly" } };
		RunAnimation(scene, 300);
		CHECK_FALSE(hangsStraight());

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
		const auto run = [](bool collide, bool ownBodies) {
			Scene scene;
			auto chain = RightArm();
			chain.CollidePhysics = collide;
			auto biped = AddBiped(scene, { chain });
			auto ground = scene.CreateEntity("Ground");
			ground.GetTransform().Translation = { 0, -0.5f, 0 };
			ground.AddComponent<RigidBodyComponent>();
			ground.AddComponent<BoxColliderComponent>().HalfExtents = { 5, 0.5f, 5 };
			// The character's own colliders cover the arm's path: they never push it.
			if (ownBodies)
			{
				biped.AddComponent<CharacterControllerComponent>();
				biped.AddComponent<CapsuleColliderComponent>();
				auto hitbox = scene.CreateEntity("Hitbox");
				scene.SetParent(hitbox, biped, false);
				hitbox.GetTransform().Translation = { -0.25f, 1.1f, 0 };
				hitbox.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
				hitbox.AddComponent<BoxColliderComponent>().HalfExtents = { 0.3f, 0.3f, 0.3f };
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
			auto& triggerBody = trigger.AddComponent<RigidBodyComponent>();
			triggerBody.IsTrigger = true;
			trigger.AddComponent<BoxColliderComponent>().HalfExtents = { 0.5f, 0.05f, 0.5f };
			for (int i = 0; i < 300; i++)
				scene.OnUpdate(Step);
			const glm::vec3 hand = BoneWorld(scene, biped, "RightHand"), elbow = BoneWorld(scene, biped, "RightForeArm");
			scene.OnSimulationStop();
			return std::tuple{ shoulder, elbow, hand };
		};
		// Off: the arm hangs straight through the shelf.
		const auto [shoulder, elbow, hand] = run(false, false);
		CHECK(Near(hand, shoulder + glm::vec3(0, -0.6f, 0), 5e-3f));
		// On: the hand rests on the shelf's top (0.35 m below the shoulder, plus the point radius), the trigger aside.
		for (const bool own : { false, true })
		{
			CAPTURE(own);
			const auto [collidedShoulder, collidedElbow, collidedHand] = run(true, own);
			CHECK(collidedHand.y >= collidedShoulder.y - 0.35f + 0.02f - 2e-3f);
			CHECK(collidedElbow.y >= collidedShoulder.y - 0.35f + 0.02f - 2e-3f);
			CHECK(glm::length(collidedHand - collidedElbow) == doctest::Approx(0.3f).epsilon(1e-3));
		}
	}

	TEST_CASE("Physics shape snapshots gather the nearest bodies and push spheres out of them")
	{
		Scene scene;
		const auto addBox = [&](const std::string& name, const glm::vec3& center, bool trigger = false) {
			auto box = scene.CreateEntity(name);
			box.GetTransform().Translation = center;
			box.AddComponent<RigidBodyComponent>().IsTrigger = trigger;
			box.AddComponent<BoxColliderComponent>().HalfExtents = { 1, 1, 1 };
			return box;
		};
		auto floor = addBox("Floor", { 0, -1, 0 });
		auto sensor = addBox("Sensor", { 0, 3, 0 }, true);
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
		CHECK(physics.CollectShapes({ -0.5f, -0.5f, -0.5f }, { 0.5f, 3, 0.5f }, 0.1f, { .IgnoreEntity = floor.GetUUID() }).Empty());
		CHECK(physics.CollectShapes({ -0.5f, -0.5f, -0.5f }, { 0.5f, 3, 0.5f }, 0.1f, { .LayerMask = 0 }).Empty());
		// Bad input gathers nothing.
		CHECK(physics.CollectShapes({ 1, 1, 1 }, { -1, -1, -1 }, 0.1f).Empty());
		CHECK(physics.CollectShapes({ -1, -1, -1 }, { 1, std::numeric_limits<float>::infinity(), 1 }, 0.1f).Empty());
		CHECK(physics.CollectShapes({ -1, -1, -1 }, { 1, 1, 1 }, -1).Empty());
		CHECK(PhysicsShapeSnapshot().Empty());
		CHECK_FALSE(PhysicsShapeSnapshot().PushOut({ 0, 0, 0 }));
		(void)sensor;
		scene.OnSimulationStop();

		// Past MaxShapes, the nearest are kept.
		Scene crowded;
		for (uint32_t i = 0; i < PhysicsShapeSnapshot::MaxShapes + 4; i++)
		{
			auto box = crowded.CreateEntity("Box");
			box.GetTransform().Translation = { 3.0f * static_cast<float>(i), 0, 0 };
			box.AddComponent<RigidBodyComponent>();
			box.AddComponent<BoxColliderComponent>().HalfExtents = { 1, 1, 1 };
		}
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
		// Physics bodies wait for Play: a shelf in the way changes nothing.
		auto shelf = scene.CreateEntity("Shelf");
		shelf.AddComponent<RigidBodyComponent>();
		shelf.AddComponent<BoxColliderComponent>().HalfExtents = { 2, 0.05f, 2 };
		const auto since = Log::GetHistory().GetTotalCount();
		scene.UpdateAnimationPreview(Step);
		AnimationSystem& preview = *scene.GetPoseAnimationSystem();
		const auto handWorld = [&](Entity entity) {
			glm::mat4 hand, shoulder;
			std::string error;
			REQUIRE(preview.GetBoneTransform(entity, "RightHand", hand, error));
			REQUIRE(preview.GetBoneTransform(entity, "RightUpperArm", shoulder, error));
			return std::pair{ glm::vec3(scene.GetWorldTransform(entity) * hand[3]), glm::vec3(scene.GetWorldTransform(entity) * shoulder[3]) };
		};
		shelf.GetTransform().Translation = handWorld(biped).second + glm::vec3(0, -0.4f, 0);
		const json before = SceneSerializer::SerializeScene(scene);
		for (int i = 0; i < 300; i++)
			scene.UpdateAnimationPreview(Step);
		const auto [hand, shoulder] = handWorld(biped);
		CHECK(Near(hand, shoulder + glm::vec3(0, -0.6f, 0), 5e-3f));
		CHECK(preview.GetPose(still) == nullptr);
		CHECK(CountLogMessages(since, "SpringBone") == 0);
		CHECK(SceneSerializer::SerializeScene(scene) == before);
		// Moving the entity in the editor swings the chain.
		biped.GetTransform().Translation.z += 0.2f;
		scene.UpdateAnimationPreview(Step);
		CHECK(handWorld(biped).first.z < handWorld(biped).second.z - 0.01f);
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
		RunAnimation(scene, 30);
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
		const std::string armature = (std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/BipedArmature.gltf").string();
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
