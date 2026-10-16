#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace Basalt {

	struct ProcessSpec
	{
		std::filesystem::path Executable;
		std::vector<std::string> Arguments;
		// Empty: the caller's working directory.
		std::filesystem::path WorkingDirectory;
		// Variables set (or replaced) in the child's copy of the caller's environment.
		std::vector<std::pair<std::string, std::string>> Environment;
		// The child is killed when it runs longer.
		std::chrono::milliseconds Timeout{ 60000 };
		// Combined stdout/stderr beyond this keeps only its last bytes (the end of a log explains a failure).
		size_t MaxOutputBytes = size_t{ 64 } * 1024;
	};

	enum class ProcessOutcome : uint8_t
	{
		// The child ran to completion; ExitCode is its status.
		Exited,
		// The child never started (missing or non-executable file, bad working directory, no resources), or its
		// status could not be read. Error says which.
		Failed,
		// The child was killed after Timeout; ExitCode is meaningless.
		TimedOut
	};

	struct ProcessResult
	{
		ProcessOutcome Outcome = ProcessOutcome::Failed;
		int ExitCode = -1;
		// Combined stdout and stderr (truncated to MaxOutputBytes, keeping the end).
		std::string Output;
		// Why the child failed, timed out or was killed by a signal; empty after a normal exit.
		std::string Error;

		bool Succeeded() const { return Outcome == ProcessOutcome::Exited && ExitCode == 0; }
	};

	// Runs a program to completion with a timeout, capturing its output. Used by tools that check a build
	// (the export smoke test). Blocks the calling thread. The child inherits the environment (plus
	// spec.Environment). Output goes
	// through a temporary file rather than a pipe, so a chatty child can never block on a full pipe while
	// the caller waits for it to exit.
	ProcessResult RunProcess(const ProcessSpec& spec);

}
