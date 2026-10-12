#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Basalt/Core/Log.h>
#include <Basalt/Core/Platform.h>

#include "TestUtils.h"

#include <filesystem>
#include <string>
#include <system_error>

int main(int argc, char** argv)
{
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
