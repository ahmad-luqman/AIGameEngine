#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Basalt/Core/Log.h>
#include <Basalt/Core/Platform.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

int main(int argc, char** argv)
{
	Basalt::Log::Init();
	// Keep test output readable: engine logs still reach LogHistory, but only warnings hit the console.
	Basalt::Log::SetConsoleLevel(Basalt::LogLevel::Warn);
	// Lua Storage writes save data under the user data directory; tests must never touch the real one.
	// Unique per run, so test processes running at the same time (e.g. Debug and ASan builds) never share it.
	const std::filesystem::path userData = std::filesystem::temp_directory_path() / "BasaltTests" / ("UserData-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	if (!Basalt::Platform::SetEnvVar(Basalt::Platform::UserDataDirectoryVariable, userData.string()))
	{
		BS_CORE_CRITICAL("Tests: cannot set {}", Basalt::Platform::UserDataDirectoryVariable);
		return 1;
	}

	doctest::Context context;
	context.applyCommandLine(argc, argv);
	const int result = context.run();
	std::error_code error;
	std::filesystem::remove_all(userData, error);

	Basalt::Log::Shutdown();
	return result;
}
