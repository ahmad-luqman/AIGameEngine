// Automation requests (TCP server / CLI) through CommandRegistry::HandleRequest. Only commands that act on
// the in-memory session are allowed: no file system access, no Lua execution (an infinite loop would hang
// the fuzzer), and play.step is capped so a single input cannot run for minutes.
#include "FuzzCommon.h"

#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Core/Input.h>
#include <Basalt/Core/JsonUtils.h>

#include <set>
#include <string>

namespace {

	bool IsAllowed(const nlohmann::json& request)
	{
		static const std::set<std::string> allowed = {
			"help", "log.get", "project.info", "scene.get", "scene.hash", "scene.info", "scene.new", "scene.settings", "entity.create", "entity.destroy",
			"entity.duplicate", "entity.get", "entity.list", "entity.rename", "entity.set_parent", "entity.transform", "component.get", "component.remove",
			"component.set", "component.types", "play.start", "play.step", "play.stop", "input.key", "input.mouse", "replay.record_start"
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
		}
		return true;
	}

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	static const Basalt::CommandRegistry registry;
	nlohmann::json request = Basalt::ParseJson(BasaltFuzz::AsText(data, size));
	if (request.is_discarded())
		return 0;
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

	Basalt::AutomationSession session;
	registry.HandleRequest(session, request);
	session.StopPlay();
	Basalt::Input::Reset();
	return 0;
}
