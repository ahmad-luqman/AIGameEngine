// Automation requests (TCP server / CLI) through CommandRegistry::HandleRequest. Only commands that act on
// the in-memory session are allowed: no file system access (scene.diff only between snapshots), no Lua
// execution (an infinite loop would hang the fuzzer), and play.step is capped so a single input cannot run
// for minutes.
#include "FuzzCommon.h"

#include <Breccia/Automation/AutomationSession.h>
#include <Breccia/Automation/CommandRegistry.h>
#include <Breccia/Core/Input.h>
#include <Breccia/Core/JsonUtils.h>
#include <Breccia/Renderer/GraphicsSettings.h>

#include <set>
#include <string>

namespace {

	bool IsAllowed(const nlohmann::json& request)
	{
		static const std::set<std::string> allowed = {
			"help", "log.get", "project.info", "scene.get", "scene.hash", "scene.info", "scene.new", "scene.settings", "entity.create", "entity.destroy",
			"entity.duplicate", "entity.get", "entity.list", "entity.rename", "entity.set_parent", "entity.transform", "component.get", "component.remove",
			"component.set", "component.types", "play.start", "play.step", "play.stop", "input.key", "input.mouse", "input.gamepad", "ui.layout", "replay.record_start",
			"scene.snapshot", "scene.diff", "assert.entity_exists", "assert.component", "events.subscribe", "events.poll", "events.unsubscribe"
		};
		if (!request.is_object() || !request.contains("command") || !request["command"].is_string())
			return true; // malformed requests exercise the protocol error paths
		if (!allowed.contains(request["command"].get<std::string>()))
			return false;
		if (request.contains("params") && request["params"].is_object())
		{
			const nlohmann::json& params = request["params"];
			if (params.contains("frames") && params["frames"].is_number() && params["frames"].get<double>() > 30)
				return false;
			// Names ending in .bscene are scene files.
			for (const char* scene : { "from", "to" })
			{
				if (params.contains(scene) && params[scene].is_string() && params[scene].get<std::string>().ends_with(".bscene"))
					return false;
			}
		}
		return true;
	}

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BrecciaFuzz::Init();
	static const Breccia::CommandRegistry registry;
	nlohmann::json request = Breccia::ParseJson(BrecciaFuzz::AsText(data, size));
	if (request.is_discarded())
		return 0;
	// Exercise the project/Lua graphics parser without allowing project.set to write files.
	if (request.is_object() && request.contains("params") && request["params"].is_object() && request["params"].contains("graphics"))
	{
		Breccia::GraphicsSettings graphics;
		std::string error;
		if (Breccia::PatchGraphicsSettings(request["params"]["graphics"], graphics, error))
			Breccia::SerializeGraphicsSettings(graphics);
	}
	if (request.is_array())
	{
		for (const nlohmann::json& item : request)
		{
			if (!item.is_array() && !IsAllowed(item))
				return 0;
		}
	}
	else if (!IsAllowed(request))
	{
		return 0;
	}

	Breccia::AutomationSession session;
	registry.HandleRequest(session, request);
	session.StopPlay();
	Breccia::Input::Reset();
	Breccia::Input::ForgetInjectedGamepads();
	return 0;
}
