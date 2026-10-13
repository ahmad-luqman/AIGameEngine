#pragma once

#include <glm/glm.hpp>

#include <filesystem>
#include <vector>

struct ma_engine;

namespace Basalt {

	// Process-wide miniaudio engine. Falls back to a silent device-less engine when no audio device is
	// available (headless runs, CI), so audio code paths behave the same everywhere.
	// The device-less engine's resource manager has no job thread and no locking of its own: create and destroy
	// sounds and call AdvanceDeviceless/ReadDeviceless from one thread (the main thread; the editor's automation
	// server already hands its work to it). The mutex only makes the engine pointer and flags safe to read:
	// Shutdown must not run while another thread still uses the engine. With a device, miniaudio's own job
	// thread loads stream pages in real time instead.
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
		// Without an output device nothing pulls audio; this mixes (and discards) the given duration so
		// playback state advances exactly as it would with a device, and runs the resource manager's pending jobs
		// (stream page refills) as it goes. No-op when a device is present.
		static void AdvanceDeviceless(float seconds);
		// AdvanceDeviceless that also appends the mixed frames to output (interleaved, ma_engine_get_channels
		// channels at ma_engine_get_sample_rate), so tests can measure what reached the speakers. False (output
		// unchanged) when a device is present, the engine is not initialized, seconds is not finite or exceeds an
		// hour, or the mix fails; true with nothing appended for seconds <= 0.
		static bool ReadDeviceless(float seconds, std::vector<float>& output);
	};

}
