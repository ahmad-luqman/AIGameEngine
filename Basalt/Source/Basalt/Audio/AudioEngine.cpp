#include "Basalt/Audio/AudioEngine.h"

#include "Basalt/Core/Log.h"

#include <miniaudio.h>

#include <algorithm>
#include <memory>
#include <vector>
#include <mutex>

namespace Basalt {

	namespace {

		std::mutex s_Mutex;
		std::unique_ptr<ma_engine> s_Engine;
		bool s_HasDevice = false;

	}

	bool AudioEngine::Init(bool headless)
	{
		std::scoped_lock lock(s_Mutex);
		if (s_Engine)
			return true;

		auto engine = std::make_unique<ma_engine>();
		if (!headless)
		{
			ma_engine_config config = ma_engine_config_init();
			if (ma_engine_init(&config, engine.get()) == MA_SUCCESS)
			{
				s_Engine = std::move(engine);
				s_HasDevice = true;
				BS_CORE_INFO("AudioEngine: output device initialized");
				return true;
			}
			BS_CORE_WARN("AudioEngine: no audio output device available; audio will be silent");
		}

		ma_engine_config config = ma_engine_config_init();
		config.noDevice = MA_TRUE;
		config.channels = 2;
		config.sampleRate = 48000;
		if (ma_engine_init(&config, engine.get()) != MA_SUCCESS)
		{
			BS_CORE_ERROR("AudioEngine: failed to initialize the audio engine");
			return false;
		}
		s_Engine = std::move(engine);
		s_HasDevice = false;
		return true;
	}

	void AudioEngine::Shutdown()
	{
		std::scoped_lock lock(s_Mutex);
		if (!s_Engine)
			return;
		ma_engine_uninit(s_Engine.get());
		s_Engine.reset();
		s_HasDevice = false;
	}

	bool AudioEngine::IsInitialized()
	{
		std::scoped_lock lock(s_Mutex);
		return s_Engine != nullptr;
	}

	bool AudioEngine::HasDevice()
	{
		std::scoped_lock lock(s_Mutex);
		return s_HasDevice;
	}

	ma_engine* AudioEngine::GetEngine()
	{
		std::scoped_lock lock(s_Mutex);
		return s_Engine.get();
	}

	void AudioEngine::SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up)
	{
		ma_engine* engine = GetEngine();
		if (!engine)
			return;
		ma_engine_listener_set_position(engine, 0, position.x, position.y, position.z);
		ma_engine_listener_set_direction(engine, 0, forward.x, forward.y, forward.z);
		ma_engine_listener_set_world_up(engine, 0, up.x, up.y, up.z);
	}

	void AudioEngine::AdvanceDeviceless(float seconds)
	{
		if (HasDevice() || seconds <= 0.0f)
			return;
		ma_engine* engine = GetEngine();
		if (!engine)
			return;

		const ma_uint32 channels = ma_engine_get_channels(engine);
		ma_uint64 remaining = static_cast<ma_uint64>(seconds * static_cast<float>(ma_engine_get_sample_rate(engine)));
		std::vector<float> scratch(1024 * channels);
		while (remaining > 0)
		{
			const ma_uint64 frames = std::min<ma_uint64>(remaining, 1024);
			ma_engine_read_pcm_frames(engine, scratch.data(), frames, nullptr);
			remaining -= frames;
		}
	}

	void AudioEngine::SetMasterVolume(float volume)
	{
		if (ma_engine* engine = GetEngine())
			ma_engine_set_volume(engine, volume);
	}

}
