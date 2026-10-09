#include <doctest/doctest.h>

#include <Basalt/Core/Input.h>
#include <Basalt/Physics/PhysicsWorld.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/Scene/SceneSerializer.h>
#include <Basalt/Scripting/LuaJson.h>
#include <Basalt/Scripting/ScriptEngine.h>

#include "TestUtils.h"

using namespace Basalt;

namespace {

	constexpr float Step = 1.0f / 60.0f;

	Entity AddScripted(Scene& scene, const std::string& name, const std::string& script, nlohmann::json properties = nlohmann::json::object())
	{
		Entity entity = scene.CreateEntity(name);
		auto& component = entity.AddComponent<ScriptComponent>();
		component.Script = script;
		component.Properties = std::move(properties);
		return entity;
	}

	nlohmann::json Field(Scene& scene, Entity entity, const std::string& name)
	{
		return scene.GetScriptEngine()->GetInstanceField(entity, name).value_or(nlohmann::json());
	}

}

TEST_SUITE("Scripting")
{
	TEST_CASE("Script lifecycle, properties and per-entity overrides")
	{
		BasaltTest::TempProject project("ScriptLifecycle");
		const std::string script = project.WriteFile("Assets/Scripts/Counter.lua", R"(
			local Counter = {}
			Counter.Properties = { Speed = 2.0, Label = "default", Offset = Vec3(1, 2, 3), Enabled = true }
			function Counter:OnCreate() self.Created = true; self.Updates = 0; self.Late = 0 end
			function Counter:OnUpdate(dt) self.Updates = self.Updates + 1; self.LastDelta = dt end
			function Counter:OnLateUpdate(dt) self.Late = self.Late + 1 end
			return Counter
		)");

		Scene scene;
		Entity a = AddScripted(scene, "A", script);
		Entity b = AddScripted(scene, "B", script, { { "Speed", 7.5 }, { "Label", "custom" }, { "Offset", { 4, 5, 6 } } });

		scene.OnRuntimeStart();
		REQUIRE(scene.GetScriptEngine());
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		CHECK(Field(scene, a, "Created") == true);
		CHECK(Field(scene, a, "Speed") == 2.0);
		CHECK(Field(scene, b, "Speed") == 7.5);
		CHECK(Field(scene, b, "Label") == "custom");
		CHECK(Field(scene, b, "Offset") == nlohmann::json({ 4.0, 5.0, 6.0 }));
		CHECK(Field(scene, a, "Offset") == nlohmann::json({ 1.0, 2.0, 3.0 }));

		for (int i = 0; i < 5; i++)
			scene.OnUpdate(Step);
		CHECK(Field(scene, a, "Updates") == 5);
		CHECK(Field(scene, b, "Late") == 5);
		CHECK(Field(scene, a, "LastDelta").get<double>() == doctest::Approx(Step));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Script errors are isolated to the failing instance")
	{
		BasaltTest::TempProject project("ScriptErrors");
		const std::string broken = project.WriteFile("Assets/Scripts/Broken.lua", R"(
			local Broken = {}
			function Broken:OnUpdate(dt) local x = nil; return x.field end
			return Broken
		)");
		const std::string healthy = project.WriteFile("Assets/Scripts/Healthy.lua", R"(
			local Healthy = {}
			function Healthy:OnCreate() self.Updates = 0 end
			function Healthy:OnUpdate(dt) self.Updates = self.Updates + 1 end
			return Healthy
		)");
		const std::string syntax = project.WriteFile("Assets/Scripts/Syntax.lua", "local = = 5");
		const std::string noTable = project.WriteFile("Assets/Scripts/NoTable.lua", "return 42");

		Scene scene;
		AddScripted(scene, "Broken", broken);
		Entity good = AddScripted(scene, "Healthy", healthy);
		AddScripted(scene, "Syntax", syntax);
		AddScripted(scene, "NoTable", noTable);
		AddScripted(scene, "Missing", "Assets/Scripts/DoesNotExist.lua");

		scene.OnRuntimeStart();
		for (int i = 0; i < 10; i++)
			scene.OnUpdate(Step);

		const auto& errors = scene.GetScriptEngine()->GetErrors();
		REQUIRE(errors.size() == 4);
		CHECK(errors[0].find("Syntax.lua") != std::string::npos);
		CHECK(errors[1].find("must return a table") != std::string::npos);
		CHECK(errors[2].find("DoesNotExist.lua") != std::string::npos);
		// The runtime error is reported once, with file and line, then the instance is disabled.
		CHECK(errors[3].find("Broken.lua:3") != std::string::npos);
		CHECK(Field(scene, good, "Updates") == 10);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Lua API: transforms, components, hierarchy, spawning and destruction")
	{
		BasaltTest::TempProject project("ScriptApi");
		project.WriteFile("Assets/Prefabs/Bullet.bprefab", R"({"Entities":[{"ID":1,"Name":"Bullet","Components":{"Mesh":{"Mesh":"builtin://Sphere"}}},
			{"ID":2,"Name":"Trail","Parent":1}]})");
		const std::string script = project.WriteFile("Assets/Scripts/Api.lua", R"(
			local Api = {}
			function Api:OnCreate()
				local e = self.Entity
				e.Translation = Vec3(1, 2, 3)
				e.Scale = Vec3(2, 2, 2)
				e.EulerAngles = Vec3(0, Math.Radians(90), 0)
				self.Forward = e:GetForward()

				e:AddComponent("PointLight", { Intensity = 5, Color = {1, 0, 0} })
				local light = e:GetComponent("PointLight")
				light.Radius = 20
				e:SetComponent("PointLight", light)
				self.HasLight = e:HasComponent("PointLight")

				local child = Scene.CreateEntity("Child")
				child:SetParent(e)
				self.ChildCount = #e:GetChildren()
				self.FoundChild = e:FindChild("Child") == child

				local bullet = Scene.Instantiate("Assets/Prefabs/Bullet.bprefab", Vec3(0, 10, 0))
				self.BulletName = bullet.Name
				self.BulletY = bullet.Translation.y
				self.BulletChildren = #bullet:GetChildren()
				self.Victim = Scene.FindEntityByName("Victim")

				local ok, err = pcall(function() e:AddComponent("NotAComponent") end)
				self.BadComponentError = (not ok) and string.find(err, "unknown component") ~= nil
				self.Random = Math.Random(5, 6)
			end
			function Api:OnUpdate(dt)
				if self.Victim and self.Victim:IsValid() then
					self.Victim:Destroy()
					-- Destruction is deferred until the end of the frame.
					self.StillValidThisFrame = self.Victim:IsValid()
				end
				self.EntityCount = Scene.GetEntityCount()
			end
			return Api
		)");

		Scene scene;
		Entity entity = AddScripted(scene, "Api", script);
		Entity victim = scene.CreateEntity("Victim");

		scene.OnRuntimeStart();
		ScriptEngine& engine = *scene.GetScriptEngine();
		INFO("errors: " << (engine.GetErrors().empty() ? "" : engine.GetErrors().front()));
		REQUIRE(engine.GetErrors().empty());

		const auto& transform = entity.GetTransform();
		CHECK(transform.Translation == glm::vec3(1.0f, 2.0f, 3.0f));
		CHECK(transform.Scale == glm::vec3(2.0f));
		const nlohmann::json forward = Field(scene, entity, "Forward");
		CHECK(forward[0].get<double>() == doctest::Approx(-1.0).epsilon(1e-4));

		REQUIRE(entity.HasComponent<PointLightComponent>());
		CHECK(entity.GetComponent<PointLightComponent>().Intensity == 5.0f);
		CHECK(entity.GetComponent<PointLightComponent>().Radius == 20.0f);
		CHECK(entity.GetComponent<PointLightComponent>().Color == glm::vec3(1.0f, 0.0f, 0.0f));
		CHECK(Field(scene, entity, "ChildCount") == 1);
		CHECK(Field(scene, entity, "FoundChild") == true);
		CHECK(Field(scene, entity, "BulletName") == "Bullet");
		CHECK(Field(scene, entity, "BulletY") == 10.0);
		CHECK(Field(scene, entity, "BulletChildren") == 1);
		CHECK(Field(scene, entity, "BadComponentError") == true);
		const double random = Field(scene, entity, "Random").get<double>();
		CHECK((random >= 5.0 && random <= 6.0));

		scene.OnUpdate(Step);
		CHECK(Field(scene, entity, "StillValidThisFrame") == true);
		CHECK_FALSE(victim.IsValid());
		// Api, Child, Bullet, Trail (Victim destroyed during the first update, counted before flush).
		CHECK(Field(scene, entity, "EntityCount") == 5);
		scene.OnUpdate(Step);
		CHECK(Field(scene, entity, "EntityCount") == 4);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Scripts receive collision and trigger callbacks")
	{
		BasaltTest::TempProject project("ScriptContacts");
		const std::string script = project.WriteFile("Assets/Scripts/Contacts.lua", R"(
			local Contacts = {}
			function Contacts:OnCreate() self.Hits = 0; self.Triggers = 0 end
			function Contacts:OnCollisionBegin(other) self.Hits = self.Hits + 1; self.LastHit = other.Name end
			function Contacts:OnTriggerEnter(other) self.Triggers = self.Triggers + 1 end
			return Contacts
		)");

		Scene scene;
		Entity ground = AddScripted(scene, "Ground", script);
		ground.GetTransform().Translation = { 0.0f, -0.5f, 0.0f };
		ground.AddComponent<RigidBodyComponent>();
		ground.AddComponent<BoxColliderComponent>().HalfExtents = { 10.0f, 0.5f, 10.0f };

		Entity zone = AddScripted(scene, "Zone", script);
		zone.GetTransform().Translation = { 0.0f, 3.0f, 0.0f };
		zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
		zone.AddComponent<BoxColliderComponent>();

		Entity ball = AddScripted(scene, "Ball", script);
		ball.GetTransform().Translation = { 0.0f, 6.0f, 0.0f };
		ball.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		ball.AddComponent<SphereColliderComponent>();

		scene.OnRuntimeStart();
		for (int i = 0; i < 180; i++)
			scene.OnUpdate(Step);

		CHECK(Field(scene, ground, "Hits") == 1);
		CHECK(Field(scene, ground, "LastHit") == "Ball");
		CHECK(Field(scene, ball, "Hits") == 1);
		CHECK(Field(scene, zone, "Triggers") == 1);
		CHECK(Field(scene, ball, "Triggers") == 1);
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("OnCollisionBegin receives the contact point, normal and impulse")
	{
		BasaltTest::TempProject project("ScriptContactInfo");
		const std::string script = project.WriteFile("Assets/Scripts/Contact.lua", R"(
			local Contact = {}
			function Contact:OnCollisionBegin(other, contact)
				self.Point = contact.Point
				self.Normal = contact.Normal
				self.Impulse = contact.Impulse
				contact.Normal = Vec3(0, 0, 0) -- each side has its own table
			end
			return Contact
		)");

		// Contact pairs are ordered by UUID and the normal is flipped for the second entity, so run both
		// orders with fixed UUIDs rather than whichever order random UUIDs happen to give.
		UUID groundID = UUID(1);
		UUID ballID = UUID(2);
		SUBCASE("ground first") {}
		SUBCASE("ball first")
		{
			std::swap(groundID, ballID);
		}

		auto addScripted = [&](Scene& target, UUID id, const std::string& name) {
			Entity entity = target.CreateEntityWithUUID(id, name);
			entity.AddComponent<ScriptComponent>().Script = script;
			return entity;
		};

		Scene scene;
		Entity ground = addScripted(scene, groundID, "Ground");
		ground.GetTransform().Translation = { 0.0f, -0.5f, 0.0f };
		ground.AddComponent<RigidBodyComponent>();
		ground.AddComponent<BoxColliderComponent>().HalfExtents = { 10.0f, 0.5f, 10.0f };

		// Mass 2 sphere dropped so its bottom falls 5 m: it lands at about sqrt(2 * 9.81 * 5) = 9.9 m/s.
		Entity ball = addScripted(scene, ballID, "Ball");
		ball.GetTransform().Translation = { 1.0f, 5.5f, 2.0f };
		auto& body = ball.AddComponent<RigidBodyComponent>();
		body.Type = RigidBodyType::Dynamic;
		body.Mass = 2.0f;
		body.LinearDamping = 0.0f;
		ball.AddComponent<SphereColliderComponent>();

		scene.OnRuntimeStart();
		for (int i = 0; i < 90; i++)
			scene.OnUpdate(Step);

		const nlohmann::json ballNormal = Field(scene, ball, "Normal");
		const nlohmann::json groundNormal = Field(scene, ground, "Normal");
		REQUIRE(ballNormal.is_array());
		REQUIRE(groundNormal.is_array());
		// Each side's normal points away from the other entity.
		CHECK(ballNormal[1].get<float>() == doctest::Approx(1.0f).epsilon(0.01));
		CHECK(groundNormal[1].get<float>() == doctest::Approx(-1.0f).epsilon(0.01));
		const nlohmann::json point = Field(scene, ball, "Point");
		CHECK(point[0].get<float>() == doctest::Approx(1.0f).epsilon(0.01));
		CHECK(std::abs(point[1].get<float>()) < 0.2f);
		CHECK(point[2].get<float>() == doctest::Approx(2.0f).epsilon(0.01));
		// Stopping 2 kg at 9.9 m/s without bounce takes about 19.8 N*s.
		CHECK(Field(scene, ball, "Impulse").get<float>() == doctest::Approx(19.8f).epsilon(0.1));
		CHECK(Field(scene, ground, "Impulse") == Field(scene, ball, "Impulse"));
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Editing a mesh's render settings does not rebuild its MeshCollider")
	{
		// A rebuild would end and restart the box's contact with the floor (and re-pose joints) every frame.
		BasaltTest::TempProject project("ScriptMeshRebuild");
		const std::string script = project.WriteFile("Assets/Scripts/Floor.lua", R"(
			local Floor = {}
			function Floor:OnCreate() self.Hits = 0; self.Ends = 0 end
			function Floor:OnUpdate(dt)
				if self.Entity.Name == "Floor" then
					local mesh = self.Entity:GetComponent("Mesh")
					self.Entity:SetComponent("Mesh", { CastShadows = not mesh.CastShadows })
				end
			end
			function Floor:OnCollisionBegin(other) self.Hits = self.Hits + 1 end
			function Floor:OnCollisionEnd(other) self.Ends = self.Ends + 1 end
			return Floor
		)");

		Scene scene;
		Entity floor = AddScripted(scene, "Floor", script);
		floor.GetTransform().Scale = { 20.0f, 1.0f, 20.0f };
		floor.AddComponent<MeshComponent>().Mesh = "builtin://Plane";
		floor.AddComponent<RigidBodyComponent>();
		floor.AddComponent<MeshColliderComponent>();
		Entity box = AddScripted(scene, "Box", script);
		box.GetTransform().Translation = { 0.0f, 1.0f, 0.0f };
		box.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		box.AddComponent<BoxColliderComponent>();

		scene.OnRuntimeStart();
		for (int i = 0; i < 120; i++)
			scene.OnUpdate(Step);
		CHECK(Field(scene, box, "Hits") == 1);
		CHECK(Field(scene, box, "Ends") == 0);
		CHECK(box.GetTransform().Translation.y == doctest::Approx(0.5f).epsilon(0.02));
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Scripts drive joint motors and receive OnJointBreak")
	{
		BasaltTest::TempProject project("ScriptJoints");
		const std::string script = project.WriteFile("Assets/Scripts/Joints.lua", R"(
			local Joints = {}
			function Joints:OnCreate() self.Breaks = 0; self.BrokeWith = "" end
			function Joints:OnUpdate(dt)
				if self.Entity.Name == "Door" then
					self.Entity:SetComponent("Joint", { MotorMode = "Position", MotorTarget = 60 })
					self.Angle = self.Entity:GetJointPosition()
				end
			end
			function Joints:OnJointBreak(other)
				self.Breaks = self.Breaks + 1
				self.BrokeWith = other and other.Name or "world"
			end
			return Joints
		)");

		Scene scene;
		Entity frame = AddScripted(scene, "Frame", script);
		frame.AddComponent<RigidBodyComponent>();
		frame.AddComponent<BoxColliderComponent>();

		Entity door = AddScripted(scene, "Door", script);
		door.GetTransform().Translation = { 2.0f, 0.0f, 0.0f };
		door.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		door.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;
		door.AddComponent<BoxColliderComponent>();
		door.AddComponent<JointComponent>().ConnectedEntity = frame.GetUUID();

		// Hangs from the frame on a joint too weak for its weight.
		Entity lamp = AddScripted(scene, "Lamp", script);
		lamp.GetTransform().Translation = { -2.0f, 0.0f, 0.0f };
		lamp.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		lamp.AddComponent<SphereColliderComponent>();
		auto& chain = lamp.AddComponent<JointComponent>();
		chain.Type = JointType::Distance;
		chain.ConnectedEntity = frame.GetUUID();
		chain.BreakForce = 1.0f;

		scene.OnRuntimeStart();
		for (int i = 0; i < 180; i++)
			scene.OnUpdate(Step);

		CHECK(Field(scene, door, "Angle").get<double>() == doctest::Approx(60.0).epsilon(0.03));
		CHECK(Field(scene, lamp, "Breaks") == 1);
		CHECK(Field(scene, lamp, "BrokeWith") == "Frame");
		CHECK(Field(scene, frame, "Breaks") == 1);
		CHECK(Field(scene, frame, "BrokeWith") == "Lamp");
		CHECK(Field(scene, door, "Breaks") == 0);
		CHECK_FALSE(lamp.HasComponent<JointComponent>());
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("OnJointBreak reaches a joint entity, the body it moves and the connected entity")
	{
		BasaltTest::TempProject project("ScriptJointEntity");
		const std::string script = project.WriteFile("Assets/Scripts/Listener.lua", R"(
			local Listener = {}
			function Listener:OnCreate() self.Breaks = 0; self.BrokeWith = "" end
			function Listener:OnJointBreak(other)
				self.Breaks = self.Breaks + 1
				self.BrokeWith = other and other.Name or "world"
			end
			return Listener
		)");

		Scene scene;
		Entity hook = AddScripted(scene, "Hook", script);
		hook.GetTransform().Translation = { 0.0f, 2.0f, 0.0f };
		hook.AddComponent<RigidBodyComponent>();
		hook.AddComponent<BoxColliderComponent>();
		Entity lamp = AddScripted(scene, "Lamp", script);
		lamp.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		lamp.AddComponent<SphereColliderComponent>();
		// The lamp's joint lives on a child entity; it is too weak for the lamp's weight.
		Entity chain = AddScripted(scene, "Chain", script);
		scene.SetParent(chain, lamp);
		auto& joint = chain.AddComponent<JointComponent>();
		joint.Type = JointType::Point;
		joint.BodyEntity = lamp.GetUUID();
		joint.ConnectedEntity = hook.GetUUID();
		joint.BreakForce = 1.0f;

		scene.OnRuntimeStart();
		CHECK(scene.GetPhysicsWorld()->HasJoint(chain));
		for (int i = 0; i < 30; i++)
			scene.OnUpdate(Step);

		CHECK_FALSE(chain.HasComponent<JointComponent>());
		CHECK(Field(scene, lamp, "Breaks") == 1);
		CHECK(Field(scene, lamp, "BrokeWith") == "Hook");
		CHECK(Field(scene, hook, "Breaks") == 1);
		CHECK(Field(scene, hook, "BrokeWith") == "Lamp");
		CHECK(Field(scene, chain, "Breaks") == 1);
		CHECK(Field(scene, chain, "BrokeWith") == "Hook");
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("A joint entity that is also the connected body gets OnJointBreak once")
	{
		// Regression: the hook held a joint moving the lamp and connected to itself, and was notified twice
		// (the second time with itself as the other entity).
		BasaltTest::TempProject project("ScriptJointHolderConnected");
		const std::string script = project.WriteFile("Assets/Scripts/Listener.lua", R"(
			local Listener = {}
			function Listener:OnCreate() self.Breaks = 0; self.BrokeWith = "" end
			function Listener:OnJointBreak(other)
				self.Breaks = self.Breaks + 1
				self.BrokeWith = other and other.Name or "world"
			end
			return Listener
		)");

		Scene scene;
		Entity hook = AddScripted(scene, "Hook", script);
		hook.GetTransform().Translation = { 0.0f, 2.0f, 0.0f };
		hook.AddComponent<RigidBodyComponent>();
		hook.AddComponent<BoxColliderComponent>();
		Entity lamp = AddScripted(scene, "Lamp", script);
		lamp.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		lamp.AddComponent<SphereColliderComponent>();
		auto& joint = hook.AddComponent<JointComponent>();
		joint.Type = JointType::Point;
		joint.BodyEntity = lamp.GetUUID();
		joint.ConnectedEntity = hook.GetUUID();
		joint.BreakForce = 1.0f;

		scene.OnRuntimeStart();
		CHECK(scene.GetPhysicsWorld()->HasJoint(hook));
		for (int i = 0; i < 30; i++)
			scene.OnUpdate(Step);

		CHECK_FALSE(hook.HasComponent<JointComponent>());
		CHECK(Field(scene, hook, "Breaks") == 1);
		CHECK(Field(scene, hook, "BrokeWith") == "Lamp");
		CHECK(Field(scene, lamp, "Breaks") == 1);
		CHECK(Field(scene, lamp, "BrokeWith") == "Hook");
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("OnJointBreak may destroy the other entity during the same step")
	{
		BasaltTest::TempProject project("ScriptJointDestroy");
		const std::string script = project.WriteFile("Assets/Scripts/Breaker.lua", R"(
			local Breaker = {}
			function Breaker:OnCreate() self.Breaks = 0; self.Built = self.Entity:HasJoint() end
			function Breaker:OnJointBreak(other)
				self.Breaks = self.Breaks + 1
				-- Destroy the hook and take down the other weight's joint before its own break is handled.
				if other then other:Destroy() end
				for _, name in ipairs({ "Left", "Right" }) do
					local weight = Scene.FindEntityByName(name)
					if weight ~= self.Entity and weight:HasComponent("Joint") then weight:RemoveComponent("Joint") end
				end
			end
			return Breaker
		)");

		Scene scene;
		Entity hook = scene.CreateEntity("Hook");
		const UUID hookID = hook.GetUUID();
		hook.GetTransform().Translation = { 0.0f, 2.0f, 0.0f };
		hook.AddComponent<RigidBodyComponent>();
		hook.AddComponent<BoxColliderComponent>();
		// Two weights hang from the hook on weak joints that both break in the first step.
		auto hang = [&](const char* name, float x) {
			Entity weight = AddScripted(scene, name, script);
			weight.GetTransform().Translation = { x, 0.0f, 0.0f };
			weight.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
			weight.AddComponent<SphereColliderComponent>();
			auto& joint = weight.AddComponent<JointComponent>();
			joint.Type = JointType::Point;
			joint.ConnectedEntity = hook.GetUUID();
			joint.BreakForce = 1.0f;
			return weight;
		};
		Entity left = hang("Left", -1.0f);
		Entity right = hang("Right", 1.0f);

		scene.OnRuntimeStart();
		CHECK(Field(scene, left, "Built") == true);
		for (int i = 0; i < 30; i++)
			scene.OnUpdate(Step);

		CHECK_FALSE(scene.GetEntityByUUID(hookID));
		// Whichever breaks first removes the other joint before that one's break is handled.
		CHECK(Field(scene, left, "Breaks").get<int>() + Field(scene, right, "Breaks").get<int>() == 1);
		CHECK_FALSE(left.HasComponent<JointComponent>());
		CHECK_FALSE(right.HasComponent<JointComponent>());
		CHECK_FALSE(scene.GetPhysicsWorld()->HasJoint(right));
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Scripts read injected input, apply physics and request quit")
	{
		BasaltTest::TempProject project("ScriptInput");
		const std::string script = project.WriteFile("Assets/Scripts/Mover.lua", R"(
			local Mover = {}
			Mover.Properties = { Speed = 4.0 }
			function Mover:OnUpdate(dt)
				if Input.IsKeyDown("Right") then
					self.Entity:SetLinearVelocity(Vec3(self.Speed, 0, 0))
				end
				if Input.IsKeyPressed("Escape") then Game.Quit() end
				local hit = Physics.Raycast(self.Entity.WorldPosition, Vec3(0, -1, 0), 10, self.Entity)
				self.Grounded = hit ~= nil and hit.Entity.Name == "Floor"
			end
			return Mover
		)");

		Scene scene;
		Entity floor = scene.CreateEntity("Floor");
		floor.GetTransform().Translation = { 0.0f, -1.0f, 0.0f };
		floor.AddComponent<RigidBodyComponent>();
		floor.AddComponent<BoxColliderComponent>().HalfExtents = { 100.0f, 0.5f, 100.0f };

		Entity mover = AddScripted(scene, "Mover", script);
		auto& body = mover.AddComponent<RigidBodyComponent>();
		body.Type = RigidBodyType::Dynamic;
		body.GravityFactor = 0.0f;
		body.LinearDamping = 0.0f;
		mover.AddComponent<SphereColliderComponent>();

		Input::Reset();
		scene.OnRuntimeStart();
		Input::SetKeyState(KeyCode::Right, true);
		for (int i = 0; i < 60; i++)
		{
			scene.OnUpdate(Step);
			Input::EndFrame();
		}
		Input::SetKeyState(KeyCode::Right, false);
		CHECK(mover.GetTransform().Translation.x == doctest::Approx(4.0f).epsilon(0.05));
		CHECK(Field(scene, mover, "Grounded") == true);
		CHECK_FALSE(scene.IsQuitRequested());

		Input::SetKeyState(KeyCode::Escape, true);
		scene.OnUpdate(Step);
		CHECK(scene.IsQuitRequested());
		Input::Reset();
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Lua physics queries take layers, ignore and trigger options and return all hits")
	{
		BasaltTest::TempProject project("ScriptQueries");
		const std::string script = project.WriteFile("Assets/Scripts/Probe.lua", R"(
			local Probe = {}
			local function fails(pattern, ...)
				local ok, err = pcall(...)
				return not ok and string.find(tostring(err), pattern, 1, true) ~= nil
			end
			function Probe:OnCreate()
				local top, down = Vec3(0, 10, 0), Vec3(0, -1, 0)
				local hit = Physics.Raycast(top, down, 100)
				self.Closest = hit.Entity.Name
				self.IgnoreEntity = Physics.Raycast(top, down, 100, hit.Entity).Entity.Name
				self.IgnoreOption = Physics.Raycast(top, down, 100, { Ignore = hit.Entity }).Entity.Name
				self.DefaultLayer = Physics.Raycast(top, down, 100, { Layers = { "Default" } }).Entity.Name
				local all = Physics.Raycast(top, down, 100, { All = true, IncludeTriggers = true })
				self.AllCount = #all
				self.AllFirst = all[1].Entity.Name
				self.NoHits = #Physics.Raycast(top, Vec3(0, 1, 0), 100, { All = true })
				self.Miss = Physics.Raycast(top, Vec3(0, 1, 0), 100) == nil
				self.Sphere = Physics.SphereCast(top, 0.5, down, 100).Distance
				self.SphereAll = #Physics.SphereCast(top, 0.5, down, 100, { All = true })
				self.Box = Physics.BoxCast(top, Vec3(1, 0.1, 0.1), Quat.AngleAxis(math.rad(90), Vec3(0, 0, 1)), down, 100).Distance
				self.Overlap = #Physics.OverlapSphere(Vec3(0, 0, 0), 3)
				self.OverlapEnemy = Physics.OverlapBox(Vec3(0, 2, 0), Vec3(0.2, 0.2, 0.2), Quat.Identity(), { Layers = { "Enemy" } })[1].Name
				self.OverlapTrigger = #Physics.OverlapSphere(Vec3(0, 5, 0), 0.1, { IncludeTriggers = true })
				self.Layers = table.concat(Physics.GetLayers(), ",")
				self.BadLayer = fails("unknown physics layer 'Nope'", Physics.Raycast, top, down, 100, { Layers = { "Nope" } })
				self.BadOption = fails("unknown query option 'Layer'", Physics.Raycast, top, down, 100, { Layer = { "Enemy" } })
				self.BadType = fails("must be a boolean", Physics.Raycast, top, down, 100, { All = 1 })
				self.BadOptions = fails("must be an Entity or a table", Physics.Raycast, top, down, 100, 5)
				self.OverlapAll = fails("only applies to casts", Physics.OverlapSphere, top, 1, { All = true })
				self.EmptyLayers = fails("non-empty table", Physics.Raycast, top, down, 100, { Layers = {} })
				self.ArrayOptions = fails("named keys", Physics.Raycast, top, down, 100, { hit.Entity })
				self.ZeroDirection = fails("Raycast: direction must not be zero", Physics.Raycast, top, Vec3(0, 0, 0), 100)
				self.NegativeDistance = fails("SphereCast: maxDistance must be a positive number", Physics.SphereCast, top, 1, down, -1)
				self.ZeroRadius = fails("OverlapSphere: radius must be a positive number", Physics.OverlapSphere, top, 0)
				self.NanOrigin = fails("SphereCast: origin must be finite", Physics.SphereCast, Vec3(0 / 0, 0, 0), 1, down, 10)
				self.FlatBox = fails("BoxCast: halfExtents must be positive", Physics.BoxCast, top, Vec3(1, 0, 1), Quat.Identity(), down, 10)
				self.ZeroRotation = fails("OverlapBox: rotation must be a non-zero quaternion", Physics.OverlapBox, top, Vec3(1, 1, 1), Quat(0, 0, 0, 0))
			end
			return Probe
		)");

		PhysicsLayers layers;
		std::string error;
		REQUIRE(layers.Add("Enemy", error));
		Scene scene;
		scene.SetPhysicsLayers(layers);
		auto addBox = [&scene](const char* name, const glm::vec3& position, const glm::vec3& halfExtents) {
			Entity entity = scene.CreateEntity(name);
			entity.GetTransform().Translation = position;
			entity.AddComponent<RigidBodyComponent>();
			entity.AddComponent<BoxColliderComponent>().HalfExtents = halfExtents;
			return entity;
		};
		addBox("Floor", { 0.0f, -0.5f, 0.0f }, { 50.0f, 0.5f, 50.0f });
		addBox("Enemy", { 0.0f, 2.0f, 0.0f }, glm::vec3(0.5f)).GetComponent<RigidBodyComponent>().Layer = "Enemy";
		addBox("Zone", { 0.0f, 5.0f, 0.0f }, glm::vec3(0.5f)).GetComponent<RigidBodyComponent>().IsTrigger = true;
		Entity probe = AddScripted(scene, "Probe", script);

		scene.OnRuntimeStart();
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		CHECK(Field(scene, probe, "Closest") == "Enemy");
		CHECK(Field(scene, probe, "IgnoreEntity") == "Floor");
		CHECK(Field(scene, probe, "IgnoreOption") == "Floor");
		CHECK(Field(scene, probe, "DefaultLayer") == "Floor");
		CHECK(Field(scene, probe, "AllCount") == 3);
		CHECK(Field(scene, probe, "AllFirst") == "Zone");
		CHECK(Field(scene, probe, "NoHits") == 0);
		CHECK(Field(scene, probe, "Miss") == true);
		CHECK(Field(scene, probe, "Sphere").get<double>() == doctest::Approx(7.0).epsilon(0.01));
		CHECK(Field(scene, probe, "SphereAll") == 2);
		CHECK(Field(scene, probe, "Box").get<double>() == doctest::Approx(6.5).epsilon(0.01));
		CHECK(Field(scene, probe, "Overlap") == 2);
		CHECK(Field(scene, probe, "OverlapEnemy") == "Enemy");
		CHECK(Field(scene, probe, "OverlapTrigger") == 1);
		CHECK(Field(scene, probe, "Layers") == "Default,Enemy");
		CHECK(Field(scene, probe, "BadLayer") == true);
		CHECK(Field(scene, probe, "BadOption") == true);
		CHECK(Field(scene, probe, "BadType") == true);
		CHECK(Field(scene, probe, "BadOptions") == true);
		for (const char* check : { "OverlapAll", "EmptyLayers", "ArrayOptions", "ZeroDirection", "NegativeDistance", "ZeroRadius", "NanOrigin", "FlatBox", "ZeroRotation" })
		{
			INFO(check);
			CHECK(Field(scene, probe, check) == true);
		}
		scene.OnRuntimeStop();
	}

	TEST_CASE("Bodies on an ignored layer pair raise no collision or trigger events")
	{
		BasaltTest::TempProject project("IgnoredPairEvents");
		const std::string script = project.WriteFile("Assets/Scripts/Counter.lua", R"(
			local Counter = {}
			function Counter:OnCreate() self.Collisions = 0; self.Triggers = 0 end
			function Counter:OnCollisionBegin(other) self.Collisions = self.Collisions + 1 end
			function Counter:OnTriggerEnter(other) self.Triggers = self.Triggers + 1 end
			return Counter
		)");

		auto fallThrough = [&script](bool ignored) {
			PhysicsLayers layers;
			std::string error;
			REQUIRE(layers.Add("Ghost", error));
			if (ignored)
				layers.SetCollides(0, 1, false);
			Scene scene;
			scene.SetPhysicsLayers(layers);
			Entity zone = scene.CreateEntity("Zone");
			zone.GetTransform().Translation = { 0.0f, 2.0f, 0.0f };
			zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
			zone.AddComponent<BoxColliderComponent>();
			Entity floor = scene.CreateEntity("Floor");
			floor.GetTransform().Translation = { 0.0f, -0.5f, 0.0f };
			floor.AddComponent<RigidBodyComponent>();
			floor.AddComponent<BoxColliderComponent>().HalfExtents = { 10.0f, 0.5f, 10.0f };
			Entity ghost = AddScripted(scene, "Ghost", script);
			ghost.GetTransform().Translation = { 0.0f, 4.0f, 0.0f };
			auto& body = ghost.AddComponent<RigidBodyComponent>();
			body.Type = RigidBodyType::Dynamic;
			body.Layer = "Ghost";
			ghost.AddComponent<SphereColliderComponent>().Radius = 0.25f;

			scene.OnRuntimeStart();
			for (int i = 0; i < 90; i++)
				scene.OnUpdate(Step);
			const auto counts = std::make_pair(Field(scene, ghost, "Collisions").get<int>(), Field(scene, ghost, "Triggers").get<int>());
			CHECK(scene.GetScriptEngine()->GetErrors().empty());
			scene.OnRuntimeStop();
			return counts;
		};
		// The control run shows the same setup does produce both events when the layers collide.
		const auto colliding = fallThrough(false);
		CHECK(colliding.first >= 1);
		CHECK(colliding.second >= 1);
		CHECK(fallThrough(true) == std::make_pair(0, 0));
	}

	TEST_CASE("ExecuteString and property discovery")
	{
		BasaltTest::TempProject project("ScriptConsole");
		const std::string script = project.WriteFile("Assets/Scripts/Props.lua", R"(
			local Props = {}
			Props.Properties = { Health = 100, Name = "Orc", Velocity = Vec3(0, 1, 0), Alive = true }
			return Props
		)");

		std::string error;
		const auto properties = ScriptEngine::LoadScriptProperties(script, error);
		REQUIRE_MESSAGE(properties.has_value(), error);
		CHECK((*properties)["Health"] == 100);
		CHECK((*properties)["Name"] == "Orc");
		CHECK((*properties)["Velocity"] == nlohmann::json({ 0.0, 1.0, 0.0 }));
		CHECK((*properties)["Alive"] == true);

		Scene scene;
		scene.CreateEntity("One");
		scene.OnRuntimeStart();
		std::string result;
		REQUIRE(scene.GetScriptEngine()->ExecuteString("Scene.GetEntityCount() + 41", result));
		CHECK(result == "42");
		REQUIRE(scene.GetScriptEngine()->ExecuteString("local e = Scene.CreateEntity('Two')", result));
		CHECK(scene.GetEntityCount() == 2);
		CHECK_FALSE(scene.GetScriptEngine()->ExecuteString("error('boom')", result));
		CHECK(result.find("boom") != std::string::npos);
		CHECK_FALSE(scene.GetScriptEngine()->ExecuteString("dofile('x.lua')", result));
		scene.OnRuntimeStop();
	}

	// Regression (FuzzLuaJson): JsonToLua(null) returned a nil object without a Lua state, and LuaToJson
	// dereferenced that null state.
	TEST_CASE("JSON values round-trip through Lua, including null")
	{
		sol::state lua;
		const nlohmann::json values[] = { nullptr, true, 42, -7.5, "text", nlohmann::json::array({ 1, 2, 3 }), { { "a", { { "b", nullptr } } } } };
		for (const nlohmann::json& value : values)
		{
			const sol::object object = JsonToLua(lua, value);
			CHECK(object.lua_state() != nullptr);
			const nlohmann::json back = LuaToJson(object);
			if (value.is_object())
				CHECK(back["a"].is_object()); // Lua tables drop nil members, so {"b": null} becomes {}
			else
				CHECK(back == value);
		}
		CHECK(LuaToJson(sol::object()).is_null());
	}

	// Regression: Lua 5.4 seeds math.random from the clock, so two runs of the same game diverged.
	TEST_CASE("Lua math.random gives the same sequence in every play session")
	{
		auto sequence = []() {
			Scene scene;
			scene.OnRuntimeStart();
			std::string result;
			REQUIRE(scene.GetScriptEngine()->ExecuteString("local t = {} for i = 1, 8 do t[i] = math.random(1000) end return table.concat(t, ',') .. ';' .. Math.RandomInt(1, 1000)", result));
			scene.OnRuntimeStop();
			return result;
		};
		const std::string first = sequence();
		CHECK_FALSE(first.empty());
		CHECK(sequence() == first);
	}

	TEST_CASE("Scripts added at runtime start on the next update; require loads project modules")
	{
		BasaltTest::TempProject project("ScriptRuntimeAdd");
		project.WriteFile("Assets/Scripts/Lib/Util.lua", "return { Double = function(x) return x * 2 end }");
		const std::string script = project.WriteFile("Assets/Scripts/Late.lua", R"(
			local Util = require("Assets.Scripts.Lib.Util")
			local Late = {}
			function Late:OnCreate() self.Value = Util.Double(21) end
			return Late
		)");

		Scene scene;
		scene.OnRuntimeStart();
		Entity entity = AddScripted(scene, "Late", script);
		CHECK_FALSE(scene.GetScriptEngine()->HasInstance(entity));
		scene.OnUpdate(Step);
		CHECK(scene.GetScriptEngine()->HasInstance(entity));
		CHECK(Field(scene, entity, "Value") == 42);
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}
}
