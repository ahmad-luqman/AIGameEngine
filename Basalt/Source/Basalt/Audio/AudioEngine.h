#pragma once

#include <glm/glm.hpp>

#include <filesystem>

struct ma_engine;

namespace Basalt {

	// Process-wide miniaudio engine. Falls back to a silent device-less engine when no audio device is
	// available (headless runs, CI), so audio code paths behave the same everywhere.
	class AudioEngine
	{
	public:
		// headless: never open an output device.
		static bool Init(bool headless);
		static void Shutdown();
		static bool IsInitialized();
		static bool HasDevice();

		// Null when the engine is not initialized (audio is then silently skipped). The Application owns the
		// engine's lifetime; standalone users (tests, tools) call Init/Shutdown themselves.
		static ma_engine* GetEngine();

		static void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);
		static void SetMasterVolume(float volume);
		// Without an output device nothing pulls audio; this mixes (and discards) the given duration so
		// playback state advances exactly as it would with a device. No-op when a device is present.
		static void AdvanceDeviceless(float seconds);
	};

}
