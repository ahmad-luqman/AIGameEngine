#include <doctest/doctest.h>

#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/AutomationConnection.h>
#include <Basalt/Automation/EditorCommandSchemas.h>
#include <Basalt/Automation/EditorPreferences.h>
#include <Basalt/Automation/GameView.h>
#include <Basalt/Automation/ViewportSettings.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Automation/CommandSchema.h>
#include <Basalt/Automation/McpServer.h>
#include <Basalt/Core/JsonUtils.h>
#include <Basalt/Scene/Scene.h>

#include "TestUtils.h"

#include <limits>
#include <sstream>

using namespace Basalt;
using json = nlohmann::json;

namespace {

	json Request(const std::string& method, json params = json::object())
	{
		return { { "jsonrpc", "2.0" }, { "id", 7 }, { "method", method }, { "params", std::move(params) } };
	}

	json Initialize()
	{
		return Request("initialize", { { "protocolVersion", McpServer::ProtocolVersion }, { "capabilities", json::object() }, { "clientInfo", { { "name", "tests" }, { "version", "1" } } } });
	}

	json Reply(McpServer& server, const CommandRegistry& registry, AutomationSession& session, const json& request)
	{
		const auto response = server.HandleRequest(registry, session, request);
		REQUIRE(response);
		return *response;
	}

	// A witness for the supported schema subset; semantic scene/project constraints remain handler checks.
	json MinimalValue(const json& schema)
	{
		if (schema.contains("enum"))
			return schema["enum"][0];
		if (schema.contains("anyOf"))
			return MinimalValue(schema["anyOf"][0]);
		const std::string type = schema.value("type", "object");
		if (type == "object")
		{
			json result = json::object();
			for (const auto& name : schema.value("required", json::array()))
				result[name.get<std::string>()] = MinimalValue(schema["properties"][name.get<std::string>()]);
			return result;
		}
		if (type == "array")
		{
			json result = json::array();
			for (size_t i = 0; i < schema.value("minItems", size_t{ 0 }); ++i)
				result.push_back(MinimalValue(schema["items"]));
			return result;
		}
		if (type == "integer")
			return schema.value("minimum", json(0));
		if (type == "number")
			return schema.contains("exclusiveMinimum") ? json(schema["exclusiveMinimum"].get<double>() + 0.001) : schema.value("minimum", json(0.0));
		if (type == "boolean")
			return false;
		if (type == "null")
			return nullptr;
		return "SchemaEntity";
	}

	void Start(McpServer& server, const CommandRegistry& registry, AutomationSession& session)
	{
		const auto response = server.HandleRequest(registry, session, Initialize());
		REQUIRE(response);
		REQUIRE(response->contains("result"));
		CHECK((*response)["result"]["protocolVersion"].get<std::string>() == std::string(McpServer::ProtocolVersion));
		CHECK_FALSE(server.HandleRequest(registry, session, { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } }));
	}
}

