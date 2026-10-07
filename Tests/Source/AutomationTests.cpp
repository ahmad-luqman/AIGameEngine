#include <doctest/doctest.h>

#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/Input.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

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

		CHECK(RunError(registry, reopened, "render.screenshot", { { "path", "x.png" } }).find("GPU host") != std::string::npos);
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
