#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Basalt/Core/Log.h>

int main(int argc, char** argv)
{
	Basalt::Log::Init();
	// Keep test output readable: engine logs still reach LogHistory, but only warnings hit the console.
	Basalt::Log::SetConsoleLevel(Basalt::LogLevel::Warn);

	doctest::Context context;
	context.applyCommandLine(argc, argv);
	const int result = context.run();

	Basalt::Log::Shutdown();
	return result;
}
