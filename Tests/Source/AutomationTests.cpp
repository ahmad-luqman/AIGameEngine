#include <doctest/doctest.h>

#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/Input.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/UI/GameUI.h>

#include "TestUtils.h"

using namespace Basalt;
using json = nlohmann::json;

namespace {

	// Runs one command and returns its result; fails the test with the error message otherwise.
	json Run(const CommandRegistry& registry, AutomationSession& session, const std::string& command, const json& params = json::object())
	{
		const json response = registry.HandleRequest(session, { { "id", 7 }, { "command", command }, { "params", params } });
		INFO(command << ": " << response.dump());
		REQUIRE(response["ok"] == true);
		CHECK(response["id"] == 7);
		return response["result"];
	}

	std::string RunError(const CommandRegistry& registry, AutomationSession& session, const std::string& command, const json& params = json::object())
	{
		const json response = registry.HandleRequest(session, { { "command", command }, { "params", params } });
		REQUIRE(response["ok"] == false);
		return response["error"].get<std::string>();
	}

}

TEST_SUITE("Automation")
{
	TEST_CASE("Protocol: unknown commands, malformed requests, batches")
	{
		CommandRegistry registry;
		AutomationSession session;
		CHECK(RunError(registry, session, "does.not.exist").find("unknown command") != std::string::npos);
		const json bad = registry.HandleRequest(session, json::array({ 42, { { "command", "help" } } }));
		REQUIRE(bad.is_array());
		CHECK(bad[0]["ok"] == false);
		CHECK(bad[1]["ok"] == true);
		CHECK(Run(registry, session, "help")["commands"].size() == registry.GetCommands().size());
		CHECK(Run(registry, session, "component.types")["components"].size() > 10);
		CHECK(RunError(registry, session, "entity.create", { { "name", 5 } }).find("must be a string") != std::string::npos);
	}

	// Regression: nested batches recursed once per nesting level, so deep input could overflow the stack.
	TEST_CASE("Nested batches are rejected without recursion")
	{
		CommandRegistry registry;
		AutomationSession session;
		// Built by moving (copying a json value is itself recursive); parsed requests are depth-limited by
		// ParseJson, but HandleRequest must not depend on that.
		json deep = json::array();
		deep.push_back({ { "command", "help" } });
		for (int i = 0; i < 100000; i++)
		{
			json wrapper = json::array();
			wrapper.push_back(std::move(deep));
			deep = std::move(wrapper);
		}
		json batch = json::array();
		batch.push_back(std::move(deep));
		batch.push_back({ { "command", "scene.hash" } });
		const json responses = registry.HandleRequest(session, batch);
		REQUIRE(responses.size() == 2);
		CHECK(responses[0]["ok"] == false);
		CHECK(responses[0]["error"] == "batches cannot be nested");
		CHECK(responses[1]["ok"] == true);
	}

	TEST_CASE("The render scale is a validated project setting that survives save and load")
	{
		BasaltTest::TempProject temp("RenderScaleProject");
		REQUIRE(temp.IsValid());
		CommandRegistry registry;
		AutomationSession session;
		CHECK(Run(registry, session, "project.info")["renderScale"] == 1.0);
		Run(registry, session, "project.set", { { "renderScale", 0.75 } });
		CHECK(Run(registry, session, "project.info")["renderScale"] == 0.75);
		for (const json& bad : { json(0.1), json(2.5), json("half"), json(-1) })
		{
			INFO(bad.dump());
			CHECK(RunError(registry, session, "project.set", { { "renderScale", bad } }).find("renderScale") != std::string::npos);
		}
		CHECK(Run(registry, session, "project.info")["renderScale"] == 0.75);

		std::string error;
		const Ref<Project> loaded = Project::Load(temp.GetDirectory(), error);
		REQUIRE(loaded);
		CHECK(loaded->GetConfig().RenderScale == 0.75f);

		// Older project files have no RenderScale: full resolution.
		temp.WriteFile(Project::FileName, R"({"Name": "Old"})");
		REQUIRE(Project::Load(temp.GetDirectory(), error));
		CHECK(Project::Load(temp.GetDirectory(), error)->GetConfig().RenderScale == 1.0f);
		for (const char* bad : { R"({"RenderScale": 0})", R"({"RenderScale": 3})", R"({"RenderScale": "1"})", R"({"RenderScale": 1e300})" })
		{
			INFO(bad);
			temp.WriteFile(Project::FileName, bad);
			CHECK_FALSE(Project::Load(temp.GetDirectory(), error));
			CHECK(error.find("RenderScale") != std::string::npos);
		}
	}

	TEST_CASE("The random seed is a validated project setting that survives save and load")
	{
		BasaltTest::TempProject temp("RandomSeedProject");
		REQUIRE(temp.IsValid());
		CommandRegistry registry;
		AutomationSession session;
		CHECK(Run(registry, session, "project.info")["randomSeed"] == Random::DefaultSeed);
		CHECK(Project::GetActiveRandomSeed() == Random::DefaultSeed);
		Run(registry, session, "project.set", { { "randomSeed", 4294967295u } });
		CHECK(Run(registry, session, "project.info")["randomSeed"] == 4294967295u);
		CHECK(Project::GetActiveRandomSeed() == 4294967295u);
		for (const json& bad : { json(-1), json(4294967296), json(1.5), json("7") })
		{
			INFO(bad.dump());
			CHECK(RunError(registry, session, "project.set", { { "randomSeed", bad } }).find("randomSeed") != std::string::npos);
		}
		Run(registry, session, "project.set", { { "randomSeed", 7 } });

		std::string error;
		const Ref<Project> loaded = Project::Load(temp.GetDirectory(), error);
		REQUIRE(loaded);
		CHECK(loaded->GetConfig().RandomSeed == 7u);

		// Older project files have no seed: the default, so their games play as before.
		temp.WriteFile(Project::FileName, R"({"Name": "Old"})");
		REQUIRE(Project::Load(temp.GetDirectory(), error));
		CHECK(Project::Load(temp.GetDirectory(), error)->GetConfig().RandomSeed == Random::DefaultSeed);
		temp.WriteFile(Project::FileName, R"({"RandomSeed": 0})");
		REQUIRE(Project::Load(temp.GetDirectory(), error));
		CHECK(Project::Load(temp.GetDirectory(), error)->GetConfig().RandomSeed == 0u);
		temp.WriteFile(Project::FileName, R"({"RandomSeed": 4294967295})");
		REQUIRE(Project::Load(temp.GetDirectory(), error));
		CHECK(Project::Load(temp.GetDirectory(), error)->GetConfig().RandomSeed == 4294967295u);
		for (const char* bad : { R"({"RandomSeed": -1})", R"({"RandomSeed": 4294967296})", R"({"RandomSeed": 18446744073709551615})", R"({"RandomSeed": 2.5})", R"({"RandomSeed": "1"})" })
		{
			INFO(bad);
			temp.WriteFile(Project::FileName, bad);
			CHECK_FALSE(Project::Load(temp.GetDirectory(), error));
			CHECK(error.find("RandomSeed") != std::string::npos);
		}
		Project::SetActive(nullptr);
		CHECK(Project::GetActiveRandomSeed() == Random::DefaultSeed);
	}

	TEST_CASE("Dynamic resolution settings are validated project settings that survive save and load")
	{
		BasaltTest::TempProject temp("DynamicResolutionProject");
		REQUIRE(temp.IsValid());
		CommandRegistry registry;
		AutomationSession session;
		json info = Run(registry, session, "project.info");
		CHECK(info["dynamicResolution"] == false);
		CHECK(info["targetFrameRate"] == 60.0);
		CHECK(info["minRenderScale"] == 0.5);

		CHECK_FALSE(Run(registry, session, "project.set", { { "dynamicResolution", true }, { "targetFrameRate", 120 }, { "minRenderScale", 0.75 } }).contains("warnings"));
		info = Run(registry, session, "project.info");
		CHECK(info["dynamicResolution"] == true);
		CHECK(info["targetFrameRate"] == 120.0);
		CHECK(info["minRenderScale"] == 0.75);

		// A bad value anywhere leaves every setting untouched.
		const std::vector<std::pair<const char*, json>> bad = { { "dynamicResolution", json(1) }, { "targetFrameRate", json(5) }, { "targetFrameRate", json(2000) }, { "targetFrameRate", json("60") }, { "minRenderScale", json(0.1) }, { "minRenderScale", json(3) } };
		for (const auto& [name, value] : bad)
		{
			INFO(name << " = " << value.dump());
			CHECK(RunError(registry, session, "project.set", { { "renderScale", 0.5 }, { name, value } }).find(name) != std::string::npos);
		}
		info = Run(registry, session, "project.info");
		CHECK(info["renderScale"] == 1.0);
		CHECK(info["dynamicResolution"] == true);

		std::string error;
		const Ref<Project> loaded = Project::Load(temp.GetDirectory(), error);
		REQUIRE(loaded);
		const DynamicResolutionSettings expected{ true, 120.0f, 0.75f };
		CHECK(loaded->GetConfig().DynamicResolution == expected);

		// Valid but useless: a minimum at or above the render scale leaves nothing to adjust, which headless
		// hosts (no renderer to warn) learn from the result.
		const json warned = Run(registry, session, "project.set", { { "renderScale", 0.75 } });
		REQUIRE(warned.contains("warnings"));
		CHECK(warned["warnings"][0].get<std::string>().find("nothing to adjust") != std::string::npos);
		CHECK_FALSE(Run(registry, session, "project.set", { { "renderScale", 1.0 } }).contains("warnings"));

		// Older project files have none of the keys: off, 60 frames per second, half resolution at worst.
		temp.WriteFile(Project::FileName, R"({"Name": "Old"})");
		const Ref<Project> old = Project::Load(temp.GetDirectory(), error);
		REQUIRE(old);
		CHECK(old->GetConfig().DynamicResolution == DynamicResolutionSettings{});
		for (const char* file : { R"({"DynamicResolution": 1})", R"({"DynamicResolution": {"Enabled": true}})", R"({"TargetFrameRate": 0})", R"({"TargetFrameRate": 1e300})", R"({"TargetFrameRate": "60"})", R"({"MinRenderScale": 0.2})", R"({"MinRenderScale": null})" })
		{
			INFO(file);
			temp.WriteFile(Project::FileName, file);
			CHECK_FALSE(Project::Load(temp.GetDirectory(), error));
			CHECK(error.find("invalid project file") != std::string::npos);
			// The message names the key.
			CHECK((error.find("DynamicResolution") != std::string::npos || error.find("TargetFrameRate") != std::string::npos || error.find("MinRenderScale") != std::string::npos));
		}
		// The range bounds themselves are valid.
		for (const char* file : { R"({"TargetFrameRate": 10, "MinRenderScale": 0.25})", R"({"TargetFrameRate": 1000, "MinRenderScale": 2})" })
		{
			INFO(file);
			temp.WriteFile(Project::FileName, file);
			CHECK(Project::Load(temp.GetDirectory(), error));
		}
	}

	TEST_CASE("render.stats reports the host's renderer statistics and needs a GPU host")
	{
		CommandRegistry registry;
		AutomationSession session;
		CHECK(RunError(registry, session, "render.stats").find("GPU host") != std::string::npos);
		CHECK(RunError(registry, session, "render.stats").find("'cli'") != std::string::npos);

		session.RenderStats = [](json& stats, std::string&) {
			stats = { { "drawCalls", 3 } };
			return true;
		};
		CHECK(Run(registry, session, "render.stats")["drawCalls"] == 3);
		CHECK(RunError(registry, session, "render.stats", { { "frames", 1 } }).find("frames") != std::string::npos);
		session.RenderStats = [](json&, std::string& error) {
			error = "renderer gone";
			return false;
		};
		CHECK(RunError(registry, session, "render.stats") == "renderer gone");
	}

	TEST_CASE("Build, save, reopen and export a project through commands")
	{
		BasaltTest::TempProject temp("AutomationProject");
		Project::SetActive(nullptr);
		const auto directory = temp.GetDirectory() / "Game";
		CommandRegistry registry;
		AutomationSession session;

		Run(registry, session, "project.create", { { "path", directory.string() }, { "name", "Demo" } });
		Run(registry, session, "scene.new", { { "name", "Main" } });
		CHECK(Run(registry, session, "entity.list")["entities"].size() == 3);

		const json player = Run(registry, session, "entity.create", { { "name", "Player" }, { "components", { { "Mesh", { { "Mesh", "builtin://Capsule" } } }, { "Transform", { { "Translation", { 0, 1, 0 } } } } } } });
		CHECK(player["components"].size() == 3);
		const json gun = Run(registry, session, "entity.create", { { "name", "Gun" }, { "parent", "Player" } });
		CHECK(gun["parent"] == player["id"]);

		Run(registry, session, "component.set", { { "entity", "Player" }, { "component", "RigidBody" }, { "data", { { "Type", "Dynamic" }, { "Mass", 70 } } } });
		CHECK(Run(registry, session, "component.get", { { "entity", player["id"] }, { "component", "RigidBody" } })["Mass"] == 70.0);
		CHECK(RunError(registry, session, "component.set", { { "entity", "Player" }, { "component", "RigidBody" }, { "data", { { "Mas", 1 } } } }).find("unknown field") != std::string::npos);
		CHECK(RunError(registry, session, "component.remove", { { "entity", "Player" }, { "component", "Transform" } }).find("cannot be removed") != std::string::npos);

		// Entity reference fields accept names as well as IDs; files keep the ID.
		const json joint = Run(registry, session, "component.set", { { "entity", "Gun" }, { "component", "Joint" }, { "data", { { "ConnectedEntity", "Player" } } } });
		CHECK(joint["ConnectedEntity"] == player["id"]);
		const json door = Run(registry, session, "entity.create", { { "name", "Door" }, { "components", { { "Joint", { { "Type", "Hinge" }, { "ConnectedEntity", "Player" } } } } } });
		CHECK(Run(registry, session, "component.get", { { "entity", door["id"] }, { "component", "Joint" } })["ConnectedEntity"] == player["id"]);
		CHECK(RunError(registry, session, "component.set", { { "entity", "Gun" }, { "component", "Joint" }, { "data", { { "ConnectedEntity", "Nobody" } } } }).find("no entity named") != std::string::npos);
		// A joint entity names the body it moves the same way.
		const json rope = Run(registry, session, "entity.create", { { "name", "Rope" }, { "parent", "Player" }, { "components", { { "Joint", { { "BodyEntity", "Player" }, { "ConnectedEntity", "Gun" } } } } } });
		const json ropeJoint = Run(registry, session, "component.get", { { "entity", rope["id"] }, { "component", "Joint" } });
		CHECK(ropeJoint["BodyEntity"] == player["id"]);
		CHECK(ropeJoint["ConnectedEntity"] == gun["id"]);
		CHECK(RunError(registry, session, "component.set", { { "entity", "Rope" }, { "component", "Joint" }, { "data", { { "BodyEntity", "Nobody" } } } }).find("no entity named") != std::string::npos);
		Run(registry, session, "entity.destroy", { { "entity", "Rope" } });
		const size_t entityCount = Run(registry, session, "entity.list")["entities"].size();
		CHECK(RunError(registry, session, "entity.create", { { "name", "Orphan" }, { "components", { { "Joint", { { "ConnectedEntity", "Nobody" } } } } } }).find("no entity named") != std::string::npos);
		CHECK(Run(registry, session, "entity.list")["entities"].size() == entityCount);
		// Numeric IDs are checked too; 0 means "none" (the world for joints).
		CHECK(RunError(registry, session, "component.set", { { "entity", "Gun" }, { "component", "Joint" }, { "data", { { "ConnectedEntity", 123456789 } } } }).find("no entity with ID") != std::string::npos);
		CHECK(Run(registry, session, "component.set", { { "entity", "Gun" }, { "component", "Joint" }, { "data", { { "ConnectedEntity", 0 } } } })["ConnectedEntity"] == 0);
		// A name in the components of entity.create never resolves to the entity being created.
		CHECK(RunError(registry, session, "entity.create", { { "name", "Self" }, { "components", { { "Joint", { { "ConnectedEntity", "Self" } } } } } }).find("no entity named 'Self'") != std::string::npos);
		Run(registry, session, "entity.destroy", { { "entity", "Door" } });
		CHECK(RunError(registry, session, "entity.get", { { "entity", "Nobody" } }).find("no entity named") != std::string::npos);
		CHECK(RunError(registry, session, "entity.set_parent", { { "entity", "Player" }, { "parent", "Gun" } }).find("descendants") != std::string::npos);

		Run(registry, session, "entity.duplicate", { { "entity", "Gun" }, { "name", "Gun2" } });
		Run(registry, session, "entity.rename", { { "entity", "Gun2" }, { "name", "Sword" } });
		Run(registry, session, "prefab.create", { { "entity", "Player" }, { "path", "Assets/Prefabs/Player.bprefab" } });
		const json clone = Run(registry, session, "prefab.instantiate", { { "path", "Assets/Prefabs/Player.bprefab" }, { "position", { 5, 0, 0 } } });
		CHECK(Run(registry, session, "entity.transform", { { "entity", clone["id"] } })["world"]["Translation"][0] == 5.0);
		Run(registry, session, "scene.settings", { { "Renderer", { { "Exposure", 1.5 } } }, { "Physics", { { "Gravity", { 0, -20, 0 } } } } });
		CHECK(RunError(registry, session, "scene.settings", { { "Renderer", { { "Exposur", 1 } } } }).find("Exposur") != std::string::npos);

		CHECK(Run(registry, session, "scene.info")["dirty"] == true);
		CHECK(RunError(registry, session, "scene.save").find("never been saved") != std::string::npos);
		CHECK(RunError(registry, session, "scene.save", { { "path", "../escape.bscene" } }).find("inside the project") != std::string::npos);
		Run(registry, session, "scene.save", { { "path", "Assets/Scenes/Main.bscene" } });
		Run(registry, session, "project.set", { { "startScene", "Assets/Scenes/Main.bscene" } });
		const json layers = json::parse(R"({"Names": ["Default", "Player", "Pickup"], "IgnoredPairs": [["Player", "Pickup"]]})");
		Run(registry, session, "project.set", { { "physicsLayers", layers } });
		CHECK(Run(registry, session, "project.info")["physicsLayers"] == layers);
		CHECK(RunError(registry, session, "project.set", { { "name", "Renamed" }, { "physicsLayers", { { "Names", { "Player" } } } } }).find("Default") != std::string::npos);
		CHECK(Run(registry, session, "project.info")["name"] == "Demo");
		// Every parameter is validated before any is applied.
		CHECK(RunError(registry, session, "project.set", { { "name", "Renamed" }, { "windowWidth", 1 } }).find("windowWidth") != std::string::npos);
		CHECK(Run(registry, session, "project.info")["name"] == "Demo");
		// Removing a layer that a body uses is reported right away, by project.set and scene.info.
		Run(registry, session, "component.set", { { "entity", player["id"] }, { "component", "RigidBody" }, { "data", { { "Layer", "Pickup" } } } });
		CHECK(Run(registry, session, "scene.info")["unknownPhysicsLayers"].empty());
		const json stranded = Run(registry, session, "project.set", { { "physicsLayers", { { "Names", { "Default", "Player" } } } } })["unknownPhysicsLayers"];
		REQUIRE(stranded.size() == 1);
		CHECK(stranded[0]["name"] == "Player");
		CHECK(stranded[0]["layer"] == "Pickup");
		CHECK(stranded[0]["entity"] == player["id"]);
		CHECK(Run(registry, session, "scene.info")["unknownPhysicsLayers"] == stranded);
		// A character controller replaces the RigidBody, so only the controller's layer counts.
		Run(registry, session, "component.set", { { "entity", player["id"] }, { "component", "CharacterController" }, { "data", { { "Layer", "Player" } } } });
		CHECK(Run(registry, session, "scene.info")["unknownPhysicsLayers"].empty());
		Run(registry, session, "component.remove", { { "entity", player["id"] }, { "component", "CharacterController" } });
		CHECK(Run(registry, session, "scene.info")["unknownPhysicsLayers"] == stranded);
		CHECK(Run(registry, session, "project.set", { { "physicsLayers", layers } })["unknownPhysicsLayers"].empty());

		Run(registry, session, "asset.write", { { "path", "Assets/Scripts/Hello.lua" }, { "content", "return { Properties = { Speed = 3 } }" } });
		CHECK(Run(registry, session, "script.check", { { "path", "Assets/Scripts/Hello.lua" } })["properties"]["Speed"] == 3);
		CHECK(Run(registry, session, "asset.read", { { "path", "Assets/Scripts/Hello.lua" } })["content"].get<std::string>().find("Speed") != std::string::npos);
		CHECK(Run(registry, session, "asset.list", { { "extensions", { ".lua" } } })["assets"].size() == 1);

		// Reopen from disk.
		AutomationSession reopened;
		const json opened = Run(registry, reopened, "project.open", { { "path", directory.string() } });
		CHECK(opened["scene"] == "Assets/Scenes/Main.bscene");
		CHECK(Run(registry, reopened, "entity.list", { { "nameContains", "Player" } })["entities"].size() == 2);
		CHECK(Run(registry, reopened, "scene.get")["Renderer"]["Exposure"] == 1.5);
		CHECK(Run(registry, reopened, "project.info")["physicsLayers"] == layers);

		CHECK(RunError(registry, reopened, "render.screenshot", { { "path", "x.png" } }).find("GPU host") != std::string::npos);
	}

	TEST_CASE("play.step reports a scene switched by Lua, and stopping returns to the edit scene")
	{
		BasaltTest::TempProject temp("AutomationSceneSwitch");
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "entity.create", { { "name", "InA" } });
		Run(registry, session, "scene.save", { { "path", "Assets/Scenes/A.bscene" } });
		Run(registry, session, "scene.new", { { "name", "B" }, { "empty", true } });
		Run(registry, session, "entity.create", { { "name", "InB" } });
		Run(registry, session, "scene.save", { { "path", "Assets/Scenes/B.bscene" } });
		Run(registry, session, "scene.open", { { "path", "Assets/Scenes/A.bscene" } });

		Run(registry, session, "play.start");
		Run(registry, session, "lua.exec", { { "code", "Scene.Load('Assets/Scenes/B.bscene')" } });
		const json step = Run(registry, session, "play.step");
		CHECK(step["scene"] == "B");
		CHECK(step["scenePath"] == "Assets/Scenes/B.bscene");
		CHECK(Run(registry, session, "lua.exec", { { "code", "tostring(Scene.FindEntityByName('InB') ~= nil)" } })["result"] == "true");

		// The edit scene is untouched: still A, and saving writes A's file.
		Run(registry, session, "play.stop");
		const json info = Run(registry, session, "scene.info");
		CHECK(info["path"] == "Assets/Scenes/A.bscene");
		CHECK(info["entities"] == 1);
		Run(registry, session, "scene.save");
		const auto saved = FileSystem::ReadTextFile(temp.GetDirectory() / "Assets/Scenes/A.bscene");
		REQUIRE(saved);
		CHECK(saved->find("InA") != std::string::npos);
		CHECK(saved->find("InB") == std::string::npos);
	}

	TEST_CASE("Play mode runs a copy of the scene, steps deterministically and accepts input")
	{
		BasaltTest::TempProject temp("AutomationPlay");
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "asset.write", { { "path", "Assets/Scripts/Jumper.lua" }, { "content", R"(
			local J = {}
			function J:OnUpdate(dt)
				if Input.IsKeyPressed("Space") then self.Entity.Translation = self.Entity.Translation + Vec3(0, 1, 0) end
			end
			return J
		)" } });
		Run(registry, session, "entity.create", { { "name", "Jumper" }, { "components", { { "Script", { { "Script", "Assets/Scripts/Jumper.lua" } } } } } });

		CHECK(RunError(registry, session, "play.step").find("not playing") != std::string::npos);
		Run(registry, session, "play.start");
		CHECK(RunError(registry, session, "play.start").find("already playing") != std::string::npos);
		Run(registry, session, "input.key", { { "key", "Space" }, { "down", true } });
		const json step = Run(registry, session, "play.step", { { "frames", 3 } });
		CHECK(step["frame"] == 3);
		CHECK(step["scriptErrors"].empty());
		// The playing scene, which Lua Scene.Load may have switched (an unsaved scene has no path).
		CHECK(step["scene"] == "Untitled");
		CHECK(step["scenePath"] == "");
		Run(registry, session, "input.key", { { "key", "Space" }, { "down", false } });
		// Pressed only on the first frame.
		CHECK(Run(registry, session, "entity.transform", { { "entity", "Jumper" } })["local"]["Translation"][1] == 1.0);
		CHECK(Run(registry, session, "lua.exec", { { "code", "Scene.FindEntityByName('Jumper').Translation.y" } })["result"] == "1.0");

		Run(registry, session, "play.stop");
		// The edit scene was never modified.
		CHECK(Run(registry, session, "entity.transform", { { "entity", "Jumper" } })["local"]["Translation"][1] == 0.0);
		CHECK(RunError(registry, session, "input.key", { { "key", "NotAKey" } }).find("unknown key") != std::string::npos);
		Input::Reset();
	}

	TEST_CASE("State hash ignores entity UUIDs but tracks every state change")
	{
		CommandRegistry registry;
		AutomationSession session;
		const json a = Run(registry, session, "entity.create", { { "name", "A" } });
		Run(registry, session, "entity.create", { { "name", "B" }, { "parent", a["id"] } });
		const std::string hash = Run(registry, session, "scene.hash")["hash"];
		CHECK(hash.size() == 16);

		// The same hierarchy rebuilt in a fresh session gets new UUIDs but the same hash.
		AutomationSession other;
		const json otherA = Run(registry, other, "entity.create", { { "name", "A" } });
		Run(registry, other, "entity.create", { { "name", "B" }, { "parent", otherA["id"] } });
		CHECK(otherA["id"] != a["id"]);
		CHECK(Run(registry, other, "scene.hash")["hash"] == hash);

		Run(registry, other, "component.set", { { "entity", "B" }, { "component", "Transform" }, { "data", { { "Translation", { 0.0, 1e-6, 0.0 } } } } });
		CHECK(Run(registry, other, "scene.hash")["hash"] != hash);

		// Rotations count too (they are hashed as quaternions; the cross-platform PhysicsDeterminism test is
		// what pins that form).
		const std::string beforeTurn = Run(registry, other, "scene.hash")["hash"];
		Run(registry, other, "component.set", { { "entity", "B" }, { "component", "Transform" }, { "data", { { "Rotation", { 0.0, 0.001, 0.0 } } } } });
		CHECK(Run(registry, other, "scene.hash")["hash"] != beforeTurn);

		// expectHash passes on a match and fails naming both hashes; present but empty or null is an error, not a
		// check that silently passes.
		CHECK(Run(registry, session, "scene.hash", { { "expectHash", hash } })["hash"] == hash);
		CHECK(RunError(registry, session, "scene.hash", { { "expectHash", "0000000000000000" } }).find("state hash " + hash + " differs from the expected 0000000000000000") != std::string::npos);
		CHECK(RunError(registry, session, "scene.hash", { { "expectHash", "" } }).find("must be a non-empty string") != std::string::npos);
		CHECK(RunError(registry, session, "scene.hash", { { "expectHash", nullptr } }).find("must be a non-empty string") != std::string::npos);
		Run(registry, session, "play.start");
		const std::string stepped = Run(registry, session, "play.step", { { "frames", 2 }, { "hash", true } })["stateHash"];
		CHECK(Run(registry, session, "scene.hash", { { "expectHash", stepped } })["hash"] == stepped);
		// Nothing here moves, so a step keeps the hash; expectHash then passes and returns it.
		CHECK(Run(registry, session, "play.step", { { "expectHash", stepped } })["stateHash"] == stepped);
		CHECK(RunError(registry, session, "play.step", { { "expectHash", "0000000000000000" } }).find("differs from the expected") != std::string::npos);
		Run(registry, session, "play.stop");
	}

	TEST_CASE("Commands reject parameters they do not take")
	{
		CommandRegistry registry;
		AutomationSession session;
		// A misspelled check must fail instead of passing without checking anything.
		CHECK(RunError(registry, session, "scene.hash", { { "expecthash", "0000000000000000" } }) == "unknown parameter 'expecthash' for scene.hash (expected: expectHash)");
		CHECK(RunError(registry, session, "scene.new", { { "name", "A" }, { "size", 3 } }).find("unknown parameter 'size' for scene.new") != std::string::npos);
		CHECK(RunError(registry, session, "editor.nothing", {}).find("unknown command") != std::string::npos);
	}

	TEST_CASE("Batch requests with expectError succeed only on the expected failure")
	{
		CommandRegistry registry;
		AutomationSession session;
		const json responses = registry.HandleRequest(session, json::array({
																   { { "id", 1 }, { "command", "scene.hash" }, { "params", { { "expectHash", "0000000000000000" } } }, { "expectError", "differs from the expected" } },
																   { { "id", 2 }, { "command", "scene.hash" }, { "expectError", "anything" } },
																   { { "id", 3 }, { "command", "scene.hash" }, { "params", { { "expectHash", "" } } }, { "expectError", "something else" } },
																   { { "id", 4 }, { "command", "scene.hash" }, { "expectError", "" } },
															   }));
		REQUIRE(responses.size() == 4);
		CHECK(responses[0]["ok"] == true);
		CHECK(responses[0]["expectedError"].get<std::string>().find("differs from the expected 0000000000000000") != std::string::npos);
		CHECK(responses[1]["ok"] == false);
		CHECK(responses[1]["error"] == "expected an error containing 'anything', but the command succeeded");
		CHECK(responses[2]["ok"] == false);
		CHECK(responses[2]["error"].get<std::string>().find("got: parameter 'expectHash' must be a non-empty string") != std::string::npos);
		CHECK(responses[3]["ok"] == false);
		CHECK(responses[3]["id"] == 4);
	}

	TEST_CASE("Recorded input replays to the same state")
	{
		BasaltTest::TempProject temp("AutomationReplay");
		CommandRegistry registry;
		AutomationSession session;
		// Moves on key presses and spawns an entity (with a random UUID) on every jump.
		Run(registry, session, "asset.write", { { "path", "Assets/Scripts/Mover.lua" }, { "content", R"(
			local M = {}
			function M:OnUpdate(dt)
				if Input.IsKeyDown("Right") then self.Entity.Translation = self.Entity.Translation + Vec3(dt, 0, 0) end
				if Input.IsKeyPressed("Space") then Scene.CreateEntity("Spark").Translation = self.Entity.Translation + Vec3(0, Math.Random(), 0) end
			end
			return M
		)" } });
		Run(registry, session, "entity.create", { { "name", "Mover" }, { "components", { { "Script", { { "Script", "Assets/Scripts/Mover.lua" } } } } } });

		Run(registry, session, "replay.record_start");
		Run(registry, session, "play.start");
		Run(registry, session, "input.key", { { "key", "Right" }, { "down", true } });
		Run(registry, session, "play.step", { { "frames", 10 } });
		Run(registry, session, "input.key", { { "key", "Space" }, { "down", true } });
		Run(registry, session, "play.step", { { "frames", 1 } });
		Run(registry, session, "input.key", { { "key", "Space" }, { "down", false } });
		Run(registry, session, "input.key", { { "key", "Right" }, { "down", false } });
		const json live = Run(registry, session, "play.step", { { "frames", 5 }, { "hash", true } });
		const json saved = Run(registry, session, "replay.record_stop", { { "path", "Replays/Test.breplay" } });
		CHECK(saved["events"] == 4);
		CHECK(saved["frames"] == 16);

		const json first = Run(registry, session, "replay.run", { { "path", "Replays/Test.breplay" }, { "assertHash", true } });
		CHECK(first["stateHash"] == live["stateHash"]);
		CHECK(first["frame"] == 16);
		CHECK(Run(registry, session, "entity.list", { { "nameContains", "Spark" } })["entities"].size() == 1);
		const json second = Run(registry, session, "replay.run", { { "path", "Replays/Test.breplay" }, { "assertHash", true } });
		CHECK(second["stateHash"] == first["stateHash"]);

		// Stopping before the jump ends in a different state (the hash covers scene content, not the frame
		// counter: frames where nothing changes hash equally), and cannot be checked against the recorded hash.
		const json shorter = Run(registry, session, "replay.run", { { "path", "Replays/Test.breplay" }, { "frames", 5 } });
		CHECK(shorter["frame"] == 5);
		CHECK(shorter["stateHash"] != live["stateHash"]);
		CHECK(RunError(registry, session, "replay.run", { { "path", "Replays/Test.breplay" }, { "frames", 5 }, { "assertHash", true } }).find("full recorded length") != std::string::npos);

		// A seed set during a recording applies from the next play: the replay stores the one the run used.
		Run(registry, session, "play.stop"); // replay.run keeps playing
		Run(registry, session, "replay.record_start");
		Run(registry, session, "play.start");
		Run(registry, session, "project.set", { { "randomSeed", 55 } });
		Run(registry, session, "play.step", { { "frames", 2 } });
		Run(registry, session, "replay.record_stop", { { "path", "Replays/SeedChanged.breplay" } });
		CHECK(json::parse(*FileSystem::ReadTextFile(temp.GetDirectory() / "Replays/SeedChanged.breplay"))["RandomSeed"] == Random::DefaultSeed);
		Run(registry, session, "play.stop");
		// Also when play stopped before the recording did.
		Run(registry, session, "replay.record_start");
		Run(registry, session, "play.start");
		Run(registry, session, "play.step", { { "frames", 2 } });
		Run(registry, session, "play.stop");
		Run(registry, session, "replay.record_stop", { { "path", "Replays/StoppedFirst.breplay" } });
		CHECK(json::parse(*FileSystem::ReadTextFile(temp.GetDirectory() / "Replays/StoppedFirst.breplay"))["RandomSeed"] == 55);
		Run(registry, session, "project.set", { { "randomSeed", Random::DefaultSeed } });

		// Replays from before seeds were stored ran with the default seed.
		temp.WriteFile("Replays/Legacy.breplay", R"({ "Format": "BasaltReplay", "Version": 1, "Frames": 1, "Events": [] })");
		Run(registry, session, "replay.run", { { "path", "Replays/Legacy.breplay" } });
		Run(registry, session, "project.set", { { "randomSeed", 3 } });
		CHECK(RunError(registry, session, "replay.run", { { "path", "Replays/Legacy.breplay" } }).find("has no RandomSeed (it was recorded before seeds were stored, with the default seed " + std::to_string(Random::DefaultSeed)) != std::string::npos);
		Run(registry, session, "project.set", { { "randomSeed", Random::DefaultSeed } });

		// The replay stored the seed it ran with; another seed would replay different random numbers.
		CHECK(json::parse(*FileSystem::ReadTextFile(temp.GetDirectory() / "Replays/Test.breplay"))["RandomSeed"] == Random::DefaultSeed);
		Run(registry, session, "project.set", { { "randomSeed", 99 } });
		CHECK(RunError(registry, session, "replay.run", { { "path", "Replays/Test.breplay" } }).find("recorded with random seed") != std::string::npos);
		Run(registry, session, "project.set", { { "randomSeed", Random::DefaultSeed } });
		CHECK(Run(registry, session, "replay.run", { { "path", "Replays/Test.breplay" }, { "assertHash", true } })["stateHash"] == live["stateHash"]);
		temp.WriteFile("Replays/BadSeed.breplay", R"({ "Format": "BasaltReplay", "RandomSeed": -3, "Events": [] })");
		CHECK(RunError(registry, session, "replay.run", { { "path", "Replays/BadSeed.breplay" } }).find("invalid RandomSeed") != std::string::npos);

		temp.WriteFile("Replays/Bad.breplay", R"({ "Format": "Other" })");
		CHECK(RunError(registry, session, "replay.run", { { "path", "Replays/Bad.breplay" } }).find("not a Basalt replay") != std::string::npos);
		CHECK(RunError(registry, session, "replay.record_stop", { { "path", "Replays/X.breplay" } }).find("not recording") != std::string::npos);
		Run(registry, session, "play.stop");
		Input::Reset();
	}
}

#include <Basalt/Automation/AutomationServer.h>
#include <Basalt/Project/Exporter.h>

#if defined(_WIN32)
	#include <winsock2.h>
	#include <ws2tcpip.h>
#else
	#include <arpa/inet.h>
	#include <netinet/in.h>
	#include <sys/socket.h>
	#include <unistd.h>
#endif

#include <cstring>

namespace {

	// Minimal blocking client: sends one line and reads one line back.
	std::string SendLine(uint16_t port, const std::string& line)
	{
#if defined(_WIN32)
		SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
		int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
		sockaddr_in address;
		std::memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		std::string response;
		if (connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
		{
			const std::string payload = line + "\n";
			send(client, payload.data(), static_cast<int>(payload.size()), 0);
			char buffer[4096];
			while (response.find('\n') == std::string::npos)
			{
				const auto received = recv(client, buffer, sizeof(buffer), 0);
				if (received <= 0)
					break;
				response.append(buffer, static_cast<size_t>(received));
			}
		}
#if defined(_WIN32)
		closesocket(client);
#else
		close(client);
#endif
		return response.substr(0, response.find('\n'));
	}

}

TEST_SUITE("Automation")
{
	TEST_CASE("Automation server answers newline-delimited requests on localhost")
	{
		Basalt::CommandRegistry registry;
		Basalt::AutomationSession session;
		Basalt::AutomationServer server;
		std::string error;
		REQUIRE_MESSAGE(server.Start(0, [&](const std::string& request) {
			const json parsed = json::parse(request, nullptr, false);
			return registry.HandleRequest(session, parsed).dump(); }, error), error);
		REQUIRE(server.GetPort() != 0);

		const json first = json::parse(SendLine(server.GetPort(), R"({"id":1,"command":"scene.info"})"));
		CHECK(first["ok"] == true);
		CHECK(first["id"] == 1);
		const json second = json::parse(SendLine(server.GetPort(), R"({"command":"nope"})"));
		CHECK(second["ok"] == false);

		Basalt::AutomationServer duplicate;
		CHECK_FALSE(duplicate.Start(server.GetPort(), [](const std::string&) { return std::string(); }, error));
		server.Stop();
		CHECK_FALSE(server.IsRunning());
	}

	TEST_CASE("Exporter refuses unsafe or incomplete exports")
	{
		BasaltTest::TempProject temp("ExporterChecks");
		const Basalt::Ref<Basalt::Project>& project = Basalt::Project::GetActive();
		REQUIRE(project);

		Basalt::ExportResult result = Basalt::Exporter::Export(*project, temp.GetDirectory().parent_path() / "ExportOut");
		CHECK_FALSE(result.Success);
		CHECK(result.Error.find("StartScene") != std::string::npos);

		temp.WriteFile("Assets/Scenes/Main.bscene", R"({"Entities":[]})");
		project->GetConfig().StartScene = "Assets/Scenes/Main.bscene";
		result = Basalt::Exporter::Export(*project, temp.GetDirectory().parent_path() / "ExportOut", temp.GetDirectory() / "NoSuchRuntime");
		CHECK(result.Error.find("runtime executable not found") != std::string::npos);

		temp.WriteFile("FakeRuntime", "binary");
		result = Basalt::Exporter::Export(*project, temp.GetDirectory() / "Inside", temp.GetDirectory() / "FakeRuntime");
		CHECK(result.Error.find("outside the project") != std::string::npos);

		const auto occupied = temp.GetDirectory().parent_path() / "ExporterOccupied";
		std::filesystem::create_directories(occupied);
		Basalt::FileSystem::WriteTextFile(occupied / "precious.txt", "do not delete");
		result = Basalt::Exporter::Export(*project, occupied, temp.GetDirectory() / "FakeRuntime");
		CHECK(result.Error.find("not empty") != std::string::npos);
		CHECK(Basalt::FileSystem::Exists(occupied / "precious.txt"));
		std::filesystem::remove_all(occupied);
	}
}

TEST_SUITE("Automation")
{
	TEST_CASE("ui.layout reports every visible UI rect in draw order for a viewport size")
	{
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "scene.new", { { "name", "UI" }, { "empty", true } });
		CHECK(Run(registry, session, "ui.layout")["items"].empty());

		const json bar = Run(registry, session, "entity.create", { { "name", "Bar" }, { "components", { { "UIRect", { { "AnchorMin", { 0, 1 } }, { "AnchorMax", { 1, 1 } }, { "Pivot", { 0.5, 1 } }, { "Size", { 0, 100 } } } }, { "UIImage", json::object() } } } });
		Run(registry, session, "entity.create", { { "name", "Label" }, { "parent", bar["id"] }, { "components", { { "UIText", { { "Text", "Hi" } } } } } });
		Run(registry, session, "entity.create", { { "name", "Hidden" }, { "components", { { "UIRect", { { "Visible", false } } } } } });

		json layout = Run(registry, session, "ui.layout");
		CHECK(layout["canvas"] == json({ 1920.0, 1080.0 }));
		REQUIRE(layout["items"].size() == 2);
		CHECK(layout["items"][0]["name"] == "Bar");
		CHECK(layout["items"][0]["id"] == bar["id"]);
		CHECK(layout["items"][0]["rect"] == json({ 0.0, 980.0, 1920.0, 100.0 }));
		CHECK(layout["items"][1]["name"] == "Label");
		CHECK(layout["items"][1]["rect"] == layout["items"][0]["rect"]);

		// A 4:3 viewport narrows the canvas; the stretched bar follows it.
		layout = Run(registry, session, "ui.layout", { { "width", 800 }, { "height", 600 } });
		CHECK(layout["canvas"] == json({ 1440.0, 1080.0 }));
		CHECK(layout["items"][0]["rect"] == json({ 0.0, 980.0, 1440.0, 100.0 }));

		CHECK(layout["problems"].empty());

		// While playing it lays out the play copy.
		Run(registry, session, "play.start");
		Run(registry, session, "component.set", { { "entity", "Bar" }, { "component", "UIRect" }, { "data", { { "Size", { 0, 50 } } } } });
		CHECK(Run(registry, session, "ui.layout")["items"][0]["rect"] == json({ 0.0, 1030.0, 1920.0, 50.0 }));
		Run(registry, session, "play.stop");
		CHECK(Run(registry, session, "ui.layout")["items"][0]["rect"] == json({ 0.0, 980.0, 1920.0, 100.0 }));

		CHECK(RunError(registry, session, "ui.layout", { { "width", 0 } }).find("'width' must be in") != std::string::npos);
		CHECK(RunError(registry, session, "ui.layout", { { "height", "tall" } }).find("must be an integer") != std::string::npos);
	}

	TEST_CASE("input.gamepad drives an injected gamepad, is recorded and replays")
	{
		BasaltTest::TempProject temp("AutomationGamepad");
		CommandRegistry registry;
		AutomationSession session;
		Input::ForgetInjectedGamepads();
		// Moves with the stick, and toggles a checkbox through gamepad navigation.
		Run(registry, session, "asset.write", { { "path", "Assets/Scripts/Pad.lua" }, { "content", R"(
			local P = {}
			function P:OnUpdate(dt)
				self.Entity.Translation = self.Entity.Translation + Vec3(Input.GetGamepadAxis(1, "LeftX") * dt, 0, 0)
			end
			function P:OnGamepadConnected(index)
				self.Entity.Translation = self.Entity.Translation + Vec3(0, index, 0)
			end
			return P
		)" } });
		Run(registry, session, "entity.create", { { "name", "Pad" }, { "components", { { "Script", { { "Script", "Assets/Scripts/Pad.lua" } } } } } });
		Run(registry, session, "entity.create", { { "name", "Option" }, { "components", { { "UIRect", json::object() }, { "UICheckbox", { { "OnValueChanged", "" } } } } } });

		CHECK(RunError(registry, session, "input.gamepad", { { "index", 1 } }).find("give 'connected', 'button' or 'axis'") != std::string::npos);
		CHECK(RunError(registry, session, "input.gamepad", { { "index", 16 }, { "connected", true } }).find("'index' must be in") != std::string::npos);
		CHECK(RunError(registry, session, "input.gamepad", { { "index", 1 }, { "button", "A" } }).find("gamepad 1 is not connected") != std::string::npos);
		CHECK(RunError(registry, session, "input.gamepad", { { "connected", true }, { "button", "Turbo" } }).find("unknown gamepad button 'Turbo'") != std::string::npos);
		CHECK(RunError(registry, session, "input.gamepad", { { "connected", true }, { "axis", "LeftTrigger" }, { "value", -0.5 } }).find("'value' must be a number from 0 to 1") != std::string::npos);
		CHECK(RunError(registry, session, "input.gamepad", { { "connected", true }, { "axis", "LeftX" } }).find("'value' must be a number from -1 to 1") != std::string::npos);
		CHECK(RunError(registry, session, "input.gamepad", { { "connected", true }, { "axis", "Wheel" }, { "value", 0 } }).find("unknown gamepad axis") != std::string::npos);
		CHECK(RunError(registry, session, "input.gamepad", { { "connected", true }, { "button", "A" }, { "down", "yes" } }).find("'down' must be") != std::string::npos);
		CHECK(RunError(registry, session, "input.gamepad", { { "connected", false }, { "name", "Pad" } }).find("'name' names a gamepad being connected") != std::string::npos);
		CHECK(RunError(registry, session, "input.gamepad", { { "connected", true }, { "down", true } }).find("'down' goes with 'button'") != std::string::npos);
		CHECK(RunError(registry, session, "input.gamepad", { { "connected", true }, { "value", 1 } }).find("'value' goes with 'axis'") != std::string::npos);
		CHECK(RunError(registry, session, "input.gamepad", { { "connected", true }, { "name", std::string(300, 'x') } }).find("'name' must be at most 256") != std::string::npos);
		// A rejected request changes nothing, not even the connection it asked for.
		CHECK_FALSE(Input::IsGamepadConnected(0));
		// A physical gamepad is the device's: it cannot be driven or disconnected without taking it over.
		GamepadDeviceState device;
		Input::UpdateGamepadFromDevice(5, &device);
		CHECK(RunError(registry, session, "input.gamepad", { { "index", 5 }, { "button", "A" } }).find("gamepad 5 is a physical device; pass connected: true") != std::string::npos);
		CHECK(RunError(registry, session, "input.gamepad", { { "index", 5 }, { "connected", false } }).find("cannot be disconnected") != std::string::npos);
		// Taking it over: connected: true makes it injected, released.
		Run(registry, session, "input.gamepad", { { "index", 5 }, { "connected", true }, { "button", "A" } });
		CHECK(Input::GetGamepadSource(5) == GamepadSource::Injected);
		CHECK(Input::IsGamepadButtonDown(5, GamepadButton::A));
		Run(registry, session, "input.gamepad", { { "index", 5 }, { "connected", false } });

		Run(registry, session, "replay.record_start");
		Run(registry, session, "play.start");
		Run(registry, session, "input.gamepad", { { "index", 1 }, { "connected", true }, { "name", "Test Pad" }, { "axis", "LeftX" }, { "value", 1 } });
		CHECK(Input::GetGamepadName(1) == "Test Pad");
		Run(registry, session, "play.step", { { "frames", 30 } });
		Run(registry, session, "input.gamepad", { { "index", 1 }, { "axis", "LeftX" }, { "value", 0 } });
		Run(registry, session, "input.gamepad", { { "index", 1 }, { "button", "DPadDown" } });
		Run(registry, session, "play.step", { { "frames", 1 } });
		Run(registry, session, "input.gamepad", { { "index", 1 }, { "button", "DPadDown" }, { "down", false } });
		Run(registry, session, "input.gamepad", { { "index", 1 }, { "button", "A" } });
		Run(registry, session, "play.step", { { "frames", 1 } });
		Run(registry, session, "input.gamepad", { { "index", 1 }, { "button", "A" }, { "down", false } });
		const json live = Run(registry, session, "play.step", { { "frames", 2 }, { "hash", true } });

		// ui.layout describes the widget, its state and focus while playing.
		const json item = Run(registry, session, "ui.layout")["items"][0];
		CHECK(item["widget"]["kind"] == "UICheckbox");
		CHECK(item["widget"]["checked"] == true);
		CHECK(item["widget"]["focused"] == true);
		CHECK(item["widget"]["state"] == "Highlighted");
		CHECK(item["widget"]["interactable"] == true);
		CHECK(item["widget"]["target"] == 0);

		const json saved = Run(registry, session, "replay.record_stop", { { "path", "Replays/Pad.breplay" } });
		CHECK(saved["events"] == 6);
		// The replay starts without the injected gamepad (still connected here) and connects it on its frame.
		CHECK(Input::IsGamepadConnected(1));
		const json replayed = Run(registry, session, "replay.run", { { "path", "Replays/Pad.breplay" }, { "assertHash", true }, { "assertNoErrors", true } });
		CHECK(replayed["stateHash"] == live["stateHash"]);
		Run(registry, session, "play.stop");
		Input::SetGamepadConnected(1, false);
	}

	TEST_CASE("Recording starts with the injected gamepads; replays run with the headless view whatever the host")
	{
		BasaltTest::TempProject temp("AutomationReplayHost");
		CommandRegistry registry;
		AutomationSession session;
		Input::ForgetInjectedGamepads();
		Run(registry, session, "asset.write", { { "path", "Assets/Scripts/Count.lua" }, { "content", R"(
			local C = {}
			function C:OnClick() self.Entity.Translation = self.Entity.Translation + Vec3(1, 0, 0) end
			function C:OnUpdate()
				local held = Input.IsGamepadButtonDown(2, "B") and 1 or 0
				local mouse = Input.GetMousePosition().x
				self.Entity.Translation = self.Entity.Translation + Vec3(0, Input.GetGamepadAxis(2, "RightX") + held + mouse, #Input.GetGamepads())
			end
			return C
		)" } });
		Run(registry, session, "entity.create", { { "name", "Button" }, { "components", { { "UIRect", { { "AnchorMin", { 0, 0 } }, { "AnchorMax", { 0, 0 } }, { "Pivot", { 0, 0 } }, { "Size", { 200, 100 } } } }, { "UIButton", json::object() }, { "Script", { { "Script", "Assets/Scripts/Count.lua" } } } } } });

		// A gamepad connected (and pushed) before recording starts is part of the recording.
		Run(registry, session, "play.start");
		Run(registry, session, "input.gamepad", { { "index", 2 }, { "connected", true }, { "name", "Early" }, { "axis", "RightX" }, { "value", 0.5 } });
		Run(registry, session, "input.gamepad", { { "index", 2 }, { "button", "B" } });
		// No mouse position yet when recording starts: the first recorded frame reads 0, and so must the replay's.
		Input::ResetMousePosition();
		CHECK(Run(registry, session, "replay.record_start")["physicalGamepads"].empty());
		Run(registry, session, "play.step", { { "frames", 1 } });
		Run(registry, session, "input.mouse", { { "position", { 50, 50 } }, { "button", "Left" }, { "down", true } });
		Run(registry, session, "play.step", { { "frames", 1 } });
		Run(registry, session, "input.mouse", { { "button", "Left" }, { "down", false } });
		const json live = Run(registry, session, "play.step", { { "frames", 3 }, { "hash", true } });
		Run(registry, session, "replay.record_stop", { { "path", "Replays/Host.breplay" } });
		CHECK(Run(registry, session, "lua.exec", { { "code", "return Scene.FindEntityByName('Button').Translation.x" } })["result"] == "1.0");

		// An editor-like host: the view offset and scaled, the mouse elsewhere, a physical gamepad connected. The
		// replay still clicks (headless view, from no mouse position) and sees only its own gamepad, restored with
		// what it held.
		const GameUI::HostState editor{ { 300.0f, 200.0f }, { 640.0f, 360.0f }, false, false };
		GameUI::SetHostState(editor);
		Input::SetMousePosition({ 900.0f, 900.0f });
		GamepadDeviceState device;
		Input::UpdateGamepadFromDevice(0, &device);
		const json replayed = Run(registry, session, "replay.run", { { "path", "Replays/Host.breplay" }, { "assertHash", true }, { "assertNoErrors", true } });
		CHECK(replayed["stateHash"] == live["stateHash"]);
		CHECK(Run(registry, session, "lua.exec", { { "code", "return Input.GetGamepadName(2)" } })["result"] == "Early");
		// Physical gamepads are back after the replay.
		CHECK(Input::IsDeviceGamepadsEnabled());
		Input::UpdateGamepadFromDevice(0, &device);
		CHECK(Input::IsGamepadConnected(0));
		Input::UpdateGamepadFromDevice(0, nullptr);
		const GameUI::HostState restored = GameUI::GetHostState();
		CHECK(restored.ViewportOrigin == editor.ViewportOrigin);
		CHECK(restored.ViewportSize == editor.ViewportSize);
		CHECK_FALSE(restored.PointerEnabled);
		CHECK_FALSE(restored.NavigationEnabled);
		// A replay that fails restores the host too, and names the event that failed.
		const json bad = { { "Format", "BasaltReplay" }, { "Version", 1 }, { "Frames", 2 }, { "RandomSeed", Project::GetActiveRandomSeed() }, { "Events", json::array({ { { "frame", 1 }, { "command", "input.gamepad" }, { "params", { { "button", "A" } } } } }) } };
		Run(registry, session, "asset.write", { { "path", "Replays/Bad.breplay" }, { "content", bad.dump() } });
		const std::string error = RunError(registry, session, "replay.run", { { "path", "Replays/Bad.breplay" } });
		INFO(error);
		CHECK(error.find("'Replays/Bad.breplay' event 0 (frame 1, input.gamepad): gamepad 0 is not connected") != std::string::npos);
		CHECK(GameUI::GetHostState().ViewportOrigin == editor.ViewportOrigin);
		CHECK(Input::IsDeviceGamepadsEnabled());
		GameUI::SetHostState({});
		// Stopping play forgets injected gamepads.
		Run(registry, session, "play.stop");
		CHECK_FALSE(Input::IsGamepadConnected(2));
	}

	TEST_CASE("ui.layout reports widgets with several widget components or a missing target")
	{
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "scene.new", { { "name", "UI" }, { "empty", true } });
		Run(registry, session, "entity.create", { { "name", "Both" }, { "components", { { "UIRect", json::object() }, { "UISlider", { { "Value", 0.25 } } }, { "UICheckbox", json::object() } } } });
		const json menu = Run(registry, session, "entity.create", { { "name", "Menu" } });
		Run(registry, session, "entity.create", { { "name", "Plain" } });
		Run(registry, session, "entity.create", { { "name", "Calls" }, { "components", { { "UIRect", json::object() }, { "UIButton", { { "Target", "Plain" }, { "OnClick", "Go" } } } } } });
		// Targets resolve by name in commands, like every entity reference.
		Run(registry, session, "entity.create", { { "name", "Lost" }, { "components", { { "UIRect", json::object() }, { "UIButton", { { "Target", "Menu" } } } } } });
		Run(registry, session, "entity.destroy", { { "entity", "Menu" } });
		const json layout = Run(registry, session, "ui.layout");
		REQUIRE(layout["items"].size() == 3);
		CHECK(layout["items"][0]["widget"]["kind"] == "UISlider");
		CHECK(layout["items"][0]["widget"]["value"] == 0.25);
		// Not playing: no state or focus.
		CHECK_FALSE(layout["items"][0]["widget"].contains("state"));
		CHECK(layout["items"][0]["widget"]["callback"] == "OnValueChanged");
		REQUIRE(layout["problems"].size() == 4);
		CHECK(layout["problems"][0]["problem"] == "several widget components: it acts as its UISlider only");
		// The default callback on the widget itself, which has no Script.
		CHECK(layout["problems"][1]["problem"] == "UISlider calls OnValueChanged on 'Both', which has no Script");
		CHECK(layout["problems"][2]["problem"] == "UIButton calls Go on 'Plain', which has no Script");
		CHECK(layout["problems"][3]["problem"] == "UIButton Target " + std::to_string(menu["id"].get<uint64_t>()) + " does not exist (its callback cannot run)");
		// The host's view: headless, mouse positions are canvas units.
		CHECK(layout["view"]["origin"] == json({ 0.0, 0.0 }));
		CHECK(layout["view"]["size"] == json({ 1920.0, 1080.0 }));
		CHECK(layout["view"]["pointerEnabled"] == true);
		CHECK(layout["view"]["navigationEnabled"] == true);
		// A callback name only C++ can write (the registry rejects it) calls nothing, and says so.
		Entity calls = session.GetScene()->FindEntityByName("Calls");
		REQUIRE(calls);
		calls.GetComponent<UIButtonComponent>().OnClick = "not a name";
		const json repaired = Run(registry, session, "ui.layout");
		CHECK(repaired["problems"][2]["problem"] == "UIButton callback 'not a name' is not a script method name (it calls nothing)");
	}

	TEST_CASE("ui.layout reports images and fonts that will not load")
	{
		BasaltTest::TempProject project("UILayoutProblems");
		project.WriteFile("Assets/Fonts/Bad.ttf", std::string(500, 'x'));
		// Images and fonts that load are no problem.
		const std::filesystem::path data = std::filesystem::path(BASALT_TEST_DATA_DIR) / "FeatureTest/Assets";
		std::filesystem::create_directories(project.GetDirectory() / "Assets/Textures");
		std::filesystem::copy_file(data / "Textures/UIPanel.png", project.GetDirectory() / "Assets/Textures/Panel.png");
		std::filesystem::copy_file(data / "Fonts/Karla-Regular.ttf", project.GetDirectory() / "Assets/Fonts/Karla.ttf");
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "scene.new", { { "name", "UI" }, { "empty", true } });
		Run(registry, session, "entity.create", { { "name", "Panel" }, { "components", { { "UIRect", json::object() }, { "UIImage", { { "Texture", "Assets/Textures/Missing.png" } } } } } });
		Run(registry, session, "entity.create", { { "name", "Label" }, { "components", { { "UIRect", json::object() }, { "UIText", { { "Font", "Assets/Fonts/Bad.ttf" } } } } } });
		Run(registry, session, "entity.create", { { "name", "Plain" }, { "components", { { "UIText", json::object() } } } });
		Run(registry, session, "entity.create", { { "name", "Good" }, { "components", { { "UIImage", { { "Texture", "Assets/Textures/Panel.png" } } }, { "UIText", { { "Font", "Assets/Fonts/Karla.ttf" } } } } } });
		// Each entity using a bad font is reported (the file is checked once).
		Run(registry, session, "entity.create", { { "name", "Label2" }, { "components", { { "UIText", { { "Font", "Assets/Fonts/Bad.ttf" } } } } } });

		const json problems = Run(registry, session, "ui.layout")["problems"];
		REQUIRE(problems.size() == 3);
		CHECK(problems[2]["name"] == "Label2");
		CHECK(problems[0]["name"] == "Panel");
		CHECK(problems[0]["problem"].get<std::string>().find("UIImage texture 'Assets/Textures/Missing.png'") != std::string::npos);
		CHECK(problems[1]["name"] == "Label");
		CHECK(problems[1]["problem"].get<std::string>().find("not a TrueType font") != std::string::npos);
	}
}
