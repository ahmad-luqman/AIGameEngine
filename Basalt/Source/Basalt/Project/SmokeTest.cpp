#include "Basalt/Project/SmokeTest.h"

#include "Basalt/Core/Platform.h"

#include <spdlog/fmt/fmt.h>

#include <system_error>

namespace Basalt {

	namespace {

		// Runtime exit codes (Basalt-Runtime/Source/RuntimeLayer.cpp; AGENTS.md lists them).
		const char* DescribeRuntimeExitCode(int code)
		{
			switch (code)
			{
				case 1:
					return "the game failed to start (bad project, missing start scene, or a scene that does not load)";
				case 3:
					return "GPU validation errors";
				case 4:
					return "a screenshot could not be written";
				case 5:
					return "the validation layer was required but missing";
				case 6:
					return "dynamic resolution did not run";
				case 7:
					return "scripts reported errors";
				default:
					return code >= 128 ? "the game crashed" : "an unexpected exit code";
			}
		}

	}

	std::string LastLines(const std::string& text, size_t lines)
	{
		size_t start = text.size();
		// Skip a trailing newline so it does not count as an empty last line.
		size_t end = !text.empty() && text.back() == '\n' ? text.size() - 1 : text.size();
		for (size_t found = 0; found < lines && start > 0;)
		{
			const size_t newline = text.rfind('\n', end == 0 ? 0 : end - 1);
			if (newline == std::string::npos)
			{
				start = 0;
				break;
			}
			start = newline + 1;
			end = newline;
			found++;
		}
		return text.substr(start);
	}

	SmokeTestResult RunSmokeTest(const std::filesystem::path& executable, uint32_t frames, std::chrono::milliseconds timeout)
	{
		SmokeTestResult result;
		std::error_code error;
		const std::filesystem::path program = std::filesystem::absolute(executable, error);
		const std::filesystem::path userData = std::filesystem::temp_directory_path(error) / fmt::format("BasaltSmokeTest-{}", std::chrono::steady_clock::now().time_since_epoch().count());

		ProcessSpec spec;
		spec.Executable = program;
		spec.Arguments = { "--headless", "--frames", std::to_string(frames), "--fail-on-script-errors" };
		// An exported game finds its project next to the executable; run it from there as a player would.
		spec.WorkingDirectory = program.parent_path();
		spec.Timeout = timeout;
		spec.Environment = { { Platform::UserDataDirectoryVariable, userData.string() } };
		result.Process = RunProcess(spec);
		std::filesystem::remove_all(userData, error);

		switch (result.Process.Outcome)
		{
			case ProcessOutcome::Failed:
				result.Summary = "the game could not be run: " + result.Process.Error;
				break;
			case ProcessOutcome::TimedOut:
				result.Summary = fmt::format("the game did not finish {} frames within {} s and was stopped", frames, timeout.count() / 1000.0);
				break;
			case ProcessOutcome::Exited:
				result.Passed = result.Process.ExitCode == 0;
				result.Summary = result.Passed ? fmt::format("the game ran {} frames and exited cleanly", frames) : fmt::format("the game exited with code {}: {}", result.Process.ExitCode, DescribeRuntimeExitCode(result.Process.ExitCode));
				break;
		}
		return result;
	}

}
