#pragma once

// Shared setup for the libFuzzer targets (Tests/Fuzz). Each target is its own executable: libFuzzer
// provides main(), so the doctest runner is not linked.

#include <Basalt/Audio/AudioEngine.h>
#include <Basalt/Core/Log.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace BasaltFuzz {

	// Engine state every target needs, initialized once per process. Console logging is silenced: invalid
	// input is reported through logs, and printing millions of messages would dominate the run time.
	inline void Init()
	{
		static const bool initialized = [] {
			Basalt::Log::Init();
			Basalt::Log::SetConsoleLevel(Basalt::LogLevel::Critical);
			Basalt::AudioEngine::Init(true);
			return true;
		}();
		(void)initialized;
	}

	inline std::string_view AsText(const uint8_t* data, size_t size)
	{
		return { reinterpret_cast<const char*>(data), size };
	}

}
