#include "Basalt/Audio/AudioEngine.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"

#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string_view>
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

		// Longer requests are refused: they would only come from a broken caller, and the frame count must fit.
		constexpr float MaxMixSeconds = 3600.0f;

		// Uninitializes and frees a log.
		struct LogDeleter
		{
			void operator()(ma_log* log) const
			{
				ma_log_uninit(log);
				delete log;
			}
		};
		using LogHandle = std::unique_ptr<ma_log, LogDeleter>;

		std::mutex s_Mutex;
		// miniaudio's diagnostics (decoder and device messages), forwarded to the engine log. Declared first so
		// it outlives the engine and the resource manager, which both write to it.
		LogHandle s_Log;
		std::unique_ptr<ma_engine> s_Engine;
		// Only the device-less engine has one of its own; the device engine uses miniaudio's internal manager.
		std::unique_ptr<ma_resource_manager> s_ResourceManager;
		bool s_HasDevice = false;
		// Report a persistent problem once until it clears, instead of every mix step.
		std::atomic<bool> s_JobBoundReported = false;
		std::atomic<bool> s_MixFailureReported = false;

		void ForwardMiniaudioLog(void* /*userData*/, ma_uint32 level, const char* message)
		{
			// Forwarded as warnings, miniaudio's errors included: it reports probing failures (a missing output
			// device, an unsupported backend) that the engine recovers from, and the failures that matter (a clip
			// that does not load) are logged as errors by the engine itself, with context. May run on the audio
			// thread; the engine log is thread-safe.
			if (level > MA_LOG_LEVEL_WARNING)
				return;
			std::string_view text = message ? message : "";
			text = text.substr(0, text.find_last_not_of("\r\n") + 1);
			BS_CORE_WARN("miniaudio: {}", text);
		}

		// Null (logged) when miniaudio cannot create a log; its diagnostics are then dropped.
		LogHandle CreateForwardingLog()
		{
			auto log = std::make_unique<ma_log>();
			if (const ma_result result = ma_log_init(nullptr, log.get()); result != MA_SUCCESS)
			{
				BS_CORE_WARN("AudioEngine: cannot create miniaudio's log ({}); its messages are dropped", ma_result_description(result));
				return nullptr;
			}
			LogHandle handle(log.release());
			if (const ma_result result = ma_log_register_callback(handle.get(), ma_log_callback_init(&ForwardMiniaudioLog, nullptr)); result != MA_SUCCESS)
				BS_CORE_WARN("AudioEngine: cannot forward miniaudio's log ({}); its messages are dropped", ma_result_description(result));
			return handle;
		}

		// Runs the resource manager's pending jobs. This manager has no job thread, so nothing else runs them: a
		// stream posts a job to refill each page it finishes and would fall silent after its first two pages.
		void RunResourceJobs(ma_resource_manager* resourceManager)
		{
			for (int i = 0; i < MaxJobsPerChunk; i++)
			{
				const ma_result result = ma_resource_manager_process_next_job(resourceManager);
				if (result == MA_NO_DATA_AVAILABLE || result == MA_CANCELLED)
				{
					s_JobBoundReported = false;
					return;
				}
				// Only a stream already in an error state fails a job, and that affects only its sound: keep going.
				// (miniaudio treats a decode error inside a page as the end of the clip, so a file that is corrupt
				// part-way just ends early; its load errors come through the forwarded log.)
				if (result != MA_SUCCESS)
					BS_CORE_ERROR("AudioEngine: a resource-manager job failed: {}", ma_result_description(result));
			}
			if (!s_JobBoundReported.exchange(true))
				BS_CORE_ERROR("AudioEngine: more than {} resource-manager jobs in one mix step; a stream's job is stuck re-posting itself", MaxJobsPerChunk);
		}

		// Mixes the given duration when there is no device, appending the frames to output when it is not null.
		// False (output unchanged) when a device is present (it pulls the mix itself), the engine is not
		// initialized, seconds is not a finite number up to MaxMixSeconds, or the mix fails.
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
			if (!std::isfinite(seconds) || seconds > MaxMixSeconds)
			{
				BS_CORE_ERROR("AudioEngine: cannot mix a duration of {} seconds (at most {})", seconds, MaxMixSeconds);
				return false;
			}
			if (seconds <= 0.0f)
				return true;

			const size_t outputSize = output ? output->size() : 0;
			const ma_uint32 channels = ma_engine_get_channels(engine);
			ma_uint64 remaining = static_cast<ma_uint64>(seconds * static_cast<float>(ma_engine_get_sample_rate(engine)));
			std::vector<float> scratch(static_cast<size_t>(channels) * MixChunkFrames);
			while (remaining > 0)
			{
				// Before every step, so the stream pages stay ahead of the read: a stream plays exactly the
				// samples of the decoded clip.
				RunResourceJobs(resourceManager);
				const ma_uint64 frames = std::min(remaining, MixChunkFrames);
				// A short read is padded with silence, which is what a device would play: the frames all count.
				const ma_result result = ma_engine_read_pcm_frames(engine, scratch.data(), frames, nullptr);
				// Not a failure: miniaudio's graph passes MA_AT_END up from an input it read last, and the frames are
				// still a complete mix.
				if (result != MA_SUCCESS && result != MA_AT_END)
				{
					if (!s_MixFailureReported.exchange(true))
						BS_CORE_ERROR("AudioEngine: mixing without a device failed: {}", ma_result_description(result));
					if (output)
						output->resize(outputSize);
					return false;
				}
				if (output)
					output->insert(output->end(), scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(frames * channels));
				remaining -= frames;
			}
			s_MixFailureReported = false;
			return true;
		}

	}

	bool AudioEngine::Init(bool headless)
	{
		std::scoped_lock lock(s_Mutex);
		if (s_Engine)
			return true;

		LogHandle log = CreateForwardingLog();
		auto engine = std::make_unique<ma_engine>();
		if (!headless)
		{
			ma_engine_config config = ma_engine_config_init();
			config.pLog = log.get();
			const ma_result result = ma_engine_init(&config, engine.get());
			if (result == MA_SUCCESS)
			{
				s_Log = std::move(log);
				s_Engine = std::move(engine);
				s_HasDevice = true;
				BS_CORE_INFO("AudioEngine: output device initialized");
				return true;
			}
			BS_CORE_WARN("AudioEngine: no audio output device available ({}); audio will be silent", ma_result_description(result));
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
		// After the engine, which was built on it (an engine never uninitializes a manager it was given). Every
		// AudioSystem must already be destroyed: each sound holds a data source from this manager.
		if (s_ResourceManager)
		{
			ma_resource_manager_uninit(s_ResourceManager.get());
			s_ResourceManager.reset();
		}
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