TEST_SUITE("MCP")
{
	TEST_CASE("Initialization gates tools and resets per connection")
	{
		CommandRegistry registry;
		AutomationSession session;
		McpServer server;
		CHECK_FALSE(server.HandleRequest(registry, session, { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } }));
		CHECK((Reply(server, registry, session, Request("tools/list")))["error"]["code"] == -32600);
		CHECK((Reply(server, registry, session, Request("ping")))["result"].is_object());
		CHECK((Reply(server, registry, session, Request("initialize")))["error"]["code"] == -32602);
		auto initialize = Initialize();
		initialize["params"]["protocolVersion"] = "future-version";
		CHECK((Reply(server, registry, session, initialize))["result"]["protocolVersion"].get<std::string>() == std::string(McpServer::ProtocolVersion));
		const auto before = registry.Execute(session, "scene.hash", json::object());
		CHECK(Reply(server, registry, session, Request("tools/call", { { "name", "scene.new" } }))["error"]["code"] == -32600);
		CHECK(registry.Execute(session, "scene.hash", json::object()) == before);
		CHECK((Reply(server, registry, session, Request("tools/list")))["error"]["code"] == -32600);
		CHECK_FALSE(server.HandleRequest(registry, session, { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } }));
		CHECK((Reply(server, registry, session, Initialize()))["error"]["code"] == -32600);
		server.Reset();
		Start(server, registry, session);
	}

	TEST_CASE("Tool discovery is exactly the registered schemas including host commands")
	{
		CommandRegistry registry;
		registry.Register({ "test.host", "Host tool", CommandSchema::Object({ { "flag", CommandSchema::Boolean() } }), [](AutomationSession&, const json& params) { return params; } });
		AutomationSession session;
		McpServer server;
		Start(server, registry, session);
		const auto response = server.HandleRequest(registry, session, Request("tools/list"));
		REQUIRE(response);
		const auto& tools = (*response)["result"]["tools"];
		CHECK(tools.size() == registry.GetCommands().size());
		for (const auto& tool : tools)
		{
			const auto* command = registry.Find(tool["name"]);
			REQUIRE(command);
			CHECK(tool["inputSchema"] == command->InputSchema);
			const auto help = registry.Execute(session, "help", { { "command", command->Name } });
			CHECK(help["inputSchema"] == tool["inputSchema"]);
		}
		CHECK((Reply(server, registry, session, Request("tools/list", { { "cursor", "bogus" } })))["error"]["code"] == -32602);
	}

	TEST_CASE("Tool calls share validation and failures do not mutate the scene")
	{
		CommandRegistry registry;
		AutomationSession session;
		McpServer server;
		Start(server, registry, session);
		const auto call = [&](const std::string& name, json arguments) {
			return Reply(server, registry, session, Request("tools/call", { { "name", name }, { "arguments", std::move(arguments) } }));
		};
		CHECK(call("scene.new", { { "empty", true } })["result"]["isError"] == false);
		CHECK(call("entity.create", { { "name", "MCP" } })["result"]["structuredContent"]["name"] == "MCP");
		const auto before = registry.Execute(session, "scene.hash", json::object());
		CHECK(call("entity.duplicate", { { "entity", "MCP" }, { "name", "" } })["result"]["isError"] == true);
		for (const json& bad : { json{ { "name", 1 } }, json{ { "name", "bad" }, { "components", 4 } }, json{ { "parent", -1 } }, json{ { "naem", "typo" } } })
			CHECK(call("entity.create", bad)["result"]["isError"] == true);
		CHECK(registry.Execute(session, "scene.hash", json::object()) == before);
		const auto good = call("scene.info", json::object());
		CHECK(ParseJson(good["result"]["content"][0]["text"].get<std::string>()) == good["result"]["structuredContent"]);
		CHECK(call("entity.create", { { "name", 1 } })["result"]["content"][0]["text"] == "parameter 'name' must be a string; or parameter 'name' must be a null");
		CHECK(call("component.set", { { "entity", "MCP" }, { "component", "Transform" } })["result"]["isError"] == true);
		CHECK(call("missing.tool", json::object())["error"]["code"] == -32602);
		CHECK(call("entity.create", nullptr)["error"]["code"] == -32602);
	}

	TEST_CASE("Malformed messages and notifications cannot execute tools")
	{
		CommandRegistry registry;
		AutomationSession session;
		McpServer server;
		Start(server, registry, session);
		const auto before = registry.Execute(session, "scene.hash", json::object());
		auto call = Request("tools/call", { { "name", "scene.new" }, { "arguments", { { "name", "must not run" } } } });
		call.erase("id");
		CHECK_FALSE(server.HandleRequest(registry, session, call));
		call["params"] = 1;
		CHECK_FALSE(server.HandleRequest(registry, session, call));
		CHECK(registry.Execute(session, "scene.hash", json::object()) == before);
		for (const json& bad : { json(), json::array(), json{ { "jsonrpc", "1.0" }, { "id", 7 }, { "method", "ping" } }, json{ { "jsonrpc", "2.0" }, { "id", nullptr }, { "method", "ping" } }, json{ { "jsonrpc", "2.0" }, { "id", true }, { "method", "ping" } } })
			CHECK((Reply(server, registry, session, bad))["error"]["code"] == -32600);
		CHECK((Reply(server, registry, session, ParseJson("{")))["error"]["code"] == -32700);
		CHECK((Reply(server, registry, session, Request("unknown")))["error"]["code"] == -32601);
		CHECK_FALSE(server.HandleRequest(registry, session, { { "jsonrpc", "2.0" }, { "id", 7 }, { "result", json::object() } }));
		CHECK_FALSE(server.HandleRequest(registry, session, { { "jsonrpc", "2.0" }, { "id", nullptr }, { "error", { { "code", -32700 }, { "message", "Parse error" } } } }));
		auto ping = Request("ping");
		ping["id"] = "client-id";
		CHECK((Reply(server, registry, session, ping))["id"] == "client-id");
	}

	TEST_CASE("Stdio emits only replies, handles CRLF and rejects partial final frames")
	{
		CommandRegistry registry;
		AutomationSession session;
		McpServer server;
		std::istringstream input(Initialize().dump() + "\r\n{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n\n\r\n{bad}\n" + Request("tools/list").dump() + "\n");
		std::ostringstream output;
		CHECK(server.Serve(input, output, registry, session));
		std::istringstream responses(output.str());
		std::string line;
		std::vector<json> parsed;
		while (std::getline(responses, line))
			parsed.push_back(ParseJson(line));
		REQUIRE(parsed.size() == 5);
		CHECK(parsed[0].contains("result"));
		CHECK(parsed[1]["error"]["code"] == -32700);
		CHECK(parsed[2]["error"]["code"] == -32700);
		CHECK(parsed[3]["error"]["code"] == -32700);
		CHECK(parsed[4]["result"]["tools"].is_array());
		std::istringstream partial(Request("ping").dump());
		std::ostringstream ignored;
		CHECK_FALSE(server.Serve(partial, ignored, registry, session));
		CHECK(ignored.str().empty());
	}
}

