#include <doctest/doctest.h>

#include <Basalt/Physics/PhysicsWorld.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

#include <cmath>

using namespace Basalt;

namespace {

	constexpr float Step = 1.0f / 60.0f;

	Entity CreateGround(Scene& scene)
	{
		Entity ground = scene.CreateEntity("Ground");
		ground.GetTransform().Translation = { 0.0f, -0.5f, 0.0f };
		ground.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		ground.AddComponent<BoxColliderComponent>().HalfExtents = { 50.0f, 0.5f, 50.0f };
		return ground;
	}

	Entity CreateBox(Scene& scene, const glm::vec3& position)
	{
		Entity box = scene.CreateEntity("Box");
		box.GetTransform().Translation = position;
		box.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		box.AddComponent<BoxColliderComponent>();
		return box;
	}

	void Simulate(Scene& scene, float seconds)
	{
		const int frames = static_cast<int>(std::lround(seconds / Step));
		for (int i = 0; i < frames; i++)
			scene.OnUpdate(Step);
	}

}

TEST_SUITE("Physics")
{
	TEST_CASE("A dynamic box falls under gravity and rests on static ground")
	{
		Scene scene;
		CreateGround(scene);
		Entity box = CreateBox(scene, { 0.0f, 5.0f, 0.0f });

		scene.OnSimulationStart();
		REQUIRE(scene.GetPhysicsWorld());
		CHECK(scene.GetPhysicsWorld()->GetBodyCount() == 2);

		Simulate(scene, 0.5f);
		const float afterHalfSecond = box.GetTransform().Translation.y;
		// Free fall: 5 - 0.5 * 9.81 * 0.25 ~= 3.77
		CHECK(afterHalfSecond == doctest::Approx(3.77f).epsilon(0.03));

		Simulate(scene, 3.0f);
		CHECK(box.GetTransform().Translation.y == doctest::Approx(0.5f).epsilon(0.02));
		CHECK(glm::length(scene.GetPhysicsWorld()->GetLinearVelocity(box)) < 0.05f);
		scene.OnSimulationStop();
		CHECK(scene.GetPhysicsWorld() == nullptr);
	}

	TEST_CASE("Collision masks let bodies pass through each other")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		ground.GetComponent<RigidBodyComponent>().Layer = 1;
		Entity ghost = CreateBox(scene, { 0.0f, 2.0f, 0.0f });
		ghost.GetComponent<RigidBodyComponent>().CollisionMask = ~(1u << 1);

		scene.OnSimulationStart();
		Simulate(scene, 2.0f);
		CHECK(ghost.GetTransform().Translation.y < -5.0f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Raycasts hit the closest non-trigger body")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		Entity trigger = scene.CreateEntity("Trigger");
		trigger.GetTransform().Translation = { 0.0f, 3.0f, 0.0f };
		trigger.AddComponent<RigidBodyComponent>().IsTrigger = true;
		trigger.AddComponent<BoxColliderComponent>();

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();

		const auto hit = physics.Raycast({ 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f);
		REQUIRE(hit.has_value());
		CHECK(hit->EntityID == ground.GetUUID());
		CHECK(hit->Distance == doctest::Approx(10.0f).epsilon(0.01));
		CHECK(hit->Normal.y == doctest::Approx(1.0f).epsilon(0.01));

		CHECK_FALSE(physics.Raycast({ 0.0f, 10.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 100.0f).has_value());
		CHECK_FALSE(physics.Raycast({ 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 5.0f).has_value());
		CHECK_FALSE(physics.Raycast({ 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f, ground.GetUUID()).has_value());
		scene.OnSimulationStop();
	}

	TEST_CASE("Velocities, impulses and teleporting through the transform")
	{
		Scene scene;
		Entity box = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
		box.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;
		box.GetComponent<RigidBodyComponent>().LinearDamping = 0.0f;

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		physics.SetLinearVelocity(box, { 2.0f, 0.0f, 0.0f });
		Simulate(scene, 1.0f);
		CHECK(box.GetTransform().Translation.x == doctest::Approx(2.0f).epsilon(0.02));

		// Mass 1: an impulse of 1 adds 1 m/s.
		physics.AddImpulse(box, { 0.0f, 0.0f, 1.0f });
		CHECK(physics.GetLinearVelocity(box).z == doctest::Approx(1.0f).epsilon(0.01));

		// Moving the entity teleports the body.
		box.GetTransform().Translation = { 100.0f, 0.0f, 0.0f };
		scene.OnUpdate(Step);
		CHECK(box.GetTransform().Translation.x == doctest::Approx(100.0f + 2.0f * Step).epsilon(0.01));
		scene.OnSimulationStop();
	}

	TEST_CASE("Kinematic bodies follow their transform and push dynamic bodies")
	{
		Scene scene;
		Entity pusher = scene.CreateEntity("Pusher");
		pusher.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		pusher.AddComponent<BoxColliderComponent>();
		Entity box = CreateBox(scene, { 1.5f, 0.0f, 0.0f });
		box.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;

		scene.OnSimulationStart();
		for (int i = 0; i < 60; i++)
		{
			pusher.GetTransform().Translation.x += 2.0f * Step;
			scene.OnUpdate(Step);
		}
		CHECK(pusher.GetTransform().Translation.x == doctest::Approx(2.0f));
		CHECK(box.GetTransform().Translation.x > 2.5f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Bodies are created and removed when components change at runtime")
	{
		Scene scene;
		CreateGround(scene);
		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		CHECK(physics.GetBodyCount() == 1);

		Entity box = CreateBox(scene, { 0.0f, 3.0f, 0.0f });
		CHECK_FALSE(physics.HasBody(box));
		scene.OnUpdate(Step);
		CHECK(physics.HasBody(box));

		// Without a collider there is no body.
		Entity noCollider = scene.CreateEntity("NoCollider");
		noCollider.AddComponent<RigidBodyComponent>();
		scene.OnUpdate(Step);
		CHECK_FALSE(physics.HasBody(noCollider));

		box.RemoveComponent<BoxColliderComponent>();
		scene.OnUpdate(Step);
		CHECK_FALSE(physics.HasBody(box));

		Entity sphere = scene.CreateEntity("Sphere");
		sphere.AddComponent<RigidBodyComponent>();
		sphere.AddComponent<SphereColliderComponent>();
		sphere.AddComponent<CapsuleColliderComponent>();
		scene.OnUpdate(Step);
		CHECK(physics.HasBody(sphere));
		scene.DestroyEntity(sphere);
		CHECK(physics.GetBodyCount() == 1);
		scene.OnSimulationStop();
	}

	TEST_CASE("Simulation is deterministic")
	{
		auto run = []() {
			Scene scene;
			CreateGround(scene);
			std::vector<Entity> boxes;
			boxes.reserve(5);
			for (int i = 0; i < 5; i++)
				boxes.push_back(CreateBox(scene, { 0.3f * i, 2.0f + 1.1f * i, 0.1f * i }));
			scene.OnSimulationStart();
			Simulate(scene, 2.0f);
			std::vector<glm::vec3> positions;
			positions.reserve(boxes.size());
			for (Entity box : boxes)
				positions.push_back(box.GetTransform().Translation);
			scene.OnSimulationStop();
			return positions;
		};
		CHECK(run() == run());
	}

	TEST_CASE("Hinge joints swing around their axis and respect limits")
	{
		Scene scene;
		// A pendulum: the box's centre is 1 m from the hinge, which is pinned to the world at the origin.
		Entity pendulum = CreateBox(scene, { 1.0f, 0.0f, 0.0f });
		auto& hinge = pendulum.AddComponent<JointComponent>();
		hinge.Type = JointType::Hinge;
		hinge.Anchor = { -1.0f, 0.0f, 0.0f };
		hinge.Axis = { 0.0f, 0.0f, 1.0f };

		Entity limited = CreateBox(scene, { 11.0f, 0.0f, 0.0f });
		auto& limitedHinge = limited.AddComponent<JointComponent>(hinge);
		limitedHinge.UseLimits = true;
		limitedHinge.LimitMin = -30.0f;
		limitedHinge.LimitMax = 30.0f;

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		REQUIRE(physics.HasJoint(pendulum));
		REQUIRE(physics.HasJoint(limited));
		CHECK(physics.GetJointPosition(pendulum).value() == doctest::Approx(0.0f).epsilon(0.001));

		float lowest = 0.0f;
		float maxLimitedAngle = 0.0f;
		for (int i = 0; i < 120; i++)
		{
			scene.OnUpdate(Step);
			const glm::vec3 position = pendulum.GetTransform().Translation;
			CHECK(glm::length(position) == doctest::Approx(1.0f).epsilon(0.02));
			CHECK(position.z == doctest::Approx(0.0f).epsilon(0.001));
			lowest = std::min(lowest, position.y);
			maxLimitedAngle = std::max(maxLimitedAngle, std::abs(physics.GetJointPosition(limited).value()));
		}
		CHECK(lowest < -0.95f);
		CHECK(maxLimitedAngle == doctest::Approx(30.0f).epsilon(0.05));
		CHECK(std::abs(physics.GetJointPosition(pendulum).value()) > 45.0f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Slider joints move only along their axis")
	{
		Scene scene;
		Entity slider = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
		auto& joint = slider.AddComponent<JointComponent>();
		joint.Type = JointType::Slider;
		joint.Axis = { 1.0f, 0.0f, 0.0f };
		joint.UseLimits = true;
		joint.LimitMin = -1.0f;
		joint.LimitMax = 2.0f;

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		// Gravity pulls down and the push has a sideways part; only the X part may move the body.
		physics.SetLinearVelocity(slider, { 1.0f, 0.0f, 1.0f });
		Simulate(scene, 1.0f);
		const glm::vec3 position = slider.GetTransform().Translation;
		CHECK(position.x == doctest::Approx(1.0f).epsilon(0.05));
		CHECK(std::abs(position.y) < 0.01f);
		CHECK(std::abs(position.z) < 0.01f);
		CHECK(physics.GetJointPosition(slider).value() == doctest::Approx(1.0f).epsilon(0.05));

		physics.SetLinearVelocity(slider, { 5.0f, 0.0f, 0.0f });
		Simulate(scene, 1.0f);
		CHECK(slider.GetTransform().Translation.x == doctest::Approx(2.0f).epsilon(0.02));
		scene.OnSimulationStop();
	}

	TEST_CASE("Point, distance and fixed joints hold their bodies together")
	{
		Scene scene;
		Entity ceiling = CreateGround(scene);
		ceiling.GetTransform().Translation = { 0.0f, 10.0f, 0.0f };

		// Ball-and-socket hanging from the world at (0, 5, 0), starting sideways.
		Entity ball = CreateBox(scene, { 2.0f, 5.0f, 0.0f });
		auto& point = ball.AddComponent<JointComponent>();
		point.Type = JointType::Point;
		point.Anchor = { -2.0f, 0.0f, 0.0f };

		// Rope of length 3 below the static ceiling entity; starts slack, 1 m below its anchor.
		Entity weight = CreateBox(scene, { 10.0f, 8.0f, 0.0f });
		auto& rope = weight.AddComponent<JointComponent>();
		rope.Type = JointType::Distance;
		rope.ConnectedEntity = ceiling.GetUUID();
		rope.ConnectedAnchor = { 10.0f, -1.0f, 0.0f }; // ceiling local space: world (10, 9, 0)
		rope.UseLimits = true;
		rope.LimitMin = 0.0f;
		rope.LimitMax = 3.0f;

		// Two weightless boxes welded 2 m apart; only the first one is pushed.
		Entity first = CreateBox(scene, { -10.0f, 0.0f, 0.0f });
		Entity second = CreateBox(scene, { -10.0f, 0.0f, 2.0f });
		first.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;
		second.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;
		auto& weld = second.AddComponent<JointComponent>();
		weld.Type = JointType::Fixed;
		weld.ConnectedEntity = first.GetUUID();

		scene.OnSimulationStart();
		scene.GetPhysicsWorld()->SetLinearVelocity(first, { 0.0f, 1.0f, 0.0f });
		for (int i = 0; i < 120; i++)
		{
			scene.OnUpdate(Step);
			CHECK(glm::distance(ball.GetTransform().Translation, glm::vec3(0.0f, 5.0f, 0.0f)) == doctest::Approx(2.0f).epsilon(0.02));
			// The rope may stretch briefly when it snaps taut; Jolt corrects position error over a few steps.
			CHECK(glm::distance(weight.GetTransform().Translation, glm::vec3(10.0f, 9.0f, 0.0f)) <= 3.15f);
			CHECK(glm::distance(first.GetTransform().Translation, second.GetTransform().Translation) == doctest::Approx(2.0f).epsilon(0.01));
		}
		CHECK(weight.GetTransform().Translation.y == doctest::Approx(6.0f).epsilon(0.02));
		// The push is off-centre for the welded pair, so it tumbles; its centre still rises at about 0.5 m/s.
		CHECK((first.GetTransform().Translation.y + second.GetTransform().Translation.y) * 0.5f == doctest::Approx(0.95f).epsilon(0.1));
		CHECK(scene.GetPhysicsWorld()->GetJointPosition(ball) == std::nullopt);
		scene.OnSimulationStop();
	}

	TEST_CASE("Joints follow body rebuilds, component changes and destruction")
	{
		Scene scene;
		Entity frame = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
		frame.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		Entity door = CreateBox(scene, { 1.0f, 0.0f, 0.0f });
		door.AddComponent<JointComponent>().ConnectedEntity = frame.GetUUID();
		door.GetComponent<JointComponent>().Anchor = { -0.5f, 0.0f, 0.0f };

		// Joints on static-only pairs, without a body, or to missing entities are skipped with a warning.
		Entity wall = CreateGround(scene);
		wall.GetTransform().Translation.y = -50.0f;
		wall.AddComponent<JointComponent>().ConnectedEntity = frame.GetUUID();
		Entity noBody = scene.CreateEntity("NoBody");
		noBody.AddComponent<JointComponent>();
		Entity dangling = CreateBox(scene, { 5.0f, 0.0f, 0.0f });
		dangling.AddComponent<JointComponent>().ConnectedEntity = 12345;

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		CHECK(physics.HasJoint(door));
		CHECK_FALSE(physics.HasJoint(wall));
		CHECK_FALSE(physics.HasJoint(noBody));
		CHECK_FALSE(physics.HasJoint(dangling));

		// Rebuilding either body rebuilds the joint.
		door.AddOrReplaceComponent<BoxColliderComponent>(BoxColliderComponent{ { 0.4f, 0.4f, 0.4f } });
		scene.OnUpdate(Step);
		CHECK(physics.HasJoint(door));
		frame.AddOrReplaceComponent<BoxColliderComponent>(BoxColliderComponent{ { 0.4f, 0.4f, 0.4f } });
		scene.OnUpdate(Step);
		CHECK(physics.HasJoint(door));

		door.RemoveComponent<JointComponent>();
		CHECK_FALSE(physics.HasJoint(door));
		door.AddComponent<JointComponent>().ConnectedEntity = frame.GetUUID();
		scene.OnUpdate(Step);
		CHECK(physics.HasJoint(door));

		// Destroying the connected entity removes the joint before its body; the door falls.
		scene.DestroyEntity(frame);
		CHECK_FALSE(physics.HasJoint(door));
		Simulate(scene, 0.5f);
		CHECK_FALSE(physics.HasJoint(door));
		CHECK(door.GetTransform().Translation.y < -0.5f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Simulation with joints is deterministic")
	{
		auto run = []() {
			Scene scene;
			Entity previous;
			std::vector<Entity> links;
			for (int i = 0; i < 6; i++)
			{
				Entity link = CreateBox(scene, { 1.2f * static_cast<float>(i + 1), 0.0f, 0.0f });
				auto& joint = link.AddComponent<JointComponent>();
				joint.Type = JointType::Point;
				joint.Anchor = { -0.6f, 0.0f, 0.0f };
				joint.ConnectedEntity = previous ? previous.GetUUID() : UUID(0);
				links.push_back(link);
				previous = link;
			}
			scene.OnSimulationStart();
			Simulate(scene, 2.0f);
			std::vector<glm::vec3> positions;
			positions.reserve(links.size());
			for (Entity link : links)
				positions.push_back(link.GetTransform().Translation);
			scene.OnSimulationStop();
			return positions;
		};
		CHECK(run() == run());
	}
}
