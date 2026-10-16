#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Basalt/Core/Log.h>
#include <Basalt/Core/Platform.h>

#include "TestUtils.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

	// `BasaltTests --basalt-child <actions...>` acts as a child process for the RunProcess tests, so they need no
	// shell or other platform tool. Actions run in order: print <text> (stdout), error <text> (stderr), cwd, sleep
	// <ms>, spam <bytes>, args (prints every remaining argument on its own line), exit <code>.
	int RunChild(int argc, char** argv)
	{
		for (int i = 2; i < argc; i++)
		{
			const std::string_view action = argv[i];
			const char* value = i + 1 < argc ? argv[i + 1] : "";
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
			else if (action == "cwd")
				std::printf("%s\n", std::filesystem::current_path().string().c_str());
			else if (action == "sleep")
			{
				std::fflush(stdout);
				std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(value)));
				i++;
			}
			else if (action == "spam")
			{
				std::fputs(std::string(static_cast<size_t>(std::atoi(value)), 'x').c_str(), stdout);
				i++;
			}
			else if (action == "args")
			{
				while (++i < argc)
					std::printf("[%s]\n", argv[i]);
			}
			else if (action == "exit")
			{
				std::fflush(stdout);
				return std::atoi(value);
			}
		}
		return 0;
	}

}

int main(int argc, char** argv)
{
	if (argc > 1 && std::string_view(argv[1]) == "--basalt-child")
		return RunChild(argc, argv);
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
