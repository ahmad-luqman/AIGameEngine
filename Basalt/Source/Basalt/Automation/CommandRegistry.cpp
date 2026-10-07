#include "Basalt/Automation/CommandRegistry.h"

#include "Basalt/Automation/AutomationSession.h"
#include "Basalt/Core/Log.h"

namespace Basalt {

	CommandRegistry::CommandRegistry()
	{
		RegisterBuiltinCommands(*this);
	}

	void CommandRegistry::Register(CommandInfo command)
	{
		const std::string name = command.Name;
		m_Commands[name] = std::move(command);
	}

	const CommandInfo* CommandRegistry::Find(const std::string& name) const
	{
		auto it = m_Commands.find(name);
		return it == m_Commands.end() ? nullptr : &it->second;
	}

	nlohmann::json CommandRegistry::Execute(AutomationSession& session, const std::string& command, const nlohmann::json& params) const
	{
		const CommandInfo* info = Find(command);
		if (!info)
			throw CommandError("unknown command '" + command + "' (run 'help' to list commands)");
		if (!params.is_object())
			throw CommandError("'params' must be a JSON object");
		return info->Handler(session, params);
	}

	nlohmann::json CommandRegistry::HandleRequest(AutomationSession& session, const nlohmann::json& request) const
	{
		if (request.is_array())
		{
			// One level of batching only: nested arrays would recurse once per level of untrusted input.
			nlohmann::json responses = nlohmann::json::array();
			for (const nlohmann::json& item : request)
			{
				if (item.is_array())
					responses.push_back({ { "ok", false }, { "error", "batches cannot be nested" } });
				else
					responses.push_back(HandleRequest(session, item));
			}
			return responses;
		}

		nlohmann::json response = nlohmann::json::object();
		if (request.is_object() && request.contains("id"))
			response["id"] = request["id"];

		try
		{
			if (!request.is_object() || !request.contains("command") || !request["command"].is_string())
				throw CommandError("a request must be an object with a string 'command'");
			const nlohmann::json params = request.value("params", nlohmann::json::object());
			response["result"] = Execute(session, request["command"].get<std::string>(), params);
			response["ok"] = true;
		}
		catch (const CommandError& e)
		{
			response["ok"] = false;
			response["error"] = e.what();
		}
		catch (const nlohmann::json::exception& e)
		{
			response["ok"] = false;
			response["error"] = std::string("invalid parameter: ") + e.what();
		}
		catch (const std::exception& e)
		{
			BS_CORE_ERROR("Automation: command failed unexpectedly: {}", e.what());
			response["ok"] = false;
			response["error"] = std::string("internal error: ") + e.what();
		}
		return response;
	}

}
