#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Basalt {

	// What a script hot reload (ScriptEngine::ReloadScripts) did.
	struct ScriptReloadResult
	{
		// Scripts whose class was replaced (sorted).
		std::vector<std::string> Reloaded;
		// Scripts that did not load again, with the error (the previous version keeps running).
		std::vector<std::pair<std::string, std::string>> Failed;
		// Instances moved onto a new class, plus those created for a class that failed to load before.
		size_t Instances = 0;
	};

}
