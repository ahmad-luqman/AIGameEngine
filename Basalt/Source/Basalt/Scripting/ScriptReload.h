#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Basalt {

	// A script that did not load again during a hot reload.
	struct ScriptReloadFailure
	{
		std::string Path;
		std::string Error;
	};

	// What a script hot reload (ScriptEngine::ReloadScripts) did.
	struct ScriptReloadResult
	{
		// Scripts that loaded: classes replaced, and classes that had failed to load before (sorted).
		std::vector<std::string> Reloaded;
		// Scripts that did not load again (a class that loaded before keeps running its previous version).
		std::vector<ScriptReloadFailure> Failed;
		// Instances now running the new code: those moved onto a reloaded class (after OnReload, or OnCreate for one
		// that had not started or whose OnCreate failed) and those created for a class that failed to load before. An
		// instance whose callback raised an error is disabled, as after any callback error, and not counted; nor is one
		// whose script set its own metatable (kept as it is).
		size_t Instances = 0;
		// The errors raised while the instances were updated: property values, OnReload, OnCreate.
		std::vector<std::string> InstanceErrors;
		// Set when a Lua error stopped the reload part way (some classes may be replaced, others not).
		std::string Error;
	};

}
