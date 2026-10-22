// basalt - headless access to Basalt's automation API.
//
//   basalt [options] <command> [params-json]   run one command, print the response
//   basalt [options] batch <file.json | ->     run an array of requests, print an array of responses
//   basalt [options] mcp                       MCP JSON-RPC on stdin/stdout
//   basalt [options] serve                     JSON requests on stdin (one per line), responses on stdout
//
// Options:
//   --project <dir>   open this project first
//   --scene <path>    open this project-relative scene first; it is saved automatically after
//                     successful commands or clean MCP EOF (use --no-save to prevent that)
//   --no-save         never save automatically
//   --verbose         also print engine info logs (stderr)
//
// Responses go to stdout as JSON; logs go to stderr. Legacy modes fail if any command fails.
// MCP tool errors are replies; only startup, transport or auto-save failure makes the process fail.

#include "Basalt/Audio/AudioEngine.h"
#include "Basalt/Automation/AutomationSession.h"
#include "Basalt/Automation/CommandRegistry.h"
#include "Basalt/Automation/McpServer.h"
#include "Basalt/Core/CommandLine.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"

#include <iostream>
#include <string>

using namespace Basalt;

namespace {

	// Script errors, log lines and names in responses may not be valid UTF-8; the output must still be JSON.
	std::string Dump(const nlohmann::json& response, int indent)
	{
		return response.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
	}

	void PrintUsage()
	{
		std::cerr << "usage: basalt [--project <dir>] [--scene <path>] [--no-save] [--verbose] <command> [params-json]\n"
				  << "       basalt [...] batch <requests.json | ->\n"
				  << "       basalt [...] serve | mcp\n"
				  << "Run 'basalt help' to list commands.\n";
	}

	bool AllSucceeded(const nlohmann::json& response)
	{
		if (response.is_array())
		{
			for (const nlohmann::json& item : response)
			{
				if (!AllSucceeded(item))
					return false;
			}
			return true;
		}
		return response.value("ok", false);
	}

	// Saves the scene opened with --scene after changes, so one-shot invocations persist their work.
	bool AutoSave(const CommandRegistry& registry, AutomationSession& session, bool enabled)
	{
		if (!enabled)
			return true;
		// The scene to save waits behind the open prefab, and saving it would need prefab.close first.
		if (session.IsEditingPrefab() && (session.IsDirty() || session.IsWaitingSceneDirty()))
		{
			std::cerr << "basalt: '" << session.GetPrefabPath() << "' is still open, so unsaved edits (in the prefab or in the scene waiting behind it) were not saved: prefab.save and prefab.close it first\n";
			return false;
		}
		if (!session.IsDirty() || session.GetScenePath().empty())
			return true;
		const nlohmann::json response = registry.HandleRequest(session, { { "command", "scene.save" } });
		if (!response.value("ok", false))
			std::cerr << "basalt: auto-save failed: " << response.value("error", "") << "\n";
		return response.value("ok", false);
	}

}

int main(int argc, char** argv)
{
	Log::Init();
	const CommandLine commandLine(argc, argv, { "project", "scene" });
	Log::SetConsoleLevel(commandLine.HasFlag("verbose") ? LogLevel::Info : LogLevel::Warn);
	for (const std::string& error : commandLine.GetErrors())
		std::cerr << "basalt: " << error << "\n";

	const std::vector<std::string>& positional = commandLine.GetPositional();
	if (positional.empty() || !commandLine.GetErrors().empty())
	{
		PrintUsage();
		return 2;
	}

	const std::string& mode = positional[0];
	AudioEngine::Init(true);
	CommandRegistry registry;
	AutomationSession session;
	session.HostName = "cli";
	int exitCode = 0;

	auto run = [&](const nlohmann::json& request) {
		const nlohmann::json response = registry.HandleRequest(session, request);
		if (!AllSucceeded(response))
			exitCode = 1;
		return response;
	};

	// Startup: project and scene.
	if (const auto project = commandLine.GetValue("project"))
	{
		const nlohmann::json response = run({ { "command", "project.open" }, { "params", { { "path", *project } } } });
		if (!response.value("ok", false))
		{
			(mode == "mcp" ? std::cerr : std::cout) << Dump(response, 2) << std::endl;
			return 1;
		}
	}
	if (const auto scene = commandLine.GetValue("scene"))
	{
		const nlohmann::json response = run({ { "command", "scene.open" }, { "params", { { "path", *scene } } } });
		if (!response.value("ok", false))
		{
			(mode == "mcp" ? std::cerr : std::cout) << Dump(response, 2) << std::endl;
			return 1;
		}
		// Opening merges instances of prefabs saved since the scene was. That alone must not make a read-only
		// invocation rewrite the scene: it is saved with the session's own edits, if any.
		const nlohmann::json& result = response["result"];
		if (result.value("prefabInstances", 0) > 0)
			std::cerr << "basalt: merged " << result["prefabInstances"].get<size_t>() << " prefab instance(s) saved since the scene was\n";
		if (const auto warnings = result.find("warnings"); warnings != result.end())
			for (const auto& warning : *warnings)
				std::cerr << "basalt: " << warning.get<std::string>() << "\n";
		session.ClearDirty();
	}
	const bool autoSave = commandLine.GetValue("scene").has_value() && !commandLine.HasFlag("no-save");

	if (mode == "mcp")
	{
		McpServer server;
		if (!server.Serve(std::cin, std::cout, registry, session))
			exitCode = 1;
	}
	else if (mode == "serve")
	{
		std::string line;
		while (std::getline(std::cin, line))
		{
			if (line.empty())
				continue;
			const nlohmann::json request = ParseJson(line);
			const nlohmann::json response = request.is_discarded() ? nlohmann::json{ { "ok", false }, { "error", "invalid JSON" } } : run(request);
			std::cout << Dump(response, -1) << std::endl;
		}
	}
	else if (mode == "batch")
	{
		if (positional.size() < 2)
		{
			PrintUsage();
			return 2;
		}
		std::string text;
		if (positional[1] == "-")
			text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
		else if (auto file = FileSystem::ReadTextFile(positional[1]))
			text = *file;
		else
		{
			std::cerr << "basalt: cannot read '" << positional[1] << "'\n";
			return 2;
		}
		const nlohmann::json requests = ParseJson(text);
		if (requests.is_discarded())
		{
			std::cerr << "basalt: batch input is not valid JSON\n";
			return 2;
		}
		std::cout << Dump(run(requests), 2) << std::endl;
	}
	else
	{
		nlohmann::json params = nlohmann::json::object();
		if (positional.size() >= 2)
		{
			params = ParseJson(positional[1]);
			if (mode == "help" && params.is_discarded())
				params = { { "command", positional[1] } };
			if (params.is_discarded() || !params.is_object())
			{
				std::cerr << "basalt: params must be a JSON object, e.g. '{\"name\": \"Player\"}'\n";
				return 2;
			}
		}
		std::cout << Dump(run({ { "command", mode }, { "params", params } }), 2) << std::endl;
	}

	session.StopPlay();
	if (autoSave && session.IsDirty() && exitCode != 0)
		std::cerr << "basalt: dirty scene was not auto-saved because the session failed\n";
	if (!AutoSave(registry, session, autoSave && exitCode == 0))
		exitCode = 1;
	AudioEngine::Shutdown();
	Log::Shutdown();
	return exitCode;
}
