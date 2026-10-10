#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace Basalt {

	class AutomationSession;

	// Thrown by command handlers to report a user-facing error (bad parameters, missing entity...).
	class CommandError : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	struct CommandInfo
	{
		std::string Name;
		std::string Description;
		// Human-readable parameter documentation: name -> "type, description".
		std::map<std::string, std::string> Parameters;
		std::function<nlohmann::json(AutomationSession&, const nlohmann::json&)> Handler;
	};

	// The automation API: named commands taking and returning JSON. Used by the basalt CLI and by the
	// editor's TCP server, so AI agents (and scripts) can build games without the GUI.
	//
	// Protocol (one request):   { "id": <any>, "command": "entity.create", "params": { ... } }
	// Response:                 { "id": <same>, "ok": true, "result": { ... } }
	//                           { "id": <same>, "ok": false, "error": "message" }
	// A JSON array of requests is executed in order and answered with an array of responses.
	// A request with "expectError": "<text>" (for tests) succeeds only if the command fails with an error
	// containing <text>; the response then has "ok": true and the error as "expectedError".
	class CommandRegistry
	{
	public:
		CommandRegistry();

		void Register(CommandInfo command);
		const CommandInfo* Find(const std::string& name) const;
		const std::map<std::string, CommandInfo>& GetCommands() const { return m_Commands; }

		nlohmann::json Execute(AutomationSession& session, const std::string& command, const nlohmann::json& params) const;
		// Handles a request object or an array of requests (see protocol above).
		nlohmann::json HandleRequest(AutomationSession& session, const nlohmann::json& request) const;

	private:
		std::map<std::string, CommandInfo> m_Commands;
	};

	// Registers every engine command (project, scene, entity, component, prefab, asset, script, play,
	// input, log). Defined in BuiltinCommands.cpp.
	void RegisterBuiltinCommands(CommandRegistry& registry);

}
