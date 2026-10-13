#include "Basalt/Audio/AudioEngine.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"

#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Basalt {

	namespace {

		// The device-less engine mixes at this rate; tests read its output at it (see ReadDeviceless).
		constexpr ma_uint32 DevicelessSampleRate = 48000;
		// Frames mixed per step without a device. Stream page refills run between steps.
		constexpr ma_uint64 MixChunkFrames = 1024;
		// Bounds the job loop of one step: miniaudio re-posts a job that ran out of order and reports success, so
		// a job whose predecessor was lost could otherwise spin forever. Far above the jobs one step creates.
		constexpr int MaxJobsPerChunk = 256;

		std::mutex s_Mutex;
		std::unique_ptr<ma_engine> s_Engine;
		// Only the device-less engine has one of its own; the device engine uses miniaudio's internal manager.
		std::unique_ptr<ma_resource_manager> s_ResourceManager;
		// miniaudio's diagnostics (decoder and device errors), forwarded to the engine log. Outlives the engine
		// and the resource manager, which both write to it.
		std::unique_ptr<ma_log> s_Log;
		bool s_HasDevice = false;

		void UninitLog(ma_log* log)
		{
			if (log)
				ma_log_uninit(log);
		}

		void ForwardMiniaudioLog(void* /*userData*/, ma_uint32 level, const char* message)
		{
			// Warnings, not errors: miniaudio reports probing failures (a missing output device, an unsupported
			// backend) that the engine recovers from. Callers log a real failure themselves, with context.
			// May run on the audio thread; the engine log is thread-safe.
			if (level > MA_LOG_LEVEL_WARNING)
				return;
			std::string text = message ? message : "";
			while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
				text.pop_back();
			BS_CORE_WARN("miniaudio: {}", text);
		}

		// Runs the resource manager's pending jobs. This manager has no job thread, so nothing else runs them: a
		// stream posts a job to refill each page it finishes and would fall silent after its first two pages.
		void RunResourceJobs(ma_resource_manager* resourceManager)
		{
			for (int i = 0; i < MaxJobsPerChunk; i++)
			{
				const ma_result result = ma_resource_manager_process_next_job(resourceManager);
				if (result == MA_NO_DATA_AVAILABLE || result == MA_CANCELLED)
					return;
				// A failed job (e.g. a page of a stream whose file broke) affects only its sound; keep going.
				if (result != MA_SUCCESS)
					BS_CORE_ERROR("AudioEngine: a streaming job failed: {}", ma_result_description(result));
			}
		}

		// Mixes the given duration when there is no device, appending the frames to output when it is not null.
		// False when a device is present (it pulls the mix itself), the engine is not initialized, seconds is
		// not finite, or the mix fails.
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
			BS_CORE_ASSERT(resourceManager, "the device-less engine always has its own resource manager");
			if (!std::isfinite(seconds))
			{
				BS_CORE_ERROR("AudioEngine: cannot mix a duration of {} seconds", seconds);
				return false;
			}
			if (seconds <= 0.0f)
				return true;

			const ma_uint32 channels = ma_engine_get_channels(engine);
			ma_uint64 remaining = static_cast<ma_uint64>(seconds * static_cast<float>(ma_engine_get_sample_rate(engine)));
			std::vector<float> scratch(static_cast<size_t>(channels) * MixChunkFrames);
			while (remaining > 0)
			{
				// Before every step, so the stream pages stay ahead of the read: a stream plays exactly the
				// samples of the decoded clip.
				RunResourceJobs(resourceManager);
				const ma_uint64 frames = std::min(remaining, MixChunkFrames);
				const ma_result result = ma_engine_read_pcm_frames(engine, scratch.data(), frames, nullptr);
				// MA_AT_END only means nothing is playing (the frames are silence).
				if (result != MA_SUCCESS && result != MA_AT_END)
				{
					BS_CORE_ERROR("AudioEngine: mixing without a device failed: {}", ma_result_description(result));
					return false;
				}
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

		auto log = std::make_unique<ma_log>();
		if (ma_log_init(nullptr, log.get()) == MA_SUCCESS)
			ma_log_register_callback(log.get(), ma_log_callback_init(&ForwardMiniaudioLog, nullptr));
		else
			log.reset();

		auto engine = std::make_unique<ma_engine>();
		if (!headless)
		{
			ma_engine_config config = ma_engine_config_init();
			config.pLog = log.get();
			if (ma_engine_init(&config, engine.get()) == MA_SUCCESS)
			{
				s_Log = std::move(log);
				s_Engine = std::move(engine);
				s_HasDevice = true;
				BS_CORE_INFO("AudioEngine: output device initialized");
				return true;
			}
			BS_CORE_WARN("AudioEngine: no audio output device available; audio will be silent");
		}

		// Without a device the mix runs on the caller's thread, so the resource manager does too: no job thread.
		// Loads and frees then run inside ma_sound_init/uninit, and stream page refills in MixDeviceless. Same
		// decoded format as miniaudio's internal manager.
		auto resourceManager = std::make_unique<ma_resource_manager>();
		ma_resource_manager_config resourceConfig = ma_resource_manager_config_init();
		resourceConfig.pLog = log.get();
		resourceConfig.decodedFormat = ma_format_f32;
		resourceConfig.decodedChannels = 0;
		resourceConfig.decodedSampleRate = DevicelessSampleRate;
		resourceConfig.jobThreadCount = 0;
		resourceConfig.flags |= MA_RESOURCE_MANAGER_FLAG_NO_THREADING;
		if (const ma_result result = ma_resource_manager_init(&resourceConfig, resourceManager.get()); result != MA_SUCCESS)
		{
			BS_CORE_ERROR("AudioEngine: failed to initialize the audio resource manager: {}", ma_result_description(result));
			UninitLog(log.get());
			return false;
		}

		ma_engine_config config = ma_engine_config_init();
		config.pLog = log.get();
		config.noDevice = MA_TRUE;
		config.channels = 2;
		config.sampleRate = DevicelessSampleRate;
		config.pResourceManager = resourceManager.get();
		if (const ma_result result = ma_engine_init(&config, engine.get()); result != MA_SUCCESS)
		{
			BS_CORE_ERROR("AudioEngine: failed to initialize the audio engine: {}", ma_result_description(result));
			ma_resource_manager_uninit(resourceManager.get());
			UninitLog(log.get());
			return false;
		}
		s_Log = std::move(log);
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
		// After the engine, which was built on it: miniaudio requires a caller-supplied resource manager to
		// outlive its engine. Every sound is already uninitialized (each holds a data source from it).
		if (s_ResourceManager)
		{
			ma_resource_manager_uninit(s_ResourceManager.get());
			s_ResourceManager.reset();
		}
		UninitLog(s_Log.get());
		s_Log.reset();
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
