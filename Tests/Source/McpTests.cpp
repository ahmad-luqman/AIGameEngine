#include <doctest/doctest.h>

#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Automation/CommandSchema.h>
#include <Basalt/Automation/McpServer.h>
#include <Basalt/Core/JsonUtils.h>
#include <Basalt/Scene/Scene.h>

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
		CHECK((*server.HandleRequest(registry, session, Request("tools/list")))["error"]["code"] == -32600);
		CHECK((*server.HandleRequest(registry, session, Request("ping")))["result"].is_object());
		CHECK((*server.HandleRequest(registry, session, Request("initialize")))["error"]["code"] == -32602);
		auto initialize = Initialize();
		initialize["params"]["protocolVersion"] = "future-version";
		CHECK((*server.HandleRequest(registry, session, initialize))["result"]["protocolVersion"].get<std::string>() == std::string(McpServer::ProtocolVersion));
		CHECK((*server.HandleRequest(registry, session, Request("tools/list")))["error"]["code"] == -32600);
		CHECK_FALSE(server.HandleRequest(registry, session, { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } }));
		CHECK((*server.HandleRequest(registry, session, Initialize()))["error"]["code"] == -32600);
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
		CHECK((*server.HandleRequest(registry, session, Request("tools/list", { { "cursor", "bogus" } })))["error"]["code"] == -32602);
	}

	TEST_CASE("Tool calls share validation and failures do not mutate the scene")
	{
		CommandRegistry registry;
		AutomationSession session;
		McpServer server;
		Start(server, registry, session);
		const auto call = [&](const std::string& name, json arguments) {
			return *server.HandleRequest(registry, session, Request("tools/call", { { "name", name }, { "arguments", std::move(arguments) } }));
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
			CHECK((*server.HandleRequest(registry, session, bad))["error"]["code"] == -32600);
		CHECK((*server.HandleRequest(registry, session, ParseJson("{")))["error"]["code"] == -32700);
		CHECK((*server.HandleRequest(registry, session, Request("unknown")))["error"]["code"] == -32601);
		CHECK_FALSE(server.HandleRequest(registry, session, { { "jsonrpc", "2.0" }, { "id", 7 }, { "result", json::object() } }));
		auto ping = Request("ping");
		ping["id"] = "client-id";
		CHECK((*server.HandleRequest(registry, session, ping))["id"] == "client-id");
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
		CommandSchema::CheckDefinition(schema);
		CHECK(CommandSchema::Validate(schema, { { "id", UINT64_MAX }, { "position", { 1, 2.0, 3 } }, { "count", nullptr } }, error));
		CHECK(CommandSchema::Validate(schema, { { "id", "name" }, { "count", INT64_MAX } }, error));
		for (const json& bad : { json::object(), json{ { "id", -1 } }, json{ { "id", true } }, json{ { "id", 1 }, { "count", UINT64_MAX } }, json{ { "id", 1 }, { "position", { 1, 2 } } }, json{ { "id", 1 }, { "position", { 1, 2, "3" } } }, json{ { "id", 1 }, { "position", { 1, 2, 1e100 } } } })
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
		CHECK_THROWS_AS(CommandSchema::CheckDefinition({ { "pattern", "x" } }), std::invalid_argument);
		CHECK_THROWS_AS(CommandSchema::CheckDefinition({ { "type", "typo" } }), std::invalid_argument);
		CHECK_THROWS_AS(CommandSchema::CheckDefinition({ { "minimum", "x" } }), std::invalid_argument);
		CHECK_THROWS_AS(CommandSchema::CheckDefinition({ { "required", { "missing" } } }), std::invalid_argument);
		CHECK_THROWS_AS(CommandSchema::CheckDefinition({ { "anyOf", json::array() } }), std::invalid_argument);
	}
}
