#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Basalt/Core/Base.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Core/Platform.h>

#include "TestUtils.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
	#include <windows.h>
#else
	#include <fcntl.h>
#endif

namespace {

	int Number(const char* text)
	{
		return static_cast<int>(std::strtol(text, nullptr, 10));
	}

	// `BasaltTests --basalt-child <actions...>` acts as a child process for the RunProcess tests, so they need no
	// shell or other platform tool. With BASALT_TEST_CHILD_ACTIONS set (space-separated actions), it does the same
	// whatever its arguments, standing in for an exported game (smoke tests). Actions run in order:
	// print <text> (stdout), error <text> (stderr), env <name>, cwd, sleep <ms>, spam <bytes>, utf8 <count>
	// (prints "é" count times), invalid (prints bytes that are not UTF-8), fd <descriptor> (POSIX: whether it is
	// open), crash (a real access violation on Windows, SIGABRT elsewhere), trap (BS_DEBUGBREAK), args (prints
	// every remaining argument on its own line), exit <code>.
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
