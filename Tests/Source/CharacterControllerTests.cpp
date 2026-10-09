#include <doctest/doctest.h>

#include <Basalt/Core/Log.h>
#include <Basalt/Physics/PhysicsLayers.h>
#include <Basalt/Physics/PhysicsWorld.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/Scripting/ScriptEngine.h>

#include "TestUtils.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

using namespace Basalt;

namespace {

	constexpr float Step = 1.0f / 60.0f;
	// The capsule's half height plus radius: where the centre rests above the ground. Jolt keeps the
	// character a small padding (2 cm) above it.
	constexpr float StandingHeight = 1.0f;

	Entity CreateGround(Scene& scene)
	{
		Entity ground = scene.CreateEntity("Ground");
		ground.GetTransform().Translation = { 0.0f, -0.5f, 0.0f };
		ground.AddComponent<RigidBodyComponent>();
		ground.AddComponent<BoxColliderComponent>().HalfExtents = { 50.0f, 0.5f, 50.0f };
		return ground;
	}

	Entity CreateStaticBox(Scene& scene, const std::string& name, const glm::vec3& position, const glm::vec3& halfExtents)
	{
		Entity box = scene.CreateEntity(name);
		box.GetTransform().Translation = position;
		box.AddComponent<RigidBodyComponent>();
		box.AddComponent<BoxColliderComponent>().HalfExtents = halfExtents;
		return box;
	}

	// A 2 m tall capsule character (radius 0.5) centred on its entity.
	Entity CreateCharacter(Scene& scene, const glm::vec3& position)
	{
		Entity character = scene.CreateEntity("Character");
		character.GetTransform().Translation = position;
		character.AddComponent<CapsuleColliderComponent>();
		character.AddComponent<CharacterControllerComponent>();
		return character;
	}

	void Simulate(Scene& scene, float seconds, const std::function<void()>& everyFrame = {})
	{
		const int frames = static_cast<int>(std::lround(seconds / Step));
		for (int i = 0; i < frames; i++)
		{
			if (everyFrame)
				everyFrame();
			scene.OnUpdate(Step);
		}
	}

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
	void SetController(Entity entity, const std::function<void(CharacterControllerComponent&)>& change)
	{
		CharacterControllerComponent controller = entity.GetComponent<CharacterControllerComponent>();
		change(controller);
		entity.AddOrReplaceComponent<CharacterControllerComponent>(controller);
	}

	Entity AddScripted(Scene& scene, const std::string& name, const std::string& script)
	{
		Entity entity = scene.CreateEntity(name);
		entity.AddComponent<ScriptComponent>().Script = script;
		return entity;
	}

	nlohmann::json Field(Scene& scene, Entity entity, const std::string& name)
	{
		return scene.GetScriptEngine()->GetInstanceField(entity, name).value_or(nlohmann::json());
	}

	glm::vec3 Position(Entity entity)
	{
		return entity.GetTransform().Translation;
	}

}