TEST_SUITE("CommandSchema")
{
	TEST_CASE("Required fields, types, ranges, arrays and uint64 boundaries are validated")
	{
		std::string error;
		const auto schema = CommandSchema::Object({ { "id", CommandSchema::Entity("entity") }, { "position", CommandSchema::Vector("position", 3) }, { "count", CommandSchema::Nullable(CommandSchema::Integer("count", 0, INT64_MAX)) } }, { "id" });
		CHECK(CommandSchema::CheckDefinition(schema, error));
		CHECK(CommandSchema::Validate(schema, { { "id", UINT64_MAX }, { "position", { 1, 2.0, 3 } }, { "count", nullptr } }, error));
		CHECK(CommandSchema::Validate(schema, { { "id", "name" }, { "count", INT64_MAX } }, error));
		for (const json& bad : { json::object(), json{ { "id", -1 } }, json{ { "id", 1.0 } }, json{ { "id", true } }, json{ { "id", 1 }, { "count", UINT64_MAX } }, json{ { "id", 1 }, { "position", { 1, 2 } } }, json{ { "id", 1 }, { "position", { 1, 2, "3" } } }, json{ { "id", 1 }, { "position", { 1, 2, 1e100 } } } })
			CHECK_FALSE(CommandSchema::Validate(schema, bad, error));
		CHECK_FALSE(CommandSchema::Validate(CommandSchema::Number("", 0, 1), std::numeric_limits<double>::infinity(), error));
		CHECK_FALSE(CommandSchema::Validate({ { "enum", { "a", "b" } } }, "c", error));
		CHECK(CommandSchema::Validate({ { "type", "string" }, { "minLength", 2 } }, "éx", error));
		CHECK_FALSE(CommandSchema::Validate({ { "type", "string" }, { "minLength", 2 } }, "é", error));
	}

	TEST_CASE("All command schemas reject malformed input before invoking handlers")
	{
		CommandRegistry registry;
		AutomationSession session;
		for (const auto& [name, command] : registry.GetCommands())
		{
			INFO(name);
			CHECK(command.InputSchema["type"] == "object");
			CHECK(command.InputSchema["additionalProperties"] == false);
			CHECK(command.InputSchema["properties"].is_object());
			std::string error;
			CHECK_FALSE(CommandSchema::Validate(command.InputSchema, { { "__unknown", true } }, error));
			for (const auto& field : command.InputSchema["required"])
			{
				json params = json::object();
				for (const auto& other : command.InputSchema["required"])
					if (other != field)
						params[other.get<std::string>()] = nullptr;
				CHECK_FALSE(CommandSchema::Validate(command.InputSchema, params, error));
				CHECK(error.find("missing parameter '" + field.get<std::string>() + "'") != std::string::npos);
			}
		}
	}

	TEST_CASE("Bad schema definitions fail at registration")
	{
		std::string error;
		CHECK_FALSE(CommandSchema::CheckDefinition({ { "pattern", "x" } }, error));
		CHECK_FALSE(CommandSchema::CheckDefinition({ { "type", "typo" } }, error));
		CHECK_FALSE(CommandSchema::CheckDefinition({ { "minimum", "x" } }, error));
		CHECK_FALSE(CommandSchema::CheckDefinition({ { "required", { "missing" } } }, error));
		CHECK_FALSE(CommandSchema::CheckDefinition({ { "anyOf", json::array() } }, error));
		CHECK_FALSE(CommandSchema::CheckDefinition({ { "type", "string" }, { "enum", { 3 } } }, error));
		CHECK_FALSE(CommandSchema::CheckDefinition({ { "exclusiveMinimum", 1 }, { "maximum", 1 } }, error));
	}
}

