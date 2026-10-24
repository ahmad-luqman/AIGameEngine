#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Basalt/Core/Base.h>
#include <Basalt/Core/CrashReporter.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Core/Platform.h>

#include "TestUtils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
	#include <crtdbg.h>
	#include <windows.h>
#else
	#include <fcntl.h>
#endif

// A pure virtual call for crash-report purecall: the base constructor calls Pure through a function the compiler cannot
// see into. Outside the anonymous namespace on purpose: GCC devirtualizes calls on a class hierarchy only one file can
// see (a pure virtual call being undefined, to the only override), and then nothing crashes.
namespace BasaltCrashTest {

	struct PureBase;
	void CallPure(PureBase& object);

	struct PureBase
	{
		PureBase()
		{
			void (*volatile call)(PureBase&) = &CallPure;
			call(*this);
		}
		virtual ~PureBase() = default;
		PureBase(const PureBase&) = delete;
		PureBase& operator=(const PureBase&) = delete;
		virtual void Pure() = 0;
	};

	struct PureDerived : PureBase
	{
		void Pure() override {}
	};

	void CallPure(PureBase& object)
	{
		object.Pure();
	}

}

namespace {

	int Number(const char* text)
	{
		return static_cast<int>(std::strtol(text, nullptr, 10));
	}

	// Recursion the compiler cannot remove or prove infinite (limit comes from a parse), deep enough to overflow any stack:
	// the volatile read sequenced after the call keeps it from being a tail call that a loop could replace.
	int Recurse(int depth, int limit)
	{
		volatile char frame[4096];
		frame[0] = static_cast<char>(depth);
		if (depth >= limit)
			return frame[0];
		const int result = Recurse(depth + 1, limit);
		return result + frame[0];
	}

	// A real crash of the requested kind (crash-report).
	void CrashNow(std::string_view kind)
	{
		if (kind == "segv")
		{
			// volatile: the store happens; the address comes from a parse so the compiler cannot see it is null.
			auto* volatile target = reinterpret_cast<volatile int*>(static_cast<uintptr_t>(std::strtoull("0", nullptr, 10)));
			*target = 1;
		}
		else if (kind == "abort")
			std::abort();
		else if (kind == "terminate")
			// Escapes main: std::terminate with the exception current.
			throw std::runtime_error("crash-test exception");
		else if (kind == "throw-int")
			// An uncaught exception that is not a std::exception.
			throw Number("42"); // NOLINT(bugprone-std-exception-baseclass)
		else if (kind == "purecall")
		{
			const BasaltCrashTest::PureDerived derived;
		}
#if defined(_WIN32)
		else if (kind == "invalid-parameter")
		{
			// The C runtime validates its arguments and calls the invalid parameter handler. A debug C runtime first reports
			// the failed check, in a dialog unless told to write to stderr.
			_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
			_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
			const char* volatile format = nullptr;
			std::printf(format);
		}
#endif
		else if (kind == "trap")
			BS_DEBUGBREAK();
		else if (kind == "stackoverflow")
			std::printf("%d\n", Recurse(0, Number("1000000000")));
		else if (kind == "thread-segv")
		{
			// A worker thread's crash: reported once, by that thread.
			std::thread worker([] { CrashNow("segv"); });
			worker.join();
		}
		else if (kind == "two-thread-segv")
		{
			// Two workers crashing at once: one reports, the other waits rather than ending the process mid-report.
			std::atomic<bool> start = false;
			std::thread first([&] {
				while (!start)
					std::this_thread::yield();
				CrashNow("segv");
			});
			std::thread second([&] {
				while (!start)
					std::this_thread::yield();
				CrashNow("segv");
			});
			start = true;
			first.join();
			second.join();
		}
		std::fprintf(stderr, "crash kind '%.*s' is unknown or did not crash\n", static_cast<int>(kind.size()), kind.data());
		std::exit(2);
	}

