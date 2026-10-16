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
		// The output goes to a temporary file while the child runs; a child that writes more than this is stopped
		// (OutputLimit), so a runaway log cannot fill the disk before the timeout.
		uint64_t MaxOutputFileBytes = uint64_t{ 256 } * 1024 * 1024;
	};

	enum class ProcessOutcome : uint8_t
	{
		// The child exited by itself; ExitCode is its status.
		Exited,
		// The child was killed by a signal (POSIX; ExitCode is 128 + the signal, as a shell reports it) or ended with
		// an exception code (Windows: an NTSTATUS error or breakpoint code, which is what an unhandled exception or
		// __debugbreak leaves; ExitCode is that code as a negative int). Error names it. A Windows program exiting
		// with such a value itself looks the same; ordinary negative exit codes (-1) do not.
		Crashed,
		// The child never started (missing or non-executable file, bad working directory, no resources), or its
		// status could not be read. Error says which; ExitCode is meaningless.
		Failed,
		// The child was killed after Timeout; ExitCode is meaningless.
		TimedOut,
		// The child was killed for writing more than MaxOutputFileBytes; ExitCode is meaningless.
		OutputLimit
	};

	// How a RunProcess child ended, with its output.
	struct ProcessResult
	{
		ProcessOutcome Outcome = ProcessOutcome::Failed;
		int ExitCode = -1;
		// Combined stdout and stderr (truncated to MaxOutputBytes, keeping the end), as valid UTF-8: a character cut
		// at the start is dropped and invalid bytes become U+FFFD, so it can go into JSON.
		std::string Output;
		// Why the child crashed, failed or timed out; empty after a normal exit.
		std::string Error;

		bool Succeeded() const { return Outcome == ProcessOutcome::Exited && ExitCode == 0; }
	};

	// Runs a program to completion with a timeout, capturing its output, and blocks the calling thread until it
	// ends. Used by tools that check a build (the export smoke test). The child inherits the environment (plus
	// spec.Environment) but no other open files or handles (POSIX closes descriptors up to 65536): its stdin is
	// the null device and stdout/stderr go to a temporary file rather than a pipe, so a chatty child can never
	// block on a full pipe while the caller waits for it to exit (the file is bounded by MaxOutputFileBytes).
	// Never throws.
	ProcessResult RunProcess(const ProcessSpec& spec);

}
