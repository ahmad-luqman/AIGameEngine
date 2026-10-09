#include <doctest/doctest.h>

#include <Basalt/Core/Log.h>
#include <Basalt/Physics/PhysicsWorld.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

#include <cmath>
#include <functional>
#include <string>

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

	// Log messages containing text since the history's total count was `since`.
	int CountMessages(uint64_t since, const std::string& text)
	{
		uint64_t next = 0;
		int count = 0;
		for (const LogMessage& message : Log::GetHistory().GetMessagesSince(since, next))
		{
			if (message.Text.find(text) != std::string::npos)
				count++;
		}
		return count;
	}

	// Replaces the component through the registry, as SetComponent and component.set do.
	void SetJoint(Entity entity, const std::function<void(JointComponent&)>& change)
	{
		JointComponent joint = entity.GetComponent<JointComponent>();
		change(joint);
		entity.AddOrReplaceComponent<JointComponent>(joint);
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

		// Joints skipped for a missing body are built once that body exists.
		noBody.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		noBody.AddComponent<BoxColliderComponent>();
		scene.OnUpdate(Step);
		CHECK(physics.HasJoint(noBody));
		Entity late = scene.CreateEntityWithUUID(12345, "Late");
		late.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		late.AddComponent<BoxColliderComponent>();
		scene.OnUpdate(Step);
		CHECK(physics.HasJoint(dangling));

		// Destroying the joint's own entity removes its constraint; the connected body is unaffected.
		scene.DestroyEntity(dangling);
		Simulate(scene, 0.1f);
		CHECK(physics.HasBody(late));

		// Destroying the connected entity removes the joint before its body; the door falls.
		scene.DestroyEntity(frame);
		CHECK_FALSE(physics.HasJoint(door));
		Simulate(scene, 0.5f);
		CHECK_FALSE(physics.HasJoint(door));
		CHECK(door.GetTransform().Translation.y < -0.5f);
		scene.OnSimulationStop();
	}

	TEST_CASE("A joint to an entity that gets its body during play is built then")
	{
		// Regression: the joint was dropped for good when its connected body did not exist yet.
		Scene scene;
		Entity frame = scene.CreateEntity("Frame");
		Entity door = CreateBox(scene, { 1.0f, 0.0f, 0.0f });
		auto& hinge = door.AddComponent<JointComponent>();
		hinge.ConnectedEntity = frame.GetUUID();
		hinge.Anchor = { -0.5f, 0.0f, 0.0f };

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		CHECK_FALSE(physics.HasJoint(door));
		frame.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		frame.AddComponent<BoxColliderComponent>().HalfExtents = { 0.1f, 0.1f, 0.1f };
		scene.OnUpdate(Step);
		CHECK(physics.HasJoint(door));
		Simulate(scene, 0.5f);
		// The vertical hinge holds the door up.
		CHECK(std::abs(door.GetTransform().Translation.y) < 0.05f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Joint warnings are logged once, not on every update")
	{
		Scene scene;
		Entity ball = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
		auto& point = ball.AddComponent<JointComponent>();
		point.Type = JointType::Point;
		point.MotorMode = JointMotorMode::Velocity;
		Entity orphan = CreateBox(scene, { 5.0f, 0.0f, 0.0f });
		orphan.AddComponent<JointComponent>().ConnectedEntity = 999;

		const uint64_t before = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		for (int i = 0; i < 30; i++)
		{
			// Scripts set joints every frame: a changing motor target and an unchanged broken joint.
			SetJoint(ball, [i](JointComponent& joint) { joint.MotorTarget = static_cast<float>(i); });
			SetJoint(orphan, [](JointComponent&) {});
			scene.OnUpdate(Step);
		}
		CHECK(scene.GetPhysicsWorld()->HasJoint(ball));
		CHECK(CountMessages(before, "MotorMode is ignored") == 1);
		CHECK(CountMessages(before, "missing entity 999") == 1);

		// A different problem is reported again.
		SetJoint(orphan, [](JointComponent& joint) { joint.ConnectedEntity = 998; });
		scene.OnUpdate(Step);
		CHECK(CountMessages(before, "missing entity 998") == 1);
		scene.OnSimulationStop();
	}

	TEST_CASE("Invalid joint limits are clamped with a warning, at creation and during play")
	{
		Scene scene;
		// Pendulums pinned to the world at the origin, swinging down (negative angles) under gravity.
		auto pendulum = [&](float x, float min, float max) {
			Entity box = CreateBox(scene, { x + 1.0f, 0.0f, 0.0f });
			auto& hinge = box.AddComponent<JointComponent>();
			hinge.Anchor = { -1.0f, 0.0f, 0.0f };
			hinge.Axis = { 0.0f, 0.0f, 1.0f };
			hinge.UseLimits = true;
			hinge.LimitMin = min;
			hinge.LimitMax = max;
			return box;
		};
		Entity inverted = pendulum(0.0f, 30.0f, -30.0f);      // -> [0, 0]
		Entity excludesRest = pendulum(10.0f, 10.0f, 400.0f); // -> [0, 180]

		Entity slider = CreateBox(scene, { 20.0f, 0.0f, 0.0f });
		slider.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;
		auto& rail = slider.AddComponent<JointComponent>();
		rail.Type = JointType::Slider;
		rail.Axis = { 1.0f, 0.0f, 0.0f };
		rail.UseLimits = true;
		rail.LimitMin = 1.0f; // -> 0
		rail.LimitMax = 2.0f;

		// A rope whose LimitMax is below LimitMin becomes a rigid rod of length LimitMin.
		Entity weight = CreateBox(scene, { 30.0f, -1.0f, 0.0f });
		weight.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;
		auto& rope = weight.AddComponent<JointComponent>();
		rope.Type = JointType::Distance;
		rope.ConnectedAnchor = { 30.0f, 0.0f, 0.0f };
		rope.UseLimits = true;
		rope.LimitMin = 3.0f;
		rope.LimitMax = 1.0f;

		const uint64_t before = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		REQUIRE(physics.HasJoint(inverted));
		REQUIRE(physics.HasJoint(excludesRest));
		REQUIRE(physics.HasJoint(slider));
		REQUIRE(physics.HasJoint(weight));
		CHECK(CountMessages(before, "must satisfy -180") == 2);
		CHECK(CountMessages(before, "must satisfy LimitMin <= 0") == 1);
		CHECK(CountMessages(before, "must satisfy 0 <= LimitMin") == 1);

		physics.SetLinearVelocity(slider, { -2.0f, 0.0f, 0.0f });
		Simulate(scene, 1.0f);
		CHECK(std::abs(physics.GetJointPosition(inverted).value()) < 2.0f);
		CHECK(physics.GetJointPosition(excludesRest).value() > -2.0f);
		CHECK(physics.GetJointPosition(slider).value() > -0.05f);
		CHECK(glm::distance(weight.GetTransform().Translation, glm::vec3(30.0f, 0.0f, 0.0f)) == doctest::Approx(3.0f).epsilon(0.03));

		// Updating limits in play clamps and warns the same way; a zero-length rope is called out.
		const uint64_t during = Log::GetHistory().GetTotalCount();
		SetJoint(inverted, [](JointComponent& joint) { joint.LimitMax = -10.0f; });
		SetJoint(weight, [](JointComponent& joint) {
			joint.LimitMin = 0.0f;
			joint.LimitMax = 0.0f;
		});
		scene.OnUpdate(Step);
		CHECK(physics.HasJoint(inverted));
		CHECK(CountMessages(during, "must satisfy -180") == 1);
		CHECK(CountMessages(during, "pull the anchors together") == 1);
		scene.OnSimulationStop();
	}

	TEST_CASE("Joint settings changed during play take effect")
	{
		Scene scene;
		// Slider limits narrow in place; the rest pose stays where it was.
		Entity slider = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
		slider.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;
		auto& rail = slider.AddComponent<JointComponent>();
		rail.Type = JointType::Slider;
		rail.Axis = { 1.0f, 0.0f, 0.0f };
		rail.UseLimits = true;
		rail.LimitMin = -1.0f;
		rail.LimitMax = 2.0f;

		// A pendulum that gets limits (a structural change) once it has swung.
		Entity pendulum = CreateBox(scene, { 11.0f, 0.0f, 0.0f });
		auto& hinge = pendulum.AddComponent<JointComponent>();
		hinge.Anchor = { -1.0f, 0.0f, 0.0f };
		hinge.Axis = { 0.0f, 0.0f, 1.0f };

		// A box held up by a point joint until it is given a break force.
		Entity hanging = CreateBox(scene, { 20.0f, 0.0f, 0.0f });
		hanging.AddComponent<JointComponent>().Type = JointType::Point;

		// A box resting on a plate through a vertical slider that collides with it, until it does not.
		Entity plate = CreateGround(scene);
		plate.GetTransform().Translation = { 40.0f, -0.5f, 0.0f };
		plate.GetComponent<BoxColliderComponent>().HalfExtents = { 2.0f, 0.5f, 2.0f };
		Entity resting = CreateBox(scene, { 40.0f, 0.5f, 0.0f });
		auto& guide = resting.AddComponent<JointComponent>();
		guide.Type = JointType::Slider;
		guide.ConnectedEntity = plate.GetUUID();
		guide.EnableCollision = true;

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		physics.SetLinearVelocity(slider, { 1.0f, 0.0f, 0.0f });
		Simulate(scene, 0.25f);
		SetJoint(slider, [](JointComponent& joint) { joint.LimitMax = 0.5f; });
		Simulate(scene, 1.0f);
		CHECK(physics.GetJointPosition(slider).value() == doctest::Approx(0.5f).epsilon(0.05));
		CHECK(slider.GetTransform().Translation.x == doctest::Approx(0.5f).epsilon(0.05));

		const float swung = physics.GetJointPosition(pendulum).value();
		CHECK(swung < -20.0f);
		SetJoint(pendulum, [](JointComponent& joint) {
			joint.UseLimits = true;
			joint.LimitMin = -5.0f;
			joint.LimitMax = 5.0f;
		});
		scene.OnUpdate(Step);
		// Rebuilt from the current pose, which is the new rest pose.
		CHECK(std::abs(physics.GetJointPosition(pendulum).value()) < 2.0f);

		CHECK(hanging.HasComponent<JointComponent>());
		SetJoint(hanging, [](JointComponent& joint) { joint.BreakForce = 5.0f; });
		CHECK(resting.GetTransform().Translation.y == doctest::Approx(0.5f).epsilon(0.02));
		SetJoint(resting, [](JointComponent& joint) { joint.EnableCollision = false; });
		Simulate(scene, 1.0f);
		CHECK_FALSE(hanging.HasComponent<JointComponent>());
		CHECK(resting.GetTransform().Translation.y < -2.0f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Hinge and slider joints break on limit and motor effort")
	{
		Scene scene;
		// A pendulum locked level by [0, 0] limits: the limit carries about 9.8 N·m of gravity torque.
		Entity locked = CreateBox(scene, { 1.0f, 0.0f, 0.0f });
		auto& hinge = locked.AddComponent<JointComponent>();
		hinge.Anchor = { -1.0f, 0.0f, 0.0f };
		hinge.Axis = { 0.0f, 0.0f, 1.0f };
		hinge.UseLimits = true;
		hinge.BreakTorque = 5.0f;

		// A box resting on the lower end of a vertical slider: the limit carries its 9.8 N weight.
		Entity rail = CreateBox(scene, { 10.0f, 0.0f, 0.0f });
		auto& slider = rail.AddComponent<JointComponent>();
		slider.Type = JointType::Slider;
		slider.UseLimits = true;
		slider.LimitMax = 1.0f;
		slider.BreakForce = 5.0f;
		Entity strongRail = CreateBox(scene, { 12.0f, 0.0f, 0.0f });
		strongRail.AddComponent<JointComponent>(slider).BreakForce = 50.0f;

		// A piston motor pushing into a static block stalls at its force limit, above BreakForce.
		Entity piston = CreateBox(scene, { 20.0f, 0.0f, 0.0f });
		piston.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;
		auto& motor = piston.AddComponent<JointComponent>();
		motor.Type = JointType::Slider;
		motor.MotorMode = JointMotorMode::Velocity;
		// Reaching 0.5 m/s within one step takes 30 N, below the break force.
		motor.MotorTarget = 0.5f;
		motor.MotorMaxForce = 200.0f;
		motor.BreakForce = 50.0f;
		Entity block = CreateBox(scene, { 20.0f, 1.1f, 0.0f });
		block.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		// The same piston with nothing in its way only overcomes damping.
		Entity freePiston = CreateBox(scene, { 30.0f, 0.0f, 0.0f });
		freePiston.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;
		freePiston.AddComponent<JointComponent>(motor);

		scene.OnSimulationStart();
		Simulate(scene, 1.0f);
		CHECK_FALSE(locked.HasComponent<JointComponent>());
		CHECK_FALSE(rail.HasComponent<JointComponent>());
		CHECK(strongRail.HasComponent<JointComponent>());
		CHECK_FALSE(piston.HasComponent<JointComponent>());
		CHECK(freePiston.HasComponent<JointComponent>());
		scene.OnSimulationStop();
	}

	TEST_CASE("Bodies collide again once a non-colliding joint between them breaks")
	{
		Scene scene;
		Entity plate = CreateGround(scene);
		// Resting on the plate, joined by a point joint too weak to carry the box.
		Entity box = CreateBox(scene, { 0.0f, 0.5f, 0.0f });
		auto& joint = box.AddComponent<JointComponent>();
		joint.Type = JointType::Point;
		joint.ConnectedEntity = plate.GetUUID();
		joint.BreakForce = 5.0f;

		scene.OnSimulationStart();
		Simulate(scene, 1.0f);
		CHECK_FALSE(box.HasComponent<JointComponent>());
		CHECK(box.GetTransform().Translation.y == doctest::Approx(0.5f).epsilon(0.02));
		scene.OnSimulationStop();
	}

	TEST_CASE("Hinge and slider axes ignore non-uniform scale")
	{
		Scene scene;
		// A diagonal slider axis on a stretched box: a scaled matrix would bend it toward X.
		Entity box = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
		box.GetTransform().Scale = { 4.0f, 1.0f, 1.0f };
		box.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;
		auto& joint = box.AddComponent<JointComponent>();
		joint.Type = JointType::Slider;
		joint.Axis = { 1.0f, 1.0f, 0.0f };

		scene.OnSimulationStart();
		scene.GetPhysicsWorld()->SetLinearVelocity(box, { 1.0f, 1.0f, 0.0f });
		Simulate(scene, 1.0f);
		const glm::vec3 position = box.GetTransform().Translation;
		CHECK(position.x > 0.5f);
		CHECK(position.x == doctest::Approx(position.y).epsilon(0.01));
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

	TEST_CASE("Joint motors drive hinges and sliders and update without a rebuild")
	{
		Scene scene;
		Entity wheel = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
		wheel.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;
		wheel.GetComponent<RigidBodyComponent>().AngularDamping = 0.0f;
		auto& hinge = wheel.AddComponent<JointComponent>();
		hinge.Axis = { 0.0f, 0.0f, 1.0f };
		hinge.MotorMode = JointMotorMode::Velocity;
		hinge.MotorTarget = 90.0f;

		Entity arm = CreateBox(scene, { 5.0f, 0.0f, 0.0f });
		arm.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;
		auto& servo = arm.AddComponent<JointComponent>();
		servo.Axis = { 0.0f, 0.0f, 1.0f };
		servo.MotorMode = JointMotorMode::Position;
		servo.MotorTarget = 45.0f;

		Entity piston = CreateBox(scene, { 10.0f, 0.0f, 0.0f });
		auto& slider = piston.AddComponent<JointComponent>();
		slider.Type = JointType::Slider;
		slider.Axis = { 0.0f, 1.0f, 0.0f };
		slider.MotorMode = JointMotorMode::Position;
		slider.MotorTarget = 1.5f;

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		Simulate(scene, 0.5f);
		CHECK(physics.GetJointPosition(wheel).value() == doctest::Approx(45.0f).epsilon(0.05));
		Simulate(scene, 2.5f);
		CHECK(physics.GetJointPosition(arm).value() == doctest::Approx(45.0f).epsilon(0.02));
		// The position motor holds the piston up against gravity.
		CHECK(piston.GetTransform().Translation.y == doctest::Approx(1.5f).epsilon(0.05));

		// Changing the target through the registry (as SetComponent does) keeps the original rest pose:
		// a rebuilt joint would measure from the current 45 degrees instead.
		JointComponent retarget = arm.GetComponent<JointComponent>();
		retarget.MotorTarget = -30.0f;
		arm.AddOrReplaceComponent<JointComponent>(retarget);
		Simulate(scene, 3.0f);
		CHECK(physics.GetJointPosition(arm).value() == doctest::Approx(-30.0f).epsilon(0.02));
		CHECK(glm::degrees(arm.GetTransform().GetRotationEuler().z) == doctest::Approx(-30.0f).epsilon(0.02));

		// Turning the motor off lets gravity take the piston back down.
		JointComponent off = piston.GetComponent<JointComponent>();
		off.MotorMode = JointMotorMode::Off;
		piston.AddOrReplaceComponent<JointComponent>(off);
		Simulate(scene, 0.5f);
		CHECK(piston.GetTransform().Translation.y < 0.5f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Joints break above their break force and are removed")
	{
		Scene scene;
		// A 1 kg box hanging from the world needs about 9.8 N to hold.
		Entity weak = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
		auto& weakJoint = weak.AddComponent<JointComponent>();
		weakJoint.Type = JointType::Point;
		weakJoint.BreakForce = 5.0f;

		Entity strong = CreateBox(scene, { 5.0f, 0.0f, 0.0f });
		strong.AddComponent<JointComponent>(weakJoint).BreakForce = 50.0f;

		// A welded box that is twisted hard breaks on torque.
		Entity twisted = CreateBox(scene, { 10.0f, 0.0f, 0.0f });
		twisted.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;
		auto& weld = twisted.AddComponent<JointComponent>();
		weld.Type = JointType::Fixed;
		weld.BreakTorque = 10.0f;

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		physics.SetAngularVelocity(twisted, { 0.0f, 20.0f, 0.0f });
		Simulate(scene, 1.0f);
		CHECK_FALSE(weak.HasComponent<JointComponent>());
		CHECK_FALSE(physics.HasJoint(weak));
		CHECK(weak.GetTransform().Translation.y < -3.0f);
		CHECK(strong.HasComponent<JointComponent>());
		CHECK(strong.GetTransform().Translation.y == doctest::Approx(0.0f).epsilon(0.01));
		CHECK_FALSE(twisted.HasComponent<JointComponent>());
		scene.OnSimulationStop();
	}

	TEST_CASE("Jointed bodies collide with each other only when EnableCollision is set")
	{
		auto settle = [](bool enableCollision) {
			Scene scene;
			Entity plate = CreateGround(scene);
			// A box resting on the plate, joined to it by a vertical slider: without collision it slides through.
			Entity box = CreateBox(scene, { 0.0f, 0.5f, 0.0f });
			auto& joint = box.AddComponent<JointComponent>();
			joint.Type = JointType::Slider;
			joint.ConnectedEntity = plate.GetUUID();
			joint.EnableCollision = enableCollision;
			scene.OnSimulationStart();
			Simulate(scene, 1.0f);
			const float y = box.GetTransform().Translation.y;
			scene.OnSimulationStop();
			return y;
		};
		CHECK(settle(true) == doctest::Approx(0.5f).epsilon(0.02));
		CHECK(settle(false) < -2.0f);
	}
}
