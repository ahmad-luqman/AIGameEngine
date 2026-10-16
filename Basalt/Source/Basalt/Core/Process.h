#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace Basalt {

	// What RunProcess runs and how.
	struct ProcessSpec
	{
		// The program file. Never searched on PATH; a relative path is made absolute against the caller's working
		// directory (not WorkingDirectory) on every platform.
		std::filesystem::path Executable;
		std::vector<std::string> Arguments;
		// Empty: the caller's working directory.
		std::filesystem::path WorkingDirectory;
		// Variables set (or replaced) in the child's copy of the caller's environment. Text is UTF-8.
		std::vector<std::pair<std::string, std::string>> Environment;
		// The child is killed when it runs longer.
		std::chrono::milliseconds Timeout{ 60000 };
		// Combined stdout/stderr beyond this keeps only its last bytes (the end of a log explains a failure).
		size_t MaxOutputBytes = size_t{ 64 } * 1024;
	};

	enum class ProcessOutcome : uint8_t
	{
		// The child exited by itself; ExitCode is its status.
		Exited,
		// The child was killed by a signal (POSIX; ExitCode is 128 + the signal, as a shell reports it) or ended
		// with an unhandled exception (Windows; ExitCode is the exception code). Error names it.
		Crashed,
		// The child never started (missing or non-executable file, bad working directory, no resources), or its
		// status could not be read. Error says which; ExitCode is meaningless.
		Failed,
		// The child was killed after Timeout; ExitCode is meaningless.
		TimedOut
	};

	// How a RunProcess child ended, with its output.
	struct ProcessResult
	{
		ProcessOutcome Outcome = ProcessOutcome::Failed;
		int ExitCode = -1;
		// Combined stdout and stderr (truncated to MaxOutputBytes, keeping the end).
		std::string Output;
		// Why the child crashed, failed or timed out; empty after a normal exit.
		std::string Error;

		bool Succeeded() const { return Outcome == ProcessOutcome::Exited && ExitCode == 0; }
	};

	// Runs a program to completion with a timeout, capturing its output, and blocks the calling thread until it
	// ends. Used by tools that check a build (the export smoke test). The child inherits the environment (plus
	// spec.Environment) but no other open files: its stdin is the null device and stdout/stderr go to a
	// temporary file rather than a pipe, so a chatty child can never block on a full pipe while the caller
	// waits for it to exit. Never throws.
	ProcessResult RunProcess(const ProcessSpec& spec);

}
