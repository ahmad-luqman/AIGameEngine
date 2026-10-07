// basalt - headless access to Basalt's automation API.
//
//   basalt [options] <command> [params-json]   run one command, print the response
//   basalt [options] batch <file.json | ->     run an array of requests, print an array of responses
//   basalt [options] serve                     JSON requests on stdin (one per line), responses on stdout
//
// Options:
//   --project <dir>   open this project first
//   --scene <path>    open this project-relative scene first; it is saved automatically after
//                     successful commands that changed it (use --no-save to prevent that)
//   --no-save         never save automatically
//   --verbose         also print engine info logs (stderr)
//
// Responses go to stdout as JSON; logs go to stderr. Exit code: 0 if every command succeeded.

#include "Basalt/Audio/AudioEngine.h"
#include "Basalt/Automation/AutomationSession.h"
#include "Basalt/Automation/CommandRegistry.h"
#include "Basalt/Core/CommandLine.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"

#include <iostream>
#include <string>

using namespace Basalt;

namespace {

	void PrintUsage()
	{
		std::cerr << "usage: basalt [--project <dir>] [--scene <path>] [--no-save] [--verbose] <command> [params-json]\n"
				  << "       basalt [...] batch <requests.json | ->\n"
				  << "       basalt [...] serve\n"
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
	void AutoSave(const CommandRegistry& registry, AutomationSession& session, bool enabled)
	{
		if (!enabled || !session.IsDirty() || session.GetScenePath().empty() || session.IsPlaying())
			return;
		const nlohmann::json response = registry.HandleRequest(session, { { "command", "scene.save" } });
		if (!response.value("ok", false))
			std::cerr << "basalt: auto-save failed: " << response.value("error", "") << "\n";
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
			std::cout << response.dump(2) << std::endl;
			return 1;
		}
	}
	if (const auto scene = commandLine.GetValue("scene"))
	{
		const nlohmann::json response = run({ { "command", "scene.open" }, { "params", { { "path", *scene } } } });
		if (!response.value("ok", false))
		{
			std::cout << response.dump(2) << std::endl;
			return 1;
		}
	}
	const bool autoSave = commandLine.GetValue("scene").has_value() && !commandLine.HasFlag("no-save");

	const std::string& mode = positional[0];
	if (mode == "serve")
	{
		std::string line;
		while (std::getline(std::cin, line))
		{
			if (line.empty())
				continue;
			const nlohmann::json request = nlohmann::json::parse(line, nullptr, false);
			const nlohmann::json response = request.is_discarded() ? nlohmann::json{ { "ok", false }, { "error", "invalid JSON" } } : run(request);
			std::cout << response.dump() << std::endl;
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
		const nlohmann::json requests = nlohmann::json::parse(text, nullptr, false);
		if (requests.is_discarded())
		{
			std::cerr << "basalt: batch input is not valid JSON\n";
			return 2;
		}
		std::cout << run(requests).dump(2) << std::endl;
	}
	else
	{
		nlohmann::json params = nlohmann::json::object();
		if (positional.size() >= 2)
		{
			params = nlohmann::json::parse(positional[1], nullptr, false);
			if (params.is_discarded() || !params.is_object())
			{
				std::cerr << "basalt: params must be a JSON object, e.g. '{\"name\": \"Player\"}'\n";
				return 2;
			}
		}
		std::cout << run({ { "command", mode }, { "params", params } }).dump(2) << std::endl;
	}

	AutoSave(registry, session, autoSave && exitCode == 0);
	session.StopPlay();
	AudioEngine::Shutdown();
	Log::Shutdown();
	return exitCode;
}
