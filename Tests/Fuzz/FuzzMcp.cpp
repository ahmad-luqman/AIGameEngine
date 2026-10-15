// MCP envelope, lifecycle, discovery and schema validation with only read-only engine tools allowed.
#include "FuzzCommon.h"

#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Automation/McpServer.h>
#include <Basalt/Core/JsonUtils.h>

namespace {

	bool IsSafe(const nlohmann::json& request)
	{
		if (!request.is_object() || !request.contains("params") || !request["params"].is_object())
			return true;
		const auto& params = request["params"];
		if (!params.contains("name") || !params["name"].is_string())
			return true;
		return params["name"] == "help" || params["name"] == "scene.info" || params["name"] == "component.types" || params["name"] == "missing.tool";
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	static const Basalt::CommandRegistry registry;
	Basalt::AutomationSession session;
	Basalt::McpServer server;
	const auto request = Basalt::ParseJson(BasaltFuzz::AsText(data, size));
	if (!IsSafe(request))
		return 0;
	server.HandleRequest(registry, session, request);
	server.Reset();
	server.HandleRequest(registry, session, { { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "initialize" }, { "params", { { "protocolVersion", "2025-11-25" }, { "capabilities", nlohmann::json::object() }, { "clientInfo", { { "name", "fuzz" }, { "version", "1" } } } } } });
	server.HandleRequest(registry, session, { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } });
	server.HandleRequest(registry, session, request);
	return 0;
}
