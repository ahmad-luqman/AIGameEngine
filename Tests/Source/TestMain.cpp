#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Basalt/Core/Log.h>
#include <Basalt/Core/Platform.h>

#include <filesystem>

int main(int argc, char** argv)
{
	Basalt::Log::Init();
	// Keep test output readable: engine logs still reach LogHistory, but only warnings hit the console.
	Basalt::Log::SetConsoleLevel(Basalt::LogLevel::Warn);
	// Lua Storage writes save data under the user data directory; tests must never touch the real one.
	const std::filesystem::path userData = std::filesystem::temp_directory_path() / "BasaltTests" / "UserData";
	if (!Basalt::Platform::SetEnvVarIfUnset(Basalt::Platform::UserDataDirectoryVariable, userData.string()))
	{
		BS_CORE_CRITICAL("Tests: cannot set {}", Basalt::Platform::UserDataDirectoryVariable);
		return 1;
	}

	doctest::Context context;
	context.applyCommandLine(argc, argv);
	const int result = context.run();

	Basalt::Log::Shutdown();
	return result;
}