	// `BasaltTests --basalt-child <actions...>` acts as a child process for the RunProcess tests, so they need no
	// shell or other platform tool. With BASALT_TEST_CHILD_ACTIONS set (space-separated actions), it does the same
	// whatever its arguments, standing in for an exported game (smoke tests). Actions run in order:
	// print <text> (stdout), error <text> (stderr), env <name>, cwd, sleep <ms>, spam <bytes>, utf8 <count>
	// (prints "é" count times), invalid (prints bytes that are not UTF-8), fd <descriptor> (POSIX: whether it is
	// open), crash (a real access violation on Windows, SIGABRT elsewhere), trap (BS_DEBUGBREAK), args (prints
	// every remaining argument on its own line), exit <code>, crash-report <kind> (installs the crash reporter as
	// "CrashChild", then calls Install again from two threads at once, logs "crash-test sentinel 42", then crashes: kind is
	// segv, abort, terminate, throw-int, purecall, invalid-parameter (Windows), trap, stackoverflow, thread-segv or
	// two-thread-segv; exits 77 when crash reports are unavailable, as under AddressSanitizer; Tests/Automation/CrashReport.py
	// checks the report).
	int RunChild(const std::vector<std::string>& actions)
	{
		for (size_t i = 0; i < actions.size(); i++)
		{
			const std::string_view action = actions[i];
			const char* value = i + 1 < actions.size() ? actions[i + 1].c_str() : "";
			if (action == "print")
			{
				std::printf("%s\n", value);
				i++;
			}
			else if (action == "error")
			{
				std::fprintf(stderr, "%s\n", value);
				i++;
			}
			else if (action == "env")
			{
				const auto variable = Basalt::Platform::GetEnvVar(value);
				std::printf("%s=%s\n", value, variable ? variable->c_str() : "<unset>");
				i++;
			}
			else if (action == "cwd")
				std::printf("%s\n", std::filesystem::current_path().string().c_str());
			else if (action == "sleep")
			{
				std::fflush(stdout);
				std::this_thread::sleep_for(std::chrono::milliseconds(Number(value)));
				i++;
			}
			else if (action == "flood")
			{
				// Writes N bytes in chunks, as a runaway log would, rather than holding them in memory.
				const std::string chunk(size_t{ 64 } * 1024, 'y');
				const auto total = static_cast<unsigned long long>(std::strtod(value, nullptr));
				for (unsigned long long written = 0; written < total; written += chunk.size())
					std::fwrite(chunk.data(), 1, chunk.size(), stdout);
				i++;
			}
			else if (action == "spam")
			{
				std::fputs(std::string(static_cast<size_t>(Number(value)), 'x').c_str(), stdout);
				i++;
			}
			else if (action == "utf8")
			{
				for (int count = Number(value); count > 0; count--)
					std::fputs("\xC3\xA9", stdout);
				i++;
			}
			else if (action == "invalid")
				std::fputs("[\xFF\xFE]", stdout);
			else if (action == "fd")
			{
#if defined(_WIN32)
				std::printf("fd n/a\n");
#else
				std::printf("fd %s\n", fcntl(Number(value), F_GETFD) >= 0 ? "open" : "closed");
#endif
				i++;
			}
			else if (action == "args")
			{
				while (++i < actions.size())
					std::printf("[%s]\n", actions[i].c_str());
			}
			else if (action == "crash")
			{
				std::fflush(stdout);
#if defined(_WIN32)
				RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
#endif
				std::abort();
			}
			else if (action == "trap")
			{
				std::fflush(stdout);
				BS_DEBUGBREAK();
			}
			else if (action == "crash-report")
			{
				Basalt::Log::Init();
				// This thread installs (it gets the alternate signal stack the stack overflow needs); later calls, two of them at
				// once, return the first call's result and change nothing.
				if (!Basalt::CrashReporter::Install({ "CrashChild" }))
				{
					std::printf("crash reports unavailable\n");
					return 77;
				}
				bool again = false;
				std::thread other([&again] { again = Basalt::CrashReporter::Install({ "Other" }); });
				const bool repeated = Basalt::CrashReporter::Install({ "Other" });
				other.join();
				if (!again || !repeated || Basalt::CrashReporter::GetReportDirectory().parent_path().filename() != "CrashChild")
				{
					std::fprintf(stderr, "a repeated Install disagreed with the first\n");
					return 2;
				}
				Basalt::CrashReporter::SetContext("Project: Crash Test 1.2.3");
				BS_CORE_ERROR("crash-test sentinel {}", 42);
				std::fflush(stdout);
				CrashNow(value);
			}
			else if (action == "exit")
			{
				std::fflush(stdout);
				return Number(value);
			}
		}
		return 0;
	}

}

int main(int argc, char** argv)
{
	if (const auto actions = Basalt::Platform::GetEnvVar("BASALT_TEST_CHILD_ACTIONS"))
	{
		std::vector<std::string> list;
		std::istringstream stream(*actions);
		for (std::string action; stream >> action;)
			list.push_back(action);
		return RunChild(list);
	}
	if (argc > 1 && std::string_view(argv[1]) == "--basalt-child")
		return RunChild(std::vector<std::string>(argv + 2, argv + argc));
	Basalt::Log::Init();
	// Keep test output readable: engine logs still reach LogHistory, but only warnings hit the console.
	Basalt::Log::SetConsoleLevel(Basalt::LogLevel::Warn);
	// Lua Storage writes save data under the user data directory; tests must never touch the real one.
	// Inside this run's scratch directory, so test processes running at the same time never share it.
	const std::filesystem::path userData = BasaltTest::GetRunDirectory() / "UserData";
	if (!Basalt::Platform::SetEnvVar(Basalt::Platform::UserDataDirectoryVariable, userData.string()))
	{
		BS_CORE_CRITICAL("Tests: cannot set {}", Basalt::Platform::UserDataDirectoryVariable);
		return 1;
	}

	doctest::Context context;
	context.applyCommandLine(argc, argv);
	const int result = context.run();
	std::error_code error;
	std::filesystem::remove_all(BasaltTest::GetRunDirectory(), error);

	Basalt::Log::Shutdown();
	return result;
}
