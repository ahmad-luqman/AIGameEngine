#include "Basalt/Project/SmokeTest.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/Platform.h"

#include <spdlog/fmt/fmt.h>

#include <atomic>
#include <string>
#include <string_view>
#include <system_error>

namespace Basalt {

	namespace {

		// Exit codes of a --frames runtime run (RuntimeApp.cpp and RuntimeLayer.cpp; AGENTS.md lists them).
		const char* DescribeRuntimeExitCode(int code)
		{
			switch (code)
			{
				case 1:
					return "the game failed to start or render (bad option or project, missing or broken start scene, renderer failure)";
				case 3:
					// Smoke tests run headless, so this is not GPU validation: on Windows abort() exits with 3.
					return "the game aborted (or, with a GPU, reported validation errors)";
				case 4:
					return "a screenshot could not be written";
				case 5:
					return "the validation layer was required but missing";
				case 6:
					return "dynamic resolution did not run";
				case 7:
					return "scripts reported errors";
				default:
					return "an unexpected exit code";
			}
		}

		constexpr std::string_view NotRunPrefix = "the game could not be run: ";

		SmokeTestResult Fail(std::string summary)
		{
			SmokeTestResult result;
			result.Process.Error = summary;
			result.Summary = std::string(NotRunPrefix) + std::move(summary);
			return result;
		}

	}

	std::string LastLines(const std::string& text, size_t lines)
	{
		if (lines == 0)
			return {};
		// A trailing newline ends the last line rather than starting an empty one.
		size_t end = !text.empty() && text.back() == '\n' ? text.size() - 1 : text.size();
		size_t start = 0;
		for (size_t found = 0; found < lines; found++)
		{
			const size_t newline = end == 0 ? std::string::npos : text.rfind('\n', end - 1);
			if (newline == std::string::npos)
				return text;
			start = newline + 1;
			end = newline;
		}
		return text.substr(start);
	}

	SmokeTestResult RunSmokeTest(const std::filesystem::path& executable, uint32_t frames, std::chrono::milliseconds timeout)
	{
		if (frames == 0)
			return Fail("a smoke test needs at least one frame");
		std::error_code error;
		const std::filesystem::path program = std::filesystem::absolute(executable, error);
		if (error)
			return Fail("invalid executable path '" + FileSystem::PathToUtf8(executable) + "': " + error.message());
		// The game's save data goes to a fresh directory, never the player's (and never next to the game).
		const std::filesystem::path temporary = std::filesystem::temp_directory_path(error);
		if (error || !temporary.is_absolute())
			return Fail("cannot find the temporary directory for the game's save data" + (error ? ": " + error.message() : std::string()));
		// Unique without std::random_device (which may throw): the clock, plus a counter for runs in the same tick.
		static std::atomic<uint32_t> s_Run{ 0 };
		const std::filesystem::path userData = temporary / fmt::format("BasaltSmokeTest-{:x}-{}", std::chrono::steady_clock::now().time_since_epoch().count(), s_Run++);

		ProcessSpec spec;
		spec.Executable = program;
		spec.Arguments = { "--headless", "--frames", std::to_string(frames), "--fail-on-script-errors" };
		// An exported game finds its project next to the executable; run it from there as a player would.
		spec.WorkingDirectory = program.parent_path();
		spec.Timeout = timeout;
		spec.Environment = { { Platform::UserDataDirectoryVariable, FileSystem::PathToUtf8(userData) } };
		SmokeTestResult result;
		result.Process = RunProcess(spec);
		std::filesystem::remove_all(userData, error);
		if (error)
			BS_CORE_WARN("Smoke test: cannot remove its save data directory '{}': {}", FileSystem::PathToUtf8(userData), error.message());

		result.Passed = result.Process.Succeeded();
		switch (result.Process.Outcome)
		{
			case ProcessOutcome::Failed:
				result.Summary = std::string(NotRunPrefix) + result.Process.Error;
				break;
			case ProcessOutcome::TimedOut:
				result.Summary = fmt::format("the game did not finish {} frames within {} s and was stopped", frames, static_cast<double>(timeout.count()) / 1000.0);
				break;
			case ProcessOutcome::Crashed:
				result.Summary = "the game crashed: " + result.Process.Error;
				break;
			case ProcessOutcome::Exited:
				// --frames is a limit: a game may also quit earlier (Application.Quit), which is still a clean exit.
				result.Summary = result.Passed ? fmt::format("the game ran (up to {} frames) and exited cleanly", frames) : fmt::format("the game exited with code {}: {}", result.Process.ExitCode, DescribeRuntimeExitCode(result.Process.ExitCode));
				break;
		}
		return result;
	}

}