TEST_CASE("Automation connections select, retain and reset their protocol without a GUI")
{
	CommandRegistry registry;
	AutomationSession session;
	AutomationConnection connection;
	CHECK(connection.HandleRequest(registry, session, "").empty());
	CHECK(ParseJson(connection.HandleRequest(registry, session, R"({"jsonrpc":)"))["error"]["code"] == -32700);
	CHECK(ParseJson(connection.HandleRequest(registry, session, ""))["error"]["code"] == -32700);
	CHECK(ParseJson(connection.HandleRequest(registry, session, Initialize().dump()))["result"]["serverInfo"]["version"] == BS_VERSION);
	CHECK(connection.HandleRequest(registry, session, R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty());
	CHECK(ParseJson(connection.HandleRequest(registry, session, Request("tools/list").dump()))["result"]["tools"].size() == registry.GetCommands().size());
	CHECK(ParseJson(connection.HandleRequest(registry, session, R"({"command":"scene.info"})"))["error"]["code"] == -32600);
	const auto shutdown = ParseJson(connection.ShutdownResponse(Request("ping").dump()));
	CHECK(shutdown["jsonrpc"] == "2.0");
	CHECK(shutdown["id"] == 7);
	CHECK(shutdown["error"]["code"] == -32603);
	CHECK(connection.ShutdownResponse(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty());
	CHECK(connection.ShutdownResponse(R"({"jsonrpc":"2.0","id":null,"error":{}})").empty());
	AutomationConnection fresh;
	CHECK(ParseJson(fresh.ShutdownResponse(Request("ping").dump()))["id"] == 7);
	CHECK(ParseJson(fresh.HandleRequest(registry, session, Request("tools/list").dump()))["error"]["code"] == -32600);
	AutomationConnection legacy;
	CHECK(ParseJson(legacy.HandleRequest(registry, session, "{bad}"))["ok"] == false);
	CHECK(ParseJson(legacy.HandleRequest(registry, session, R"({"command":"scene.info"})"))["ok"] == true);
	CHECK(legacy.HandleRequest(registry, session, "").empty());
	CHECK(ParseJson(legacy.ShutdownResponse("{}"))["ok"] == false);
	CHECK(ParseJson(legacy.HandleRequest(registry, session, Request("ping").dump()))["ok"] == false);
}

TEST_CASE("Schema registration failures are reported without throwing or replacing commands")
{
	CommandRegistry registry;
	const auto count = registry.GetCommands().size();
	CHECK(count == 83);
	const auto handler = [](AutomationSession&, const json& params) { return params; };
	for (const auto& name : { std::string(), std::string(129, 'a'), std::string("bad/name") })
		CHECK_FALSE(registry.Register({ name, "", CommandSchema::Object(), handler }));
	CHECK_FALSE(registry.Register({ "test.empty", "", CommandSchema::Object(), {} }));
	CHECK_FALSE(registry.Register({ "test.open", "", { { "type", "object" }, { "properties", json::object() } }, handler }));
	CHECK_FALSE(registry.Register({ "test.missing", "", { { "type", "object" }, { "additionalProperties", false } }, handler }));
	CHECK_FALSE(registry.Register({ "help", "replacement", CommandSchema::Object(), handler }));
	CHECK(registry.GetCommands().size() == count);
	CHECK(registry.Find("help")->Description != "replacement");
	std::string error;
	for (const json& bad : { json{ { "properties", { { "nested", { { "pattern", "x" } } } } } }, json{ { "items", { { "pattern", "x" } } } }, json{ { "anyOf", json::array({ { { "pattern", "x" } } }) } }, json{ { "minimum", 2 }, { "maximum", 1 } }, json{ { "minItems", 2 }, { "maxItems", 1 } } })
		CHECK_FALSE(CommandSchema::CheckDefinition(bad, error));
	const auto dt = registry.Find("play.step")->InputSchema["properties"]["dt"];
	CHECK_FALSE(CommandSchema::Validate(dt, 0, error));
	CHECK(CommandSchema::Validate(dt, 0.001, error));
	CHECK_FALSE(CommandSchema::Validate(dt, 0.26, error));
	CHECK_FALSE(CommandSchema::Validate(CommandSchema::Object({ { "nested", CommandSchema::Object({ { "field", CommandSchema::String() } }, { "field" }) } }), { { "nested", json::object() } }, error));
	CHECK(error == "missing parameter 'nested.field'");
}

TEST_CASE("All editor tool schemas reject invalid values before the host handler")
{
	CommandRegistry registry;
	AutomationSession session;
	bool called = false;
	const auto handler = [&](AutomationSession&, const json& params) { called = true; return params; };
	const std::vector<std::pair<std::string, json>> schemas = {
		{ "editor.history", EditorCommandSchemas::Undo() }, { "editor.transaction", EditorCommandSchemas::Transaction() }, { "editor.transform", EditorCommandSchemas::Transform() }, { "editor.component_set", EditorCommandSchemas::ComponentSet() }, { "editor.box_select", EditorCommandSchemas::BoxSelect() }, { "editor.delete", EditorCommandSchemas::Undo() }, { "editor.duplicate", EditorCommandSchemas::Undo() }, { "editor.viewport", ViewportSettings::Schema() }, { "editor.pick", EditorCommandSchemas::Pick() }, { "editor.vertex_snap", EditorCommandSchemas::VertexSnap() }, { "editor.select", EditorCommandSchemas::Select() }, { "editor.camera", EditorCommandSchemas::Camera() }, { "editor.undo", EditorCommandSchemas::Undo() }, { "editor.redo", EditorCommandSchemas::Redo() }, { "editor.screenshot", EditorCommandSchemas::Screenshot() }, { "editor.window_screenshot", EditorCommandSchemas::WindowScreenshot() }, { "editor.panel", EditorCommandSchemas::Panel() }, { "editor.game_view", GameViewSettings::Schema() }, { "editor.thumbnail", EditorCommandSchemas::Thumbnail() }, { "editor.material_preview", EditorCommandSchemas::MaterialPreview() }, { "editor.material_editor", EditorCommandSchemas::MaterialEditor() }, { "editor.animation_graph", EditorCommandSchemas::AnimationGraph() }, { "editor.preferences", EditorPreferences::Schema() }
	};
	McpServer server;
	for (const auto& [name, schema] : schemas)
		REQUIRE(registry.Register({ name, "Editor host", schema, handler }));
	Start(server, registry, session);
	CHECK(Reply(server, registry, session, Request("tools/list"))["result"]["tools"].size() == 106);
	for (const auto& [name, schema] : schemas)
	{
		INFO(name);
		called = false;
		CHECK(registry.HandleRequest(session, { { "command", name }, { "params", { { "typo", true } } } })["ok"] == false);
		CHECK_FALSE(called);
		json valid = schema["required"].empty() ? json::object() : json{ { "path", "capture.png" } };
		if (name == "editor.transaction")
			valid = { { "action", "begin" }, { "name", "Edit" } };
		if (name == "editor.transform")
			valid = { { "delta", { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1 } } };
		if (name == "editor.component_set")
			valid = { { "component", "Transform" }, { "data", json::object() } };
		if (name == "editor.box_select")
			valid = { { "start", { 0, 0 } }, { "end", { 10, 10 } } };
		if (name == "editor.pick")
			valid = { { "x", 0 }, { "y", 0 } };
		if (name == "editor.vertex_snap")
			valid = { { "source", { 0, 0 } }, { "target", { 1, 1 } } };
		if (name == "editor.material_preview")
			valid = { { "entity", "Ball" }, { "output", "ball.png" } };
		if (name == "editor.panel")
			valid = { { "name", "Project Settings" }, { "open", true } };
		CHECK(registry.HandleRequest(session, { { "command", name }, { "params", valid } })["ok"] == true);
		CHECK(called);
	}
	for (const json& bad : { json{ { "position", { 0, 0, 1e100 } } }, json{ { "target", { 0, 1 } } }, json{ { "position", nullptr } } })
	{
		called = false;
		CHECK(registry.HandleRequest(session, { { "command", "editor.camera" }, { "params", bad } })["ok"] == false);
		CHECK_FALSE(called);
	}
	for (const auto& [command, bad] : std::initializer_list<std::pair<const char*, json>>{
			 { "editor.panel", json::object() }, { "editor.panel", { { "name", "Timeline" } } }, { "editor.panel", { { "name", "Game" }, { "focus", "yes" } } }, { "editor.thumbnail", json::object() }, { "editor.thumbnail", { { "path", "" } } }, { "editor.thumbnail", { { "path", "a.png" }, { "refresh", 1 } } }, { "editor.material_preview", { { "entity", "Ball" } } }, { "editor.material_preview", { { "output", "a.png" } } }, { "editor.material_preview", { { "entity", true }, { "output", "a.png" } } }, { "editor.material_editor", { { "path", "" } } }, { "editor.material_editor", { { "close", "yes" } } }, { "editor.game_view", { { "resolution", { 63, 100 } } } }, { "editor.game_view", { { "path", "" } } }, { "editor.preferences", { { "theme", "Neon" } } }, { "editor.preferences", { { "cameraSpeed", 0 } } }, { "editor.preferences", { { "recentProjects", { 1 } } } } })
	{
		INFO(command << " " << bad.dump());
		called = false;
		CHECK(registry.HandleRequest(session, { { "command", command }, { "params", bad } })["ok"] == false);
		CHECK_FALSE(called);
	}
	// The closable windows are editor.panel windows, and only Project Settings is one.
	CHECK(EditorCommandSchemas::IsClosablePanel(EditorCommandSchemas::ProjectSettingsTitle));
	CHECK_FALSE(EditorCommandSchemas::IsClosablePanel("Game"));
	CHECK_FALSE(EditorCommandSchemas::IsClosablePanel("Timeline"));
	CHECK(EditorCommandSchemas::Panel()["properties"]["name"]["enum"].size() == std::size(EditorCommandSchemas::Panels));
	for (const json& bad : { json::object(), json{ { "path", "" } }, json{ { "path", "capture.png" }, { "width", 15 } }, json{ { "path", "capture.png" }, { "height", 8193 } } })
	{
		called = false;
		CHECK(registry.HandleRequest(session, { { "command", "editor.screenshot" }, { "params", bad } })["ok"] == false);
		CHECK_FALSE(called);
	}
}

TEST_CASE("MCP stream failures are diagnosed and invalid host UTF8 cannot break framing")
{
	CommandRegistry registry;
	AutomationSession session;
	McpServer server;
	uint64_t since = 0;
	Log::GetHistory().GetMessagesSince(0, since);
	std::istringstream brokenInput;
	brokenInput.setstate(std::ios::badbit);
	std::ostringstream output;
	CHECK_FALSE(server.Serve(brokenInput, output, registry, session));
	std::istringstream input(Request("ping").dump() + "\n");
	std::ostringstream brokenOutput;
	brokenOutput.setstate(std::ios::badbit);
	CHECK_FALSE(server.Serve(input, brokenOutput, registry, session));
	CHECK(BasaltTest::CountLogMessages(since, "failed to read stdin") == 1);
	CHECK(BasaltTest::CountLogMessages(since, "failed to write response") == 1);
	REQUIRE(registry.Register({ "test.utf8", "", CommandSchema::Object(), [](AutomationSession&, const json&) { return json{ { "value", std::string(1, static_cast<char>(0xff)) } }; } }));
	Start(server, registry, session);
	std::istringstream call(Request("tools/call", { { "name", "test.utf8" } }).dump() + "\n");
	CHECK(server.Serve(call, output, registry, session));
	const auto response = ParseJson(output.str());
	REQUIRE_FALSE(response.is_discarded());
	CHECK(response["result"]["isError"] == false);
	CHECK(ParseJson(response["result"]["content"][0]["text"].get<std::string>()) == response["result"]["structuredContent"]);
}

TEST_CASE("Every built-in schema admits a request the handler can read")
{
	CommandRegistry registry;
	for (const auto& [name, command] : registry.GetCommands())
	{
		INFO(name);
		BasaltTest::TempProject project("SchemaWitness");
		REQUIRE(project.IsValid());
		AutomationSession session;
		registry.Execute(session, "scene.new", { { "empty", true } });
		registry.Execute(session, "entity.create", { { "name", "SchemaEntity" } });
		json params = MinimalValue(command.InputSchema);
		// Keep every file-writing witness confined to this iteration's disposable project.
		if (params.contains("path"))
			params["path"] = (project.GetDirectory() / "SchemaArtifact").string();
		if (params.contains("output"))
			params["output"] = (project.GetDirectory() / "Export").string();
		std::string error;
		REQUIRE(CommandSchema::Validate(command.InputSchema, params, error));
		const auto response = registry.HandleRequest(session, { { "command", name }, { "params", params } });
		if (response["ok"] == false)
		{
			error = response["error"].get<std::string>();
			INFO(error);
			CHECK(error.find("unknown parameter") == std::string::npos);
			CHECK(error.find("missing parameter") == std::string::npos);
			CHECK(error.find("parameter '") == std::string::npos);
		}
	}
}
