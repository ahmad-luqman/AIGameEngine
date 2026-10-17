#include <doctest/doctest.h>

#include <Basalt/Audio/AudioEngine.h>
#include <Basalt/Audio/AudioRules.h>
#include <Basalt/Audio/AudioSystem.h>
#include <Basalt/Audio/LowPassNode.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Physics/PhysicsWorld.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

#include "TestUtils.h"

#include <miniaudio.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <span>
#include <vector>

using namespace Basalt;

// Loudness checks compare RMS levels of the device-less mix. Exact zeros rely on miniaudio applying volume changes
// at once (it has no volume smoothing unless asked for one).

namespace {

	// 16-bit mono PCM WAV containing a sine tone.
	std::string MakeWav(float seconds, uint32_t sampleRate = 22050, double frequency = 440.0)
	{
		const uint32_t samples = static_cast<uint32_t>(seconds * static_cast<float>(sampleRate));
		std::vector<int16_t> pcm(samples);
		for (uint32_t i = 0; i < samples; i++)
			pcm[i] = static_cast<int16_t>(8000.0 * std::sin(2.0 * 3.14159265 * frequency * i / sampleRate));

		auto put32 = [](std::string& out, uint32_t value) { out.append(reinterpret_cast<const char*>(&value), 4); };
		auto put16 = [](std::string& out, uint16_t value) { out.append(reinterpret_cast<const char*>(&value), 2); };
		const uint32_t dataBytes = samples * 2;
		std::string wav = "RIFF";
		put32(wav, 36 + dataBytes);
		wav += "WAVEfmt ";
		put32(wav, 16);
		put16(wav, 1);
		put16(wav, 1);
		put32(wav, sampleRate);
		put32(wav, sampleRate * 2);
		put16(wav, 2);
		put16(wav, 16);
		wav += "data";
		put32(wav, dataBytes);
		wav.append(reinterpret_cast<const char*>(pcm.data()), dataBytes);
		return wav;
	}

	// The device-less engine for one test case, shut down even when a REQUIRE ends the case early. Declare it
	// before the scene so the scene's sounds are released first.
	struct HeadlessAudio
	{
		explicit HeadlessAudio(const AudioDevicelessFormat& format = {}) { REQUIRE(AudioEngine::Init(true, format)); }
		~HeadlessAudio() { AudioEngine::Shutdown(); }

		HeadlessAudio(const HeadlessAudio&) = delete;
		HeadlessAudio& operator=(const HeadlessAudio&) = delete;
	};

	// The device-less engine's default format: 48 kHz, interleaved stereo (two samples per frame).
	constexpr size_t FramesPerSecond = 48000;
	constexpr size_t SamplesPerSecond = size_t{ 2 } * FramesPerSecond;
	constexpr double SpeedOfSound = AudioSpeedOfSound;
	// The scene's update step; 800 frames at 48 kHz.
	constexpr float StepSeconds = 1.0f / 60.0f;

	int StepCount(float seconds)
	{
		return static_cast<int>(std::lround(seconds / StepSeconds));
	}

	// Mixes the given duration and returns the interleaved output (the engine's channel count per frame).
	std::vector<float> Mix(float seconds)
	{
		std::vector<float> frames;
		REQUIRE(AudioEngine::ReadDeviceless(seconds, frames));
		return frames;
	}

	// Mixes the given duration in 60 Hz steps, as the scene's updates do.
	std::vector<float> MixInFrames(float seconds)
	{
		std::vector<float> frames;
		for (int i = 0; i < StepCount(seconds); i++)
			REQUIRE(AudioEngine::ReadDeviceless(StepSeconds, frames));
		return frames;
	}

	// RMS of every channel-th sample starting at first (first 0 = left, 1 = right), or of all samples.
	float Rms(std::span<const float> samples, size_t first = 0, size_t channels = 1)
	{
		double sum = 0.0;
		size_t count = 0;
		for (size_t i = first; i < samples.size(); i += channels)
		{
			sum += static_cast<double>(samples[i]) * samples[i];
			count++;
		}
		return count ? static_cast<float>(std::sqrt(sum / static_cast<double>(count))) : 0.0f;
	}

	// The frequency of a tone in the left channel of a stereo mix, from the time between its first and last upward
	// zero crossings (interpolated within a sample, so a fraction of a hertz is measurable in a tenth of a second).
	double Frequency(std::span<const float> mix)
	{
		constexpr auto FrameRate = static_cast<double>(FramesPerSecond);
		double first = -1.0;
		double last = -1.0;
		int crossings = 0;
		for (size_t i = 2; i < mix.size(); i += 2)
		{
			const float before = mix[i - 2];
			const float after = mix[i];
			if (before < 0.0f && after >= 0.0f)
			{
				const size_t frame = i / 2 - 1;
				const double time = (static_cast<double>(frame) + before / (before - after)) / FrameRate;
				if (crossings == 0)
					first = time;
				last = time;
				crossings++;
			}
		}
		REQUIRE(crossings > 2);
		return (crossings - 1) / (last - first);
	}

	// The tone's frequency over the next `seconds` of the mix. miniaudio applies a new pitch from its next block on,
	// so the first 20 ms are skipped.
	double MixedFrequency(float seconds)
	{
		const std::vector<float> mix = Mix(seconds);
		return Frequency(std::span(mix).subspan(SamplesPerSecond / 50));
	}

	// Whether every sample is a finite number (a NaN anywhere in the graph spreads through the mix).
	bool AllFinite(std::span<const float> samples)
	{
		return std::ranges::all_of(samples, [](float sample) { return std::isfinite(sample); });
	}

	// The number of samples in which two mixes differ by more than rounding.
	size_t CountMismatches(const std::vector<float>& a, const std::vector<float>& b)
	{
		REQUIRE(a.size() == b.size());
		size_t mismatches = 0;
		for (size_t i = 0; i < a.size(); i++)
		{
			if (std::abs(a[i] - b[i]) > 1e-6f)
				mismatches++;
		}
		return mismatches;
	}

	// A looping, non-spatial source that starts with play mode.
	AudioSourceComponent Source(const std::string& clip, AudioBus bus = AudioBus::SFX)
	{
		AudioSourceComponent source;
		source.Clip = clip;
		source.Bus = bus;
		source.Spatial = false;
		source.Loop = true;
		source.PlayOnStart = true;
		return source;
	}

	// A looping spatial source with the given Doppler factor that starts with play mode.
	AudioSourceComponent DopplerSource(const std::string& clip, float doppler = 1.0f)
	{
		AudioSourceComponent source = Source(clip);
		source.Spatial = true;
		source.Doppler = doppler;
		return source;
	}

	// A scene with an active listener at listenerPosition (the origin by default, looking down -Z) and the given
	// sources; Start() begins play.
	class AudioScene
	{
	public:
		explicit AudioScene(const glm::vec3& listenerPosition = glm::vec3(0.0f))
		{
			m_Listener = m_Scene.CreateEntity("Listener");
			m_Listener.GetComponent<TransformComponent>().Translation = listenerPosition;
			m_Listener.AddComponent<AudioListenerComponent>();
		}
		~AudioScene() { m_Scene.OnRuntimeStop(); }

		AudioScene(const AudioScene&) = delete;
		AudioScene& operator=(const AudioScene&) = delete;

		Entity Add(const std::string& name, const AudioSourceComponent& source, const glm::vec3& position = glm::vec3(0.0f))
		{
			Entity entity = m_Scene.CreateEntity(name);
			entity.GetComponent<TransformComponent>().Translation = position;
			entity.AddComponent<AudioSourceComponent>(source);
			return entity;
		}

		AudioSystem& Start()
		{
			m_Scene.OnRuntimeStart();
			REQUIRE(m_Scene.GetAudioSystem());
			return *m_Scene.GetAudioSystem();
		}

		// Advances the scene (and the mix) by whole 60 Hz frames.
		void Run(float seconds)
		{
			for (int i = 0; i < StepCount(seconds); i++)
				m_Scene.OnUpdate(StepSeconds);
		}

		Scene& GetScene() { return m_Scene; }
		Entity GetListener() const { return m_Listener; }

	private:
		Scene m_Scene;
		Entity m_Listener;
	};

	// Loudness of one source playing alone, e.g. at a distance.
	float MeasureRms(const AudioSourceComponent& source, const glm::vec3& position, const glm::vec3& listenerPosition = glm::vec3(0.0f))
	{
		AudioScene scene(listenerPosition);
		Entity speaker = scene.Add("Speaker", source, position);
		AudioSystem& audio = scene.Start();
		REQUIRE(audio.IsPlaying(speaker));
		return Rms(Mix(0.1f));
	}

	// The mix of one source played to its end (or for seconds, when it loops), in 60 Hz steps.
	std::vector<float> PlayAlone(const AudioSourceComponent& source, float seconds, bool expectStreaming)
	{
		AudioScene scene;
		Entity entity = scene.Add("Clip", source);
		AudioSystem& audio = scene.Start();
		REQUIRE(audio.IsPlaying(entity));
		CHECK(audio.IsStreaming(entity) == expectStreaming);
		return MixInFrames(seconds);
	}

}

