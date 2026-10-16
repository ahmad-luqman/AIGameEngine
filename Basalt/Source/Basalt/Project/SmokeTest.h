#pragma once

#include "Basalt/Core/Process.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Basalt {

	// How a smoke test went: the exported game's process result and a one-line verdict.
	struct SmokeTestResult
	{
		ProcessResult Process;
		// One line: what happened, with the meaning of a known runtime exit code.
		std::string Summary;

		// Exit code 0 within the timeout.
		bool Passed() const { return Process.Succeeded(); }
	};

	// Runs an exported game headless (no GPU or window needed) for up to `frames` frames at the runtime's fixed
	// 1/60 s step and reports whether it started, played and exited cleanly: exit code 0 and no script errors
	// (--fail-on-script-errors) within the timeout. Other engine errors in the log (a model that does not load)
	// do not fail it; script errors OnDestroy raises as the game stops do. The game's save data goes to a fresh temporary directory, never the player's. Blocks the
	// caller.
	SmokeTestResult RunSmokeTest(const std::filesystem::path& executable, uint32_t frames, std::chrono::milliseconds timeout);

	// The last `lines` lines of a log (smoke test output in command results and error messages).
	std::string LastLines(const std::string& text, size_t lines);

}
