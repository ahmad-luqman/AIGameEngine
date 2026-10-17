#include <doctest/doctest.h>

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/Input.h>
#include <Basalt/Core/Platform.h>
#include <Basalt/Physics/PhysicsWorld.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Project/SaveData.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/Scene/SceneSerializer.h>
#include <Basalt/Scripting/LuaJson.h>
#include <Basalt/Scripting/ScriptEngine.h>
#include <Basalt/UI/GameUI.h>

#include "TestUtils.h"

#include <spdlog/fmt/fmt.h>

#include <array>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <tuple>

using namespace Basalt;

namespace {

	using BasaltTest::Step;

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
	TEST_CASE("Spawned script and compound contact callbacks repeat in the same order")
	{
		BasaltTest::TempProject project("RuntimeContactOrder");
		const std::string script = project.WriteFile("Assets/Scripts/Order.lua", R"(
			local Order = {}
			function Order:OnCreate() Trace = (Trace or "") .. "C" .. self.Entity.Name .. ";" end
			function Order:OnTriggerEnter(other)
				Trace = Trace .. "B" .. self.Entity.Name .. other.Name .. ";"
				Begins = (Begins or 0) + 1
			end
			function Order:OnTriggerExit(other)
				Trace = Trace .. "E" .. self.Entity.Name .. other.Name .. ";"
				Ends = (Ends or 0) + 1
			end
			return Order
		)");
		auto run = [&]() {
			Scene scene;
			scene.OnRuntimeStart();
			Entity zone = AddScripted(scene, "Zone", script);
			zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
			zone.AddComponent<BoxColliderComponent>().HalfExtents = { 10.0f, 0.5f, 10.0f };
			std::vector<Entity> boxes;
			for (int i = 0; i < 4; ++i)
			{
				Entity body = AddScripted(scene, "Body" + std::to_string(i), script);
				body.GetTransform().Translation.x = static_cast<float>(i * 2);
				auto& rigidBody = body.AddComponent<RigidBodyComponent>();
				rigidBody.Type = RigidBodyType::Kinematic;
				body.AddComponent<BoxColliderComponent>();
				body.AddComponent<SphereColliderComponent>().Radius = 0.2f;
				boxes.push_back(body);
			}
			scene.OnUpdate(Step);
			CHECK(BasaltTest::Eval(scene, "return Begins") == "8");
			// One compound child leaves while the other still touches: no entity-pair end yet.
			for (Entity body : boxes)
				body.GetTransform().Translation.y = 0.8f;
			scene.OnUpdate(Step);
			scene.OnUpdate(Step);
			CHECK(BasaltTest::Eval(scene, "return Ends or 0") == "0");
			// A rebuild releases every sub-shape once, then starts one new pair per entity.
			scene.GetPhysicsWorld()->RecreateBody(zone);
			CHECK(BasaltTest::Eval(scene, "return Ends") == "8");
			scene.OnUpdate(Step);
			CHECK(BasaltTest::Eval(scene, "return Begins") == "16");
			for (Entity body : boxes)
				body.GetTransform().Translation.y = 3.0f;
			scene.OnUpdate(Step);
			scene.OnUpdate(Step);
			CHECK(BasaltTest::Eval(scene, "return Ends") == "16");
			CHECK(scene.GetScriptEngine()->GetErrors().empty());
			const std::string trace = BasaltTest::Eval(scene, "return Trace");
			scene.OnRuntimeStop();
			return trace;
		};
		const std::string first = run();
		CHECK(first ==
			  "CBody3;CBody1;CBody2;CZone;CBody0;BBody3Zone;BZoneBody3;BBody1Zone;"
			  "BZoneBody1;BBody2Zone;BZoneBody2;BZoneBody0;BBody0Zone;EBody3Zone;EZoneBody3;EBody1Zone;"
			  "EZoneBody1;EBody2Zone;EZoneBody2;EZoneBody0;EBody0Zone;BBody3Zone;BZoneBody3;BBody1Zone;"
			  "BZoneBody1;BBody2Zone;BZoneBody2;BZoneBody0;BBody0Zone;EBody3Zone;EZoneBody3;EBody1Zone;"
			  "EZoneBody1;EBody2Zone;EZoneBody2;EZoneBody0;EBody0Zone;");
		CHECK(run() == first);
		CHECK(run() == first);
	}

