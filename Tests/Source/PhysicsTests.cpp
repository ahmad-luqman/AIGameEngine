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
}
