#pragma once

#include "Basalt/Core/Process.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Basalt {

	struct SmokeTestResult
	{
		bool Passed = false;
		ProcessResult Process;
		// One line: what happened, with the meaning of a known runtime exit code.
		std::string Summary;
	};

	// Runs an exported game headless (no GPU or window needed) for `frames` fixed frames and reports whether it
	// started, played and exited cleanly: exit code 0, no script errors (--fail-on-script-errors), within the
	// timeout. The game's save data goes to a temporary directory, never the player's. Blocks the caller.
	SmokeTestResult RunSmokeTest(const std::filesystem::path& executable, uint32_t frames, std::chrono::milliseconds timeout);

	// The last `lines` lines of a log (smoke test output in command results and error messages).
	std::string LastLines(const std::string& text, size_t lines);

}
