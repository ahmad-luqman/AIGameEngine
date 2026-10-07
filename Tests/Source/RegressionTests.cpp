// Regression tests for defects found in code review. Each test names the behaviour it protects.
#include <doctest/doctest.h>

#include <Basalt/Math/Math.h>
#include <Basalt/Physics/PhysicsWorld.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/Scene/SceneSerializer.h>
#include <Basalt/Scripting/ScriptEngine.h>

#include "TestUtils.h"

#include <glm/gtc/epsilon.hpp>

using namespace Basalt;

namespace {

	constexpr float Step = 1.0f / 60.0f;

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

	const char* s_ContactCounter = R"(
		local Counter = {}
		function Counter:OnCreate() self.Begin = 0; self.End = 0; self.Enter = 0; self.Exit = 0 end
		function Counter:OnCollisionBegin(other) self.Begin = self.Begin + 1 end
		function Counter:OnCollisionEnd(other) self.End = self.End + 1 end
		function Counter:OnTriggerEnter(other) self.Enter = self.Enter + 1 end
		function Counter:OnTriggerExit(other) self.Exit = self.Exit + 1 end
		return Counter
	)";

}

TEST_SUITE("Regressions")
{
	TEST_CASE("Destroying entities from OnDestroy neither recurses nor touches destroyed entities")
	{
		BasaltTest::TempProject project("RegressionOnDestroy");
		const std::string killParent = project.WriteFile("Assets/Scripts/KillParent.lua", R"(
			local K = {}
			function K:OnDestroy()
				Destroyed = (Destroyed or 0) + 1
				local parent = self.Entity:GetParent()
				if parent then Scene.Destroy(parent) end
				self.Entity:Destroy()
			end
			return K
		)");

		Scene scene;
		Entity parent = scene.CreateEntity("Parent");
		Entity child = AddScripted(scene, "Child", killParent);
		scene.SetParent(child, parent, false);
		Entity bystander = scene.CreateEntity("Bystander");

		scene.OnRuntimeStart();
		scene.DestroyEntity(parent);
		CHECK_FALSE(parent.IsValid());
		CHECK_FALSE(child.IsValid());
		CHECK(bystander.IsValid());
		std::string result;
		REQUIRE(scene.GetScriptEngine()->ExecuteString("Destroyed", result));
		CHECK(result == "1");
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Removing a ScriptComponent stops the script and runs OnDestroy")
	{
		BasaltTest::TempProject project("RegressionRemoveScript");
		const std::string script = project.WriteFile("Assets/Scripts/Ticker.lua", R"(
			local T = {}
			function T:OnUpdate(dt) Ticks = (Ticks or 0) + 1 end
			function T:OnDestroy() TornDown = true end
			return T
		)");
		const std::string remover = project.WriteFile("Assets/Scripts/Remover.lua", R"(
			local R = {}
			function R:OnUpdate(dt)
				if Time.GetFrame() == 2 then Scene.FindEntityByName("Ticker"):RemoveComponent("Script") end
			end
			return R
		)");

		Scene scene;
		AddScripted(scene, "Remover", remover);
		Entity ticker = AddScripted(scene, "Ticker", script);
		scene.OnRuntimeStart();
		for (int i = 0; i < 10; i++)
			scene.OnUpdate(Step);

		std::string ticks;
		REQUIRE(scene.GetScriptEngine()->ExecuteString("Ticks", ticks));
		CHECK(ticks == "3");
		std::string tornDown;
		REQUIRE(scene.GetScriptEngine()->ExecuteString("TornDown", tornDown));
		CHECK(tornDown == "true");
		CHECK_FALSE(scene.GetScriptEngine()->HasInstance(ticker));
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Resting (sleeping) bodies keep their contacts: no spurious End/Exit")
	{
		BasaltTest::TempProject project("RegressionSleep");
		const std::string counter = project.WriteFile("Assets/Scripts/Counter.lua", s_ContactCounter);

		Scene scene;
		Entity ground = AddScripted(scene, "Ground", counter);
		ground.GetTransform().Translation = { 0.0f, -0.5f, 0.0f };
		ground.AddComponent<RigidBodyComponent>();
		ground.AddComponent<BoxColliderComponent>().HalfExtents = { 10.0f, 0.5f, 10.0f };

		Entity zone = AddScripted(scene, "Zone", counter);
		zone.GetTransform().Translation = { 3.0f, 0.5f, 0.0f };
		zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
		zone.AddComponent<BoxColliderComponent>().HalfExtents = { 1.0f, 1.0f, 1.0f };

		Entity box = AddScripted(scene, "Box", counter);
		box.GetTransform().Translation = { 3.0f, 0.6f, 0.0f };
		box.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		box.AddComponent<BoxColliderComponent>();

		scene.OnRuntimeStart();
		for (int i = 0; i < 60 * 6; i++)
			scene.OnUpdate(Step);

		CHECK(Field(scene, ground, "Begin") == 1);
		CHECK(Field(scene, ground, "End") == 0);
		CHECK(Field(scene, zone, "Enter") == 1);
		CHECK(Field(scene, zone, "Exit") == 0);

		// Leaving for real still ends the contacts.
		box.GetTransform().Translation = { 3.0f, 20.0f, 0.0f };
		for (int i = 0; i < 10; i++)
			scene.OnUpdate(Step);
		CHECK(Field(scene, ground, "End") == 1);
		CHECK(Field(scene, zone, "Exit") == 1);
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Rebuilding or destroying a body keeps contact begin/end balanced")
	{
		BasaltTest::TempProject project("RegressionRebuild");
		const std::string counter = project.WriteFile("Assets/Scripts/Counter.lua", s_ContactCounter);

		Scene scene;
		Entity ground = AddScripted(scene, "Ground", counter);
		ground.GetTransform().Translation = { 0.0f, -0.5f, 0.0f };
		ground.AddComponent<RigidBodyComponent>();
		ground.AddComponent<BoxColliderComponent>().HalfExtents = { 10.0f, 0.5f, 10.0f };
		Entity ball = scene.CreateEntity("Ball");
		ball.GetTransform().Translation = { 0.0f, 1.0f, 0.0f };
		ball.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		ball.AddComponent<SphereColliderComponent>();

		scene.OnRuntimeStart();
		for (int i = 0; i < 60; i++)
			scene.OnUpdate(Step);
		REQUIRE(Field(scene, ground, "Begin") == 1);

		// A rigid body patch rebuilds the body; contacts may end and begin again, but never go stale.
		std::string error;
		REQUIRE(ComponentRegistry::AddOrPatch(ball, "RigidBody", { { "Friction", 0.2 } }, error));
		for (int i = 0; i < 30; i++)
			scene.OnUpdate(Step);
		const int begins = Field(scene, ground, "Begin").get<int>();
		const int ends = Field(scene, ground, "End").get<int>();
		CHECK(begins - ends == 1);

		// Destroying the ball ends its contact with the ground.
		scene.DestroyEntity(ball);
		CHECK(Field(scene, ground, "End").get<int>() == begins);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Transforms decompose at tiny scales and with mirroring")
	{
		glm::vec3 translation;
		glm::quat rotation;
		glm::vec3 scale;
		const glm::quat expected = Math::QuatFromEuler(glm::radians(glm::vec3(10.0f, 20.0f, 30.0f)));
		REQUIRE(Math::DecomposeTransform(Math::ComposeTransform({ 1, 2, 3 }, expected, glm::vec3(0.001f)), translation, rotation, scale));
		CHECK(scale.x == doctest::Approx(0.001f));
		CHECK(glm::abs(glm::dot(rotation, expected)) == doctest::Approx(1.0f));

		REQUIRE(Math::DecomposeTransform(Math::ComposeTransform({ 0, 0, 0 }, expected, { -2.0f, 3.0f, 4.0f }), translation, rotation, scale));
		CHECK(scale.x == doctest::Approx(-2.0f));
		CHECK(scale.y == doctest::Approx(3.0f));

		CHECK_FALSE(Math::DecomposeTransform(glm::mat4(0.0f), translation, rotation, scale));
		CHECK(rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

		Scene scene;
		Entity parent = scene.CreateEntity("Tiny");
		parent.GetTransform().Scale = glm::vec3(0.01f);
		Entity child = scene.CreateEntity("Child");
		child.GetTransform().Translation = { 1.0f, 0.0f, 0.0f };
		child.GetTransform().Scale = glm::vec3(0.1f);
		REQUIRE(scene.SetParent(child, parent, true));
		CHECK(glm::all(glm::epsilonEqual(glm::vec3(scene.GetWorldTransform(child)[3]), glm::vec3(1.0f, 0.0f, 0.0f), 1e-4f)));
		CHECK(child.GetTransform().Scale.x == doctest::Approx(10.0f));
	}

	TEST_CASE("Scene files reject wrong types, out-of-range settings, unknown fields and parent cycles")
	{
		Scene scene;
		std::string error;
		CHECK_FALSE(SceneSerializer::DeserializeScene(scene, { { "Physics", { { "MaxStepsPerFrame", -1 } } } }, error));
		CHECK(error.find("MaxStepsPerFrame") != std::string::npos);
		CHECK_FALSE(SceneSerializer::DeserializeScene(scene, { { "Renderer", { { "Exposure", true } } } }, error));
		CHECK_FALSE(SceneSerializer::DeserializeScene(scene, { { "Renderer", { { "Exposur", 1.0 } } } }, error));
		CHECK(error.find("unknown field 'Exposur'") != std::string::npos);
		CHECK_FALSE(SceneSerializer::DeserializeScene(scene, { { "Version", 1.5 } }, error));
		CHECK_FALSE(SceneSerializer::DeserializeScene(scene, { { "Entites", nlohmann::json::array() } }, error));
		CHECK_FALSE(SceneSerializer::DeserializeScene(scene, { { "Physics", { { "FixedTimestep", 0 } } } }, error));
		CHECK_FALSE(SceneSerializer::DeserializeScene(scene, { { "Entities", { { { "ID", 1 }, { "Component", nlohmann::json::object() } } } } }, error));
		CHECK(error.find("unknown field 'Component'") != std::string::npos);

		Scene cyclic;
		const nlohmann::json cycle = { { "Entities", { { { "ID", 1 }, { "Parent", 2 } }, { { "ID", 2 }, { "Parent", 1 } } } } };
		CHECK_FALSE(SceneSerializer::DeserializeScene(cyclic, cycle, error));
		CHECK(error.find("invalid parent") != std::string::npos);
	}

	TEST_CASE("Camera components reject degenerate projections")
	{
		Scene scene;
		Entity entity = scene.CreateEntity();
		std::string error;
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Camera", { { "FOV", 0 } }, error));
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Camera", { { "Near", 10 }, { "Far", 5 } }, error));
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Camera", { { "OrthoSize", -1 } }, error));
		CHECK_FALSE(entity.HasComponent<CameraComponent>());
		REQUIRE(ComponentRegistry::AddOrPatch(entity, "Camera", { { "FOV", 75 } }, error));
		CHECK(glm::degrees(entity.GetComponent<CameraComponent>().Camera.GetPerspectiveVerticalFov()) == doctest::Approx(75.0f));
	}

	TEST_CASE("Duplicating a scripted entity while playing instantiates its script")
	{
		BasaltTest::TempProject project("RegressionDuplicate");
		const std::string script = project.WriteFile("Assets/Scripts/Counter.lua", R"(
			local C = {}
			function C:OnCreate() self.Updates = 0 end
			function C:OnUpdate(dt) self.Updates = self.Updates + 1 end
			return C
		)");
		Scene scene;
		Entity original = AddScripted(scene, "Original", script);
		scene.OnRuntimeStart();
		Entity copy = scene.DuplicateEntity(original);
		scene.OnUpdate(Step);
		scene.OnUpdate(Step);
		CHECK(Field(scene, original, "Updates") == 2);
		CHECK(Field(scene, copy, "Updates") == 2);

		// Copying a running scene produces an independent edit-mode scene.
		Ref<Scene> sceneRef = CreateRef<Scene>();
		Ref<Scene> copied = Scene::Copy(sceneRef);
		CHECK_FALSE(copied->IsRunning());
		scene.OnRuntimeStop();
	}
}
