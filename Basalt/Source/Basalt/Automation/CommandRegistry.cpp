#include "Basalt/Automation/CommandRegistry.h"

#include "Basalt/Automation/AutomationSession.h"
#include "Basalt/Automation/CommandSchema.h"
#include "Basalt/Core/Log.h"

#include <algorithm>
#include <exception>

namespace Basalt {

	CommandRegistry::CommandRegistry()
	{
		RegisterBuiltinCommands(*this);
	}

	bool CommandRegistry::Register(CommandInfo command)
	{
		const auto fail = [&](const std::string& error) { BS_CORE_ERROR("Automation: cannot register command '{}': {}", command.Name, error); return false; };
		if (command.Name.empty() || command.Name.size() > 128 || !command.Handler || !std::all_of(command.Name.begin(), command.Name.end(), [](unsigned char c) {
				return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
			}))
			return fail("command needs a handler and an MCP-compatible name");
		std::string error;
		if (!CommandSchema::CheckDefinition(command.InputSchema, error))
			return fail(error);
		if (command.InputSchema.value("type", "") != "object" || !command.InputSchema.contains("properties") || command.InputSchema.value("additionalProperties", true))
			return fail("command schema must be a closed object with properties");
		if (m_Commands.contains(command.Name))
			return fail("duplicate command name");
		const std::string name = command.Name;
		m_Commands.emplace(name, std::move(command));
		return true;
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
		// While a prefab is open, the waiting scene must not be replaced or saved, and asset moves would migrate only the
		// prefab's scene while the waiting scene's references go stale. Every such command is an edit boundary; export
		// would read the waiting scene's file while its edits are unsaved.
		const bool leavesPrefab = command == "prefab.save" || command == "prefab.close";
		const bool replacesEditState = info->Policy == CommandPolicy::EditBoundary && !leavesPrefab;
		if (session.IsEditingPrefab() && (replacesEditState || command == "project.export"))
			throw CommandError(command + " is unavailable while '" + session.GetPrefabPath() + "' is open: prefab.save or prefab.close first");
		if (!params.is_object())
			throw CommandError("'params' must be a JSON object");
		// Keep the legacy diagnostic listing available parameters before the general schema validator.
		for (const auto& [key, value] : params.items())
		{
			if (!info->InputSchema["properties"].contains(key))
			{
				std::string message = "unknown parameter '";
				message.append(key).append("' for ").append(command);
				if (info->InputSchema["properties"].empty())
					message += " (it takes none)";
				else
				{
					message += " (expected: ";
					for (auto it = info->InputSchema["properties"].begin(); it != info->InputSchema["properties"].end(); ++it)
					{
						if (it != info->InputSchema["properties"].begin())
							message += ", ";
						message += it.key();
					}
					message += ')';
				}
				throw CommandError(message);
			}
		}
		std::string error;
		if (!CommandSchema::Validate(info->InputSchema, params, error))
			throw CommandError(error);
		const bool outer = session.m_CommandDepth == 0;
		CommandObserver* observer = outer ? session.m_CommandObserver : nullptr;
		if (observer)
			observer->BeforeCommand(*info, params);
		nlohmann::json result;
		std::exception_ptr failure;
		{
			struct DepthGuard
			{
				size_t& Depth;
				explicit DepthGuard(size_t& depth)
					: Depth(depth)
				{
					++Depth;
				}
				~DepthGuard() { --Depth; }
			} depthGuard(session.m_CommandDepth);
			try
			{
				result = info->Handler(session, params);
			}
			catch (...)
			{
				failure = std::current_exception();
			}
		}
		// Capture partial edits under the failed command's name without masking its original error.
		if (observer)
			observer->AfterCommand(*info, params);
		if (failure)
			std::rethrow_exception(failure);
		return result;
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

		if (request.is_object() && request.contains("expectError"))
		{
			const nlohmann::json& expected = request["expectError"];
			if (!expected.is_string() || expected.get<std::string>().empty())
			{
				response = { { "ok", false }, { "error", "'expectError' must be a non-empty string" } };
				if (request.contains("id"))
					response["id"] = request["id"];
			}
			else if (response["ok"] == true)
			{
				response.erase("result");
				response["ok"] = false;
				response["error"] = "expected an error containing '" + expected.get<std::string>() + "', but the command succeeded";
			}
			else if (response["error"].get<std::string>().find(expected.get<std::string>()) == std::string::npos)
			{
				response["error"] = "expected an error containing '" + expected.get<std::string>() + "', got: " + response["error"].get<std::string>();
			}
			else
			{
				response["ok"] = true;
				response["expectedError"] = response["error"];
				response.erase("error");
			}
		}
		return response;
	}

}
