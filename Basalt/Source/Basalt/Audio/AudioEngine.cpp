#include "Basalt/Audio/AudioEngine.h"

#include "Basalt/Core/Log.h"

#include <miniaudio.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace Basalt {

	namespace {

		std::mutex s_Mutex;
		std::unique_ptr<ma_engine> s_Engine;
		// Only the device-less engine has one of its own; the device engine uses miniaudio's internal manager.
		std::unique_ptr<ma_resource_manager> s_ResourceManager;
		bool s_HasDevice = false;

		// Mixes the given duration when there is no device, appending the frames to output when it is not null.
		// False when a device is present (it pulls the mix itself) or the engine is not initialized.
		bool MixDeviceless(float seconds, std::vector<float>* output)
		{
			ma_engine* engine = nullptr;
			ma_resource_manager* resourceManager = nullptr;
			{
				std::scoped_lock lock(s_Mutex);
				if (s_HasDevice || !s_Engine)
					return false;
				engine = s_Engine.get();
				resourceManager = s_ResourceManager.get();
			}
			if (seconds <= 0.0f)
				return true;

			const ma_uint32 channels = ma_engine_get_channels(engine);
			ma_uint64 remaining = static_cast<ma_uint64>(seconds * static_cast<float>(ma_engine_get_sample_rate(engine)));
			std::vector<float> scratch(static_cast<size_t>(channels) * 1024);
			while (remaining > 0)
			{
				// Streamed clips refill their pages through resource-manager jobs. Running them before every
				// chunk keeps the pages ahead of the read, so a stream plays out identically on every run
				// instead of reading silence when the mix outpaces a background loader.
				while (ma_resource_manager_process_next_job(resourceManager) == MA_SUCCESS)
				{
				}
				const ma_uint64 frames = std::min<ma_uint64>(remaining, 1024);
				ma_engine_read_pcm_frames(engine, scratch.data(), frames, nullptr);
				if (output)
					output->insert(output->end(), scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(frames * channels));
				remaining -= frames;
			}
			return true;
		}

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

		// Without a device the mix runs on the caller's thread (AdvanceDeviceless), so the resource manager does
		// too: no job thread, its jobs run in MixDeviceless. Same decoded format as miniaudio's internal manager.
		constexpr ma_uint32 SampleRate = 48000;
		auto resourceManager = std::make_unique<ma_resource_manager>();
		ma_resource_manager_config resourceConfig = ma_resource_manager_config_init();
		resourceConfig.decodedFormat = ma_format_f32;
		resourceConfig.decodedChannels = 0;
		resourceConfig.decodedSampleRate = SampleRate;
		resourceConfig.jobThreadCount = 0;
		resourceConfig.flags |= MA_RESOURCE_MANAGER_FLAG_NO_THREADING;
		if (ma_resource_manager_init(&resourceConfig, resourceManager.get()) != MA_SUCCESS)
		{
			BS_CORE_ERROR("AudioEngine: failed to initialize the audio resource manager");
			return false;
		}

		ma_engine_config config = ma_engine_config_init();
		config.noDevice = MA_TRUE;
		config.channels = 2;
		config.sampleRate = SampleRate;
		config.pResourceManager = resourceManager.get();
		if (ma_engine_init(&config, engine.get()) != MA_SUCCESS)
		{
			BS_CORE_ERROR("AudioEngine: failed to initialize the audio engine");
			ma_resource_manager_uninit(resourceManager.get());
			return false;
		}
		s_Engine = std::move(engine);
		s_ResourceManager = std::move(resourceManager);
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
		// After the engine: its sounds hold the manager's data until they are uninitialized.
		if (s_ResourceManager)
		{
			ma_resource_manager_uninit(s_ResourceManager.get());
			s_ResourceManager.reset();
		}
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
		MixDeviceless(seconds, nullptr);
	}

	bool AudioEngine::ReadDeviceless(float seconds, std::vector<float>& output)
	{
		return MixDeviceless(seconds, &output);
	}

}