	TEST_CASE("Rebuilding a body remains safe when its contact exit destroys its entity")
	{
		BasaltTest::TempProject project("RebuildExitDestroy");
		const std::string script = project.WriteFile("Assets/Scripts/DestroyOnExit.lua", R"(
			return { OnTriggerExit = function(self, other) self.Entity:Destroy() end }
		)");
		Scene scene;
		Entity zone = AddScripted(scene, "Zone", script);
		zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
		zone.AddComponent<BoxColliderComponent>();
		Entity body = scene.CreateEntity("Body");
		body.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		body.AddComponent<BoxColliderComponent>();
		scene.OnRuntimeStart();
		scene.OnUpdate(Step);
		REQUIRE(scene.GetPhysicsWorld()->HasBody(zone));
		// Public C++ calls outside OnUpdate do not defer destruction during contact callbacks.
		scene.GetPhysicsWorld()->RecreateBody(zone);
		CHECK_FALSE(zone);
		CHECK(scene.GetPhysicsWorld()->GetBodyCount() == 1);
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

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

	TEST_CASE("Trigger and end callbacks receive where the bodies met or parted")
	{
		BasaltTest::TempProject project("ScriptContactEnds");
		const std::string script = project.WriteFile("Assets/Scripts/Contact.lua", R"(
			local Contact = {}
			-- Keeps the first event of each kind; a destroyed body's contacts end without one.
			local function record(self, key, contact)
				if contact == nil then
					self.NilEvents = (self.NilEvents or 0) + 1
					return
				end
				if self[key .. "Normal"] then return end
				self[key .. "Point"] = contact.Point
				self[key .. "Normal"] = contact.Normal
				self[key .. "Impulse"] = contact.Impulse
			end
			function Contact:OnTriggerEnter(other, contact) record(self, "Enter", contact) end
			function Contact:OnTriggerExit(other, contact) record(self, "Exit", contact) end
			function Contact:OnCollisionEnd(other, contact) record(self, "End", contact) end
			return Contact
		)");

		// Pairs are ordered by UUID and the second entity gets the flipped normal: run both orders.
		std::array<UUID, 3> ids = { UUID(1), UUID(2), UUID(3) };
		SUBCASE("ground first") {}
		SUBCASE("ball first")
		{
			std::swap(ids[0], ids[2]);
		}
		auto addScripted = [&](Scene& target, UUID id, const std::string& name) {
			Entity entity = target.CreateEntityWithUUID(id, name);
			entity.AddComponent<ScriptComponent>().Script = script;
			return entity;
		};

		Scene scene;
		Entity ground = addScripted(scene, ids[0], "Ground");
		ground.GetTransform().Translation = { 0.0f, -0.5f, 0.0f };
		ground.AddComponent<RigidBodyComponent>();
		ground.AddComponent<BoxColliderComponent>().HalfExtents = { 10.0f, 0.5f, 10.0f };
		// The ball falls through the zone (y 2.5 to 3.5), then bounces off the ground.
		Entity zone = addScripted(scene, ids[1], "Zone");
		zone.GetTransform().Translation = { 1.0f, 3.0f, 2.0f };
		zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
		zone.AddComponent<BoxColliderComponent>();
		Entity ball = addScripted(scene, ids[2], "Ball");
		ball.GetTransform().Translation = { 1.0f, 6.0f, 2.0f };
		auto& body = ball.AddComponent<RigidBodyComponent>();
		body.Type = RigidBodyType::Dynamic;
		body.Restitution = 0.8f;
		ball.AddComponent<SphereColliderComponent>();
		Entity crate = scene.CreateEntity("Crate");
		crate.GetTransform().Translation = { 5.0f, 0.5f, 0.0f };
		crate.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		crate.AddComponent<BoxColliderComponent>();

		scene.OnRuntimeStart();
		for (int i = 0; i < 90; i++)
			scene.OnUpdate(Step);

		auto vec = [&](Entity entity, const std::string& field) {
			const nlohmann::json value = Field(scene, entity, field);
			REQUIRE_MESSAGE(value.is_array(), field);
			return glm::vec3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
		};
		auto near = [](const glm::vec3& a, const glm::vec3& b, float tolerance) { return glm::all(glm::epsilonEqual(a, b, tolerance)); };
		// Entering from above at the zone's top face; each side's normal points away from the other.
		CHECK(near(vec(ball, "EnterPoint"), { 1.0f, 3.5f, 2.0f }, 0.2f));
		CHECK(near(vec(ball, "EnterNormal"), { 0.0f, 1.0f, 0.0f }, 0.01f));
		CHECK(near(vec(zone, "EnterNormal"), { 0.0f, -1.0f, 0.0f }, 0.01f));
		CHECK(Field(scene, ball, "EnterImpulse") == 0.0);
		// Leaving through the bottom face: the ball left downward, away from the zone.
		CHECK(near(vec(ball, "ExitPoint"), { 1.0f, 2.5f, 2.0f }, 0.2f));
		CHECK(near(vec(ball, "ExitNormal"), { 0.0f, -1.0f, 0.0f }, 0.01f));
		CHECK(near(vec(zone, "ExitNormal"), { 0.0f, 1.0f, 0.0f }, 0.01f));
		// Bouncing off the ground, just above where it hit.
		CHECK(near(vec(ball, "EndPoint"), { 1.0f, 0.0f, 2.0f }, 0.2f));
		CHECK(near(vec(ball, "EndNormal"), { 0.0f, 1.0f, 0.0f }, 0.01f));
		CHECK(near(vec(ground, "EndNormal"), { 0.0f, -1.0f, 0.0f }, 0.01f));
		CHECK(Field(scene, ground, "EndImpulse") == 0.0);
		CHECK(Field(scene, ground, "NilEvents").is_null());

		// A destroyed body is gone before its contacts end, so the ground hears of it without a contact.
		scene.DestroyEntity(crate);
		CHECK(Field(scene, ground, "NilEvents") == 1);
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Collision ends against a triangle-mesh floor report the parting, in either UUID order")
	{
		BasaltTest::TempProject project("ScriptMeshEnds");
		const std::string script = project.WriteFile("Assets/Scripts/End.lua", R"(
			local End = {}
			function End:OnCollisionEnd(other, contact)
				if contact and not self.Normal then self.Normal = contact.Normal end
			end
			return End
		)");
		// The floor's mesh is the query shape when it has the lower UUID, the other shape otherwise.
		UUID floorID = UUID(1);
		UUID ballID = UUID(2);
		SUBCASE("floor first") {}
		SUBCASE("ball first")
		{
			std::swap(floorID, ballID);
		}
		Scene scene;
		Entity floor = scene.CreateEntityWithUUID(floorID, "Floor");
		floor.AddComponent<ScriptComponent>().Script = script;
		floor.GetTransform().Scale = { 20.0f, 1.0f, 20.0f };
		floor.AddComponent<RigidBodyComponent>();
		floor.AddComponent<MeshColliderComponent>().Mesh = "builtin://Plane";
		Entity ball = scene.CreateEntityWithUUID(ballID, "Ball");
		ball.AddComponent<ScriptComponent>().Script = script;
		ball.GetTransform().Translation = { 1.0f, 3.0f, 2.0f };
		auto& body = ball.AddComponent<RigidBodyComponent>();
		body.Type = RigidBodyType::Dynamic;
		body.Restitution = 0.8f;
		ball.AddComponent<SphereColliderComponent>();

		scene.OnRuntimeStart();
		for (int i = 0; i < 90; i++)
			scene.OnUpdate(Step);
		const nlohmann::json ballNormal = Field(scene, ball, "Normal");
		const nlohmann::json floorNormal = Field(scene, floor, "Normal");
		REQUIRE(ballNormal.is_array());
		REQUIRE(floorNormal.is_array());
		CHECK(ballNormal[1].get<float>() == doctest::Approx(1.0f).epsilon(0.01));
		CHECK(floorNormal[1].get<float>() == doctest::Approx(-1.0f).epsilon(0.01));
		scene.OnRuntimeStop();
	}

	TEST_CASE("A rebuilt body ends its contacts without a contact on both sides, then begins them again")
	{
		BasaltTest::TempProject project("ScriptRebuildEnds");
		const std::string script = project.WriteFile("Assets/Scripts/Count.lua", R"(
			local Count = {}
			function Count:OnCreate() self.Begins = 0; self.NilEnds = 0 end
			function Count:OnCollisionBegin(other, contact) self.Begins = self.Begins + 1 end
			function Count:OnCollisionEnd(other, contact) if contact == nil then self.NilEnds = self.NilEnds + 1 end end
			return Count
		)");
		Scene scene;
		Entity ground = AddScripted(scene, "Ground", script);
		ground.GetTransform().Translation = { 0.0f, -0.5f, 0.0f };
		ground.AddComponent<RigidBodyComponent>();
		ground.AddComponent<BoxColliderComponent>().HalfExtents = { 10.0f, 0.5f, 10.0f };
		Entity ball = AddScripted(scene, "Ball", script);
		ball.GetTransform().Translation = { 0.0f, 0.5f, 0.0f };
		ball.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		ball.AddComponent<SphereColliderComponent>();
		scene.OnRuntimeStart();
		for (int i = 0; i < 30; i++)
			scene.OnUpdate(Step);
		REQUIRE(Field(scene, ball, "Begins") == 1);

		// A collider change rebuilds the body: the old body is gone, so neither side gets a contact.
		ball.AddOrReplaceComponent<SphereColliderComponent>(SphereColliderComponent{ .Radius = 0.45f });
		for (int i = 0; i < 30; i++)
			scene.OnUpdate(Step);
		CHECK(Field(scene, ball, "NilEnds") == 1);
		CHECK(Field(scene, ground, "NilEnds") == 1);
		CHECK(Field(scene, ball, "Begins") == 2);
		CHECK(Field(scene, ground, "Begins") == 2);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A resting body with a mirrored, non-uniform scale under a turned, scaled parent stays in contact")
	{
		// No rebuild or teleport may sneak in from rounding in the decomposed scale or rotation.
		BasaltTest::TempProject project("ScriptRestingScale");
		const std::string script = project.WriteFile("Assets/Scripts/Rest.lua", R"(
			local Rest = {}
			function Rest:OnCreate() self.Begins = 0; self.Ends = 0 end
			function Rest:OnCollisionBegin(other) self.Begins = self.Begins + 1 end
			function Rest:OnCollisionEnd(other) self.Ends = self.Ends + 1 end
			return Rest
		)");
		Scene scene;
		Entity ground = AddScripted(scene, "Ground", script);
		ground.GetTransform().Translation = { 0.0f, -0.5f, 0.0f };
		ground.AddComponent<RigidBodyComponent>();
		ground.AddComponent<BoxColliderComponent>().HalfExtents = { 10.0f, 0.5f, 10.0f };
		Entity parent = scene.CreateEntity("Parent");
		parent.GetTransform().Scale = glm::vec3(1.5f);
		parent.GetTransform().SetRotationEuler({ 0.0f, glm::radians(30.0f), 0.0f });
		Entity box = AddScripted(scene, "Box", script);
		box.GetTransform().Scale = { -0.7f, 1.3f, 0.9f };
		box.GetTransform().Translation = { 0.0f, 0.5f * 1.3f, 0.0f };
		box.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		box.AddComponent<BoxColliderComponent>();
		scene.SetParent(box, parent, false);

		scene.OnRuntimeStart();
		for (int i = 0; i < 1200; i++)
			scene.OnUpdate(Step);
		CHECK(Field(scene, ground, "Begins") == 1);
		CHECK(Field(scene, ground, "Ends") == 0);
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Animating a trigger's or a body's scale keeps their overlaps and contacts")
	{
		// The zone and the ground pulse every frame; the ball rests on the ground inside the zone.
		BasaltTest::TempProject project("ScriptPulse");
		const std::string script = project.WriteFile("Assets/Scripts/Pulse.lua", R"(
			local Pulse = {}
			function Pulse:OnCreate() self.Begins = 0; self.Ends = 0; self.Enters = 0; self.Exits = 0; self.Frame = 0 end
			function Pulse:OnUpdate(dt)
				self.Frame = self.Frame + 1
				local s = 1 + 0.05 * (self.Frame % 10)
				if self.Entity.Name == "Zone" then self.Entity.Scale = Vec3(4 * s, 4 * s, 4 * s) end
				if self.Entity.Name == "Ground" then self.Entity.Scale = Vec3(s, 1, s) end
			end
			function Pulse:OnCollisionBegin(other) self.Begins = self.Begins + 1 end
			function Pulse:OnCollisionEnd(other) self.Ends = self.Ends + 1 end
			function Pulse:OnTriggerEnter(other) self.Enters = self.Enters + 1 end
			function Pulse:OnTriggerExit(other) self.Exits = self.Exits + 1 end
			return Pulse
		)");

		Scene scene;
		Entity ground = AddScripted(scene, "Ground", script);
		ground.GetTransform().Translation = { 0.0f, -0.5f, 0.0f };
		ground.AddComponent<RigidBodyComponent>();
		ground.AddComponent<BoxColliderComponent>().HalfExtents = { 10.0f, 0.5f, 10.0f };
		Entity zone = AddScripted(scene, "Zone", script);
		zone.GetTransform().Translation = { 0.0f, 1.0f, 0.0f };
		zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
		zone.AddComponent<BoxColliderComponent>();
		Entity ball = AddScripted(scene, "Ball", script);
		ball.GetTransform().Translation = { 0.0f, 0.5f, 0.0f };
		ball.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		ball.AddComponent<SphereColliderComponent>();

		scene.OnRuntimeStart();
		for (int i = 0; i < 120; i++)
			scene.OnUpdate(Step);
		CHECK(Field(scene, zone, "Enters") == 1);
		CHECK(Field(scene, zone, "Exits") == 0);
		CHECK(Field(scene, ground, "Begins") == 1);
		CHECK(Field(scene, ground, "Ends") == 0);
		CHECK(ball.GetTransform().Translation.y == doctest::Approx(0.5f).epsilon(0.02));
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

	TEST_CASE("Entity:IsSleeping reports sleeping bodies, awake ones and nil without a body")
	{
		BasaltTest::TempProject project("ScriptSleep");
		const std::string script = project.WriteFile("Assets/Scripts/Watcher.lua", R"(
			local Watcher = {}
			function Watcher:OnUpdate(dt)
				self.Resting = Scene.FindEntityByName("Resting"):IsSleeping()
				self.Awake = Scene.FindEntityByName("Awake"):IsSleeping()
				self.Floor = Scene.FindEntityByName("Floor"):IsSleeping()
				self.NoBody = self.Entity:IsSleeping() == nil
				if self.Resting and not self.Woken then
					-- A velocity wakes it.
					Scene.FindEntityByName("Resting"):SetLinearVelocity(Vec3(1, 0, 0))
					self.Woken = true
					self.AfterWake = Scene.FindEntityByName("Resting"):IsSleeping()
				end
			end
			return Watcher
		)");

		Scene scene;
		Entity floor = scene.CreateEntity("Floor");
		floor.GetTransform().Translation = { 0.0f, -0.5f, 0.0f };
		floor.AddComponent<RigidBodyComponent>();
		floor.AddComponent<BoxColliderComponent>().HalfExtents = { 100.0f, 0.5f, 100.0f };
		for (const auto& [name, allowSleeping, x] : { std::tuple{ "Resting", true, -2.0f }, std::tuple{ "Awake", false, 2.0f } })
		{
			Entity box = scene.CreateEntity(name);
			box.GetTransform().Translation = { x, 0.5f, 0.0f };
			auto& rigidBody = box.AddComponent<RigidBodyComponent>();
			rigidBody.Type = RigidBodyType::Dynamic;
			rigidBody.AllowSleeping = allowSleeping;
			box.AddComponent<BoxColliderComponent>();
		}
		Entity watcher = AddScripted(scene, "Watcher", script);

		scene.OnRuntimeStart();
		// Half a second at rest puts a body to sleep.
		for (int i = 0; i < 90; i++)
			scene.OnUpdate(Step);
		CHECK(Field(scene, watcher, "Woken") == true);
		CHECK(Field(scene, watcher, "AfterWake") == false);
		CHECK(Field(scene, watcher, "Awake") == false);
		CHECK(Field(scene, watcher, "Floor") == false);
		CHECK(Field(scene, watcher, "NoBody") == true);
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

	TEST_CASE("Lua's random numbers start from the project seed; math.randomseed() returns to it")
	{
		BasaltTest::TempProject project("LuaRandomSeed");
		REQUIRE(project.IsValid());
		auto draw = [](const char* code) {
			Scene scene;
			scene.OnRuntimeStart();
			std::string result;
			REQUIRE(scene.GetScriptEngine()->ExecuteString(code, result));
			scene.OnRuntimeStop();
			return result;
		};
		const char* sequence = "local t = {} for i = 1, 4 do t[i] = math.random(1000000) end return table.concat(t, ',')";
		const std::string defaultSeed = draw(sequence);
		Project::GetActive()->GetConfig().RandomSeed = 4242;
		const std::string otherSeed = draw(sequence);
		CHECK(otherSeed != defaultSeed);
		CHECK(draw(sequence) == otherSeed);
		// Reseeding without an argument starts the project's sequence again, not the engine default.
		CHECK(draw("math.random(10) math.random(10) math.randomseed() local t = {} for i = 1, 4 do t[i] = math.random(1000000) end return table.concat(t, ',')") == otherSeed);

		// The session keeps its seed when the project's changes mid-session (it applies from the next play).
		Scene scene;
		scene.OnRuntimeStart();
		Project::GetActive()->GetConfig().RandomSeed = 7;
		CHECK(scene.GetRandomSeed() == 4242u);
		std::string result;
		REQUIRE(scene.GetScriptEngine()->ExecuteString(std::string("math.randomseed() ") + sequence, result));
		CHECK(result == otherSeed);
		scene.OnRuntimeStop();
	}

	TEST_CASE("math.randomseed takes integers (or integral floats) and rejects anything else")
	{
		Scene scene;
		scene.OnRuntimeStart();
		ScriptEngine& lua = *scene.GetScriptEngine();
		std::string seeded;
		std::string result;
		REQUIRE(lua.ExecuteString("math.randomseed(12) return math.random(1000000)", seeded));
		REQUIRE(lua.ExecuteString("math.randomseed(12.0) return math.random(1000000)", result));
		CHECK(result == seeded);
		// Seeds are 32-bit: 2^32 + 12 seeds like 12.
		REQUIRE(lua.ExecuteString("math.randomseed(4294967308) return math.random(1000000)", result));
		CHECK(result == seeded);
		// Negative seeds wrap like any other 64-bit integer: -1 seeds like 2^32 - 1.
		std::string wrapped;
		REQUIRE(lua.ExecuteString("math.randomseed(-1) return math.random(1000000)", wrapped));
		REQUIRE(lua.ExecuteString("math.randomseed(4294967295) return math.random(1000000)", result));
		CHECK(result == wrapped);
		for (const char* call : { "math.randomseed(1.5)", "math.randomseed(math.huge)", "math.randomseed(-math.huge)", "math.randomseed(1e300)", "math.randomseed(0/0)", "math.randomseed(2^63)", "Math.Seed(2.5)" })
		{
			INFO(call);
			CHECK_FALSE(lua.ExecuteString(call, result));
			CHECK(result.find("no integer representation") != std::string::npos);
		}
		for (const char* call : { "math.randomseed('12')", "math.randomseed({})", "Math.Seed('x')" })
		{
			INFO(call);
			CHECK_FALSE(lua.ExecuteString(call, result));
			CHECK(result.find("number expected") != std::string::npos);
		}
		// The largest integral float below 2^63 is accepted; Math.Seed takes the same values.
		CHECK(lua.ExecuteString("math.randomseed(2^62)", result));
		REQUIRE(lua.ExecuteString("Math.Seed(12) return math.random(1000000)", result));
		CHECK(result == seeded);
		scene.OnRuntimeStop();
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

TEST_SUITE("Gameplay scripting")
{
	TEST_CASE("Timer.After and Timer.Every fire on game time, in creation order, and can be cancelled")
	{
		BasaltTest::TempProject project("ScriptTimers");
		const std::string script = project.WriteFile("Assets/Scripts/Timers.lua", R"(
			local T = {}
			local function record(name) return function() table.insert(Fired, name .. "@" .. Time.GetFrame()) end end
			function T:OnCreate()
				Fired = {}
				self.After = Timer.After(0.5, record("after"))
				self.Every = Timer.Every(0.25, record("every"), 3)
				self.Zero = Timer.After(0, record("zero"))
				local cancelled = Timer.After(0.1, record("cancelled"))
				CancelReturned = cancelled:Cancel()
				CancelAgain = cancelled:Cancel()
				self.SelfCancel = Timer.Every(0.1, function(handle)
					SelfCancelCount = (SelfCancelCount or 0) + 1
					if SelfCancelCount == 2 then handle:Cancel() end
				end)
			end
			function T:OnUpdate()
				-- Created during an update, a zero delay still waits for the next frame.
				if Time.GetFrame() == 2 then Timer.After(0, record("fromUpdate")) end
			end
			return T
		)");

		Scene scene;
		AddScripted(scene, "Timers", script);
		scene.OnRuntimeStart();
		ScriptEngine& engine = *scene.GetScriptEngine();
		std::string result;
		// Nothing fires before the first update, even with a zero delay.
		CHECK(BasaltTest::Eval(engine, "#Fired") == "0");
		CHECK(engine.ExecuteString("CancelReturned == true and CancelAgain == false", result));
		CHECK(result == "true");

		for (int i = 0; i < 60; i++)
			scene.OnUpdate(Step);
		CHECK(engine.GetErrors().empty());
		// Update N runs with Time.GetFrame() == N - 1. 0.5 s is update 30; Every fires at 15, 30 and 45 and the
		// earlier-created After runs first at 30.
		CHECK(BasaltTest::Eval(engine, "table.concat(Fired, ' ')") == "zero@0 fromUpdate@3 every@14 after@29 every@29 every@44");
		CHECK(BasaltTest::Eval(engine, "SelfCancelCount") == "2");
		CHECK(BasaltTest::Eval(engine, "local s = Scene.FindEntityByName('Timers'):GetScript() return tostring(s.After:IsActive()) .. tostring(s.Every:IsActive()) .. tostring(s.SelfCancel:IsActive())") == "falsefalsefalse");

		// Bad arguments are errors, not silently ignored timers.
		const std::pair<const char*, const char*> invalid[] = {
			{ "Timer.After(-1, function() end)", "seconds must be a finite number >= 0" },
			{ "Timer.After(0 / 0, function() end)", "seconds must be a finite number >= 0" },
			{ "Timer.After('1', function() end)", "seconds must be a finite number >= 0" },
			{ "Timer.Every(0, function() end)", "seconds must be a finite number > 0" },
			{ "Timer.After(1, 5)", "expected a function, got number" },
			{ "Timer.Every(1, function() end, 0)", "count must be an integer from 1" },
			{ "Timer.Every(1, function() end, 2.5)", "count must be an integer from 1" },
		};
		BasaltTest::CheckLuaErrors(engine, invalid);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Timers stop when the scene pauses; repeating timers skip periods missed in a long frame")
	{
		BasaltTest::TempProject project("ScriptTimerPause");
		Scene scene;
		scene.OnRuntimeStart();
		ScriptEngine& engine = *scene.GetScriptEngine();
		std::string result;
		REQUIRE(engine.ExecuteString("Ticks = {} Timer.Every(0.1, function() table.insert(Ticks, Time.GetFrame()) end)", result));
		scene.SetPaused(true);
		for (int i = 0; i < 30; i++)
			scene.OnUpdate(Step);
		CHECK(BasaltTest::Eval(engine, "#Ticks") == "0");
		scene.SetPaused(false);
		// One 0.35 s frame: fires once, and the next period is the next 0.1 s boundary (0.4 s).
		scene.OnUpdate(0.35f);
		scene.OnUpdate(0.04f);
		scene.OnUpdate(0.02f);
		CHECK(BasaltTest::Eval(engine, "table.concat(Ticks, ' ')") == "0 2");
		CHECK(engine.GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Coroutines wait for seconds and frames, start right away and can be stopped")
	{
		BasaltTest::TempProject project("ScriptCoroutines");
		project.WriteFile("Assets/Scripts/Base.lua", R"(
			local Base = {}
			function Base:Describe() return "base" end
			return Base
		)");
		const std::string script = project.WriteFile("Assets/Scripts/Coroutines.lua", R"(
			local Base = require("Assets.Scripts.Base")
			local C = setmetatable({}, { __index = Base })
			function C:OnCreate()
				Trace = {}
				Inherited = self:Describe()
				self.Main = self:StartCoroutine(function(a, b)
					table.insert(Trace, "start " .. a .. b .. "@" .. Time.GetFrame())
					WaitFrames(2)
					table.insert(Trace, "frames@" .. Time.GetFrame())
					Wait(0.5)
					table.insert(Trace, "seconds@" .. Time.GetFrame())
					coroutine.yield()
					table.insert(Trace, "yield@" .. Time.GetFrame())
				end, "x", "y")
				StartedImmediately = #Trace == 1
				DoneActive = self:StartCoroutine(function() end):IsActive()
				self.Loop = self:StartCoroutine(function()
					while true do
						LoopCount = (LoopCount or 0) + 1
						WaitFrames(1)
					end
				end)
				self.Other = self:StartCoroutine(function() while true do Wait(0) end end)
				self.Timer = Timer.Every(0.1, function() end)
				EarlyOk, EarlyError = pcall(function() self:StartCoroutine(function() error("early failure") end) end)
			end
			function C:OnUpdate()
				if Time.GetFrame() == 10 then
					StopReturned = self:StopCoroutine(self.Loop)
				end
				if Time.GetFrame() == 35 then
					self:StopAllCoroutines()
				end
			end
			return C
		)");

		Scene scene;
		AddScripted(scene, "Coroutines", script);
		scene.OnRuntimeStart();
		ScriptEngine& engine = *scene.GetScriptEngine();
		std::string result;
		CHECK(BasaltTest::Eval(engine, "tostring(StartedImmediately) .. tostring(DoneActive) .. Inherited") == "truefalsebase");
		REQUIRE(engine.ExecuteString("tostring(EarlyOk) .. ' ' .. EarlyError", result));
		CHECK(result.find("false") == 0);
		CHECK(result.find("early failure") != std::string::npos);

		for (int i = 0; i < 40; i++)
			scene.OnUpdate(Step);
		CHECK(engine.GetErrors().empty());
		REQUIRE(engine.ExecuteString("table.concat(Trace, ' ')", result));
		// WaitFrames(2) from OnCreate resumes on update 2, Wait(0.5) half a second later, a bare yield next frame.
		CHECK(result == "start xy@0 frames@1 seconds@31 yield@32");
		// OnCreate plus updates 1-10 (stopped in update 11's OnUpdate, before coroutines run).
		CHECK(BasaltTest::Eval(engine, "LoopCount") == "11");
		REQUIRE(engine.ExecuteString("local s = Scene.FindEntityByName('Coroutines'):GetScript() return tostring(StopReturned) .. tostring(s.Main:IsActive()) .. tostring(s.Other:IsActive()) .. tostring(s.Timer:IsActive())", result));
		// StopAllCoroutines ends the coroutines but leaves the instance's timers alone.
		CHECK(result == "truefalsefalsetrue");
		scene.OnRuntimeStop();
	}

	TEST_CASE("Wait and WaitFrames only suspend engine coroutines; coroutine errors disable the owner")
	{
		BasaltTest::TempProject project("ScriptCoroutineErrors");
		const std::string script = project.WriteFile("Assets/Scripts/Faulty.lua", R"(
			local F = {}
			function F:OnCreate()
				self.Updates = 0
				self.Ticks = 0
				Timer.Every(0.05, function() self.Ticks = self.Ticks + 1 end)
				self:StartCoroutine(function()
					WaitFrames(3)
					error("late failure")
				end)
			end
			function F:OnUpdate() self.Updates = self.Updates + 1 end
			return F
		)");
		const std::string probe = project.WriteFile("Assets/Scripts/Probe.lua", R"(
			local P = {}
			function P:OnCreate()
				NotAFunction = select(2, pcall(function() self:StartCoroutine(5) end))
				local start = self.StartCoroutine
				NotAnInstance = select(2, pcall(start, {}, function() end))
				self:StartCoroutine(function()
					WrappedOk, WrappedError = pcall(coroutine.wrap(function() Wait(1) end))
					BadSeconds = select(2, pcall(Wait, -1))
					BadFrames = select(2, pcall(WaitFrames, 0))
					FractionFrames = select(2, pcall(WaitFrames, 1.5))
					-- Last: the failing handler belongs to this instance and disables it.
					Events.On("Inside", function() Wait(1) end)
					Events.Emit("Inside")
					WaitFrames(1)
				end)
			end
			return P
		)");

		Scene scene;
		Entity faulty = AddScripted(scene, "Faulty", script);
		AddScripted(scene, "Probe", probe);
		scene.OnRuntimeStart();
		ScriptEngine& engine = *scene.GetScriptEngine();
		std::string result;

		CHECK_FALSE(engine.ExecuteString("Wait(1)", result));
		CHECK(result.find("Wait can only be called in a coroutine started with self:StartCoroutine") != std::string::npos);
		CHECK_FALSE(engine.ExecuteString("WaitFrames(1)", result));
		CHECK(result.find("WaitFrames can only be called in a coroutine") != std::string::npos);
		REQUIRE(engine.ExecuteString("tostring(WrappedOk) .. ' ' .. WrappedError", result));
		CHECK(result.find("false") == 0);
		CHECK(result.find("Wait can only be called in a coroutine") != std::string::npos);
		REQUIRE(engine.ExecuteString("BadSeconds .. ' | ' .. BadFrames .. ' | ' .. FractionFrames .. ' | ' .. NotAFunction .. ' | ' .. NotAnInstance", result));
		CHECK(result.find("seconds must be a finite number >= 0") != std::string::npos);
		CHECK(result.find("frames must be an integer >= 1") != std::string::npos);
		CHECK(result.find("StartCoroutine: expected a function, got number") != std::string::npos);
		CHECK(result.find("must be called on a script instance") != std::string::npos);
		// The event handler that tried to wait failed (it ran inside Emit, not as the coroutine itself). It was
		// the probe's own handler, so Emit raised an error in the coroutine, which StartCoroutine passed on to
		// OnCreate.
		REQUIRE(engine.GetErrors().size() == 2);
		CHECK(engine.GetErrors()[0].find("cannot suspend the coroutine here") != std::string::npos);
		CHECK(engine.GetErrors()[0].find("(in a handler of event 'Inside' of 'Probe'") != std::string::npos);
		CHECK(engine.GetErrors()[1].find("a handler of this instance failed") != std::string::npos);
		CHECK(engine.GetErrors()[1].find("(in a coroutine)") != std::string::npos);
		CHECK(engine.GetErrors()[1].find("(in OnCreate)") != std::string::npos);

		for (int i = 0; i < 10; i++)
			scene.OnUpdate(Step);
		REQUIRE(engine.GetErrors().size() == 3);
		CHECK(engine.GetErrors()[2].find("Faulty.lua:9: late failure") != std::string::npos);
		// With the coroutine's traceback and its owner.
		CHECK(engine.GetErrors()[2].find("stack traceback") != std::string::npos);
		CHECK(engine.GetErrors()[2].find("(in a coroutine of 'Faulty'") != std::string::npos);
		// The coroutine failed in update 3, after OnUpdate and the timer (created first): it disabled its
		// instance, so neither ran again.
		CHECK(Field(scene, faulty, "Updates") == 3);
		CHECK(Field(scene, faulty, "Ticks") == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Timers, coroutines and event handlers end with the instance that owns them")
	{
		BasaltTest::TempProject project("ScriptOwnership");
		const std::string owner = project.WriteFile("Assets/Scripts/Owner.lua", R"(
			local O = {}
			function O:OnCreate()
				Counts = Counts or { Timer = 0, Coroutine = 0, Event = 0 }
				self.Timer = Timer.Every(0.05, function() Counts.Timer = Counts.Timer + 1 end)
				self.Coroutine = self:StartCoroutine(function() while true do Counts.Coroutine = Counts.Coroutine + 1 WaitFrames(1) end end)
				self.Handler = Events.On("Ping", function() Counts.Event = Counts.Event + 1 end)
			end
			function O:OnDestroy()
				LateTimer = select(2, pcall(Timer.After, 1, function() end))
				LateHandler = select(2, pcall(Events.On, "Ping", function() end))
				LateCoroutine = select(2, pcall(function() self:StartCoroutine(function() end) end))
				Handles = { self.Timer, self.Coroutine, self.Handler }
			end
			return O
		)");
		const std::string failing = project.WriteFile("Assets/Scripts/FailingUpdate.lua", R"(
			local F = {}
			function F:OnCreate()
				FailingTicks = 0
				Timer.Every(0.02, function() FailingTicks = FailingTicks + 1 end)
				FailingHandler = Events.On("Ping", function() FailingHeard = true end)
			end
			function F:OnUpdate() if Time.GetFrame() == 2 then error("update failure") end end
			return F
		)");
		const std::string faulty = project.WriteFile("Assets/Scripts/FaultyTimer.lua", R"(
			local F = {}
			function F:OnCreate()
				self.Updates = 0
				self.Handler = Events.On("Ping", function() FaultyHeard = (FaultyHeard or 0) + 1 end)
				Timer.After(0.05, function() error("timer failure") end)
			end
			function F:OnUpdate() self.Updates = self.Updates + 1 end
			return F
		)");

		Scene scene;
		Entity entity = AddScripted(scene, "Owner", owner);
		Entity broken = AddScripted(scene, "Faulty", faulty);
		AddScripted(scene, "Failing", failing);
		scene.OnRuntimeStart();
		ScriptEngine& engine = *scene.GetScriptEngine();
		std::string result;
		// Session-owned (console) tasks survive the instance.
		REQUIRE(engine.ExecuteString("SessionTicks = 0 Timer.Every(0.05, function() SessionTicks = SessionTicks + 1 end)", result));
		for (int i = 0; i < 6; i++)
			scene.OnUpdate(Step);
		REQUIRE(engine.ExecuteString("Events.Emit('Ping')", result));
		// In update 3 the failing instance raised an error in OnUpdate, then the faulty one in its timer: both
		// lost their handlers, and the failing one's timer (which ticked in update 2) never ran again.
		CHECK(result == "1");
		REQUIRE(engine.GetErrors().size() == 2);
		CHECK(engine.GetErrors()[0].find("FailingUpdate.lua:8: update failure") != std::string::npos);
		CHECK(engine.GetErrors()[1].find("FaultyTimer.lua:6: timer failure") != std::string::npos);
		CHECK(engine.GetErrors()[1].find("(in a timer of 'Faulty'") != std::string::npos);
		// Released right away, not just skipped: the handle reports it.
		CHECK(BasaltTest::Eval(engine, "FailingTicks .. tostring(FailingHeard) .. tostring(FailingHandler:IsActive())") == "1nilfalse");
		// The timer was due in update 3, after that update's OnUpdate.
		CHECK(Field(scene, broken, "Updates") == 3);

		scene.DestroyEntity(entity);
		REQUIRE(engine.ExecuteString("Counts.Timer .. ' ' .. Counts.Coroutine .. ' ' .. Counts.Event .. ' ' .. SessionTicks", result));
		const std::string before = result;
		REQUIRE(engine.ExecuteString("LateTimer .. ' | ' .. LateHandler .. ' | ' .. LateCoroutine", result));
		CHECK(result.find("Timer.After: the calling script instance is being destroyed or has failed") != std::string::npos);
		CHECK(result.find("Events.On: the calling script instance is being destroyed") != std::string::npos);
		CHECK(result.find("StartCoroutine: this instance is being destroyed or has failed") != std::string::npos);
		CHECK(BasaltTest::Eval(engine, "tostring(Handles[1]:IsActive()) .. tostring(Handles[2]:IsActive()) .. tostring(Handles[3]:IsActive())") == "falsefalsefalse");

		for (int i = 0; i < 12; i++)
			scene.OnUpdate(Step);
		CHECK(BasaltTest::Eval(engine, "Events.Emit('Ping')") == "0");
		REQUIRE(engine.ExecuteString("Counts.Timer .. ' ' .. Counts.Coroutine .. ' ' .. Counts.Event", result));
		CHECK(before.find(result) == 0);
		CHECK(BasaltTest::Eval(engine, "SessionTicks") == "6");

		// Play stopping ends the session's own tasks too.
		REQUIRE(engine.ExecuteString("SessionHandle = Timer.After(1, function() end) SessionEvent = Events.On('X', function() end)", result));
		engine.Stop();
		CHECK(BasaltTest::Eval(engine, "tostring(SessionHandle:IsActive()) .. tostring(SessionEvent:IsActive())") == "falsefalse");
		scene.OnRuntimeStop();
	}

	TEST_CASE("Events reach their handlers in subscription order with every argument")
	{
		BasaltTest::TempProject project("ScriptEvents");
		const std::string listener = project.WriteFile("Assets/Scripts/Listener.lua", R"(
			local L = {}
			function L:OnCreate()
				Events.On("Hit", function(...) table.insert(Received, "listener:" .. select("#", ...)) end)
				Events.On("Hit2", function() table.insert(Received2, "listener") end)
			end
			return L
		)");
		const std::string doomed = project.WriteFile("Assets/Scripts/Doomed.lua", R"(
			local D = {}
			function D:OnCreate()
				Events.On("Hit", function() table.insert(Received, "doomed") end)
				Events.On("Hit2", function() table.insert(Received2, "doomed") end)
			end
			return D
		)");

		const std::string killer = project.WriteFile("Assets/Scripts/Killer.lua", R"(
			local K = {}
			function K:OnUpdate()
				if Killed then return end
				Killed = true
				Received2 = {}
				Scene.FindEntityByName("Doomed"):Destroy()
				-- Destruction waits for the end of the update, but the doomed owner hears nothing more.
				KilledEmit = Events.Emit("Hit2")
			end
			return K
		)");

		Scene scene;
		AddScripted(scene, "Listener", listener);
		const UUID doomedID = AddScripted(scene, "Doomed", doomed).GetUUID();
		scene.OnRuntimeStart();
		ScriptEngine& engine = *scene.GetScriptEngine();
		std::string result;
		REQUIRE(engine.ExecuteString(R"(
			Received = Received or {}
			local a, c
			a = Events.On("Hit", function(x, y) table.insert(Received, "a" .. x .. y) end)
			Once = Events.Once("Hit", function(x) table.insert(Received, "once" .. x) end)
			Events.On("Hit", function()
				table.insert(Received, "b")
				Events.On("Hit", function() table.insert(Received, "late") end)
				a:Cancel()
				c:Cancel()
			end)
			c = Events.On("Hit", function() table.insert(Received, "c") end)
			First = Events.Emit("Hit", 1, 2)
			Second = Events.Emit("Hit", 3, 4)
		)",
									 result));
		REQUIRE(engine.ExecuteString("table.concat(Received, ' ') .. ' | ' .. First .. ' ' .. Second .. ' ' .. tostring(Once:IsActive())", result));
		// Handlers added during an emit wait for the next one; c, cancelled by b before its turn, never runs (a,
		// cancelled after its turn, ran once).
		CHECK(result == "listener:2 doomed a12 once1 b listener:2 doomed b late | 5 4 false");
		CHECK(engine.GetErrors().empty());

		// An owner about to be destroyed hears nothing; once it is gone, it has no handlers left.
		AddScripted(scene, "Killer", killer);
		scene.OnUpdate(Step);
		CHECK_FALSE(scene.GetEntityByUUID(doomedID));
		CHECK(BasaltTest::Eval(engine, "KilledEmit .. ' ' .. table.concat(Received2, ' ')") == "1 listener");
		CHECK(BasaltTest::Eval(engine, "Received = {} return Events.Emit('Hit', nil, 'x')") == "4");
		CHECK(BasaltTest::Eval(engine, "table.concat(Received, ' ')") == "listener:2 b late late");
		CHECK(BasaltTest::Eval(engine, "Events.Emit('Nobody')") == "0");

		// Emit works from inside a coroutine (the handler runs on the coroutine's thread).
		REQUIRE(engine.ExecuteString("Events.On('Co', function(v) CoValue = v end)", result));
		REQUIRE(engine.ExecuteString("Scene.FindEntityByName('Listener'):GetScript():StartCoroutine(function() Events.Emit('Co', 'from coroutine') end)", result));
		CHECK(BasaltTest::Eval(engine, "CoValue") == "from coroutine");

		// Endless recursion is stopped at MaxEmitDepth and reported once.
		REQUIRE(engine.ExecuteString("Events.On('Loop', function() Events.Emit('Loop') end)", result));
		REQUIRE(engine.ExecuteString("Events.Emit('Loop')", result));
		REQUIRE(engine.GetErrors().size() == 1);
		CHECK(engine.GetErrors()[0].find("nested more than 32 levels") != std::string::npos);
		// The depth unwound, and the failing session handler was unsubscribed.
		CHECK(BasaltTest::Eval(engine, "Events.On('After', function() end) return Events.Emit('After') .. ' ' .. Events.Emit('Loop')") == "1 0");

		const std::pair<const char*, const char*> invalid[] = {
			{ "Events.On('', function() end)", "Events.On: the event name must be a non-empty string" },
			{ "Events.On(5, function() end)", "Events.On: the event name must be a non-empty string" },
			{ "Events.Once('x', 'nope')", "Events.Once: expected a function, got string" },
			{ "Events.Emit()", "Events.Emit: the event name must be a non-empty string" },
		};
		BasaltTest::CheckLuaErrors(engine, invalid);
		scene.OnRuntimeStop();
	}

	TEST_CASE("An error in an event handler disables only its owner")
	{
		BasaltTest::TempProject project("ScriptEventErrors");
		const std::string faulty = project.WriteFile("Assets/Scripts/Faulty.lua", R"(
			local F = {}
			function F:OnCreate()
				self.Updates = 0
				Events.On("Go", function() error("handler failure") end)
				Events.On("Go", function() SecondHeard = true end)
				Events.On("Other", function() OtherHeard = true end)
			end
			function F:OnUpdate() self.Updates = self.Updates + 1 end
			return F
		)");
		const std::string selfEmitter = project.WriteFile("Assets/Scripts/SelfEmitter.lua", R"(
			local S = {}
			function S:OnCreate() Events.On("Mine", function() error("own failure") end) end
			function S:OnUpdate()
				if SelfEmitOk == nil then
					-- Emit raises: this instance's own handler failed and disabled it (pcall catches it here).
					SelfEmitOk, SelfEmitError = pcall(Events.Emit, "Mine")
					AfterEmit = true
				end
			end
			return S
		)");
		Scene scene;
		Entity entity = AddScripted(scene, "Faulty", faulty);
		scene.OnRuntimeStart();
		ScriptEngine& engine = *scene.GetScriptEngine();
		std::string result;
		REQUIRE(engine.ExecuteString("Events.On('Go', function() SessionHeard = true end)", result));
		REQUIRE(engine.ExecuteString("Events.Emit('Go')", result));
		// The failing handler and the session's ran; the faulty instance's second handler was removed when its
		// owner failed, and the error did not reach the emitter.
		CHECK(result == "2");
		REQUIRE(engine.GetErrors().size() == 1);
		CHECK(engine.GetErrors()[0].find("Faulty.lua:5: handler failure") != std::string::npos);
		CHECK(engine.GetErrors()[0].find("(in a handler of event 'Go' of 'Faulty'") != std::string::npos);
		CHECK(BasaltTest::Eval(engine, "tostring(SessionHeard) .. ' ' .. tostring(SecondHeard) .. ' ' .. Events.Emit('Other')") == "true nil 0");
		scene.OnUpdate(Step);
		CHECK(Field(scene, entity, "Updates") == 0);

		// A failing handler of the session is unsubscribed (it would fail on every emit).
		CHECK(BasaltTest::Eval(engine, "Events.On('Bad', function() error('session failure') end) return Events.Emit('Bad') .. ' ' .. Events.Emit('Bad')") == "1 0");
		REQUIRE(engine.GetErrors().size() == 2);
		CHECK(engine.GetErrors()[1].find("(in a handler of event 'Bad' of the play session)") != std::string::npos);

		// A handler that destroys the emitter (or another instance's failing handler) is no failure of the emitter.
		const std::string bullet = project.WriteFile("Assets/Scripts/Bullet.lua", R"(
			local B = {}
			function B:OnUpdate()
				if not BulletEmitted then
					BulletEmitted = true
					HitCount = Events.Emit("Hit", self.Entity)
					BulletContinued = true
				end
			end
			return B
		)");
		REQUIRE(BasaltTest::Eval(engine, "Events.On('Hit', function(bullet) bullet:Destroy() end) Events.On('Hit', function() error('another instance') end)") == "");
		const size_t errorsBefore = engine.GetErrors().size();
		AddScripted(scene, "Bullet", bullet);
		scene.OnUpdate(Step);
		CHECK(BasaltTest::Eval(engine, "HitCount .. ' ' .. tostring(BulletContinued)") == "2 true");
		CHECK_FALSE(scene.FindEntityByName("Bullet"));
		// Only the failing session handler's error.
		REQUIRE(engine.GetErrors().size() == errorsBefore + 1);
		CHECK(engine.GetErrors().back().find("another instance") != std::string::npos);

		// An instance whose own handler fails while it emits does not carry on as if nothing happened.
		AddScripted(scene, "SelfEmitter", selfEmitter);
		scene.OnUpdate(Step);
		CHECK(BasaltTest::Eval(engine, "tostring(SelfEmitOk) .. ' ' .. tostring(AfterEmit)") == "false true");
		CHECK(BasaltTest::Eval(engine, "SelfEmitError").find("a handler of this instance failed") != std::string::npos);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Storage keeps values across play sessions in the user data directory")
	{
		BasaltTest::TempProject project("ScriptStorage");
		std::string error;
		const auto savePath = SaveData::GetDefaultPath("ScriptStorage", error);
		REQUIRE_MESSAGE(savePath.has_value(), error);
		std::filesystem::remove_all(savePath->parent_path());

		std::string result;
		{
			Scene scene;
			scene.OnRuntimeStart();
			ScriptEngine& engine = *scene.GetScriptEngine();
			CHECK(BasaltTest::Eval(engine, "Storage.Get('Missing', 7) == 7 and Storage.Get('Missing') == nil and not Storage.Has('Missing')") == "true");
			REQUIRE(engine.ExecuteString(R"(
				Storage.Set("Score", 42)
				Storage.Set("Name", "Ada")
				Storage.Set("Position", Vec3(1, 2, 3))
				Storage.Set("Progress", { Level = 3, Flags = { true, false } })
				Storage.Set("Temporary", 1)
				Storage.Set("Temporary", nil)
				Storage.Set("Doomed", 1)
				DeleteReturned = tostring(Storage.Delete("Doomed")) .. tostring(Storage.Delete("Doomed"))
			)",
										 result));
			CHECK(BasaltTest::Eval(engine, "table.concat(Storage.GetKeys(), ',') .. ' ' .. DeleteReturned") == "Name,Position,Progress,Score truefalse");
			CHECK_FALSE(std::filesystem::exists(*savePath));
			CHECK(BasaltTest::Eval(engine, "local ok, err = Storage.Save() return tostring(ok) .. tostring(err)") == "truenil");
			CHECK(std::filesystem::exists(*savePath));

			const std::pair<const char*, const char*> invalid[] = {
				{ "Storage.Set('', 1)", "Storage.Set: the key must be a non-empty string" },
				{ "Storage.Get(5)", "Storage.Get: the key must be a non-empty string" },
				{ "Storage.Set('Fn', function() end)", "Storage.Set('Fn'):" },
				// What Save could not write back exactly is refused up front.
				{ "Storage.Set('N', 0 / 0)", "Storage.Set('N'): numbers must be finite" },
				{ "Storage.Set('N', math.huge)", "numbers must be finite" },
				{ "Storage.Set('N', { 1, { -math.huge } })", "numbers must be finite" },
				{ "Storage.Set('S', 'a\\255b')", "Storage.Set('S'): strings must be valid UTF-8 text" },
				{ "Storage.Set('T', { ['\\255'] = 1 })", "strings must be valid UTF-8 text" },
				{ "Storage.Set('\\255', 1)", "strings must be valid UTF-8 text" },
			};
			BasaltTest::CheckLuaErrors(engine, invalid);
			// Unsaved changes are dropped by Load (and when play stops).
			CHECK(BasaltTest::Eval(engine, "Storage.Set('Score', 1) Storage.Load() return Storage.Get('Score')") == "42");
			REQUIRE(engine.ExecuteString("Storage.Set('Unsaved', true)", result));
			CHECK(engine.GetErrors().empty());
			scene.OnRuntimeStop();
		}
		{
			Scene scene;
			scene.OnRuntimeStart();
			ScriptEngine& engine = *scene.GetScriptEngine();
			REQUIRE(engine.ExecuteString(R"(
				local p = Storage.Get("Position")
				local progress = Storage.Get("Progress")
				return Storage.Get("Score") .. Storage.Get("Name") .. p[1] .. p[2] .. p[3] .. progress.Level .. tostring(progress.Flags[2]) .. tostring(Storage.Has("Unsaved"))
			)",
										 result));
			CHECK(result == "42Ada1.02.03.03falsefalse");
			scene.OnRuntimeStop();
		}

		// A corrupt file is reported, kept aside, and the game continues with empty data.
		REQUIRE(FileSystem::WriteTextFile(*savePath, "not json"));
		{
			Scene scene;
			scene.OnRuntimeStart();
			ScriptEngine& engine = *scene.GetScriptEngine();
			CHECK(BasaltTest::Eval(engine, "Storage.Get('Score') == nil") == "true");
			REQUIRE(engine.GetErrors().size() == 1);
			CHECK(engine.GetErrors()[0].find("unreadable") != std::string::npos);
			CHECK(std::filesystem::exists(savePath->parent_path() / "SaveData.corrupt.json"));
			scene.OnRuntimeStop();
		}
		std::filesystem::remove_all(savePath->parent_path());
	}

	TEST_CASE("Storage needs an active project and gameplay functions need a running scene")
	{
		Scene scene;
		scene.OnRuntimeStart();
		std::string result;
		CHECK_FALSE(scene.GetScriptEngine()->ExecuteString("Storage.Get('Score')", result));
		CHECK(result.find("Storage needs an active project") != std::string::npos);
		scene.OnRuntimeStop();

		// Script files load without a scene (property discovery): calling these at load time is an error.
		BasaltTest::TempProject project("ScriptGameplayNoScene");
		const std::pair<const char*, const char*> scripts[] = {
			{ "Assets/Scripts/A.lua", "Timer.After(1, function() end) return {}" },
			{ "Assets/Scripts/B.lua", "Events.Emit('x') return {}" },
			{ "Assets/Scripts/C.lua", "Storage.Get('x') return {}" },
			{ "Assets/Scripts/D.lua", "Scene.Load('Assets/Scenes/A.bscene') return {}" },
		};
		for (const auto& [path, code] : scripts)
		{
			project.WriteFile(path, code);
			std::string error;
			CHECK_FALSE(ScriptEngine::LoadScriptProperties(path, error));
			CHECK_MESSAGE(error.find("only available while the scene is running") != std::string::npos, path, " -> ", error);
		}
	}

	TEST_CASE("Coroutines nest, stop themselves and take many arguments")
	{
		BasaltTest::TempProject project("ScriptCoroutineNesting");
		const std::string script = project.WriteFile("Assets/Scripts/Nesting.lua", R"(
			local N = {}
			function N:OnCreate()
				Trace = {}
				-- The inner coroutine's Wait must not leak into the outer one's schedule.
				self:StartCoroutine(function()
					self:StartCoroutine(function()
						Wait(0.2)
						table.insert(Trace, "inner@" .. Time.GetFrame())
					end)
					WaitFrames(1)
					table.insert(Trace, "outer@" .. Time.GetFrame())
				end)
				local args = {}
				for i = 1, 200 do args[i] = i end
				self:StartCoroutine(function(...) ArgCount, ArgLast = select("#", ...), select(200, ...) end, table.unpack(args))
			end
			return N
		)");
		const std::string stopper = project.WriteFile("Assets/Scripts/Stopper.lua", R"(
			local S = {}
			function S:OnCreate()
				self.Timer = Timer.Every(1, function() end)
				self:StartCoroutine(function()
					WaitFrames(1)
					self:StopAllCoroutines()
					Reached = true
					WaitFrames(1)
					After = true
				end)
				self.Handle = self:StartCoroutine(function()
					WaitFrames(1)
					self.Handle:Cancel()
					ReachedSelf = true
					WaitFrames(1)
					AfterSelf = true
				end)
				-- Stopped by the first coroutine's StopAllCoroutines before it ever resumes.
				self.Other = self:StartCoroutine(function() WaitFrames(5) OtherRan = true end)
				StopTimer = select(2, pcall(function() return self:StopCoroutine(self.Timer) end))
				-- A coroutine that already finished is simply not stopped.
				StopFinished = self:StopCoroutine(self:StartCoroutine(function() end))
			end
			return S
		)");

		Scene scene;
		AddScripted(scene, "Nesting", script);
		Entity stopperEntity = AddScripted(scene, "Stopper", stopper);
		scene.OnRuntimeStart();
		ScriptEngine& engine = *scene.GetScriptEngine();
		CHECK(BasaltTest::Eval(engine, "ArgCount .. ' ' .. ArgLast .. ' ' .. tostring(StopFinished)") == "200 200 false");
		CHECK(BasaltTest::Eval(engine, "StopTimer").find("StopCoroutine: the handle is a timer or another instance's coroutine") != std::string::npos);
		// Another instance's coroutine cannot be stopped through StopCoroutine either.
		CHECK(BasaltTest::Eval(engine, "local s = Scene.FindEntityByName('Stopper'):GetScript() return select(2, pcall(function() return Scene.FindEntityByName('Nesting'):GetScript():StopCoroutine(s.Other) end))").find("another instance's coroutine") != std::string::npos);

		for (int i = 0; i < 20; i++)
			scene.OnUpdate(Step);
		CHECK(engine.GetErrors().empty());
		// WaitFrames(1) from OnCreate resumes on update 1; the inner Wait(0.2) on update 12.
		CHECK(BasaltTest::Eval(engine, "table.concat(Trace, ' ')") == "outer@0 inner@11");
		// A coroutine that stops itself runs on until its next wait, then ends.
		CHECK(BasaltTest::Eval(engine, "tostring(Reached) .. ' ' .. tostring(After) .. ' ' .. tostring(OtherRan)") == "true nil nil");
		CHECK(Field(scene, stopperEntity, "Timer").is_null());
		CHECK(BasaltTest::Eval(engine, "local s = Scene.FindEntityByName('Stopper'):GetScript() return tostring(s.Timer:IsActive()) .. tostring(s.Handle:IsActive())") == "truefalse");
		scene.OnRuntimeStop();
	}

	// Regression: timers and handlers created inside a coroutine kept references bound to its thread, which
	// was freed once the coroutine ended (heap-use-after-free under ASan when the timer fired, the event was
	// emitted, or play stopped).
	TEST_CASE("Timers and event handlers created in a coroutine outlive it")
	{
		BasaltTest::TempProject project("ScriptCoroutineReferences");
		const std::string script = project.WriteFile("Assets/Scripts/Creator.lua", R"(
			local C = {}
			function C:OnCreate()
				self:StartCoroutine(function()
					-- Lets the test check that the finished coroutine's thread really is collected.
					Weak = setmetatable({}, { __mode = "v" })
					Weak[1] = coroutine.running()
					Timer.After(0.1, function() TimerFired = (TimerFired or 0) + 1 end)
					Events.On("Ping", function(value) PingHeard = value end)
					self.Repeating = Timer.Every(0.05, function() Ticks = (Ticks or 0) + 1 end)
				end)
			end
			return C
		)");

		Scene scene;
		AddScripted(scene, "Creator", script);
		scene.OnRuntimeStart();
		ScriptEngine& engine = *scene.GetScriptEngine();
		// The finished coroutine's thread is collected.
		CHECK(BasaltTest::Eval(engine, "collectgarbage('collect') return tostring(Weak[1])") == "nil");
		for (int i = 0; i < 10; i++)
			scene.OnUpdate(Step);
		CHECK(BasaltTest::Eval(engine, "collectgarbage('collect') return Events.Emit('Ping', 7)") == "1");
		CHECK(BasaltTest::Eval(engine, "TimerFired .. ' ' .. PingHeard .. ' ' .. Ticks") == "1 7 3");
		CHECK(engine.GetErrors().empty());
		// Stopping releases the references (with the main thread).
		scene.OnRuntimeStop();
	}

	TEST_CASE("Top-level script code belongs to the play session")
	{
		BasaltTest::TempProject project("ScriptTopLevelOwner");
		project.WriteFile("Assets/Scripts/Module.lua", R"(
			Timer.After(0.05, function() TopLevelFired = true end)
			return {}
		)");
		const std::string loader = project.WriteFile("Assets/Scripts/Loader.lua", R"(
			local L = {}
			function L:OnUpdate()
				local module = Scene.CreateEntity("Module")
				module:AddComponent("Script", { Script = "Assets/Scripts/Module.lua" })
				self.Entity:Destroy()
			end
			return L
		)");
		Scene scene;
		AddScripted(scene, "Loader", loader);
		scene.OnRuntimeStart();
		for (int i = 0; i < 6; i++)
			scene.OnUpdate(Step);
		// The loader that happened to load the class first is gone; the timer was the session's.
		CHECK_FALSE(scene.FindEntityByName("Loader"));
		CHECK(BasaltTest::Eval(scene, "tostring(TopLevelFired)") == "true");
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Storage reports what it could not do instead of losing data")
	{
		BasaltTest::TempProject project("ScriptStorageFailures");
		std::string error;
		const auto savePath = SaveData::GetDefaultPath("ScriptStorageFailures", error);
		REQUIRE_MESSAGE(savePath.has_value(), error);
		std::filesystem::remove_all(savePath->parent_path());

		// The first Storage call being Storage.Load returns the problem instead of reporting it.
		REQUIRE(FileSystem::WriteTextFile(*savePath, "not json"));
		{
			Scene scene;
			scene.OnRuntimeStart();
			ScriptEngine& engine = *scene.GetScriptEngine();
			const std::string loaded = BasaltTest::Eval(engine, "local ok, err = Storage.Load() return tostring(ok) .. ' ' .. err");
			CHECK(loaded.find("false") == 0);
			CHECK(loaded.find("unreadable") != std::string::npos);
			CHECK(engine.GetErrors().empty());
			// Moved aside, so saving afresh is safe.
			CHECK(BasaltTest::Eval(engine, "Storage.Set('Score', 3) return tostring(Storage.Save())") == "true");
			// A damaged file found mid-session: Load says so.
			REQUIRE(FileSystem::WriteTextFile(*savePath, "{ broken"));
			CHECK(BasaltTest::Eval(engine, "local ok, err = Storage.Load() return tostring(ok) .. ' ' .. tostring(err ~= nil)") == "false true");
			scene.OnRuntimeStop();
		}

		// A save that cannot be written returns false and the reason.
		std::filesystem::remove_all(savePath->parent_path());
		REQUIRE(FileSystem::WriteTextFile(savePath->parent_path(), "a file where the directory should be"));
		{
			Scene scene;
			scene.OnRuntimeStart();
			ScriptEngine& engine = *scene.GetScriptEngine();
			const std::string saved = BasaltTest::Eval(engine, "Storage.Set('Score', 1) local ok, err = Storage.Save() return tostring(ok) .. ' ' .. err");
			CHECK(saved.find("false") == 0);
			CHECK(saved.find("ScriptStorageFailures") != std::string::npos);
			scene.OnRuntimeStop();
		}
		std::filesystem::remove_all(savePath->parent_path());

		// Without a usable user data directory, Storage raises an error naming the variable.
		{
			BasaltTest::ScopedEnvVar relative(Platform::UserDataDirectoryVariable, "relative/dir");
			Scene scene;
			scene.OnRuntimeStart();
			CHECK(BasaltTest::Eval(scene, "Storage.Get('Score')").find("Storage: no user data directory: BASALT_USER_DATA_DIR is set to 'relative/dir'") != std::string::npos);
			scene.OnRuntimeStop();
		}
	}

	// Regression: a coroutine's wait request leaked into the coroutine that had started it, whose bare
	// coroutine.yield() then waited as long as the inner coroutine's Wait.
	TEST_CASE("A coroutine's bare yield waits one frame even after starting a coroutine that waits")
	{
		BasaltTest::TempProject project("ScriptCoroutineWaitLeak");
		const std::string script = project.WriteFile("Assets/Scripts/Outer.lua", R"(
			local O = {}
			function O:OnCreate()
				self:StartCoroutine(function()
					self:StartCoroutine(function() Wait(5) end)
					coroutine.yield()
					ResumedAt = Time.GetFrame()
				end)
			end
			return O
		)");
		Scene scene;
		AddScripted(scene, "Outer", script);
		scene.OnRuntimeStart();
		for (int i = 0; i < 3; i++)
			scene.OnUpdate(Step);
		CHECK(BasaltTest::Eval(scene, "tostring(ResumedAt)") == "0");
		scene.OnRuntimeStop();
	}

	TEST_CASE("Coroutine errors are reported safely, with their to-be-closed variables closed")
	{
		BasaltTest::TempProject project("ScriptCoroutineErrorObjects");
		const std::string script = project.WriteFile("Assets/Scripts/Thrower.lua", R"(
			local T = {}
			T.Properties = { Kind = "" }
			function T:OnCreate()
				self.Updates = 0
				self:StartCoroutine(function()
					local guard <close> = setmetatable({}, { __close = function(_, err) Closed = (Closed or 0) + 1 ClosedWith = tostring(err) end })
					WaitFrames(1)
					if self.Kind == "raising" then
						error(setmetatable({}, { __tostring = function() error("tostring itself fails") end }))
					elseif self.Kind == "custom" then
						error(setmetatable({}, { __tostring = function() return "custom error object" end }))
					elseif self.Kind == "registering" then
						-- Created while the error is turned into text: it belongs to this failing instance.
						error(setmetatable({}, { __tostring = function()
							LateTimer = Timer.After(0, function() LateFired = true end)
							return "registering error object"
						end }))
					elseif self.Kind == "closing" then
						local failing <close> = setmetatable({}, { __close = function() error("close failed") end })
						error("first failure")
					end
				end)
			end
			function T:OnUpdate() self.Updates = self.Updates + 1 end
			return T
		)");
		Scene scene;
		Entity raising = AddScripted(scene, "Raising", script, { { "Kind", "raising" } });
		AddScripted(scene, "Custom", script, { { "Kind", "custom" } });
		AddScripted(scene, "Closing", script, { { "Kind", "closing" } });
		AddScripted(scene, "Registering", script, { { "Kind", "registering" } });
		scene.OnRuntimeStart();
		ScriptEngine& engine = *scene.GetScriptEngine();
		for (int i = 0; i < 4; i++)
			scene.OnUpdate(Step);

		// Each failure is one script error that disables its own instance; the updates go on.
		REQUIRE(engine.GetErrors().size() == 4);
		CHECK(engine.GetErrors()[0].find("(error object is a table value whose __tostring failed)") != std::string::npos);
		CHECK(engine.GetErrors()[0].find("(in a coroutine of 'Raising'") != std::string::npos);
		CHECK(engine.GetErrors()[1].find("custom error object") != std::string::npos);
		CHECK(engine.GetErrors()[2].find("first failure") != std::string::npos);
		CHECK(engine.GetErrors()[2].find("closing its to-be-closed variables also failed") != std::string::npos);
		CHECK(engine.GetErrors()[2].find("close failed") != std::string::npos);
		// It failed in update 1 (after that OnUpdate) and never updated again.
		CHECK(Field(scene, raising, "Updates") == 1);
		// Every coroutine's guard was closed, the plain one with the error it failed with.
		CHECK(BasaltTest::Eval(engine, "Closed") == "4");
		CHECK(engine.GetErrors()[3].find("registering error object") != std::string::npos);
		// The timer the failing instance's __tostring created ended with it.
		CHECK(BasaltTest::Eval(engine, "tostring(LateTimer:IsActive()) .. ' ' .. tostring(LateFired)") == "false nil");
		scene.OnRuntimeStop();
	}

	TEST_CASE("Bindings called by finalizers while the Lua state closes do nothing")
	{
		BasaltTest::TempProject project("ScriptFinalizers");
		const std::string script = project.WriteFile("Assets/Scripts/Finalizer.lua", R"(
			local F = {}
			function F:OnCreate()
				local entity = self.Entity
				Keeper = setmetatable({}, { __gc = function()
					pcall(Timer.After, 1, function() end)
					pcall(Events.On, "x", function() end)
					pcall(Events.Emit, "x")
					pcall(Storage.Get, "k")
					pcall(function() return entity:GetScript() end)
					pcall(function() return self:StartCoroutine(function() end) end)
					pcall(function() return self:StopAllCoroutines() end)
				end })
			end
			return F
		)");
		Scene scene;
		AddScripted(scene, "Finalizer", script);
		scene.OnRuntimeStart();
		scene.OnUpdate(Step);
		// lua_close runs the finalizer after the engine let go of its host: no crash (the sanitizer jobs check).
		scene.OnRuntimeStop();
		CHECK_FALSE(scene.GetScriptEngine());
	}

	TEST_CASE("Storage.Save and Storage.Load report a missing save location instead of raising")
	{
		BasaltTest::TempProject project("ScriptStorageNowhere");
		BasaltTest::ScopedEnvVar relative(Platform::UserDataDirectoryVariable, "relative/dir");
		Scene scene;
		scene.OnRuntimeStart();
		const std::string saved = BasaltTest::Eval(scene, "local ok, err = Storage.Save() return tostring(ok) .. ' ' .. err");
		CHECK(saved.find("false Storage: no user data directory") == 0);
		const std::string loaded = BasaltTest::Eval(scene, "local ok, err = Storage.Load() return tostring(ok) .. ' ' .. err");
		CHECK(loaded.find("false Storage: no user data directory") == 0);
		// The others raise.
		CHECK(BasaltTest::Eval(scene, "Storage.Set('x', 1)").find("error: ") == 0);
		scene.OnRuntimeStop();
	}
}

namespace {

	// What Docs/basalt.d.lua declares, read line by line (see the conventions in its header).
	struct LuaDefinitions
	{
		// Class name -> field name -> the rest of the line (type, then an optional description).
		std::map<std::string, std::map<std::string, std::string>> Fields;
		// Class name -> members declared with function X.Y( or function X:Y(.
		std::map<std::string, std::set<std::string>> Functions;
		// Names declared as globals: X = {} and function X(.
		std::set<std::string> Tables;
		std::set<std::string> GlobalFunctions;

		std::set<std::string> Members(const std::string& name) const
		{
			std::set<std::string> members;
			if (auto it = Fields.find(name); it != Fields.end())
				for (const auto& [field, rest] : it->second)
					members.insert(field);
			if (auto it = Functions.find(name); it != Functions.end())
				members.insert(it->second.begin(), it->second.end());
			return members;
		}
	};

	LuaDefinitions ParseLuaDefinitions(const std::string& text)
	{
		static const std::regex s_Class(R"(^---@class (\w+))");
		static const std::regex s_Field(R"(^---@field (\w+)\?? (.*)$)");
		static const std::regex s_Table(R"(^(\w+) = \{\}$)");
		static const std::regex s_Member(R"(^function (\w+)[.:](\w+)\()");
		static const std::regex s_Function(R"(^function (\w+)\()");
		LuaDefinitions definitions;
		std::string currentClass;
		std::istringstream lines(text);
		std::string line;
		std::smatch match;
		while (std::getline(lines, line))
		{
			// Windows checkouts may convert the file to CRLF line endings.
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (std::regex_search(line, match, s_Class))
				currentClass = match[1];
			else if (std::regex_search(line, match, s_Field) && !currentClass.empty())
				definitions.Fields[currentClass][match[1]] = match[2];
			else if (!line.starts_with("---"))
				currentClass.clear();
			if (std::regex_search(line, match, s_Table))
				definitions.Tables.insert(match[1]);
			else if (std::regex_search(line, match, s_Member))
				definitions.Functions[match[1]].insert(match[2]);
			else if (std::regex_search(line, match, s_Function))
				definitions.GlobalFunctions.insert(match[1]);
		}
		return definitions;
	}

	// The LuaLS type of a component field, as scripts/generate_docs.py writes it.
	std::string LuaFieldType(const ComponentInfo& info, const std::string& field)
	{
		const ComponentFieldType& type = info.FieldTypes.at(field);
		const bool array = type.Count > 0;
		std::string scalar;
		switch (type.Kind)
		{
			case ComponentFieldKind::Number:
				scalar = "number";
				break;
			case ComponentFieldKind::Integer:
			case ComponentFieldKind::Entity:
				scalar = "integer";
				break;
			case ComponentFieldKind::Boolean:
				scalar = "boolean";
				break;
			case ComponentFieldKind::String:
				scalar = "string";
				break;
			case ComponentFieldKind::Object:
				scalar = "table";
				break;
			case ComponentFieldKind::Enum:
			{
				std::string options;
				for (const std::string& option : info.EnumOptions.at(field))
					options += (options.empty() ? "\"" : "|\"") + option + "\"";
				return array ? "(" + options + ")[]" : options;
			}
		}
		return scalar + (array ? "[]" : "");
	}

	std::string Difference(const std::set<std::string>& a, const std::set<std::string>& b)
	{
		std::string missing;
		for (const std::string& name : a)
		{
			if (!b.contains(name))
				missing += (missing.empty() ? "" : ", ") + name;
		}
		return missing;
	}

	// Every global the bindings add (Basalt's are PascalCase; Lua's own are lowercase) as "G name kind", every key
	// of a plain table as "T table key", and every member of a usertype's instances as "U type member".
	constexpr const char* IntrospectBindings = R"(
		local instances = {
			Vec2 = function() return Vec2(1, 2) end,
			Vec3 = function() return Vec3(1, 2, 3) end,
			Vec4 = function() return Vec4(1, 2, 3, 4) end,
			Quat = function() return Quat.Identity() end,
			Entity = function() return Scene.CreateEntity("LuaTypes") end,
			TaskHandle = function() return Timer.After(100, function() end) end,
			EventHandle = function() return Events.On("LuaTypes", function() end) end,
		}
		local out = {}
		for name, value in pairs(_G) do
			if type(name) == "string" and name:match("^%u") then
				if type(value) == "function" then
					out[#out + 1] = "G " .. name .. " function"
				elseif type(value) == "table" and getmetatable(value) == nil then
					out[#out + 1] = "G " .. name .. " table"
					for key in pairs(value) do out[#out + 1] = "T " .. name .. " " .. tostring(key) end
				elseif instances[name] then
					out[#out + 1] = "G " .. name .. " usertype"
					for key in pairs(getmetatable(instances[name]())) do
						if type(key) == "string" and not key:match("^__") and key ~= "class_cast" and key ~= "class_check" and key ~= "new" then
							out[#out + 1] = "U " .. name .. " " .. key
						end
					end
				else
					out[#out + 1] = "G " .. name .. " unknown"
				end
			end
		end
		return table.concat(out, "\n")
	)";

}

TEST_SUITE("Scripting")
{
	// Docs/basalt.d.lua is written by hand (signatures live only in the C++ bindings) except its component section;
	// this keeps both in sync with what the engine binds.
	TEST_CASE("Lua input actions read the project's actions; scripts rebind them for the session, through Scene.Load")
	{
		using BasaltTest::Eval;
		BasaltTest::ScopedGameInput restore;
		BasaltTest::TempProject project("ScriptInputActions");
		std::string error;
		const auto actions = InputActionMap::FromJson(
			{ { "Actions",
				{
					{ { "Name", "Jump" }, { "Bindings", { { { "Key", "Space" } } } } },
					{ { "Name", "MoveX" }, { "Type", "Axis" }, { "Bindings", { { { "Key", "D" } }, { { "Key", "A" }, { "Scale", -1 } } } } },
				} } },
			error);
		REQUIRE_MESSAGE(actions, error);
		Project::GetActive()->GetConfig().InputActions = *actions;
		Scene next("Next");
		REQUIRE(SceneSerializer::SaveScene(next, Project::ResolvePath("Assets/Scenes/Next.bscene"), error));

		Scene scene;
		CHECK_FALSE(scene.SetInputBindings("Jump", {}, error));
		CHECK(error.find("require Play mode") != std::string::npos);
		scene.OnRuntimeStart();
		CHECK(Eval(scene, "return Input.GetAction('Jump') == 0 and not Input.IsActionDown('Jump')") == "true");
		Input::SetKeyState(KeyCode::Space, true);
		Input::SetKeyState(KeyCode::A, true);
		CHECK(Eval(scene, "return Input.GetAction('Jump') == 1 and Input.IsActionDown('Jump') and Input.IsActionPressed('Jump')") == "true");
		CHECK(Eval(scene, "return Input.GetAction('MoveX') == -1 and Input.IsActionDown('MoveX')") == "true");
		Input::EndFrame();
		CHECK(Eval(scene, "return Input.IsActionPressed('Jump')") == "false");
		Input::SetKeyState(KeyCode::Space, false);
		CHECK(Eval(scene, "return Input.IsActionReleased('Jump')") == "true");
		CHECK(Eval(scene, "local a = Input.GetActions(); return #a == 2 and a[1].Name == 'Jump' and a[1].Type == 'Button' and a[2].Type == 'Axis' and a[2].Bindings[2].Scale == -1") == "true");
		CHECK(Eval(scene, "local b = Input.GetBindings('MoveX'); return #b == 2 and b[1].Key == 'D' and b[2].Key == 'A'") == "true");

		// Rebinding changes the session only: W jumps now, Space does not, and the project keeps its bindings.
		CHECK(Eval(scene, "Input.SetBindings('Jump', { { Key = 'w' }, { GamepadButton = 'A', Gamepad = 1 } }) return Input.GetBindings('Jump')[1].Key") == "W");
		Input::SetKeyState(KeyCode::Space, true);
		CHECK(Eval(scene, "return Input.IsActionDown('Jump')") == "false");
		Input::SetKeyState(KeyCode::W, true);
		CHECK(Eval(scene, "return Input.IsActionDown('Jump')") == "true");
		CHECK(Project::GetActive()->GetConfig().InputActions == *actions);
		BasaltTest::CheckLuaErrors(*scene.GetScriptEngine(),
								   std::array<std::pair<const char*, const char*>, 9>{ {
									   { "Input.SetBindings('Jump', { Key = 'Space' })", "Input.SetBindings('Jump'): expected a list of bindings, such as { { Key = 'Space' } }" },
									   { "Input.SetBindings('Jump', { { Key = 'Space', Scale = 0/0 } })", "Input.SetBindings('Jump'): binding Scale must be a number" },
									   { "Input.SetBindings('Jump', { function() end })", "Input.SetBindings('Jump'): cannot convert a Lua function to data" },
									   { "Input.GetAction('Dash')", "unknown input action 'Dash' (defined: Jump, MoveX)" },
									   { "Input.IsActionPressed('jump')", "unknown input action 'jump'" },
									   { "Input.SetBindings('Jump', { { Key = 'Nope' } })", "Input.SetBindings('Jump'): binding: unknown Key 'Nope'" },
									   { "Input.SetBindings('Jump', { { Key = 'A', Scale = 3 } })", "Scale must be a number in [-1, 1]" },
									   { "Input.SetBindings('Dash', {})", "unknown input action 'Dash'" },
									   { "Input.ResetBindings('Dash')", "unknown input action 'Dash'" },
								   } });
		// An empty table removes every binding; Scene.Load keeps the session's bindings.
		CHECK(Eval(scene, "Input.SetBindings('MoveX', {}) return #Input.GetBindings('MoveX')") == "0");
		CHECK(Eval(scene, "Scene.Load('Assets/Scenes/Next.bscene') return true") == "true");
		scene.OnUpdate(Step);
		REQUIRE(scene.GetName() == "Next");
		CHECK(scene.GetInputActions().Find("MoveX")->Bindings.empty());
		CHECK(Eval(scene, "return Input.IsActionDown('Jump')") == "true");
		// Reset puts back what play started with: one action, or all.
		CHECK(Eval(scene, "Input.ResetBindings('Jump') return Input.GetBindings('Jump')[1].Key") == "Space");
		CHECK(scene.GetInputActions().Find("MoveX")->Bindings.empty());
		CHECK(Eval(scene, "Input.ResetBindings() return #Input.GetBindings('MoveX')") == "2");
		CHECK(scene.GetInputActions() == *actions);

		CHECK(Eval(scene, "Input.SetBindings('Jump', {}) return true") == "true");
		scene.OnRuntimeStop();
		CHECK(scene.GetInputActions().IsEmpty());
		CHECK_FALSE(scene.ResetAllInputBindings(error));
		scene.OnRuntimeStart();
		CHECK(scene.GetInputActions() == *actions);
		scene.OnRuntimeStop();

		Project::GetActive()->GetConfig().InputActions = {};
		scene.OnRuntimeStart();
		CHECK(Eval(scene, "return Input.GetAction('Jump')").find("the project defines none") != std::string::npos);
		CHECK(Eval(scene, "return #Input.GetActions()") == "0");
		scene.OnRuntimeStop();
	}

	TEST_CASE("Lua reads the mouse in the host's game coordinates and only the input routed to the game")
	{
		using BasaltTest::Eval;
		BasaltTest::ScopedGameInput restore;
		Scene scene;
		scene.OnRuntimeStart();
		// The headless view: script coordinates are Input's.
		Input::SetMousePosition({ 320.0f, 240.0f });
		CHECK(Eval(scene, "return Input.GetMousePosition() == Vec2(320, 240) and Input.GetViewSize() == Vec2(1920, 1080)") == "true");

		// The editor's Game view: a 960x540 image at (100, 50) showing a 1920x1080 game.
		GameUI::SetHostState({ { 100.0f, 50.0f }, { 960.0f, 540.0f }, true, true, glm::vec2(1920.0f, 1080.0f) });
		Input::SetMousePosition({ 580.0f, 320.0f });
		Input::EndFrame();
		Input::SetMousePosition({ 581.0f, 322.0f });
		CHECK(Eval(scene, "return Input.GetMousePosition() == Vec2(962, 544)") == "true");
		CHECK(Eval(scene, "return Input.GetMouseDelta() == Vec2(2, 4)") == "true");
		CHECK(Eval(scene, "return Input.GetViewSize() == Vec2(1920, 1080)") == "true");

		// The keyboard and gamepads reach scripts only while the game has focus, the mouse only with the pointer.
		Input::SetKeyState(KeyCode::Space, true);
		Input::SetMouseButtonState(MouseCode::Left, true);
		Input::SetGamepadConnected(0, true);
		REQUIRE(Input::SetGamepadButtonState(0, GamepadButton::A, true));
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftX, 1.0f));
		CHECK(Eval(scene, "return Input.IsKeyDown('Space') and Input.IsMouseButtonDown('Left') and Input.IsGamepadButtonDown(0, 'A')") == "true");
		Input::EndFrame();
		GameUI::SetNavigationEnabled(false);
		CHECK(Eval(scene, "return Input.IsKeyDown('Space') or Input.IsGamepadButtonDown(0, 'A') or Input.GetGamepadAxis(0, 'LeftX') ~= 0") == "false");
		CHECK(Eval(scene, "return Input.IsKeyReleased('Space') and Input.IsGamepadButtonReleased(0, 'A') and Input.IsMouseButtonDown('Left')") == "true");
		CHECK(Eval(scene, "return Input.IsGamepadConnected(0)") == "true");
		GameUI::SetPointerEnabled(false);
		Input::SetMousePosition({ 600.0f, 322.0f });
		CHECK(Eval(scene, "return not Input.IsMouseButtonDown('Left') and Input.IsMouseButtonReleased('Left') and Input.GetMouseDelta() == Vec2(0, 0)") == "true");
		// The position is never withheld.
		CHECK(Eval(scene, "return Input.GetMousePosition() == Vec2(1000, 544)") == "true");
		CHECK(Eval(scene, "return Input.IsKeyPressed('Space') or Input.IsMouseButtonPressed('Left') or Input.IsGamepadButtonPressed(0, 'A')") == "false");
		scene.OnRuntimeStop();
	}

	TEST_CASE("Input.SetCursorMode stores the game's cursor; play starts and stops with Normal")
	{
		using BasaltTest::Eval;
		BasaltTest::ScopedGameInput restore;
		BasaltTest::TempProject project("ScriptCursorMode");
		Scene next("Next");
		std::string error;
		REQUIRE(SceneSerializer::SaveScene(next, Project::ResolvePath("Assets/Scenes/Next.bscene"), error));
		Input::SetCursorMode(CursorMode::Hidden);
		Scene scene;
		scene.OnRuntimeStart();
		CHECK(Input::GetCursorMode() == CursorMode::Normal);
		CHECK(Eval(scene, "return Input.GetCursorMode()") == "Normal");
		CHECK(Eval(scene, "Input.SetCursorMode('locked') return Input.GetCursorMode()") == "Locked");
		CHECK(Input::GetCursorMode() == CursorMode::Locked);
		// The cursor belongs to the play session, not to a scene: Scene.Load keeps it.
		CHECK(Eval(scene, "Scene.Load('Assets/Scenes/Next.bscene') return true") == "true");
		scene.OnUpdate(Step);
		REQUIRE(scene.GetName() == "Next");
		CHECK(Input::GetCursorMode() == CursorMode::Locked);
		std::string result;
		CHECK_FALSE(scene.GetScriptEngine()->ExecuteString("Input.SetCursorMode('Grabbed')", result));
		CHECK(result.find("unknown cursor mode 'Grabbed' (valid: Normal, Hidden, Locked)") != std::string::npos);
		CHECK(Input::GetCursorMode() == CursorMode::Locked);
		scene.OnRuntimeStop();
		CHECK(Input::GetCursorMode() == CursorMode::Normal);
	}

	TEST_CASE("LuaTypeDefinitions: Docs/basalt.d.lua declares exactly the bound Lua API and every component")
	{
		const std::optional<std::string> text = FileSystem::ReadTextFile(std::filesystem::path(BASALT_SOURCE_DIR) / "Docs" / "basalt.d.lua");
		REQUIRE(text);
		const LuaDefinitions definitions = ParseLuaDefinitions(*text);

		Scene scene;
		scene.OnRuntimeStart();
		// The definitions are Lua too: they must compile (editors would report a syntax error as broken types).
		REQUIRE(text->find("]=====]") == std::string::npos);
		CHECK(BasaltTest::Eval(scene, fmt::format("local _, problem = load([=====[{}]=====], 'basalt.d.lua') return tostring(problem)", *text)) == "nil");
		const std::string introspected = BasaltTest::Eval(scene, IntrospectBindings);
		REQUIRE_FALSE(introspected.starts_with("error: "));

		std::map<std::string, std::string> globals;
		std::map<std::string, std::set<std::string>> bound;
		std::istringstream lines(introspected);
		std::string kind, owner, member;
		while (lines >> kind >> owner >> member)
		{
			if (kind == "G")
				globals[owner] = member;
			else
				bound[owner].insert(member);
		}
		REQUIRE(globals.size() > 15);

		std::set<std::string> boundTables, boundFunctions;
		for (const auto& [name, type] : globals)
		{
			INFO("global ", name);
			CHECK_MESSAGE(type != "unknown", "a usertype without an instance in the test: add one to IntrospectBindings");
			(type == "function" ? boundFunctions : boundTables).insert(name);
		}
		// Every name in `names` must be in `in`.
		const auto checkCovered = [](const std::set<std::string>& names, const std::set<std::string>& in, const char* what) {
			const std::string missing = Difference(names, in);
			CHECK_MESSAGE(missing.empty(), what, ": ", missing);
		};
		checkCovered(boundTables, definitions.Tables, "bound but not declared as X = {}");
		checkCovered(definitions.Tables, boundTables, "declared but not bound");
		checkCovered(boundFunctions, definitions.GlobalFunctions, "bound global functions not declared");
		std::set<std::string> declaredFunctions = definitions.GlobalFunctions;
		declaredFunctions.erase("print");
		checkCovered(declaredFunctions, boundFunctions, "declared global functions not bound");

		for (const std::string& name : boundTables)
		{
			INFO("global ", name);
			const std::set<std::string> declared = definitions.Members(name);
			checkCovered(bound[name], declared, "bound but not declared");
			checkCovered(declared, bound[name], "declared but not bound");
		}
		// Static functions of usertypes (Quat.Identity) are not enumerable on the type itself: they must resolve.
		for (const auto& [name, functions] : definitions.Functions)
		{
			if (const auto global = globals.find(name); global != globals.end() && global->second == "usertype")
				for (const std::string& function : functions)
				{
					const std::string check = fmt::format("return {}.{} ~= nil", name, function);
					CHECK_MESSAGE(BasaltTest::Eval(scene, check) == "true", name, ".", function, " is not bound");
				}
		}
		scene.OnRuntimeStop();

		for (const ComponentInfo& info : ComponentRegistry::GetAll())
		{
			INFO("component ", info.Name);
			const auto declared = definitions.Fields.find(info.Name + "Component");
			REQUIRE_MESSAGE(declared != definitions.Fields.end(), "no ---@class ", info.Name, "Component: run scripts/generate_docs.py");
			std::set<std::string> fields(info.Fields.begin(), info.Fields.end());
			std::set<std::string> declaredFields;
			for (const auto& [field, type] : declared->second)
			{
				declaredFields.insert(field);
				if (fields.contains(field))
					CHECK_MESSAGE(type == LuaFieldType(info, field), field, " is declared as ", type, ", expected ", LuaFieldType(info, field));
			}
			CHECK(declaredFields == fields);
		}
	}
}

TEST_SUITE("Scripting")
{
	TEST_CASE("Hot reload moves instances onto the new class and keeps their fields")
	{
		BasaltTest::TempProject project("ScriptHotReload");
		const std::string script = project.WriteFile("Assets/Scripts/Mover.lua", R"(
			local Mover = {}
			Mover.Properties = { Speed = 1.0 }
			function Mover:OnCreate() self.Count = 0; self.Creates = (self.Creates or 0) + 1 end
			function Mover:OnUpdate(dt) self.Count = self.Count + 1 end
			return Mover
		)");
		const std::string other = project.WriteFile("Assets/Scripts/Other.lua", R"(
			local Other = {}
			function Other:OnUpdate(dt) self.Ticks = (self.Ticks or 0) + 1 end
			return Other
		)");

		Scene scene;
		Entity a = AddScripted(scene, "A", script);
		Entity b = AddScripted(scene, "B", script, { { "Bonus", 7 } });
		Entity c = AddScripted(scene, "C", other);
		scene.OnRuntimeStart();
		for (int i = 0; i < 3; i++)
			scene.OnUpdate(Step);
		REQUIRE(Field(scene, a, "Count") == 3);

		// New behaviour, a new property (with B's override), and OnReload.
		project.WriteFile(script, R"(
			local Mover = {}
			Mover.Properties = { Speed = 1.0, Bonus = 2 }
			function Mover:OnCreate() self.Count = 0; self.Creates = (self.Creates or 0) + 1 end
			function Mover:OnUpdate(dt) self.Count = self.Count + 10 end
			function Mover:OnReload() self.Reloads = (self.Reloads or 0) + 1 end
			return Mover
		)");
		const ScriptReloadResult result = scene.GetScriptEngine()->ReloadScripts({ script, "Assets/Textures/Unrelated.png" });
		CHECK(result.Reloaded == std::vector<std::string>{ script });
		CHECK(result.Failed.empty());
		CHECK(result.Instances == 2);
		CHECK(Field(scene, a, "Count") == 3);
		CHECK(Field(scene, a, "Reloads") == 1);
		CHECK(Field(scene, a, "Creates") == 1);
		CHECK(Field(scene, a, "Bonus") == 2);
		CHECK(Field(scene, b, "Bonus") == 7);
		CHECK(Field(scene, c, "Reloads").is_null());
		scene.OnUpdate(Step);
		CHECK(Field(scene, a, "Count") == 13);
		CHECK(Field(scene, c, "Ticks") == 4);
		CHECK(scene.GetScriptEngine()->GetErrors().empty());

		// A file no class uses changes nothing.
		CHECK(scene.GetScriptEngine()->ReloadScripts({ "Assets/Scripts/Unused.lua" }).Reloaded.empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("A hot reload that does not load keeps the previous version running")
	{
		BasaltTest::TempProject project("ScriptHotReloadError");
		const std::string script = project.WriteFile("Assets/Scripts/Counter.lua", R"(
			local Counter = {}
			function Counter:OnCreate() self.Count = 0 end
			function Counter:OnUpdate(dt) self.Count = self.Count + 1 end
			return Counter
		)");
		Scene scene;
		Entity entity = AddScripted(scene, "Counter", script);
		scene.OnRuntimeStart();
		scene.OnUpdate(Step);

		for (const char* broken : { "local = = 5", "error('top level fails')", "return 42" })
		{
			CAPTURE(broken);
			scene.GetScriptEngine()->ClearErrors();
			project.WriteFile(script, broken);
			const ScriptReloadResult result = scene.GetScriptEngine()->ReloadScripts({ script });
			CHECK(result.Reloaded.empty());
			REQUIRE(result.Failed.size() == 1);
			CHECK(result.Failed[0].first == script);
			REQUIRE(scene.GetScriptEngine()->GetErrors().size() == 1);
			CHECK(scene.GetScriptEngine()->GetErrors()[0].find("previous version keeps running") != std::string::npos);
		}
		const nlohmann::json before = Field(scene, entity, "Count");
		scene.OnUpdate(Step);
		CHECK(Field(scene, entity, "Count") == before.get<int>() + 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Hot reload revives disabled instances and starts scripts that failed to load")
	{
		BasaltTest::TempProject project("ScriptHotReloadRevive");
		const std::string crashing = project.WriteFile("Assets/Scripts/Crashing.lua", R"(
			local Crashing = {}
			function Crashing:OnCreate() self.Count = 0 end
			function Crashing:OnUpdate(dt) local x = nil; return x.field end
			return Crashing
		)");
		const std::string syntax = project.WriteFile("Assets/Scripts/Syntax.lua", "local = = 5");
		Scene scene;
		Entity crash = AddScripted(scene, "Crashing", crashing);
		Entity late = AddScripted(scene, "Late", syntax);
		scene.OnRuntimeStart();
		scene.OnUpdate(Step);
		scene.OnUpdate(Step);
		CHECK(scene.GetScriptEngine()->GetErrors().size() == 2);
		CHECK_FALSE(scene.GetScriptEngine()->HasInstance(late));

		project.WriteFile(crashing, R"(
			local Crashing = {}
			function Crashing:OnCreate() self.Count = 0 end
			function Crashing:OnUpdate(dt) self.Count = self.Count + 1 end
			function Crashing:OnReload() self.Reloaded = true end
			return Crashing
		)");
		project.WriteFile(syntax, R"(
			local Late = {}
			function Late:OnCreate() self.Started = true end
			return Late
		)");
		const ScriptReloadResult result = scene.GetScriptEngine()->ReloadScripts({ crashing, syntax });
		CHECK(result.Reloaded == std::vector<std::string>{ crashing, syntax });
		CHECK(result.Instances == 2);
		CHECK(Field(scene, crash, "Reloaded") == true);
		CHECK(Field(scene, late, "Started") == true);
		scene.OnUpdate(Step);
		CHECK(Field(scene, crash, "Count") == 1);
		CHECK(scene.GetScriptEngine()->GetErrors().size() == 2);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Hot reload of a required module reloads the scripts")
	{
		BasaltTest::TempProject project("ScriptHotReloadModule");
		const std::string module = project.WriteFile("Assets/Scripts/Tuning.lua", "return { Step = 1 }");
		const std::string script = project.WriteFile("Assets/Scripts/User.lua", R"(
			local Tuning = require("Assets.Scripts.Tuning")
			local User = {}
			function User:OnCreate() self.Total = 0 end
			function User:OnUpdate(dt) self.Total = self.Total + Tuning.Step end
			return User
		)");
		Scene scene;
		Entity entity = AddScripted(scene, "User", script);
		scene.OnRuntimeStart();
		scene.OnUpdate(Step);
		REQUIRE(Field(scene, entity, "Total") == 1);

		project.WriteFile(module, "return { Step = 100 }");
		// The module is a changed file no instance runs directly: the classes that may require it load again.
		const ScriptReloadResult result = scene.GetScriptEngine()->ReloadScripts({ module });
		CHECK(result.Reloaded == std::vector<std::string>{ script });
		scene.OnUpdate(Step);
		CHECK(Field(scene, entity, "Total") == 101);
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}
}