TEST_SUITE("Audio")
{
	TEST_CASE("Audio sources play, finish and are positioned without an output device")
	{
		BasaltTest::TempProject project("Audio");
		project.WriteFile("Assets/Audio/Beep.wav", MakeWav(0.25f));
		HeadlessAudio engine;
		CHECK_FALSE(AudioEngine::HasDevice());

		Scene scene;
		Entity speaker = scene.CreateEntity("Speaker");
		auto& source = speaker.AddComponent<AudioSourceComponent>();
		source.Clip = "Assets/Audio/Beep.wav";
		source.PlayOnStart = true;
		Entity looping = scene.CreateEntity("Looping");
		auto& loopSource = looping.AddComponent<AudioSourceComponent>();
		loopSource.Clip = "Assets/Audio/Beep.wav";
		loopSource.Loop = true;
		// A clip that fails to load must be reported, not crash: miniaudio 0.11.25 read freed memory here (ASan job).
		Entity broken = scene.CreateEntity("Broken");
		broken.AddComponent<AudioSourceComponent>().Clip = "Assets/Audio/Missing.wav";
		scene.CreateEntity("Listener").AddComponent<AudioListenerComponent>();

		scene.OnRuntimeStart();
		REQUIRE(scene.GetAudioSystem());
		AudioSystem& audio = *scene.GetAudioSystem();
		CHECK(audio.IsPlaying(speaker));
		CHECK_FALSE(audio.IsPlaying(looping));
		CHECK(audio.Play(looping));
		const uint64_t since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(audio.Play(broken));
		CHECK(BasaltTest::CountLogMessages(since, "cannot load clip 'Assets/Audio/Missing.wav'") == 1);
		// miniaudio's own diagnostics reach the engine log, without their trailing newline.
		CHECK(BasaltTest::CountLogMessages(since, "miniaudio: Failed to load file") == 1);
		uint64_t next = 0;
		for (const LogMessage& message : Log::GetHistory().GetMessagesSince(since, next))
			CHECK(message.Text.back() != '\n');

		const glm::vec3 position(1.0f, 0.0f, 0.0f);
		CHECK(audio.PlayOneShot("Assets/Audio/Beep.wav", &position, 0.5f));
		CHECK(audio.GetActiveSoundCount() == 3);

		for (int i = 0; i < 30; i++)
			scene.OnUpdate(StepSeconds);
		CHECK_FALSE(audio.IsPlaying(speaker));
		CHECK(audio.IsPlaying(looping));
		// The finished one-shot was released; sources stay registered until stopped.
		CHECK(audio.GetActiveSoundCount() == 2);

		audio.Stop(looping);
		CHECK_FALSE(audio.IsPlaying(looping));
		scene.DestroyEntity(speaker);
		CHECK(audio.GetActiveSoundCount() == 0);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A scene load starts the new scene's PlayOnStart sources")
	{
		BasaltTest::TempProject project("AudioSceneLoad");
		project.WriteFile("Assets/Audio/Beep.wav", MakeWav(0.25f));
		project.WriteFile("Assets/Scenes/Next.bscene", R"({
			"Format": "BasaltScene", "Version": 1,
			"Entities": [
				{ "ID": 10, "Name": "Music", "Parent": 0, "Components": { "AudioSource": { "Clip": "Assets/Audio/Beep.wav", "PlayOnStart": true } } },
				{ "ID": 11, "Name": "Silent", "Parent": 0, "Components": { "AudioSource": { "Clip": "Assets/Audio/Beep.wav", "PlayOnStart": false } } }
			]
		})");
		HeadlessAudio engine;

		Scene scene;
		scene.OnRuntimeStart();
		std::string error;
		REQUIRE_MESSAGE(scene.RequestLoad("Assets/Scenes/Next.bscene", error), error);
		scene.OnUpdate(StepSeconds);
		REQUIRE(scene.GetAudioSystem());
		CHECK(scene.GetAudioSystem()->IsPlaying(scene.FindEntityByName("Music")));
		CHECK_FALSE(scene.GetAudioSystem()->IsPlaying(scene.FindEntityByName("Silent")));
		// Actually sounding: the load started it after the new scene's OnCreate.
		CHECK(Rms(Mix(0.05f)) > 0.01f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("AudioSource Bus and Stream fields round-trip through the component registry")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Speaker");
		std::string error;
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, "AudioSource", { { "Clip", "Music.wav" } }, error), error);
		const ComponentInfo* info = ComponentRegistry::Find("AudioSource");
		REQUIRE(info);
		nlohmann::json json = info->Serialize(entity);
		CHECK(json["Bus"] == "SFX");
		CHECK(json["Stream"] == false);
		CHECK(info->EnumOptions.at("Bus") == std::vector<std::string>{ "Master", "Music", "SFX", "UI" });

		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, "AudioSource", { { "Bus", "Music" }, { "Stream", true } }, error), error);
		const auto& source = entity.GetComponent<AudioSourceComponent>();
		CHECK(source.Bus == AudioBus::Music);
		CHECK(source.Stream);
		json = info->Serialize(entity);
		CHECK(json["Bus"] == "Music");
		CHECK(json["Stream"] == true);

		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "AudioSource", { { "Bus", "Voice" } }, error));
		CHECK(error.find("'Voice'") != std::string::npos);
		CHECK(error.find("Master, Music, SFX, UI") != std::string::npos);
		CHECK(entity.GetComponent<AudioSourceComponent>().Bus == AudioBus::Music);
	}

	TEST_CASE("AudioSource effect fields round-trip through the component registry")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Speaker");
		std::string error;
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, "AudioSource", { { "Clip", "Hum.wav" } }, error), error);
		const ComponentInfo* info = ComponentRegistry::Find("AudioSource");
		REQUIRE(info);
		nlohmann::json json = info->Serialize(entity);
		CHECK(json["Doppler"] == 0.0);
		CHECK(json["LowPass"] == 0.0);
		CHECK(json["ReverbSend"] == 0.0);

		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, "AudioSource", { { "Doppler", 1.5 }, { "LowPass", 800 }, { "ReverbSend", 0.25 } }, error), error);
		const auto& source = entity.GetComponent<AudioSourceComponent>();
		CHECK(source.Doppler == 1.5f);
		CHECK(source.LowPass == 800.0f);
		CHECK(source.ReverbSend == 0.25f);
		json = info->Serialize(entity);
		CHECK(json["Doppler"] == 1.5);
		CHECK(json["LowPass"] == 800.0);
		CHECK(json["ReverbSend"] == 0.25);
		// 0 turns the low-pass off again.
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, "AudioSource", { { "LowPass", 0 } }, error), error);
		CHECK(source.LowPass == 0.0f);
	}

	TEST_CASE("AudioSource values are validated: by the registry, and again before a C++-written source plays")
	{
		CHECK(ValidateAudioSource(AudioSourceComponent{}).empty());
		const float nan = std::numeric_limits<float>::quiet_NaN();
		struct Case
		{
			nlohmann::json Patch;
			const char* Problem;
		};
		const Case cases[] = {
			{ { { "Volume", -0.5 } }, "Volume must be a finite number >= 0 (got -0.5)" },
			{ { { "Pitch", 0 } }, "Pitch must be a finite number > 0 (got 0)" },
			{ { { "MinDistance", 0 } }, "MinDistance must be a finite number > 0 (got 0)" },
			{ { { "MinDistance", 10 }, { "MaxDistance", 5 } }, "MaxDistance must be a finite number >= MinDistance (got MaxDistance 5 and MinDistance 10)" },
			{ { { "Doppler", -0.5 } }, "Doppler must be a number from 0 to 2 (got -0.5)" },
			{ { { "Doppler", 2.5 } }, "Doppler must be a number from 0 to 2 (got 2.5)" },
			{ { { "LowPass", 5 } }, "LowPass must be 0 (off) or a cutoff from 10 to 20000 Hz (got 5)" },
			{ { { "LowPass", 25000 } }, "LowPass must be 0 (off) or a cutoff from 10 to 20000 Hz (got 25000)" },
			{ { { "LowPass", -100 } }, "LowPass must be 0 (off) or a cutoff from 10 to 20000 Hz (got -100)" },
			{ { { "ReverbSend", 1.5 } }, "ReverbSend must be a number from 0 to 1 (got 1.5)" },
			{ { { "ReverbSend", -0.1 } }, "ReverbSend must be a number from 0 to 1 (got -0.1)" },
		};
		Scene scene;
		Entity entity = scene.CreateEntity("Speaker");
		entity.AddComponent<AudioSourceComponent>().Volume = 0.25f;
		const ComponentInfo* info = ComponentRegistry::Find("AudioSource");
		REQUIRE(info);
		const nlohmann::json before = info->Serialize(entity);
		for (const Case& c : cases)
		{
			CAPTURE(c.Problem);
			std::string error;
			CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "AudioSource", c.Patch, error));
			CHECK(error.find(c.Problem) != std::string::npos);
			CHECK(info->Serialize(entity) == before);
		}
		// Equal distances are allowed (no attenuation range).
		std::string error;
		CHECK_MESSAGE(ComponentRegistry::AddOrPatch(entity, "AudioSource", { { "MinDistance", 5 }, { "MaxDistance", 5 } }, error), error);

		// Only C++ can store non-finite values (JSON and Lua numbers are checked); every float rule catches them.
		for (const float bad : { nan, std::numeric_limits<float>::infinity() })
		{
			for (float AudioSourceComponent::* field : { &AudioSourceComponent::Volume, &AudioSourceComponent::Pitch, &AudioSourceComponent::MinDistance, &AudioSourceComponent::MaxDistance })
			{
				AudioSourceComponent invalid;
				invalid.*field = bad;
				const std::string problem = ValidateAudioSource(invalid);
				CAPTURE(problem);
				CHECK_FALSE(problem.empty());
				CHECK(problem.find("must be a finite number") != std::string::npos);
			}
		}
		// The effect ranges are closed and also reject non-finite values.
		for (const float bad : { nan, std::numeric_limits<float>::infinity() })
		{
			for (float AudioSourceComponent::* field : { &AudioSourceComponent::Doppler, &AudioSourceComponent::LowPass, &AudioSourceComponent::ReverbSend })
			{
				AudioSourceComponent invalid;
				invalid.*field = bad;
				CHECK(ValidateAudioSource(invalid).find("must be") != std::string::npos);
			}
		}
		AudioSourceComponent limits;
		limits.Doppler = MaxAudioDoppler;
		limits.LowPass = MaxAudioLowPass;
		limits.ReverbSend = 1.0f;
		CHECK(ValidateAudioSource(limits).empty());
		limits.LowPass = MinAudioLowPass;
		CHECK(ValidateAudioSource(limits).empty());

		AudioSourceComponent invalid;
		invalid.Bus = static_cast<AudioBus>(9);
		CHECK(ValidateAudioSource(invalid) == "Bus must be one of its 4 values (got 9)");
		CHECK_FALSE(IsValidAudioVolume(nan));
		CHECK_FALSE(IsValidAudioVolume(std::numeric_limits<float>::infinity()));
		CHECK_FALSE(IsValidAudioVolume(-0.01f));
		CHECK(IsValidAudioVolume(0.0f));
		CHECK(IsValidAudioVolume(4.0f));

		// C++ code can bypass the registry; the system refuses to play such a source rather than mix an invalid gain.
		BasaltTest::TempProject project("AudioValidation");
		project.WriteFile("Assets/Audio/Beep.wav", MakeWav(0.25f));
		HeadlessAudio engine;
		AudioScene audioScene;
		AudioSourceComponent negative = Source("Assets/Audio/Beep.wav");
		negative.Volume = -1.0f;
		Entity bad = audioScene.Add("Bad", negative);
		AudioSystem& audio = audioScene.Start();
		CHECK_FALSE(audio.IsPlaying(bad));
		const uint64_t since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(audio.Play(bad));
		CHECK(BasaltTest::CountLogMessages(since, "cannot play the AudioSource of 'Bad': Volume must be a finite number >= 0 (got -1)") == 1);
		CHECK_FALSE(audio.PlayOneShot("Assets/Audio/Beep.wav", nullptr, nan));
		CHECK_FALSE(audio.PlayOneShot("Assets/Audio/Beep.wav", nullptr, -1.0f));
		CHECK(BasaltTest::CountLogMessages(since, "cannot play 'Assets/Audio/Beep.wav' at volume") == 2);
		CHECK(audio.GetActiveSoundCount() == 0);
		CHECK_FALSE(audio.SetBusVolume(AudioBus::Music, nan));
		CHECK_FALSE(audio.SetBusVolume(AudioBus::Music, -0.5f));
		CHECK(BasaltTest::CountLogMessages(since, "the Music bus volume must be a finite number >= 0") == 2);
		CHECK(audio.GetBusVolume(AudioBus::Music) == 1.0f);
		// A source without a clip says so.
		CHECK_FALSE(audio.Play(audioScene.Add("NoClip", Source(""))));
		CHECK(BasaltTest::CountLogMessages(since, "cannot play a sound without a clip") == 1);
	}

	TEST_CASE("Mixer buses scale the sounds routed to them, and Master scales every bus")
	{
		BasaltTest::TempProject project("AudioBuses");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(1.0f));
		HeadlessAudio engine;
		AudioScene scene;
		scene.Add("Music", Source("Assets/Audio/Tone.wav", AudioBus::Music));
		Entity effect = scene.Add("Effect", Source("Assets/Audio/Tone.wav", AudioBus::SFX));
		AudioSystem& audio = scene.Start();
		for (size_t i = 0; i < AudioBusCount; i++)
			CHECK(audio.GetBusVolume(static_cast<AudioBus>(i)) == 1.0f);

		CHECK(audio.SetBusVolume(AudioBus::SFX, 0.0f));
		const float musicOnly = Rms(Mix(0.1f));
		CHECK(musicOnly > 0.01f);
		CHECK(audio.SetBusVolume(AudioBus::Music, 0.5f));
		CHECK(audio.GetBusVolume(AudioBus::Music) == 0.5f);
		CHECK(Rms(Mix(0.1f)) == doctest::Approx(musicOnly * 0.5f).epsilon(0.05));
		// Bus and Master gains multiply; above 1 amplifies.
		CHECK(audio.SetBusVolume(AudioBus::Master, 0.5f));
		CHECK(Rms(Mix(0.1f)) == doctest::Approx(musicOnly * 0.25f).epsilon(0.05));
		CHECK(audio.SetBusVolume(AudioBus::Master, 1.0f));
		CHECK(audio.SetBusVolume(AudioBus::Music, 2.0f));
		CHECK(Rms(Mix(0.1f)) == doctest::Approx(musicOnly * 2.0f).epsilon(0.05));

		CHECK(audio.SetBusVolume(AudioBus::Music, 0.0f));
		CHECK(Rms(Mix(0.1f)) == 0.0f);
		CHECK(audio.SetBusVolume(AudioBus::SFX, 1.0f));
		CHECK(Rms(Mix(0.1f)) == doctest::Approx(musicOnly).epsilon(0.05));

		CHECK(audio.SetBusVolume(AudioBus::Master, 0.0f));
		CHECK(Rms(Mix(0.1f)) == 0.0f);
		CHECK(audio.SetBusVolume(AudioBus::Master, 0.5f));
		CHECK(Rms(Mix(0.1f)) == doctest::Approx(musicOnly * 0.5f).epsilon(0.05));

		// One-shots take a bus too (SFX by default). The looping effect stops so each one-shot is heard alone.
		audio.Stop(effect);
		CHECK(audio.SetBusVolume(AudioBus::Master, 1.0f));
		CHECK(audio.SetBusVolume(AudioBus::UI, 0.0f));
		REQUIRE(audio.PlayOneShot("Assets/Audio/Tone.wav", nullptr, 1.0f, AudioBus::UI));
		REQUIRE(audio.PlayOneShot("Assets/Audio/Tone.wav", nullptr));
		CHECK(audio.SetBusVolume(AudioBus::SFX, 0.0f));
		CHECK(Rms(Mix(0.1f)) == 0.0f);
		CHECK(audio.SetBusVolume(AudioBus::UI, 1.0f));
		CHECK(Rms(Mix(0.1f)) == doctest::Approx(musicOnly).epsilon(0.05));
		CHECK(audio.SetBusVolume(AudioBus::UI, 0.0f));
		CHECK(audio.SetBusVolume(AudioBus::SFX, 1.0f));
		CHECK(Rms(Mix(0.1f)) == doctest::Approx(musicOnly).epsilon(0.05));
	}

	TEST_CASE("A source routed straight to Master ignores the category buses")
	{
		BasaltTest::TempProject project("AudioMasterBus");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(1.0f));
		HeadlessAudio engine;
		AudioScene scene;
		scene.Add("Announcer", Source("Assets/Audio/Tone.wav", AudioBus::Master));
		AudioSystem& audio = scene.Start();
		const float full = Rms(Mix(0.1f));
		CHECK(full > 0.01f);
		CHECK(audio.SetBusVolume(AudioBus::Music, 0.0f));
		CHECK(audio.SetBusVolume(AudioBus::SFX, 0.0f));
		CHECK(audio.SetBusVolume(AudioBus::UI, 0.0f));
		CHECK(Rms(Mix(0.1f)) == doctest::Approx(full).epsilon(0.05));
		CHECK(audio.SetBusVolume(AudioBus::Master, 0.0f));
		CHECK(Rms(Mix(0.1f)) == 0.0f);
	}

	TEST_CASE("A bus value outside the enum (C++ only) is refused and changes nothing")
	{
		BasaltTest::TempProject project("AudioBadBus");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(1.0f));
		HeadlessAudio engine;
		AudioScene scene;
		AudioSystem& audio = scene.Start();
		const auto badBus = static_cast<AudioBus>(7);
		const uint64_t since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(audio.SetBusVolume(badBus, 0.5f));
		CHECK(audio.GetBusVolume(badBus) == 0.0f);
		CHECK_FALSE(audio.PlayOneShot("Assets/Audio/Tone.wav", nullptr, 1.0f, badBus));
		CHECK(BasaltTest::CountLogMessages(since, "bus value 7 is out of range") == 3);
		for (size_t i = 0; i < AudioBusCount; i++)
			CHECK(audio.GetBusVolume(static_cast<AudioBus>(i)) == 1.0f);
		CHECK(audio.GetActiveSoundCount() == 0);
	}

	TEST_CASE("Bus volumes last for the play session: Scene.Load keeps them, a new session resets them")
	{
		BasaltTest::TempProject project("AudioBusSession");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(1.0f));
		project.WriteFile("Assets/Scenes/Next.bscene", R"({
			"Format": "BasaltScene", "Version": 1,
			"Entities": [
				{ "ID": 10, "Name": "Music", "Parent": 0, "Components": { "AudioSource": { "Clip": "Assets/Audio/Tone.wav", "PlayOnStart": true, "Loop": true, "Spatial": false, "Bus": "Music", "Stream": true } } }
			]
		})");
		project.WriteFile("Assets/Scenes/Quiet.bscene", R"({ "Format": "BasaltScene", "Version": 1, "Entities": [ { "ID": 20, "Name": "Nobody", "Parent": 0, "Components": {} } ] })");
		HeadlessAudio engine;
		Scene scene;
		scene.OnRuntimeStart();
		REQUIRE(scene.GetAudioSystem());
		CHECK(scene.GetAudioSystem()->SetBusVolume(AudioBus::Music, 0.0f));
		std::string error;
		REQUIRE_MESSAGE(scene.RequestLoad("Assets/Scenes/Next.bscene", error), error);
		scene.OnUpdate(StepSeconds);
		AudioSystem* audio = scene.GetAudioSystem();
		REQUIRE(audio);
		Entity music = scene.FindEntityByName("Music");
		CHECK(audio->GetBusVolume(AudioBus::Music) == 0.0f);
		CHECK(audio->IsPlaying(music));
		CHECK(Rms(Mix(0.1f)) == 0.0f);
		CHECK(audio->SetBusVolume(AudioBus::Music, 1.0f));
		CHECK(Rms(Mix(0.1f)) > 0.01f);
		CHECK(audio->SetBusVolume(AudioBus::Music, 0.25f));

		// Loading a scene without audio releases the playing stream.
		REQUIRE_MESSAGE(scene.RequestLoad("Assets/Scenes/Quiet.bscene", error), error);
		scene.OnUpdate(StepSeconds);
		CHECK_FALSE(scene.FindEntityByName("Music"));
		CHECK(audio->GetActiveSoundCount() == 0);
		CHECK(Rms(Mix(0.1f)) == 0.0f);
		CHECK(audio->GetBusVolume(AudioBus::Music) == 0.25f);

		scene.OnRuntimeStop();
		scene.OnRuntimeStart();
		REQUIRE(scene.GetAudioSystem());
		CHECK(scene.GetAudioSystem()->GetBusVolume(AudioBus::Music) == 1.0f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("PlayOnStart sources only sound once the scripts' OnCreate could set bus volumes")
	{
		BasaltTest::TempProject project("AudioDeferredStart");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(1.0f));
		HeadlessAudio engine;
		// The system on its own, outside a running scene, to look between Start and BeginPlayback.
		Scene scene;
		scene.CreateEntity("Listener").AddComponent<AudioListenerComponent>();
		Entity music = scene.CreateEntity("Music");
		music.AddComponent<AudioSourceComponent>(Source("Assets/Audio/Tone.wav", AudioBus::Music));
		Entity cancelled = scene.CreateEntity("Cancelled");
		cancelled.AddComponent<AudioSourceComponent>(Source("Assets/Audio/Tone.wav"));
		{
			AudioSystem audio(&scene);
			audio.Start();
			CHECK(audio.IsPlaying(music));
			CHECK(audio.IsPlaying(cancelled));
			CHECK(Rms(Mix(0.1f)) == 0.0f);
			// What an OnCreate does: stop one source, apply the player's volume.
			audio.Stop(cancelled);
			CHECK_FALSE(audio.IsPlaying(cancelled));
			CHECK(audio.SetBusVolume(AudioBus::Music, 0.5f));
			audio.BeginPlayback();
			const float level = Rms(Mix(0.1f));
			CHECK(level > 0.01f);
			CHECK(audio.GetActiveSoundCount() == 1);
			CHECK(audio.SetBusVolume(AudioBus::Music, 1.0f));
			CHECK(Rms(Mix(0.1f)) == doctest::Approx(level * 2.0f).epsilon(0.05));
		}
		{
			// Play during OnCreate starts at once, and BeginPlayback leaves it be (no second sound).
			AudioSystem audio(&scene);
			audio.Start();
			REQUIRE(audio.Play(music));
			audio.Stop(cancelled);
			const float level = Rms(Mix(0.1f));
			CHECK(level > 0.01f);
			audio.BeginPlayback();
			CHECK(audio.GetActiveSoundCount() == 1);
			CHECK(Rms(Mix(0.1f)) == doctest::Approx(level).epsilon(0.05));
		}
		{
			// Sources a scene load adds wait for BeginPlayback too.
			AudioSystem audio(&scene);
			audio.Start();
			audio.BeginPlayback();
			audio.Stop(music);
			audio.Stop(cancelled);
			audio.PlayOnStart({ music, cancelled });
			CHECK(audio.IsPlaying(music));
			CHECK(Rms(Mix(0.1f)) == 0.0f);
			audio.BeginPlayback();
			CHECK(Rms(Mix(0.1f)) > 0.01f);
		}
	}

	TEST_CASE("A streamed clip plays exactly the samples of the decoded clip, across its pages")
	{
		BasaltTest::TempProject project("AudioStream");
		// 2.5 s at the engine's rate: three one-second pages, so the two-page stream buffer must refill a page
		// mid-clip; no resampling.
		project.WriteFile("Assets/Audio/Song.wav", MakeWav(2.5f, 48000));
		HeadlessAudio engine;

		AudioSourceComponent source = Source("Assets/Audio/Song.wav", AudioBus::Music);
		source.Loop = false;
		const std::vector<float> decoded = PlayAlone(source, 3.0f, false);
		source.Stream = true;
		const std::vector<float> streamed = PlayAlone(source, 3.0f, true);
		CHECK(CountMismatches(decoded, streamed) == 0);
		// The last page is heard, and the clip ends on time.
		CHECK(Rms(std::span(streamed).subspan(2 * SamplesPerSecond, SamplesPerSecond / 10)) > 0.01f);
		CHECK(Rms(std::span(streamed).last(SamplesPerSecond / 4)) == 0.0f);
	}

	TEST_CASE("Looping and resampled streams match their decoded clips")
	{
		BasaltTest::TempProject project("AudioStreamLoop");
		project.WriteFile("Assets/Audio/Ambience.wav", MakeWav(1.2f, 48000));
		project.WriteFile("Assets/Audio/Ambience22k.wav", MakeWav(1.2f, 22050));
		HeadlessAudio engine;
		for (const char* clip : { "Assets/Audio/Ambience.wav", "Assets/Audio/Ambience22k.wav" })
		{
			CAPTURE(clip);
			// More than two loops: the loop seam lands mid-page.
			AudioSourceComponent source = Source(clip, AudioBus::Music);
			const std::vector<float> decoded = PlayAlone(source, 3.0f, false);
			source.Stream = true;
			const std::vector<float> streamed = PlayAlone(source, 3.0f, true);
			CHECK(CountMismatches(decoded, streamed) == 0);
			CHECK(Rms(std::span(streamed).last(SamplesPerSecond / 5)) > 0.01f);
		}
	}

	TEST_CASE("Streams can be pitched, spatial, stopped mid-play, replayed and destroyed")
	{
		BasaltTest::TempProject project("AudioStreamLifecycle");
		project.WriteFile("Assets/Audio/Song.wav", MakeWav(2.5f, 48000));
		HeadlessAudio engine;
		AudioScene scene;
		AudioSourceComponent song = Source("Assets/Audio/Song.wav", AudioBus::Music);
		song.Loop = false;
		song.Stream = true;
		song.PlayOnStart = false;
		AudioSourceComponent fast = song;
		fast.Pitch = 2.0f;
		AudioSourceComponent spatial = song;
		spatial.Spatial = true;
		Entity songEntity = scene.Add("Song", song);
		Entity fastEntity = scene.Add("Fast", fast);
		Entity spatialEntity = scene.Add("Spatial", spatial, glm::vec3(0.0f, 0.0f, -4.0f));
		AudioSystem& audio = scene.Start();

		// Pitch 2 reads pages twice as fast; the clip ends in half its length.
		REQUIRE(audio.Play(fastEntity));
		scene.Run(1.2f);
		CHECK(audio.IsPlaying(fastEntity));
		scene.Run(0.15f);
		CHECK_FALSE(audio.IsPlaying(fastEntity));

		REQUIRE(audio.Play(spatialEntity));
		CHECK(audio.IsStreaming(spatialEntity));
		const float spatialLevel = Rms(Mix(0.1f));
		audio.Stop(spatialEntity);
		REQUIRE(audio.Play(songEntity));
		const float flatLevel = Rms(Mix(0.1f));
		CHECK(spatialLevel == doctest::Approx(flatLevel / 4.0f).epsilon(0.1));

		// Stopped right as the first page ran out (0.1 s + 54 updates of 800 frames = 48000 frames), with its
		// refill job still queued, then replayed from the start.
		scene.Run(0.9f);
		audio.Stop(songEntity);
		CHECK(Rms(Mix(0.1f)) == 0.0f);
		REQUIRE(audio.Play(songEntity));
		CHECK(audio.GetActiveSoundCount() == 2);
		CHECK(audio.IsStreaming(songEntity));
		CHECK(Rms(Mix(0.1f)) > 0.01f);
		// Destroyed with a refill queued the same way.
		scene.Run(0.9f);
		scene.GetScene().DestroyEntity(songEntity);
		CHECK(audio.GetActiveSoundCount() == 1);
		CHECK(Rms(Mix(0.1f)) == 0.0f);
	}

	TEST_CASE("A streamed clip that cannot be read is reported, not played")
	{
		BasaltTest::TempProject project("AudioStreamBroken");
		project.WriteFile("Assets/Audio/Garbage.wav", "not a wav file at all");
		// The header promises 2.5 s; the data holds 0.5 s.
		std::string truncated = MakeWav(2.5f, 48000);
		truncated.resize(44 + 48000);
		project.WriteFile("Assets/Audio/Truncated.wav", truncated);
		HeadlessAudio engine;
		AudioScene scene;
		AudioSourceComponent missing = Source("Assets/Audio/Missing.wav", AudioBus::Music);
		missing.Stream = true;
		AudioSourceComponent garbage = Source("Assets/Audio/Garbage.wav", AudioBus::Music);
		garbage.Stream = true;
		Entity missingEntity = scene.Add("Missing", missing);
		Entity garbageEntity = scene.Add("Garbage", garbage);
		AudioSystem& audio = scene.Start();
		CHECK_FALSE(audio.IsPlaying(missingEntity));
		CHECK_FALSE(audio.IsPlaying(garbageEntity));
		const uint64_t since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(audio.Play(missingEntity));
		CHECK_FALSE(audio.Play(garbageEntity));
		CHECK(BasaltTest::CountLogMessages(since, "cannot load clip 'Assets/Audio/Missing.wav'") == 1);
		CHECK(BasaltTest::CountLogMessages(since, "cannot load clip 'Assets/Audio/Garbage.wav'") == 1);
		CHECK(BasaltTest::CountLogMessages(since, "streamed") == 2);
		CHECK(audio.GetActiveSoundCount() == 0);
		CHECK(Rms(Mix(0.1f)) == 0.0f);

		// A file shorter than its header says streams what it has, then ends: no hang, no endless silence.
		AudioSourceComponent shortSource = Source("Assets/Audio/Truncated.wav", AudioBus::Music);
		shortSource.Stream = true;
		shortSource.Loop = false;
		shortSource.PlayOnStart = false;
		Entity shortEntity = scene.Add("Truncated", shortSource);
		REQUIRE(audio.Play(shortEntity));
		CHECK(Rms(Mix(0.1f)) > 0.01f);
		scene.Run(3.0f);
		CHECK_FALSE(audio.IsPlaying(shortEntity));
	}

	TEST_CASE("Spatial sources fade with distance from the listener; non-spatial sources do not")
	{
		BasaltTest::TempProject project("AudioSpatial");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(1.0f));
		HeadlessAudio engine;
		AudioSourceComponent spatial = Source("Assets/Audio/Tone.wav");
		spatial.Spatial = true;
		spatial.MinDistance = 1.0f;
		spatial.MaxDistance = 50.0f;
		const float nearRms = MeasureRms(spatial, glm::vec3(0.0f, 0.0f, -2.0f));
		const float farRms = MeasureRms(spatial, glm::vec3(0.0f, 0.0f, -20.0f));
		CHECK(nearRms > 0.01f);
		// Inverse distance attenuation: gain = MinDistance / distance.
		CHECK(nearRms / farRms == doctest::Approx(10.0f).epsilon(0.1));
		// Within MinDistance the gain stays 1; beyond MaxDistance it stops falling.
		CHECK(MeasureRms(spatial, glm::vec3(0.0f, 0.0f, -0.5f)) == doctest::Approx(nearRms * 2.0f).epsilon(0.05));
		CHECK(MeasureRms(spatial, glm::vec3(0.0f, 0.0f, -100.0f)) == doctest::Approx(MeasureRms(spatial, glm::vec3(0.0f, 0.0f, -50.0f))).epsilon(0.02));

		const AudioSourceComponent flat = Source("Assets/Audio/Tone.wav");
		CHECK(MeasureRms(flat, glm::vec3(0.0f, 0.0f, -20.0f)) == doctest::Approx(MeasureRms(flat, glm::vec3(0.0f, 0.0f, -2.0f))).epsilon(0.02));
		CHECK(MeasureRms(flat, glm::vec3(0.0f, 0.0f, -20.0f)) > nearRms);
		// Nor are the buses they play through: a bus is not a point in the world.
		const glm::vec3 farListener(0.0f, 0.0f, 100.0f);
		CHECK(MeasureRms(flat, glm::vec3(0.0f), farListener) == doctest::Approx(MeasureRms(flat, glm::vec3(0.0f))).epsilon(0.02));
		// A spatial source next to a listener far from the origin is as loud as next to one at the origin.
		CHECK(MeasureRms(spatial, farListener - glm::vec3(0.0f, 0.0f, 2.0f), farListener) == doctest::Approx(nearRms).epsilon(0.02));
	}

	TEST_CASE("Spatial sources follow their entity and pan by direction")
	{
		BasaltTest::TempProject project("AudioSpatialMotion");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(2.0f));
		HeadlessAudio engine;
		AudioScene scene;
		AudioSourceComponent source = Source("Assets/Audio/Tone.wav");
		source.Spatial = true;
		Entity speaker = scene.Add("Speaker", source, glm::vec3(0.0f, 0.0f, -2.0f));
		scene.Start();
		const float nearRms = Rms(Mix(0.1f));
		// The system moves the sound to the entity's position every update.
		speaker.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, -20.0f);
		scene.Run(StepSeconds);
		CHECK(nearRms / Rms(Mix(0.1f)) == doctest::Approx(10.0f).epsilon(0.1));

		// The listener looks down -Z, so +X is on its right (channel 1).
		speaker.GetComponent<TransformComponent>().Translation = glm::vec3(3.0f, 0.0f, 0.0f);
		scene.Run(StepSeconds);
		std::vector<float> mix = Mix(0.1f);
		CHECK(Rms(mix, 1, 2) > 2.0f * Rms(mix, 0, 2));
		// Turned around, the same source is on its left.
		scene.GetListener().GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
		scene.Run(StepSeconds);
		mix = Mix(0.1f);
		CHECK(Rms(mix, 0, 2) > 2.0f * Rms(mix, 1, 2));
	}

	TEST_CASE("A spatial one-shot fades with distance")
	{
		BasaltTest::TempProject project("AudioSpatialOneShot");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(1.0f));
		HeadlessAudio engine;
		auto measure = [](const glm::vec3& position) {
			AudioScene scene;
			AudioSystem& audio = scene.Start();
			REQUIRE(audio.PlayOneShot("Assets/Audio/Tone.wav", &position));
			return Rms(Mix(0.1f));
		};
		CHECK(measure(glm::vec3(0.0f, 0.0f, -2.0f)) / measure(glm::vec3(0.0f, 0.0f, -20.0f)) == doctest::Approx(10.0f).epsilon(0.1));
	}

	TEST_CASE("Pitch scales a clip's playback speed")
	{
		BasaltTest::TempProject project("AudioPitch");
		project.WriteFile("Assets/Audio/Beep.wav", MakeWav(0.5f));
		HeadlessAudio engine;
		AudioScene scene;
		auto add = [&scene](const char* name, float pitch) {
			AudioSourceComponent source = Source("Assets/Audio/Beep.wav");
			source.Loop = false;
			source.Pitch = pitch;
			return scene.Add(name, source);
		};
		Entity fast = add("Fast", 2.0f);
		Entity normal = add("Normal", 1.0f);
		Entity slow = add("Slow", 0.5f);
		AudioSystem& audio = scene.Start();

		// The fast clip lasts 0.25 s, the normal one 0.5 s, the slow one 1 s.
		scene.Run(0.2f);
		CHECK(audio.IsPlaying(fast));
		scene.Run(0.1f);
		CHECK_FALSE(audio.IsPlaying(fast));
		CHECK(audio.IsPlaying(normal));
		scene.Run(0.3f);
		CHECK_FALSE(audio.IsPlaying(normal));
		CHECK(audio.IsPlaying(slow));
		scene.Run(0.5f);
		CHECK_FALSE(audio.IsPlaying(slow));
	}

	TEST_CASE("Source lifecycle: replay restarts, stop and invalid sources are harmless, stopping play releases everything")
	{
		BasaltTest::TempProject project("AudioLifecycle");
		project.WriteFile("Assets/Audio/Beep.wav", MakeWav(0.25f));
		HeadlessAudio engine;
		AudioScene scene;
		AudioSourceComponent beepSource = Source("Assets/Audio/Beep.wav");
		beepSource.Loop = false;
		beepSource.PlayOnStart = false;
		Entity beep = scene.Add("Beep", beepSource);
		Entity silent = scene.Add("Silent", Source(""));
		Entity plain = scene.GetScene().CreateEntity("Plain");
		AudioSystem& audio = scene.Start();

		CHECK_FALSE(audio.IsPlaying(beep));
		CHECK_FALSE(audio.Play(silent));
		CHECK_FALSE(audio.Play(plain));
		audio.Stop(plain);
		audio.Stop(beep);
		CHECK(audio.GetActiveSoundCount() == 0);

		REQUIRE(audio.Play(beep));
		scene.Run(0.2f);
		CHECK(audio.IsPlaying(beep));
		// Replaying restarts from the beginning instead of adding a second sound.
		REQUIRE(audio.Play(beep));
		CHECK(audio.GetActiveSoundCount() == 1);
		scene.Run(0.15f);
		CHECK(audio.IsPlaying(beep));
		scene.Run(0.15f);
		CHECK_FALSE(audio.IsPlaying(beep));
		// A finished source stays registered until it is stopped or replayed, also through further updates.
		scene.GetScene().OnUpdate(StepSeconds);
		CHECK(audio.GetActiveSoundCount() == 1);
		audio.Stop(beep);
		CHECK(audio.GetActiveSoundCount() == 0);

		REQUIRE(audio.Play(beep));
		scene.GetScene().OnRuntimeStop();
		CHECK(scene.GetScene().GetAudioSystem() == nullptr);
	}

	TEST_CASE("Without an audio engine nothing plays, but bus volumes are still kept")
	{
		BasaltTest::TempProject project("AudioNoEngine");
		project.WriteFile("Assets/Audio/Beep.wav", MakeWav(0.25f));
		REQUIRE_FALSE(AudioEngine::IsInitialized());
		std::vector<float> frames;
		CHECK_FALSE(AudioEngine::ReadDeviceless(0.1f, frames));
		CHECK(frames.empty());

		AudioScene scene;
		Entity beep = scene.Add("Beep", Source("Assets/Audio/Beep.wav"));
		AudioSystem& audio = scene.Start();
		CHECK_FALSE(audio.IsPlaying(beep));
		CHECK_FALSE(audio.Play(beep));
		CHECK_FALSE(audio.PlayOneShot("Assets/Audio/Beep.wav", nullptr));
		CHECK(audio.GetActiveSoundCount() == 0);
		CHECK(audio.SetBusVolume(AudioBus::Music, 0.3f));
		CHECK(audio.GetBusVolume(AudioBus::Music) == doctest::Approx(0.3f));
		scene.Run(0.1f);
	}

	TEST_CASE("The device-less mix rejects durations it cannot represent")
	{
		HeadlessAudio engine;
		std::vector<float> frames;
		CHECK(AudioEngine::ReadDeviceless(0.0f, frames));
		CHECK(AudioEngine::ReadDeviceless(-1.0f, frames));
		CHECK(frames.empty());
		CHECK_FALSE(AudioEngine::ReadDeviceless(std::numeric_limits<float>::quiet_NaN(), frames));
		CHECK_FALSE(AudioEngine::ReadDeviceless(std::numeric_limits<float>::infinity(), frames));
		CHECK(frames.empty());
		CHECK_FALSE(AudioEngine::ReadDeviceless(std::numeric_limits<float>::max(), frames));
		CHECK_FALSE(AudioEngine::ReadDeviceless(3601.0f, frames));
		CHECK(frames.empty());
		// 1/64 s is exact in binary: 750 frames.
		CHECK(AudioEngine::ReadDeviceless(0.015625f, frames));
		CHECK(frames.size() == SamplesPerSecond / 64);
	}

	TEST_CASE("Doppler: approaching sources sound higher, receding ones lower, by the physical ratio")
	{
		BasaltTest::TempProject project("AudioDoppler");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(2.0f));
		HeadlessAudio engine;
		constexpr float Speed = 30.0f;
		// The pitch while the source (or, with movingListener, the listener) moves at velocity for a few frames: the
		// Doppler shift needs no actual distance covered, only the velocities the last update saw.
		auto measure = [](float doppler, const glm::vec3& velocity, bool movingListener, float* reportedPitch = nullptr) {
			AudioScene scene;
			Entity speaker = scene.Add("Speaker", DopplerSource("Assets/Audio/Tone.wav", doppler), glm::vec3(0.0f, 0.0f, -40.0f));
			AudioSystem& audio = scene.Start();
			Entity moving = movingListener ? scene.GetListener() : speaker;
			for (int i = 0; i < 6; i++)
			{
				moving.GetComponent<TransformComponent>().Translation += velocity * StepSeconds;
				scene.Run(StepSeconds);
			}
			const double frequency = MixedFrequency(0.2f);
			if (reportedPitch)
				*reportedPitch = audio.GetDopplerPitch(speaker);
			return frequency / 440.0;
		};
		const glm::vec3 towardListener(0.0f, 0.0f, Speed);
		float reported = 0.0f;
		CHECK(measure(1.0f, towardListener, false, &reported) == doctest::Approx(SpeedOfSound / (SpeedOfSound - Speed)).epsilon(0.002));
		CHECK(reported == doctest::Approx(SpeedOfSound / (SpeedOfSound - Speed)).epsilon(0.001));
		CHECK(measure(1.0f, -towardListener, false) == doctest::Approx(SpeedOfSound / (SpeedOfSound + Speed)).epsilon(0.002));
		// The listener's own movement counts too: walking toward the source raises its pitch.
		CHECK(measure(1.0f, -towardListener, true) == doctest::Approx((SpeedOfSound + Speed) / SpeedOfSound).epsilon(0.002));
		// The factor scales the effect; 0 (the default) turns it off.
		CHECK(measure(0.5f, towardListener, false) == doctest::Approx(SpeedOfSound / (SpeedOfSound - 0.5 * Speed)).epsilon(0.002));
		CHECK(measure(0.0f, towardListener, false, &reported) == doctest::Approx(1.0).epsilon(0.002));
		CHECK(reported == 1.0f);
		// Only the speed along the line to the listener counts: moving sideways, 3 m off the line 40 m away, the source
		// recedes at a fraction of its speed.
		const double receding = Speed * 3.0 / std::sqrt(3.0 * 3.0 + 40.0 * 40.0);
		CHECK(measure(1.0f, glm::vec3(Speed, 0.0f, 0.0f), false) == doctest::Approx(SpeedOfSound / (SpeedOfSound + receding)).epsilon(0.001));
	}

	TEST_CASE("Doppler: velocities come from physics bodies, a teleport is capped and one-shots never shift")
	{
		BasaltTest::TempProject project("AudioDopplerBodies");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(2.0f));
		HeadlessAudio engine;

		SUBCASE("A child of a body uses the body's velocity, steady while frames and physics steps do not line up")
		{
			AudioScene scene;
			Entity body = scene.GetScene().CreateEntity("Body");
			body.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, -60.0f);
			auto& rigidBody = body.AddComponent<RigidBodyComponent>();
			rigidBody.Type = RigidBodyType::Dynamic;
			rigidBody.GravityFactor = 0.0f;
			rigidBody.LinearDamping = 0.0f;
			body.AddComponent<BoxColliderComponent>();
			Entity speaker = scene.Add("Speaker", DopplerSource("Assets/Audio/Tone.wav"), glm::vec3(0.0f, 0.0f, -60.0f));
			REQUIRE(scene.GetScene().SetParent(speaker, body));
			AudioSystem& audio = scene.Start();
			scene.GetScene().GetPhysicsWorld()->SetLinearVelocity(body, glm::vec3(0.0f, 0.0f, 20.0f));
			// 144 Hz frames over 60 Hz physics: most frames see no step, so the speaker's world position moves in
			// jumps. Each update mixes its frame, so the pitch is measured after every one.
			const double expected = SpeedOfSound / (SpeedOfSound - 20.0);
			for (int i = 0; i < 30; i++)
			{
				scene.GetScene().OnUpdate(1.0f / 144.0f);
				if (i > 0)
				{
					CAPTURE(i);
					CHECK(audio.GetDopplerPitch(speaker) == doctest::Approx(expected).epsilon(0.001));
				}
			}
		}

		SUBCASE("A teleport is capped at MaxDopplerSpeed")
		{
			AudioScene scene;
			Entity speaker = scene.Add("Speaker", DopplerSource("Assets/Audio/Tone.wav"), glm::vec3(0.0f, 0.0f, -2000.0f));
			AudioSystem& audio = scene.Start();
			scene.Run(StepSeconds);
			speaker.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, -5.0f);
			scene.Run(StepSeconds);
			const std::vector<float> mix = Mix(0.1f);
			CHECK(audio.GetDopplerPitch(speaker) == doctest::Approx(SpeedOfSound / (SpeedOfSound - AudioSystem::MaxDopplerSpeed)).epsilon(0.001));
			CHECK(Rms(mix) > 0.01f);
			// Standing still again, the next update ends the shift.
			scene.Run(StepSeconds);
			Mix(0.05f);
			CHECK(audio.GetDopplerPitch(speaker) == 1.0f);
		}

		SUBCASE("One-shots and non-spatial sources are never shifted, however fast the listener moves")
		{
			AudioScene scene;
			AudioSourceComponent flatSource = Source("Assets/Audio/Tone.wav");
			flatSource.Doppler = 2.0f;
			Entity flat = scene.Add("Flat", flatSource);
			AudioSystem& audio = scene.Start();
			const glm::vec3 position(0.0f, 0.0f, -10.0f);
			for (int i = 0; i < 6; i++)
			{
				scene.GetListener().GetComponent<TransformComponent>().Translation.z -= 50.0f * StepSeconds;
				scene.Run(StepSeconds);
			}
			// The non-spatial source, measured in the mix rather than through GetDopplerPitch (which reports 1 for any
			// non-spatial sound).
			CHECK(MixedFrequency(0.2f) == doctest::Approx(440.0).epsilon(0.002));
			CHECK(audio.GetDopplerPitch(flat) == 1.0f);
			audio.Stop(flat);
			REQUIRE(audio.PlayOneShot("Assets/Audio/Tone.wav", &position));
			CHECK(MixedFrequency(0.2f) == doctest::Approx(440.0).epsilon(0.002));
		}
	}

	TEST_CASE("LowPass muffles a source while it plays, and turning it off restores the exact sound")
	{
		BasaltTest::TempProject project("AudioLowPass");
		project.WriteFile("Assets/Audio/High.wav", MakeWav(2.0f, 44100, 4000.0));
		project.WriteFile("Assets/Audio/Low.wav", MakeWav(2.0f, 22050, 220.0));
		HeadlessAudio engine;

		// The reference: the same clip, never filtered, mixed at the same time as the comparison below (8 updates
		// and 0.2 s of measuring mixes in).
		auto plainMix = []() {
			AudioScene scene;
			scene.Add("High", Source("Assets/Audio/High.wav"));
			scene.Start();
			scene.Run(8 * StepSeconds);
			Mix(0.2f);
			return Mix(0.1f);
		};
		const std::vector<float> plain = plainMix();
		const float plainRms = Rms(plain);
		REQUIRE(plainRms > 0.05f);

		AudioScene scene;
		Entity high = scene.Add("High", Source("Assets/Audio/High.wav"));
		AudioSourceComponent lowSource = Source("Assets/Audio/Low.wav");
		lowSource.Volume = 0.0f;
		Entity low = scene.Add("Low", lowSource);
		scene.Start();
		auto& effects = high.GetComponent<AudioSourceComponent>();
		// Set while playing: the next update applies it. A 500 Hz cutoff takes a 4 kHz tone down by ~36 dB.
		effects.LowPass = 500.0f;
		scene.Run(0.1f);
		CHECK(Rms(Mix(0.1f)) < 0.05f * plainRms);
		// A higher cutoff lets more through.
		effects.LowPass = 3000.0f;
		scene.Run(StepSeconds);
		const float brighter = Rms(Mix(0.1f));
		CHECK(brighter > 0.3f * plainRms);
		CHECK(brighter < 0.9f * plainRms);
		// Off again: the filter leaves the chain, so the output is sample for sample the unfiltered clip.
		effects.LowPass = 0.0f;
		scene.Run(StepSeconds);
		CHECK(CountMismatches(Mix(0.1f), plain) == 0);

		// Low frequencies pass a cutoff well above them.
		high.GetComponent<AudioSourceComponent>().Volume = 0.0f;
		REQUIRE(scene.GetScene().GetAudioSystem()->Play(high));
		low.GetComponent<AudioSourceComponent>().Volume = 1.0f;
		REQUIRE(scene.GetScene().GetAudioSystem()->Play(low));
		scene.Run(StepSeconds);
		const float lowRms = Rms(Mix(0.1f));
		low.GetComponent<AudioSourceComponent>().LowPass = 4000.0f;
		scene.Run(StepSeconds);
		CHECK(Rms(Mix(0.1f)) == doctest::Approx(lowRms).epsilon(0.02));
	}

	TEST_CASE("Live effect values that C++ code made invalid turn the effect off, reported once until valid again")
	{
		BasaltTest::TempProject project("AudioInvalidEffects");
		project.WriteFile("Assets/Audio/High.wav", MakeWav(2.0f, 44100, 4000.0));
		HeadlessAudio engine;
		AudioScene scene;
		Entity high = scene.Add("High", Source("Assets/Audio/High.wav"));
		AudioSystem& audio = scene.Start();
		scene.Run(StepSeconds);
		const float plainRms = Rms(Mix(0.1f));
		const uint64_t since = Log::GetHistory().GetTotalCount();
		auto& effects = high.GetComponent<AudioSourceComponent>();
		// Below the 10 Hz minimum: applied, this cutoff would silence the 4 kHz tone.
		effects.LowPass = 5.0f;
		effects.ReverbSend = 7.0f;
		effects.Doppler = -1.0f;
		scene.Run(0.1f);
		CHECK(Rms(Mix(0.1f)) == doctest::Approx(plainRms).epsilon(0.01));
		CHECK_FALSE(audio.HasReverb(AudioBus::SFX));
		// Six updates, one report (the first problem: Doppler).
		CHECK(BasaltTest::CountLogMessages(since, "an effect of the AudioSource of 'High' is off: Doppler must be a number from 0 to 2 (got -1)") == 1);
		CHECK(Log::GetHistory().GetTotalCount() == since + 1);
		// Valid again, then invalid again: reported again.
		effects.LowPass = 0.0f;
		effects.ReverbSend = 0.0f;
		effects.Doppler = 0.0f;
		scene.Run(StepSeconds);
		effects.ReverbSend = std::numeric_limits<float>::quiet_NaN();
		scene.Run(0.1f);
		CHECK(BasaltTest::CountLogMessages(since, "is off: ReverbSend must be a number from 0 to 1") == 1);
		CHECK(Log::GetHistory().GetTotalCount() == since + 2);
	}

	TEST_CASE("ReverbSend feeds the bus's reverb, whose tail outlasts the sound and follows the bus volume")
	{
		BasaltTest::TempProject project("AudioReverb");
		project.WriteFile("Assets/Audio/Beep.wav", MakeWav(0.1f));
		HeadlessAudio engine;
		// The tail's loudness from 0.15 s after a 0.1 s beep, with the send and room given.
		auto tail = [](float send, const AudioReverbSettings& room, float sfxVolume = 1.0f, AudioBus bus = AudioBus::SFX) {
			AudioScene scene;
			AudioSourceComponent source = Source("Assets/Audio/Beep.wav", bus);
			source.Loop = false;
			source.ReverbSend = send;
			Entity beep = scene.Add("Beep", source);
			AudioSystem& audio = scene.Start();
			CHECK(audio.SetReverb(room));
			CHECK(audio.HasReverb(bus) == (send > 0.0f));
			audio.SetBusVolume(AudioBus::SFX, sfxVolume);
			scene.Run(0.25f);
			CHECK_FALSE(audio.IsPlaying(beep));
			return Rms(Mix(0.2f));
		};
		const AudioReverbSettings defaults;
		CHECK(tail(0.0f, defaults) == 0.0f);
		const float wet = tail(1.0f, defaults);
		CHECK(wet > 0.002f);
		// Half the send, half the tail.
		CHECK(tail(0.5f, defaults) == doctest::Approx(wet * 0.5f).epsilon(0.01));
		// The reverb returns into its bus: silencing SFX silences the tail of an SFX source.
		CHECK(tail(1.0f, defaults, 0.0f) == 0.0f);
		// A Music source gets the Music bus's own reverb, which an SFX volume does not touch.
		CHECK(tail(1.0f, defaults, 0.0f, AudioBus::Music) > 0.002f);
		// A larger room rings longer.
		AudioReverbSettings small;
		small.RoomSize = 0.1f;
		AudioReverbSettings large;
		large.RoomSize = 0.95f;
		CHECK(tail(1.0f, large) > 2.0f * tail(1.0f, small));
	}

	TEST_CASE("Reverb settings are validated, and start over each session")
	{
		BasaltTest::TempProject project("AudioReverbSettings");
		project.WriteFile("Assets/Audio/Beep.wav", MakeWav(2.0f));
		HeadlessAudio engine;
		{
			AudioScene scene;
			AudioSourceComponent source = Source("Assets/Audio/Beep.wav");
			source.ReverbSend = 1.0f;
			Entity beep = scene.Add("Beep", source);
			AudioSystem& audio = scene.Start();
			CHECK(audio.HasReverb(AudioBus::SFX));
			CHECK_FALSE(audio.HasReverb(AudioBus::Music));

			const uint64_t since = Log::GetHistory().GetTotalCount();
			AudioReverbSettings invalid;
			invalid.Damping = 1.5f;
			CHECK_FALSE(audio.SetReverb(invalid));
			invalid = {};
			invalid.Width = std::numeric_limits<float>::quiet_NaN();
			CHECK_FALSE(audio.SetReverb(invalid));
			CHECK(BasaltTest::CountLogMessages(since, "cannot set the reverb: Damping must be a number from 0 to 1 (got 1.5)") == 1);
			CHECK(BasaltTest::CountLogMessages(since, "cannot set the reverb: Width must be a number from 0 to 1") == 1);
			CHECK(audio.GetReverb().Damping == 0.25f);

			AudioReverbSettings dry;
			dry.RoomSize = 0.0f;
			dry.Damping = 1.0f;
			REQUIRE(audio.SetReverb(dry));
			CHECK(audio.GetReverb().RoomSize == 0.0f);

			// Replaying a sending source reuses its bus's reverb.
			REQUIRE(audio.Play(beep));
			CHECK(audio.HasReverb(AudioBus::SFX));
			// Stopping the source lets the reverb's tail ring out rather than cutting it.
			REQUIRE(audio.SetReverb(AudioReverbSettings{ 0.95f, 0.0f, 1.0f }));
			scene.Run(0.5f);
			audio.Stop(beep);
			scene.Run(StepSeconds);
			CHECK(Rms(Mix(0.05f)) > 0.001f);
		}
		AudioScene next;
		AudioSystem& audio = next.Start();
		CHECK(audio.GetReverb() == AudioReverbSettings{});
		CHECK(ValidateAudioReverb(AudioReverbSettings{}).empty());
		CHECK_FALSE(audio.HasReverb(AudioBus::SFX));
	}

	TEST_CASE("Doppler: a listener on a character uses the character's velocity, and a new listener starts at rest")
	{
		BasaltTest::TempProject project("AudioDopplerListener");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(2.0f));
		HeadlessAudio engine;

		SUBCASE("A camera on a character")
		{
			AudioScene scene;
			Entity player = scene.GetScene().CreateEntity("Player");
			auto& character = player.AddComponent<CharacterControllerComponent>();
			character.GravityFactor = 0.0f;
			player.AddComponent<CapsuleColliderComponent>();
			REQUIRE(scene.GetScene().SetParent(scene.GetListener(), player));
			Entity speaker = scene.Add("Speaker", DopplerSource("Assets/Audio/Tone.wav"), glm::vec3(0.0f, 0.0f, -80.0f));
			AudioSystem& audio = scene.Start();
			PhysicsWorld* physics = scene.GetScene().GetPhysicsWorld();
			REQUIRE(physics);
			// Walking toward the source at 10 m/s, with 144 Hz frames over 60 Hz physics: the character's velocity is
			// steady where the camera's position moves in steps.
			const double expected = (SpeedOfSound + 10.0) / SpeedOfSound;
			for (int i = 0; i < 40; i++)
			{
				physics->MoveCharacter(player, glm::vec3(0.0f, 0.0f, -10.0f));
				scene.GetScene().OnUpdate(1.0f / 144.0f);
				if (i > 10)
				{
					CAPTURE(i);
					CHECK(audio.GetDopplerPitch(speaker) == doctest::Approx(expected).epsilon(0.002));
				}
			}
		}

		SUBCASE("Switching to a listener 50 m away is no movement")
		{
			AudioScene scene;
			Entity far = scene.GetScene().CreateEntity("Far");
			far.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 50.0f);
			far.AddComponent<AudioListenerComponent>().Active = false;
			Entity speaker = scene.Add("Speaker", DopplerSource("Assets/Audio/Tone.wav"), glm::vec3(0.0f, 0.0f, -10.0f));
			AudioSystem& audio = scene.Start();
			scene.Run(0.1f);
			scene.GetListener().GetComponent<AudioListenerComponent>().Active = false;
			far.GetComponent<AudioListenerComponent>().Active = true;
			scene.Run(StepSeconds);
			CHECK(audio.GetDopplerPitch(speaker) == 1.0f);
		}
	}

	TEST_CASE("Doppler: placing entities in OnCreate is no movement, and a script-moved static body is measured")
	{
		BasaltTest::TempProject project("AudioDopplerPlacement");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(2.0f));
		HeadlessAudio engine;

		SUBCASE("OnCreate")
		{
			Scene scene;
			Entity listener = scene.CreateEntity("Listener");
			listener.AddComponent<AudioListenerComponent>();
			Entity speaker = scene.CreateEntity("Speaker");
			speaker.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, -10.0f);
			speaker.AddComponent<AudioSourceComponent>(DopplerSource("Assets/Audio/Tone.wav"));
			AudioSystem audio(&scene);
			audio.Start();
			// What a spawn script does in OnCreate: move both far from where Start found them.
			speaker.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, -500.0f);
			listener.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 300.0f);
			audio.BeginPlayback();
			audio.Update(StepSeconds);
			CHECK(audio.GetDopplerPitch(speaker) == 1.0f);

			// A source that keeps moving through a scene load (PlayOnStart and BeginPlayback for the new entities)
			// keeps its shift: only the new sources and the listener start over.
			for (int i = 0; i < 3; i++)
			{
				speaker.GetComponent<TransformComponent>().Translation.z += 30.0f * StepSeconds;
				audio.Update(StepSeconds);
			}
			const float moving = audio.GetDopplerPitch(speaker);
			CHECK(moving > 1.05f);
			audio.PlayOnStart({});
			audio.BeginPlayback();
			speaker.GetComponent<TransformComponent>().Translation.z += 30.0f * StepSeconds;
			audio.Update(StepSeconds);
			CHECK(audio.GetDopplerPitch(speaker) == doctest::Approx(moving).epsilon(0.001));
		}

		SUBCASE("A static body moved by script")
		{
			AudioScene scene;
			Entity speaker = scene.Add("Speaker", DopplerSource("Assets/Audio/Tone.wav"), glm::vec3(0.0f, 0.0f, -60.0f));
			speaker.AddComponent<RigidBodyComponent>();
			speaker.AddComponent<BoxColliderComponent>();
			AudioSystem& audio = scene.Start();
			for (int i = 0; i < 6; i++)
			{
				speaker.GetComponent<TransformComponent>().Translation.z += 30.0f * StepSeconds;
				scene.Run(StepSeconds);
			}
			CHECK(audio.GetDopplerPitch(speaker) == doctest::Approx(SpeedOfSound / (SpeedOfSound - 30.0)).epsilon(0.001));
		}

		SUBCASE("A clip that played to its end while moving reports no shift")
		{
			project.WriteFile("Assets/Audio/Short.wav", MakeWav(0.1f));
			AudioScene scene;
			AudioSourceComponent source = DopplerSource("Assets/Audio/Short.wav");
			source.Loop = false;
			Entity speaker = scene.Add("Speaker", source, glm::vec3(0.0f, 0.0f, -60.0f));
			AudioSystem& audio = scene.Start();
			for (int i = 0; i < 12; i++)
			{
				speaker.GetComponent<TransformComponent>().Translation.z += 30.0f * StepSeconds;
				scene.Run(StepSeconds);
			}
			CHECK_FALSE(audio.IsPlaying(speaker));
			CHECK(audio.GetDopplerPitch(speaker) == 1.0f);
		}
	}

	TEST_CASE("Doppler: the strongest factor at the speed cap stays audible both ways")
	{
		BasaltTest::TempProject project("AudioDopplerExtremes");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(2.0f));
		HeadlessAudio engine;
		// Source and listener both jump 50 m in one frame (capped to 100 m/s each), toward or away from each other.
		auto measure = [](float direction) {
			AudioScene scene;
			Entity speaker = scene.Add("Speaker", DopplerSource("Assets/Audio/Tone.wav", MaxAudioDoppler), glm::vec3(0.0f, 0.0f, -500.0f));
			AudioSystem& audio = scene.Start();
			scene.Run(StepSeconds);
			speaker.GetComponent<TransformComponent>().Translation.z += direction * 50.0f;
			scene.GetListener().GetComponent<TransformComponent>().Translation.z -= direction * 50.0f;
			scene.Run(StepSeconds);
			const float level = Rms(Mix(0.1f));
			CHECK(level > 0.001f);
			return static_cast<double>(audio.GetDopplerPitch(speaker));
		};
		const double shift = MaxAudioDoppler * AudioSystem::MaxDopplerSpeed;
		CHECK(measure(1.0f) == doctest::Approx((SpeedOfSound + shift) / (SpeedOfSound - shift)).epsilon(0.002));
		CHECK(measure(-1.0f) == doctest::Approx((SpeedOfSound - shift) / (SpeedOfSound + shift)).epsilon(0.002));
	}

	TEST_CASE("A source whose world position is not finite keeps its last position, reported once")
	{
		BasaltTest::TempProject project("AudioNonFinite");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(2.0f));
		HeadlessAudio engine;
		AudioScene scene;
		AudioSourceComponent source = DopplerSource("Assets/Audio/Tone.wav");
		source.ReverbSend = 1.0f;
		Entity speaker = scene.Add("Speaker", source, glm::vec3(0.0f, 0.0f, -2.0f));
		scene.Start();
		scene.Run(StepSeconds);
		const uint64_t since = Log::GetHistory().GetTotalCount();
		speaker.GetComponent<TransformComponent>().Translation.x = std::numeric_limits<float>::quiet_NaN();
		scene.Run(0.1f);
		CHECK(BasaltTest::CountLogMessages(since, "'Speaker' has a world position that is not finite") == 1);
		// Nothing poisoned the mix or the reverb's feedback.
		speaker.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, -2.0f);
		scene.Run(StepSeconds);
		const std::vector<float> mix = Mix(0.2f);
		CHECK(AllFinite(mix));
		CHECK(Rms(mix) > 0.01f);
		CHECK(Log::GetHistory().GetTotalCount() == since + 1);
	}

	TEST_CASE("LowPass glides to a new cutoff, restarts cleanly after a bypass, and combines with a reverb send")
	{
		BasaltTest::TempProject project("AudioLowPassChain");
		project.WriteFile("Assets/Audio/High.wav", MakeWav(2.0f, 44100, 4000.0));
		HeadlessAudio engine;
		AudioScene scene;
		Entity high = scene.Add("High", Source("Assets/Audio/High.wav"));
		AudioSystem& audio = scene.Start();
		// A small, dead room: tails die within a fraction of a second, so each step below hears its own chain.
		REQUIRE(audio.SetReverb(AudioReverbSettings{ 0.0f, 1.0f, 1.0f }));
		scene.Run(StepSeconds);
		const float plain = Rms(Mix(0.2f));
		auto& effects = high.GetComponent<AudioSourceComponent>();
		auto settle = [&scene]() {
			scene.Run(0.5f);
			return Rms(Mix(0.1f));
		};

		// Both stages: muffled, dry and wet.
		effects.LowPass = 500.0f;
		effects.ReverbSend = 1.0f;
		CHECK(settle() < 0.1f * plain);
		// The send leaves: the filter now feeds the bus directly, still muffled.
		effects.ReverbSend = 0.0f;
		CHECK(settle() < 0.05f * plain);
		// The filter leaves, the send comes back: bright, plus a little reverb.
		effects.LowPass = 0.0f;
		effects.ReverbSend = 1.0f;
		CHECK(settle() > 0.9f * plain);
		// The filter comes back in front of the send.
		effects.LowPass = 500.0f;
		CHECK(settle() < 0.1f * plain);

		// A cutoff that moves glides there: a millisecond after a rise from 100 Hz to 15 kHz the 4 kHz tone is still
		// muffled (a jump would let it through at once), and the rise is done well within 100 ms.
		effects.ReverbSend = 0.0f;
		effects.LowPass = 100.0f;
		settle();
		effects.LowPass = 15000.0f;
		scene.GetScene().OnUpdate(0.001f);
		CHECK(Rms(Mix(0.001f)) < 0.3f * plain);
		const std::vector<float> after = Mix(0.1f);
		CHECK(Rms(std::span(after).last(SamplesPerSecond / 50)) > 0.95f * plain);
		// Back in after a bypass, the filter starts at its new cutoff rather than gliding up from the 100 Hz it had
		// (which, as above, would keep the tone muffled for the first milliseconds).
		effects.LowPass = 100.0f;
		settle();
		effects.LowPass = 0.0f;
		settle();
		effects.LowPass = 15000.0f;
		scene.GetScene().OnUpdate(0.001f);
		CHECK(Rms(Mix(0.001f)) > 0.8f * plain);

		// Swept every frame between 50 Hz and 15 kHz, the filter stays near the input's level and never produces
		// anything not finite.
		for (int i = 0; i < 60; i++)
		{
			effects.LowPass = i % 2 == 0 ? 50.0f : 15000.0f;
			scene.Run(StepSeconds);
			const std::vector<float> mix = Mix(StepSeconds);
			CHECK(AllFinite(mix));
			// The input peaks at 8000 / 32768 (0.244); a Butterworth step overshoots about 4%.
			const float peak = std::ranges::max(mix | std::views::transform([](float sample) { return std::abs(sample); }));
			CHECK(peak < 0.26f);
		}
	}

	TEST_CASE("A reverb created after SetReverb uses its settings, and the settings survive Scene.Load")
	{
		BasaltTest::TempProject project("AudioReverbLater");
		project.WriteFile("Assets/Audio/Beep.wav", MakeWav(0.1f));
		project.WriteFile("Assets/Scenes/Next.bscene", R"({ "Format": "BasaltScene", "Version": 1, "Entities": [ { "ID": 20, "Name": "Nobody", "Parent": 0, "Components": {} } ] })");
		HeadlessAudio engine;
		// The tail of a beep whose source starts sending while it already plays, into a reverb created only now, after the settings were set.
		auto tail = [](const AudioReverbSettings& room) {
			AudioScene scene;
			AudioSourceComponent source = Source("Assets/Audio/Beep.wav");
			source.Loop = false;
			Entity beep = scene.Add("Beep", source);
			AudioSystem& audio = scene.Start();
			REQUIRE(audio.SetReverb(room));
			CHECK_FALSE(audio.HasReverb(AudioBus::SFX));
			beep.GetComponent<AudioSourceComponent>().ReverbSend = 1.0f;
			scene.Run(StepSeconds);
			CHECK(audio.HasReverb(AudioBus::SFX));
			scene.Run(0.25f);
			return Rms(Mix(0.2f));
		};
		CHECK(tail(AudioReverbSettings{ 0.95f, 0.25f, 1.0f }) > 2.0f * tail(AudioReverbSettings{ 0.1f, 0.25f, 1.0f }));

		Scene scene;
		scene.OnRuntimeStart();
		REQUIRE(scene.GetAudioSystem());
		const AudioReverbSettings hall{ 0.9f, 0.1f, 0.5f };
		REQUIRE(scene.GetAudioSystem()->SetReverb(hall));
		std::string error;
		REQUIRE_MESSAGE(scene.RequestLoad("Assets/Scenes/Next.bscene", error), error);
		scene.OnUpdate(StepSeconds);
		REQUIRE(scene.GetAudioSystem());
		CHECK(scene.GetAudioSystem()->GetReverb() == hall);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A source whose AudioSource is removed while it plays keeps playing until it is stopped")
	{
		BasaltTest::TempProject project("AudioRemovedSource");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(2.0f));
		HeadlessAudio engine;
		AudioScene scene;
		AudioSourceComponent source = DopplerSource("Assets/Audio/Tone.wav");
		source.LowPass = 2000.0f;
		source.ReverbSend = 0.5f;
		Entity speaker = scene.Add("Speaker", source, glm::vec3(0.0f, 0.0f, -5.0f));
		AudioSystem& audio = scene.Start();
		scene.Run(StepSeconds);
		speaker.RemoveComponent<AudioSourceComponent>();
		for (int i = 0; i < 5; i++)
		{
			speaker.GetComponent<TransformComponent>().Translation.z += 0.2f;
			scene.Run(StepSeconds);
		}
		CHECK(audio.IsPlaying(speaker));
		CHECK(Rms(Mix(0.1f)) > 0.001f);
		audio.Stop(speaker);
		CHECK(audio.GetActiveSoundCount() == 0);
	}

	TEST_CASE("Where the reverb cannot run (surround, low rates) sends are dropped, logged once per bus, and sounds play dry")
	{
		BasaltTest::TempProject project("AudioReverbUnavailable");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(2.0f));
		auto run = [](const AudioDevicelessFormat& format, const std::string& expectedLog) {
			HeadlessAudio engine(format);
			AudioScene scene;
			AudioSourceComponent sfx = Source("Assets/Audio/Tone.wav");
			sfx.ReverbSend = 1.0f;
			// Above the 8 kHz output's Nyquist frequency (in the 8 kHz run): kept just under it, so the filter stays
			// defined.
			sfx.LowPass = 20000.0f;
			AudioSourceComponent music = Source("Assets/Audio/Tone.wav", AudioBus::Music);
			music.ReverbSend = 1.0f;
			const uint64_t since = Log::GetHistory().GetTotalCount();
			Entity sfxEntity = scene.Add("Sfx", sfx);
			scene.Add("Music", music);
			AudioSystem& audio = scene.Start();
			scene.Run(0.1f);
			REQUIRE(audio.Play(sfxEntity));
			scene.Run(0.1f);
			CHECK_FALSE(audio.HasReverb(AudioBus::SFX));
			CHECK_FALSE(audio.HasReverb(AudioBus::Music));
			CHECK(BasaltTest::CountLogMessages(since, expectedLog) == 1);
			CHECK(BasaltTest::CountLogMessages(since, "reverb for") == 2);
			const std::vector<float> mix = Mix(0.1f);
			CHECK(AllFinite(mix));
			CHECK(Rms(mix) > 0.01f);
		};
		run({ 6, 48000 }, "cannot create the SFX bus reverb for 6 channels at 48000 Hz");
		run({ 2, 8000 }, "cannot create the SFX bus reverb for 2 channels at 8000 Hz");
	}

	TEST_CASE("The device-less engine refuses formats it does not support")
	{
		const uint64_t since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(AudioEngine::Init(true, { 0, 48000 }));
		CHECK_FALSE(AudioEngine::Init(true, { 2, 4000 }));
		CHECK_FALSE(AudioEngine::IsInitialized());
		CHECK(BasaltTest::CountLogMessages(since, "unsupported device-less format") == 2);
	}

	TEST_CASE("A LowPass cutoff above the output's Nyquist frequency is kept under it")
	{
		BasaltTest::TempProject project("AudioLowPassNyquist");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(2.0f));
		// At 22.05 kHz a 15 kHz cutoff lies past Nyquist (11.025 kHz); the filter runs at 0.45 times the rate instead,
		// which a 440 Hz tone passes untouched.
		HeadlessAudio engine({ 2, 22050 });
		auto level = [](float cutoff) {
			AudioScene scene;
			AudioSourceComponent source = Source("Assets/Audio/Tone.wav");
			source.LowPass = cutoff;
			scene.Add("Tone", source);
			scene.Start();
			scene.Run(0.1f);
			const std::vector<float> mix = Mix(0.2f);
			REQUIRE(AllFinite(mix));
			return Rms(mix);
		};
		CHECK(level(15000.0f) == doctest::Approx(level(0.0f)).epsilon(0.02));
	}

	TEST_CASE("A listener whose transform is not finite keeps its last placement, reported once until it is finite")
	{
		BasaltTest::TempProject project("AudioListenerNonFinite");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(2.0f));
		HeadlessAudio engine;
		AudioScene scene;
		AudioSourceComponent source = DopplerSource("Assets/Audio/Tone.wav");
		source.ReverbSend = 1.0f;
		Entity speaker = scene.Add("Speaker", source, glm::vec3(0.0f, 0.0f, -2.0f));
		AudioSystem& audio = scene.Start();
		scene.Run(StepSeconds);
		auto& transform = scene.GetListener().GetComponent<TransformComponent>();
		const uint64_t since = Log::GetHistory().GetTotalCount();
		// A zero scale: the position is finite, its directions are not.
		transform.Scale = glm::vec3(0.0f);
		scene.Run(0.1f);
		CHECK(BasaltTest::CountLogMessages(since, "the listener 'Listener' has a transform that is not finite") == 1);
		// Back, somewhere else: no teleport reads as movement, and nothing poisoned the mix or the reverb.
		transform.Scale = glm::vec3(1.0f);
		transform.Translation = glm::vec3(0.0f, 0.0f, 30.0f);
		scene.Run(StepSeconds);
		CHECK(audio.GetDopplerPitch(speaker) == 1.0f);
		const std::vector<float> mix = Mix(0.2f);
		CHECK(AllFinite(mix));
		CHECK(Rms(mix) > 0.001f);
		// Broken again: reported again.
		transform.Translation.y = std::numeric_limits<float>::infinity();
		scene.Run(StepSeconds);
		CHECK(BasaltTest::CountLogMessages(since, "the listener 'Listener' has a transform that is not finite") == 2);
	}

	TEST_CASE("Positions that are not finite never reach a sound: Play and PlayOneShot refuse them, and a recovery is no teleport")
	{
		BasaltTest::TempProject project("AudioNonFinitePlay");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(2.0f));
		HeadlessAudio engine;
		AudioScene scene;
		AudioSourceComponent source = DopplerSource("Assets/Audio/Tone.wav");
		source.PlayOnStart = false;
		source.ReverbSend = 1.0f;
		Entity speaker = scene.Add("Speaker", source, glm::vec3(0.0f, 0.0f, -2.0f));
		AudioSystem& audio = scene.Start();
		const uint64_t since = Log::GetHistory().GetTotalCount();
		speaker.GetComponent<TransformComponent>().Translation.x = std::numeric_limits<float>::quiet_NaN();
		CHECK_FALSE(audio.Play(speaker));
		CHECK(BasaltTest::CountLogMessages(since, "cannot play the AudioSource of 'Speaker': its world position is not finite") == 1);
		const glm::vec3 nowhere(std::numeric_limits<float>::infinity(), 0.0f, 0.0f);
		CHECK_FALSE(audio.PlayOneShot("Assets/Audio/Tone.wav", &nowhere));
		CHECK(BasaltTest::CountLogMessages(since, "cannot play 'Assets/Audio/Tone.wav' at a position that is not finite") == 1);
		CHECK(audio.GetActiveSoundCount() == 0);

		// A playing source that goes non-finite and comes back 40 m away does not read as moving 40 m in a frame.
		speaker.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, -2.0f);
		REQUIRE(audio.Play(speaker));
		scene.Run(StepSeconds);
		speaker.GetComponent<TransformComponent>().Translation.x = std::numeric_limits<float>::quiet_NaN();
		scene.Run(StepSeconds);
		speaker.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, -42.0f);
		scene.Run(StepSeconds);
		CHECK(audio.GetDopplerPitch(speaker) == 1.0f);
		CHECK(AllFinite(Mix(0.2f)));
	}

	TEST_CASE("Each new invalid effect value is reported, and a NaN send never reaches the reverb")
	{
		BasaltTest::TempProject project("AudioInvalidEffectsEach");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(2.0f));
		HeadlessAudio engine;
		AudioScene scene;
		Entity tone = scene.Add("Tone", Source("Assets/Audio/Tone.wav"));
		AudioSystem& audio = scene.Start();
		auto& effects = tone.GetComponent<AudioSourceComponent>();
		const uint64_t since = Log::GetHistory().GetTotalCount();
		effects.Doppler = -1.0f;
		effects.LowPass = 5.0f;
		scene.Run(0.1f);
		CHECK(BasaltTest::CountLogMessages(since, "is off: Doppler must be") == 1);
		// Doppler fixed, LowPass still out of range: that one is reported now.
		effects.Doppler = 0.0f;
		scene.Run(0.1f);
		CHECK(BasaltTest::CountLogMessages(since, "is off: LowPass must be") == 1);
		effects.LowPass = 0.0f;
		effects.ReverbSend = std::numeric_limits<float>::quiet_NaN();
		scene.Run(0.1f);
		CHECK(BasaltTest::CountLogMessages(since, "is off: ReverbSend must be") == 1);
		CHECK_FALSE(audio.HasReverb(AudioBus::SFX));
		CHECK(AllFinite(Mix(0.2f)));
		CHECK(Log::GetHistory().GetTotalCount() == since + 3);
	}

	TEST_CASE("Doppler: a static body stops the search for a moving ancestor; no listener resets the listener's history")
	{
		BasaltTest::TempProject project("AudioDopplerAncestors");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(2.0f));
		HeadlessAudio engine;

		SUBCASE("A speaker on a script-moved static platform, riding a dynamic body that does not move")
		{
			AudioScene scene;
			Entity carrier = scene.GetScene().CreateEntity("Carrier");
			auto& carrierBody = carrier.AddComponent<RigidBodyComponent>();
			carrierBody.Type = RigidBodyType::Dynamic;
			carrierBody.GravityFactor = 0.0f;
			carrier.AddComponent<BoxColliderComponent>();
			// Below the listener's line, apart from the platform so their colliders do not overlap; the speaker ends up
			// straight ahead of the listener.
			carrier.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, -10.0f, 0.0f);
			Entity platform = scene.GetScene().CreateEntity("Platform");
			platform.AddComponent<RigidBodyComponent>();
			platform.AddComponent<BoxColliderComponent>();
			platform.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 5.0f, 0.0f);
			REQUIRE(scene.GetScene().SetParent(platform, carrier, false));
			Entity speaker = scene.Add("Speaker", DopplerSource("Assets/Audio/Tone.wav"));
			speaker.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 5.0f, -60.0f);
			REQUIRE(scene.GetScene().SetParent(speaker, platform, false));
			AudioSystem& audio = scene.Start();
			for (int i = 0; i < 6; i++)
			{
				speaker.GetComponent<TransformComponent>().Translation.z += 30.0f * StepSeconds;
				scene.Run(StepSeconds);
			}
			// The carrier's velocity (0) would give no shift; the speaker's own movement does.
			CHECK(audio.GetDopplerPitch(speaker) == doctest::Approx(SpeedOfSound / (SpeedOfSound - 30.0)).epsilon(0.001));
		}

		SUBCASE("A listener that goes away and comes back elsewhere")
		{
			AudioScene scene;
			Entity speaker = scene.Add("Speaker", DopplerSource("Assets/Audio/Tone.wav"), glm::vec3(0.0f, 0.0f, -10.0f));
			AudioSystem& audio = scene.Start();
			scene.Run(0.1f);
			auto& listener = scene.GetListener().GetComponent<AudioListenerComponent>();
			listener.Active = false;
			scene.Run(StepSeconds);
			scene.GetListener().GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 40.0f);
			listener.Active = true;
			scene.Run(StepSeconds);
			CHECK(audio.GetDopplerPitch(speaker) == 1.0f);
		}
	}

	TEST_CASE("The device-less engine takes formats within its limits, mono included, and refuses a second format")
	{
		CHECK_FALSE(AudioEngine::Init(true, { MaxDevicelessChannels + 1, 48000 }));
		CHECK_FALSE(AudioEngine::Init(true, { 2, MaxDevicelessSampleRate + 1 }));
		CHECK_FALSE(AudioEngine::IsInitialized());

		BasaltTest::TempProject project("AudioMono");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(2.0f));
		HeadlessAudio engine({ MinDevicelessChannels, MinDevicelessSampleRate });
		REQUIRE(AudioEngine::GetEngine());
		CHECK(ma_engine_get_channels(AudioEngine::GetEngine()) == 1);
		CHECK(ma_engine_get_sample_rate(AudioEngine::GetEngine()) == MinDevicelessSampleRate);
		// The same engine again is fine; another format while it runs is refused.
		CHECK(AudioEngine::Init(true, { MinDevicelessChannels, MinDevicelessSampleRate }));
		const uint64_t since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(AudioEngine::Init(true));
		CHECK(BasaltTest::CountLogMessages(since, "already running without a device at 1 channels and 8000 Hz") == 1);

		// Every effect stage runs in mono (the reverb is refused below 22.05 kHz, logged; the rest plays).
		AudioScene scene;
		AudioSourceComponent source = DopplerSource("Assets/Audio/Tone.wav");
		source.LowPass = 2000.0f;
		source.ReverbSend = 0.5f;
		scene.Add("Speaker", source, glm::vec3(0.0f, 0.0f, -2.0f));
		scene.Start();
		scene.Run(0.1f);
		const std::vector<float> mix = Mix(0.1f);
		CHECK(mix.size() == MinDevicelessSampleRate / 10);
		CHECK(AllFinite(mix));
		CHECK(Rms(mix) > 0.01f);
	}

	TEST_CASE("LowPassNode on its own: it glides in log frequency, a restart clears its memory, bad arguments are refused")
	{
		constexpr ma_uint32 Rate = 48000;
		ma_node_graph_config graphConfig = ma_node_graph_config_init(1);
		ma_node_graph graph;
		REQUIRE(ma_node_graph_init(&graphConfig, nullptr, &graph) == MA_SUCCESS);
		// 0.2 s of DC 1.0, then 0.2 s of silence.
		std::vector<float> input(Rate * 2 / 5, 0.0f);
		std::fill(input.begin(), input.begin() + Rate / 5, 1.0f);
		const ma_audio_buffer_config bufferConfig = ma_audio_buffer_config_init(ma_format_f32, 1, input.size(), input.data(), nullptr);
		ma_audio_buffer buffer;
		REQUIRE(ma_audio_buffer_init(&bufferConfig, &buffer) == MA_SUCCESS);
		const ma_data_source_node_config sourceConfig = ma_data_source_node_config_init(&buffer);
		ma_data_source_node source;
		REQUIRE(ma_data_source_node_init(&graph, &sourceConfig, nullptr, &source) == MA_SUCCESS);

		LowPassNode node;
		CHECK(LowPassNodeInit(&graph, 1, Rate, 0.0f, &node) == MA_INVALID_ARGS);
		CHECK(LowPassNodeInit(&graph, 1, Rate, std::numeric_limits<float>::quiet_NaN(), &node) == MA_INVALID_ARGS);
		CHECK(LowPassNodeInit(&graph, 1, Rate, std::numeric_limits<float>::infinity(), &node) == MA_INVALID_ARGS);
		CHECK(LowPassNodeInit(&graph, 1, Rate, 24000.0f, &node) == MA_INVALID_ARGS);
		CHECK(LowPassNodeInit(&graph, 0, Rate, 100.0f, &node) == MA_INVALID_ARGS);
		REQUIRE(LowPassNodeInit(&graph, 1, Rate, 100.0f, &node) == MA_SUCCESS);
		REQUIRE(ma_node_attach_output_bus(&source, 0, &node, 0) == MA_SUCCESS);
		REQUIRE(ma_node_attach_output_bus(&node, 0, ma_node_graph_get_endpoint(&graph), 0) == MA_SUCCESS);
		auto read = [&graph](ma_uint64 frames) {
			std::vector<float> out(frames);
			ma_uint64 framesRead = 0;
			REQUIRE(ma_node_graph_read_pcm_frames(&graph, out.data(), frames, &framesRead) == MA_SUCCESS);
			return out;
		};

		// The glide: 10 ms after a jump from 100 Hz to 10 kHz the cutoff has covered 1 - 1/e of the way in log
		// frequency, and it settles exactly on the target.
		LowPassNodeSetCutoff(&node, 10000.0f);
		read(Rate / 100);
		CHECK(LowPassNodeGetCurrentCutoff(&node) == doctest::Approx(100.0 * std::pow(100.0, 1.0 - std::exp(-1.0))).epsilon(0.01));
		read(Rate / 10);
		CHECK(LowPassNodeGetCurrentCutoff(&node) == 10000.0f);

		// Settled on DC, the output is 1; at the switch to silence a restart at 100 Hz starts from an empty filter
		// (without it, the output would decay from 1 over a few milliseconds).
		const std::vector<float> settled = read(Rate / 20);
		CHECK(settled.back() == doctest::Approx(1.0f).epsilon(0.001));
		LowPassNodeSetCutoff(&node, 100.0f);
		read(Rate / 20);
		LowPassNodeRestart(&node, 100.0f);
		const std::vector<float> after = read(Rate / 100);
		CHECK(std::abs(after.front()) < 1e-6f);
		CHECK(LowPassNodeGetCurrentCutoff(&node) == 100.0f);

		LowPassNodeUninit(&node, nullptr);
		ma_data_source_node_uninit(&source, nullptr);
		ma_audio_buffer_uninit(&buffer);
		ma_node_graph_uninit(&graph, nullptr);
	}

	TEST_CASE("Doppler: a listener that stops being valid stops moving, so sources are no longer shifted")
	{
		BasaltTest::TempProject project("AudioListenerFreeze");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(2.0f));
		HeadlessAudio engine;
		// The listener walks toward the source at 30 m/s, then its transform breaks (or it goes away).
		auto measure = [](bool breakTransform) {
			AudioScene scene;
			Entity speaker = scene.Add("Speaker", DopplerSource("Assets/Audio/Tone.wav"), glm::vec3(0.0f, 0.0f, -60.0f));
			AudioSystem& audio = scene.Start();
			auto& transform = scene.GetListener().GetComponent<TransformComponent>();
			for (int i = 0; i < 6; i++)
			{
				transform.Translation.z -= 30.0f * StepSeconds;
				scene.Run(StepSeconds);
			}
			CHECK(audio.GetDopplerPitch(speaker) > 1.05f);
			if (breakTransform)
				transform.Scale = glm::vec3(0.0f);
			else
				scene.GetListener().GetComponent<AudioListenerComponent>().Active = false;
			scene.Run(StepSeconds);
			CHECK(audio.GetDopplerPitch(speaker) == 1.0f);
			// The mix agrees: miniaudio's listener stands still too.
			CHECK(MixedFrequency(0.2f) == doctest::Approx(440.0).epsilon(0.002));
		};
		measure(true);
		measure(false);
	}

	TEST_CASE("Finite positions too far out for miniaudio are refused like non-finite ones")
	{
		BasaltTest::TempProject project("AudioExtremePositions");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(2.0f));
		HeadlessAudio engine;
		AudioScene scene(glm::vec3(-3.0e38f, 0.0f, 0.0f));
		AudioSourceComponent source = DopplerSource("Assets/Audio/Tone.wav");
		source.ReverbSend = 1.0f;
		const uint64_t since = Log::GetHistory().GetTotalCount();
		Entity far = scene.Add("Far", source, glm::vec3(3.0e38f, 0.0f, 0.0f));
		Entity near = scene.Add("Near", source, glm::vec3(0.0f, 0.0f, -2.0f));
		AudioSystem& audio = scene.Start();
		CHECK_FALSE(audio.IsPlaying(far));
		CHECK(BasaltTest::CountLogMessages(since, "cannot play the AudioSource of 'Far': its world position is not finite or is beyond") == 1);
		const glm::vec3 beyond(0.0f, 2.0f * AudioSystem::MaxAudioCoordinate, 0.0f);
		CHECK_FALSE(audio.PlayOneShot("Assets/Audio/Tone.wav", &beyond));
		// The listener out there keeps miniaudio's last placement; the near source plays, and nothing turns to NaN.
		scene.Run(0.1f);
		CHECK(BasaltTest::CountLogMessages(since, "the listener 'Listener' has a transform that is not finite (or is beyond") == 1);
		const std::vector<float> mix = Mix(0.2f);
		CHECK(AllFinite(mix));
		CHECK(Rms(mix) > 0.01f);
		const float pitch = audio.GetDopplerPitch(near);
		CHECK(std::isfinite(pitch));
		// Exactly at the limit is fine.
		near.GetComponent<TransformComponent>().Translation = glm::vec3(AudioSystem::MaxAudioCoordinate, 0.0f, 0.0f);
		scene.Run(StepSeconds);
		CHECK(AllFinite(Mix(0.1f)));
	}
}
