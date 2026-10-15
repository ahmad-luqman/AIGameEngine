#include "Basalt/Automation/McpServer.h"

#include "Basalt/Automation/CommandRegistry.h"
#include "Basalt/Core/JsonUtils.h"

#include <istream>
#include <ostream>

namespace Basalt {

	using json = nlohmann::json;

	bool McpServer::Serve(std::istream& input, std::ostream& output, const CommandRegistry& registry, AutomationSession& session)
	{
		constexpr size_t MaxLineBytes = size_t{ 64 } * 1024 * 1024;
		std::string line;
		char byte;
		while (input.get(byte))
		{
			if (byte != '\n')
			{
				if (line.size() == MaxLineBytes)
				{
					output << Error(nullptr, -32600, "Request too large").dump() << std::endl;
					return false;
				}
				line.push_back(byte);
				continue;
			}
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (const auto response = HandleRequest(registry, session, ParseJson(line)))
				output << response->dump(-1, ' ', false, json::error_handler_t::replace) << std::endl;
			line.clear();
			if (!output)
				return false;
		}
		// A trailing partial frame is never executed: stdio messages must end in a newline.
		return input.eof() && line.empty() && static_cast<bool>(output);
	}

	json McpServer::Error(const json& id, int code, const std::string& message)
	{
		return { { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", code }, { "message", message } } } };
	}

	std::optional<json> McpServer::HandleRequest(const CommandRegistry& registry, AutomationSession& session, const json& request)
	{
		if (request.is_discarded())
			return Error(nullptr, -32700, "Parse error");
		if (!request.is_object())
			return Error(nullptr, -32600, "Expected a JSON-RPC request object (batches are not supported)");
		const bool hasID = request.contains("id");
		const json id = request.value("id", json());
		if (hasID && !id.is_string() && !id.is_number_integer())
			return Error(nullptr, -32600, "Request id must be a string or integer");
		// The server issues no requests, so unsolicited client responses have nothing to complete.
		// Never answer a response with another response (which could create an error loop).
		if (hasID && request.value("jsonrpc", json()) == "2.0" && !request.contains("method") && (request.contains("result") != request.contains("error")))
			return std::nullopt;
		if (!request.contains("jsonrpc") || request["jsonrpc"] != "2.0" || !request.contains("method") || !request["method"].is_string())
			return Error(id, -32600, "Expected jsonrpc 2.0 and a string method");
		const std::string method = request["method"].get<std::string>();
		const json params = request.value("params", json::object());
		// Notifications must not receive a reply, even if their method/parameters are unsupported.
		if (!hasID)
		{
			if (method == "notifications/initialized" && params.is_object() && m_State == State::Initializing)
				m_State = State::Ready;
			return std::nullopt;
		}
		if (!params.is_object())
			return Error(id, -32602, "params must be an object");
		const auto result = [&](json value) { return json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move(value) } }; };
		if (method == "ping")
			return result(json::object());
		if (method == "initialize")
		{
			if (m_State != State::New)
				return Error(id, -32600, "Connection is already initialized");
			if (!params.contains("protocolVersion") || !params["protocolVersion"].is_string() || !params.contains("capabilities") || !params["capabilities"].is_object() || !params.contains("clientInfo") || !params["clientInfo"].is_object())
				return Error(id, -32602, "initialize requires protocolVersion, capabilities and clientInfo");
			const auto& client = params["clientInfo"];
			if (!client.contains("name") || !client["name"].is_string() || !client.contains("version") || !client["version"].is_string())
				return Error(id, -32602, "clientInfo requires string name and version");
			m_State = State::Initializing;
			return result({ { "protocolVersion", ProtocolVersion }, { "capabilities", { { "tools", json::object() } } }, { "serverInfo", { { "name", "basalt" }, { "version", "1.0.0" } } } });
		}
		if (m_State != State::Ready)
			return Error(id, -32600, "Initialize the connection and send notifications/initialized first");
		if (method == "tools/list")
		{
			if (params.contains("cursor"))
				return Error(id, -32602, "This server returns all tools in one page; omit cursor");
			json tools = json::array();
			for (const auto& [name, command] : registry.GetCommands())
				tools.push_back({ { "name", name }, { "description", command.Description }, { "inputSchema", command.InputSchema } });
			return result({ { "tools", std::move(tools) } });
		}
		if (method == "tools/call")
		{
			if (!params.contains("name") || !params["name"].is_string() || (params.contains("arguments") && !params["arguments"].is_object()))
				return Error(id, -32602, "tools/call requires a string name and object arguments");
			const std::string name = params["name"].get<std::string>();
			if (!registry.Find(name))
				return Error(id, -32602, "Unknown tool: " + name);
			const json response = registry.HandleRequest(session, { { "command", name }, { "params", params.value("arguments", json::object()) } });
			const bool ok = response["ok"].get<bool>();
			const json& value = response[ok ? "result" : "error"];
			json output = { { "content", json::array({ { { "type", "text" }, { "text", ok ? value.dump(-1, ' ', false, json::error_handler_t::replace) : value.get<std::string>() } } }) }, { "isError", !ok } };
			if (ok && value.is_object())
				output["structuredContent"] = value;
			return result(std::move(output));
		}
		return Error(id, -32601, "Method not found: " + method);
	}

}