TEST_SUITE("Physics")
{
	TEST_CASE("A character falls, lands on the ground and reports it")
	{
		Scene scene;
		CreateGround(scene);
		Entity character = CreateCharacter(scene, { 0.0f, 4.0f, 0.0f });

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		CHECK(physics.HasCharacter(character));
		CHECK(physics.GetCharacterCount() == 1);
		// The character is not a rigid body.
		CHECK(physics.GetBodyCount() == 1);
		CHECK_FALSE(physics.HasBody(character));

		Simulate(scene, 0.5f);
		// Free fall: 4 - 0.5 * 9.81 * 0.25 ~= 2.77
		CHECK(Position(character).y == doctest::Approx(2.77f).epsilon(0.03));
		CHECK_FALSE(physics.IsCharacterGrounded(character));
		CHECK_FALSE(physics.GetCharacterGroundNormal(character).has_value());
		CHECK(physics.GetLinearVelocity(character).y < -4.0f);

		Simulate(scene, 1.5f);
		CHECK(Position(character).y == doctest::Approx(StandingHeight).epsilon(0.03));
		CHECK(physics.IsCharacterGrounded(character));
		const auto normal = physics.GetCharacterGroundNormal(character);
		REQUIRE(normal.has_value());
		CHECK(normal->y == doctest::Approx(1.0f).epsilon(0.001));
		CHECK(glm::length(physics.GetLinearVelocity(character)) < 0.05f);

		// Queries and other bodies see the character through its inner body.
		const auto hit = physics.Raycast({ 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 10.0f);
		REQUIRE(hit.has_value());
		CHECK(hit->EntityID == character.GetUUID());
		CHECK(physics.OverlapSphere({ 0.0f, 1.0f, 0.0f }, 0.1f) == std::vector<UUID>{ character.GetUUID() });
		scene.OnSimulationStop();
	}

	TEST_CASE("Move walks the character, which stops at walls and slides along them")
	{
		Scene scene;
		CreateGround(scene);
		// Wall face at x = 2.5.
		CreateStaticBox(scene, "Wall", { 3.0f, 2.0f, 0.0f }, { 0.5f, 2.0f, 10.0f });
		Entity character = CreateCharacter(scene, { 0.0f, StandingHeight, 0.0f });

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		Simulate(scene, 0.25f);
		REQUIRE(physics.IsCharacterGrounded(character));

		physics.MoveCharacter(character, { 2.0f, 0.0f, 0.0f });
		// The velocity holds until the next Move.
		Simulate(scene, 0.5f);
		CHECK(Position(character).x == doctest::Approx(1.0f).epsilon(0.05));
		CHECK(Position(character).y == doctest::Approx(StandingHeight).epsilon(0.03));
		CHECK(physics.GetLinearVelocity(character).x == doctest::Approx(2.0f).epsilon(0.05));
		CHECK(physics.IsCharacterGrounded(character));

		Simulate(scene, 1.5f);
		// Stopped by the wall: centre one radius (plus padding) from its face.
		CHECK(Position(character).x == doctest::Approx(2.0f).epsilon(0.02));
		CHECK(std::abs(physics.GetLinearVelocity(character).x) < 0.05f);

		// Pushing diagonally into the wall slides along it.
		physics.MoveCharacter(character, { 2.0f, 0.0f, 2.0f });
		Simulate(scene, 0.5f);
		CHECK(Position(character).x < 2.05f);
		CHECK(Position(character).z == doctest::Approx(1.0f).epsilon(0.1));

		physics.MoveCharacter(character, { 0.0f, 0.0f, 0.0f });
		const glm::vec3 stopped = Position(character);
		Simulate(scene, 0.5f);
		CHECK(glm::length(Position(character) - stopped) < 0.01f);
		scene.OnSimulationStop();
	}

	TEST_CASE("A character climbs steps up to StepHeight")
	{
		for (const float stepHeight : { 0.3f, 0.1f })
		{
			CAPTURE(stepHeight);
			Scene scene;
			CreateGround(scene);
			// A 0.2 m step whose edge is at x = 1.5.
			CreateStaticBox(scene, "Step", { 6.5f, 0.1f, 0.0f }, { 5.0f, 0.1f, 5.0f });
			// A flat-bottomed box: a capsule's round bottom also rolls over edges lower than its radius.
			Entity character = CreateCharacter(scene, { 0.0f, StandingHeight, 0.0f });
			character.RemoveComponent<CapsuleColliderComponent>();
			character.AddComponent<BoxColliderComponent>().HalfExtents = { 0.4f, 1.0f, 0.4f };
			character.GetComponent<CharacterControllerComponent>().StepHeight = stepHeight;

			scene.OnSimulationStart();
			PhysicsWorld& physics = *scene.GetPhysicsWorld();
			Simulate(scene, 0.25f);
			physics.MoveCharacter(character, { 2.0f, 0.0f, 0.0f });
			Simulate(scene, 2.0f);
			if (stepHeight > 0.2f)
			{
				CHECK(Position(character).x > 3.5f);
				CHECK(Position(character).y == doctest::Approx(StandingHeight + 0.2f).epsilon(0.03));
				CHECK(physics.IsCharacterGrounded(character));
			}
			else
			{
				CHECK(Position(character).x < 1.1f);
				CHECK(Position(character).y == doctest::Approx(StandingHeight).epsilon(0.03));
			}
			scene.OnSimulationStop();
		}
	}

	TEST_CASE("A character stands on slopes up to SlopeLimit and slides down steeper ones")
	{
		for (const float angle : { 30.0f, 60.0f })
		{
			CAPTURE(angle);
			Scene scene;
			// A ramp rising towards +X: turning about Z tilts its top normal towards -X.
			Entity ramp = CreateStaticBox(scene, "Ramp", { 0.0f, 0.0f, 0.0f }, { 10.0f, 0.5f, 10.0f });
			ramp.GetTransform().Rotation = glm::angleAxis(glm::radians(angle), glm::vec3(0.0f, 0.0f, 1.0f));
			Entity character = CreateCharacter(scene, { 0.0f, 3.0f, 0.0f });
			REQUIRE(character.GetComponent<CharacterControllerComponent>().SlopeLimit == 45.0f);

			scene.OnSimulationStart();
			PhysicsWorld& physics = *scene.GetPhysicsWorld();
			Simulate(scene, 1.0f);
			const glm::vec3 landed = Position(character);
			Simulate(scene, 0.5f);
			const glm::vec3 expectedNormal(-std::sin(glm::radians(angle)), std::cos(glm::radians(angle)), 0.0f);
			const auto normal = physics.GetCharacterGroundNormal(character);
			REQUIRE(normal.has_value());
			CHECK(glm::dot(*normal, expectedNormal) == doctest::Approx(1.0f).epsilon(0.01));
			if (angle < 45.0f)
			{
				CHECK(physics.IsCharacterGrounded(character));
				CHECK(glm::length(Position(character) - landed) < 0.02f);
			}
			else
			{
				CHECK_FALSE(physics.IsCharacterGrounded(character));
				// Sliding down the ramp, towards -X.
				CHECK(Position(character).x < landed.x - 0.3f);
				CHECK(Position(character).y < landed.y - 0.5f);
			}
			scene.OnSimulationStop();
		}
	}

	TEST_CASE("A character walks down slopes on the ground and falls off ledges")
	{
		Scene scene;
		CreateGround(scene);
		// A 20 degree ramp descending towards +X, then a 2 m high ledge whose edge is at x = 12.
		Entity ramp = CreateStaticBox(scene, "Ramp", { 0.0f, 10.0f, 0.0f }, { 5.0f, 0.5f, 5.0f });
		ramp.GetTransform().Rotation = glm::angleAxis(glm::radians(-20.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		CreateStaticBox(scene, "Ledge", { 9.0f, 1.0f, 0.0f }, { 3.0f, 1.0f, 5.0f });
		Entity character = CreateCharacter(scene, { -3.0f, 13.0f, 0.0f });

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		Simulate(scene, 1.0f);
		REQUIRE(physics.IsCharacterGrounded(character));
		physics.MoveCharacter(character, { 2.0f, 0.0f, 0.0f });
		// Stick-to-floor keeps it on the ramp instead of hopping down it.
		int airborneFrames = 0;
		Simulate(scene, 1.5f, [&]() { airborneFrames += physics.IsCharacterGrounded(character) ? 0 : 1; });
		CHECK(airborneFrames == 0);
		CHECK(Position(character).x > -0.5f);

		// Off the ramp's end it falls to the ledge, walks on and drops off the ledge's edge to the ground.
		Simulate(scene, 7.0f);
		CHECK(Position(character).x > 13.0f);
		CHECK(Position(character).y == doctest::Approx(StandingHeight).epsilon(0.03));
		CHECK(physics.IsCharacterGrounded(character));
		scene.OnSimulationStop();
	}

	TEST_CASE("Moving up from the ground jumps; ceilings stop the rise at once")
	{
		for (const bool ceiling : { false, true })
		{
			CAPTURE(ceiling);
			Scene scene;
			CreateGround(scene);
			// Its underside at y = 2.6 leaves 0.6 m above the character's head.
			if (ceiling)
				CreateStaticBox(scene, "Ceiling", { 0.0f, 3.1f, 0.0f }, { 5.0f, 0.5f, 5.0f });
			Entity character = CreateCharacter(scene, { 0.0f, StandingHeight, 0.0f });

			scene.OnSimulationStart();
			PhysicsWorld& physics = *scene.GetPhysicsWorld();
			Simulate(scene, 0.25f);
			REQUIRE(physics.IsCharacterGrounded(character));

			physics.MoveCharacter(character, { 0.0f, 5.0f, 0.0f });
			Simulate(scene, 2.0f * Step);
			CHECK_FALSE(physics.IsCharacterGrounded(character));
			// The vertical part of Move no longer matters in the air: gravity takes over.
			physics.MoveCharacter(character, { 0.0f, 0.0f, 0.0f });
			float peak = Position(character).y;
			float landedAfter = 0.0f;
			float time = 2.0f * Step;
			while (time < 2.0f && landedAfter == 0.0f)
			{
				scene.OnUpdate(Step);
				time += Step;
				peak = std::max(peak, Position(character).y);
				if (physics.IsCharacterGrounded(character))
					landedAfter = time;
			}
			if (ceiling)
			{
				CHECK(peak < 1.65f);
				// Rising 0.6 m and falling back takes about 0.5 s; staying pressed against the ceiling until
				// gravity cancelled the jump speed would take about 0.9 s.
				CHECK(landedAfter > 0.0f);
				CHECK(landedAfter < 0.65f);
			}
			else
			{
				// 1 + 5^2 / (2 * 9.81) ~= 2.27
				CHECK(peak == doctest::Approx(2.27f).epsilon(0.03));
				CHECK(landedAfter == doctest::Approx(1.02f).epsilon(0.1));
			}
			CHECK(Position(character).y == doctest::Approx(StandingHeight).epsilon(0.03));
			scene.OnSimulationStop();
		}
	}

	TEST_CASE("Without gravity the Move velocity applies on every axis")
	{
		Scene scene;
		CreateGround(scene);
		Entity character = CreateCharacter(scene, { 0.0f, 3.0f, 0.0f });
		character.GetComponent<CharacterControllerComponent>().GravityFactor = 0.0f;

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		Simulate(scene, 0.5f);
		CHECK(Position(character).y == doctest::Approx(3.0f).epsilon(0.001));
		physics.MoveCharacter(character, { 0.0f, 1.0f, 0.0f });
		Simulate(scene, 1.0f);
		CHECK(Position(character).y == doctest::Approx(4.0f).epsilon(0.02));
		physics.MoveCharacter(character, { 0.0f, -4.0f, 0.0f });
		Simulate(scene, 2.0f);
		CHECK(Position(character).y == doctest::Approx(StandingHeight).epsilon(0.03));
		CHECK(physics.IsCharacterGrounded(character));
		scene.OnSimulationStop();
	}

	TEST_CASE("A character pushes dynamic bodies with at most MaxStrength")
	{
		for (const float strength : { 500.0f, 0.0f })
		{
			CAPTURE(strength);
			Scene scene;
			CreateGround(scene);
			Entity crate = scene.CreateEntity("Crate");
			crate.GetTransform().Translation = { 2.0f, 0.5f, 0.0f };
			auto& body = crate.AddComponent<RigidBodyComponent>();
			body.Type = RigidBodyType::Dynamic;
			body.Mass = 10.0f;
			crate.AddComponent<BoxColliderComponent>();
			Entity character = CreateCharacter(scene, { 0.0f, StandingHeight, 0.0f });
			character.GetComponent<CharacterControllerComponent>().MaxStrength = strength;

			scene.OnSimulationStart();
			PhysicsWorld& physics = *scene.GetPhysicsWorld();
			Simulate(scene, 0.25f);
			physics.MoveCharacter(character, { 2.0f, 0.0f, 0.0f });
			Simulate(scene, 2.0f);
			if (strength > 0.0f)
			{
				CHECK(Position(crate).x > 3.0f);
				CHECK(Position(character).x > 2.0f);
			}
			else
			{
				CHECK(Position(crate).x == doctest::Approx(2.0f).epsilon(0.01));
				// Stopped at the crate's face (x = 1.5) by its radius.
				CHECK(Position(character).x < 1.05f);
			}
			scene.OnSimulationStop();
		}
	}

	TEST_CASE("A character rides a kinematic platform")
	{
		Scene scene;
		Entity platform = CreateStaticBox(scene, "Platform", { 0.0f, -0.5f, 0.0f }, { 3.0f, 0.5f, 3.0f });
		platform.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		Entity character = CreateCharacter(scene, { 0.0f, StandingHeight, 0.0f });

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		Simulate(scene, 0.25f);
		REQUIRE(physics.IsCharacterGrounded(character));
		const float platformStart = Position(platform).x;
		const float characterStart = Position(character).x;
		Simulate(scene, 1.0f, [&]() { platform.GetTransform().Translation.x += Step; });
		CHECK(Position(platform).x - platformStart == doctest::Approx(1.0f).epsilon(0.01));
		CHECK(Position(character).x - characterStart == doctest::Approx(1.0f).epsilon(0.1));
		CHECK(physics.IsCharacterGrounded(character));
		scene.OnSimulationStop();
	}

	TEST_CASE("Moving a character's transform teleports it; its rotation follows the entity")
	{
		Scene scene;
		CreateGround(scene);
		Entity character = CreateCharacter(scene, { 0.0f, StandingHeight, 0.0f });
		// A box collider shows the rotation in queries: 2 m long along local X.
		character.RemoveComponent<CapsuleColliderComponent>();
		character.AddComponent<BoxColliderComponent>().HalfExtents = { 1.0f, 1.0f, 0.25f };

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		Simulate(scene, 0.25f);
		character.GetTransform().Translation = { 10.0f, 5.0f, 0.0f };
		scene.OnUpdate(Step);
		CHECK(Position(character).x == doctest::Approx(10.0f));
		CHECK(Position(character).y < 5.0f);
		CHECK(Position(character).y > 4.9f);
		Simulate(scene, 1.5f);
		CHECK(physics.IsCharacterGrounded(character));

		auto hitsAt = [&](const glm::vec3& point) {
			const auto hits = physics.OverlapSphere(point, 0.05f);
			return std::ranges::find(hits, character.GetUUID()) != hits.end();
		};
		const glm::vec3 center = Position(character);
		CHECK(hitsAt(center + glm::vec3(0.8f, 0.0f, 0.0f)));
		CHECK_FALSE(hitsAt(center + glm::vec3(0.0f, 0.0f, 0.8f)));
		character.GetTransform().Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		Simulate(scene, 2.0f * Step);
		CHECK(hitsAt(center + glm::vec3(0.0f, 0.0f, 0.8f)));
		CHECK_FALSE(hitsAt(center + glm::vec3(0.8f, 0.0f, 0.0f)));
		scene.OnSimulationStop();
	}

	TEST_CASE("CharacterController changes apply in place; the layer, colliders and removal rebuild it")
	{
		PhysicsLayers layers;
		std::string error;
		REQUIRE(layers.Add("Ghost", error));
		layers.SetCollides(0, 1, false);

		Scene scene;
		scene.SetPhysicsLayers(layers);
		CreateGround(scene);
		Entity character = CreateCharacter(scene, { 0.0f, 10.0f, 0.0f });

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		physics.MoveCharacter(character, { 1.0f, 0.0f, 0.0f });
		Simulate(scene, 0.5f);
		const float fallSpeed = physics.GetLinearVelocity(character).y;
		REQUIRE(fallSpeed < -4.0f);

		// Settings a live character takes, set every frame, neither stop the fall nor log anything.
		const uint64_t since = Log::GetHistory().GetTotalCount();
		Simulate(scene, 2.0f * Step, [&]() { SetController(character, [](auto& c) { c.SlopeLimit = 30.0f; c.MaxStrength = 50.0f; }); });
		CHECK(physics.GetLinearVelocity(character).y < fallSpeed);
		CHECK(physics.GetLinearVelocity(character).x == doctest::Approx(1.0f).epsilon(0.01));
		CHECK(CountMessages(since, "Physics:") == 0);

		// A changed layer rebuilds the character and keeps its velocity and Move: Ghost falls through the ground.
		SetController(character, [](auto& c) { c.Layer = "Ghost"; });
		const float beforeRebuild = physics.GetLinearVelocity(character).y;
		scene.OnUpdate(Step);
		CHECK(physics.HasCharacter(character));
		CHECK(physics.GetLinearVelocity(character).y < beforeRebuild);
		CHECK(physics.GetLinearVelocity(character).x == doctest::Approx(1.0f).epsilon(0.01));
		Simulate(scene, 1.5f);
		CHECK(Position(character).y < -2.0f);

		// Without a collider there is no character; adding one back builds it.
		character.RemoveComponent<CapsuleColliderComponent>();
		scene.OnUpdate(Step);
		CHECK_FALSE(physics.HasCharacter(character));
		CHECK(CountMessages(since, "no valid collider") == 1);
		character.AddComponent<SphereColliderComponent>();
		scene.OnUpdate(Step);
		CHECK(physics.HasCharacter(character));

		// A RigidBody is ignored while the controller is present and takes over once it is removed.
		character.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		scene.OnUpdate(Step);
		CHECK(physics.HasCharacter(character));
		CHECK_FALSE(physics.HasBody(character));
		CHECK(CountMessages(since, "RigidBody is ignored") == 1);
		character.RemoveComponent<CharacterControllerComponent>();
		scene.OnUpdate(Step);
		CHECK_FALSE(physics.HasCharacter(character));
		CHECK(physics.HasBody(character));
		CHECK(physics.GetCharacterCount() == 0);

		// Calls on an entity without a character do nothing.
		physics.MoveCharacter(character, { 1.0f, 0.0f, 0.0f });
		CHECK_FALSE(physics.IsCharacterGrounded(character));
		CHECK_FALSE(physics.GetCharacterGroundNormal(character).has_value());
		scene.OnSimulationStop();
	}

	TEST_CASE("Invalid CharacterController values are sanitized and reported once")
	{
		Scene scene;
		CreateGround(scene);
		Entity character = CreateCharacter(scene, { 0.0f, StandingHeight, 0.0f });
		auto& controller = character.GetComponent<CharacterControllerComponent>();
		controller.SlopeLimit = 120.0f;
		controller.StepHeight = -1.0f;
		controller.MaxStrength = -5.0f;
		controller.Mass = -1.0f;
		controller.Layer = "Nowhere";

		const uint64_t since = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		const CharacterControllerComponent same = controller;
		Simulate(scene, 0.5f, [&]() { character.AddOrReplaceComponent<CharacterControllerComponent>(same); });
		CHECK(physics.HasCharacter(character));
		CHECK(physics.IsCharacterGrounded(character));
		CHECK(CountMessages(since, "SlopeLimit 120 must be within [0, 90] degrees; using 90") == 1);
		CHECK(CountMessages(since, "StepHeight -1") == 1);
		CHECK(CountMessages(since, "MaxStrength -5") == 1);
		CHECK(CountMessages(since, "Mass -1") == 1);
		CHECK(CountMessages(since, "unknown physics layer 'Nowhere'") == 1);

		// Fixing a value logs the remaining problems once more.
		SetController(character, [](auto& c) { c.SlopeLimit = 45.0f; });
		Simulate(scene, 2.0f * Step);
		CHECK(CountMessages(since, "SlopeLimit") == 1);
		CHECK(CountMessages(since, "StepHeight -1") == 2);

		// Move ignores non-finite velocities.
		physics.MoveCharacter(character, { NAN, 0.0f, 0.0f });
		Simulate(scene, 2.0f * Step);
		CHECK(std::isfinite(Position(character).x));
		scene.OnSimulationStop();
	}

	TEST_CASE("Scripts drive a character and it enters triggers")
	{
		BasaltTest::TempProject project("ScriptCharacter");
		const std::string script = project.WriteFile("Assets/Scripts/Walker.lua", R"(
			local Walker = {}
			function Walker:OnCreate() self.Triggers = 0; self.Frames = 0; self.GroundedFrames = 0 end
			function Walker:OnUpdate(dt)
				if self.Entity.Name ~= "Walker" then return end
				self.Frames = self.Frames + 1
				self.Entity:Move(Vec3(3, 0, 0))
				if self.Entity:IsGrounded() then
					self.GroundedFrames = self.GroundedFrames + 1
					self.NormalY = self.Entity:GetGroundNormal().y
				end
				self.Speed = self.Entity:GetLinearVelocity().x
				local ok, err = pcall(function() Scene.FindEntityByName("Zone"):Move(Vec3(1, 0, 0)) end)
				self.MoveError = not ok and tostring(err):find("needs a CharacterController") ~= nil
				self.ZoneNormal = Scene.FindEntityByName("Zone"):GetGroundNormal() == nil
			end
			function Walker:OnTriggerEnter(other) self.Triggers = self.Triggers + 1; self.Other = other.Name end
			return Walker
		)");

		Scene scene;
		CreateGround(scene);
		Entity walker = AddScripted(scene, "Walker", script);
		walker.GetTransform().Translation = { 0.0f, StandingHeight, 0.0f };
		walker.AddComponent<CapsuleColliderComponent>();
		walker.AddComponent<CharacterControllerComponent>();
		Entity zone = AddScripted(scene, "Zone", script);
		zone.GetTransform().Translation = { 4.0f, 1.0f, 0.0f };
		zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
		zone.AddComponent<BoxColliderComponent>();

		scene.OnRuntimeStart();
		Simulate(scene, 3.0f);
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		// The character walks through the trigger, which does not block it.
		CHECK(Position(walker).x > 7.0f);
		CHECK(Field(scene, walker, "Triggers") == 1);
		CHECK(Field(scene, walker, "Other") == "Zone");
		CHECK(Field(scene, zone, "Triggers") == 1);
		CHECK(Field(scene, zone, "Other") == "Walker");
		CHECK(Field(scene, walker, "GroundedFrames").get<int>() > Field(scene, walker, "Frames").get<int>() - 3);
		CHECK(Field(scene, walker, "NormalY").get<double>() == doctest::Approx(1.0).epsilon(0.001));
		CHECK(Field(scene, walker, "Speed").get<double>() == doctest::Approx(3.0).epsilon(0.02));
		CHECK(Field(scene, walker, "MoveError") == true);
		CHECK(Field(scene, walker, "ZoneNormal") == true);
		scene.OnRuntimeStop();
	}

	TEST_CASE("CharacterController round-trips through the component registry")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Character");
		const ComponentInfo* info = ComponentRegistry::Find("CharacterController");
		REQUIRE(info);
		info->Add(entity);
		std::string error;
		REQUIRE(info->Deserialize(entity, { { "SlopeLimit", 30 }, { "StepHeight", 0.5 }, { "MaxStrength", 250 }, { "Mass", 80 }, { "GravityFactor", 2 }, { "Layer", "Player" } }, error));
		const auto& controller = entity.GetComponent<CharacterControllerComponent>();
		CHECK(controller.SlopeLimit == 30.0f);
		CHECK(controller.StepHeight == 0.5f);
		CHECK(controller.MaxStrength == 250.0f);
		CHECK(controller.Mass == 80.0f);
		CHECK(controller.GravityFactor == 2.0f);
		CHECK(controller.Layer == "Player");
		CHECK(info->Serialize(entity)["Layer"] == "Player");
		CHECK_FALSE(info->Deserialize(entity, { { "Slope", 30 } }, error));
	}
}
