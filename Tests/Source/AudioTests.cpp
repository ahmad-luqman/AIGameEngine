#include <doctest/doctest.h>

#include <Basalt/Audio/AudioEngine.h>
#include <Basalt/Audio/AudioRules.h>
#include <Basalt/Audio/AudioSystem.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

#include "TestUtils.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace Basalt;

// Loudness checks compare RMS levels of the device-less mix. Exact zeros rely on miniaudio applying volume changes
// at once (it has no volume smoothing unless asked for one).

namespace {

	// 16-bit mono PCM WAV containing a sine tone.
	std::string MakeWav(float seconds, uint32_t sampleRate = 22050)
	{
		const uint32_t samples = static_cast<uint32_t>(seconds * static_cast<float>(sampleRate));
		std::vector<int16_t> pcm(samples);
		for (uint32_t i = 0; i < samples; i++)
			pcm[i] = static_cast<int16_t>(8000.0 * std::sin(2.0 * 3.14159265 * 440.0 * i / sampleRate));

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
		HeadlessAudio() { REQUIRE(AudioEngine::Init(true)); }
		~HeadlessAudio() { AudioEngine::Shutdown(); }

		HeadlessAudio(const HeadlessAudio&) = delete;
		HeadlessAudio& operator=(const HeadlessAudio&) = delete;
	};

	// Interleaved stereo at the device-less engine's rate: two samples per frame.
	constexpr size_t SamplesPerSecond = size_t{ 2 } * 48000;

	// Mixes the given duration and returns the interleaved stereo output.
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
		const int steps = static_cast<int>(std::lround(seconds * 60.0f));
		for (int i = 0; i < steps; i++)
			REQUIRE(AudioEngine::ReadDeviceless(1.0f / 60.0f, frames));
		return frames;
	}

	// RMS of every channel-th sample starting at first (first 0 = left, 1 = right), or of all samples.
	float Rms(const std::vector<float>& samples, size_t first = 0, size_t channels = 1)
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

	float Rms(std::vector<float>::const_iterator begin, std::vector<float>::const_iterator end)
	{
		return Rms(std::vector<float>(begin, end));
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
			const int frames = static_cast<int>(std::lround(seconds * 60.0f));
			for (int i = 0; i < frames; i++)
				m_Scene.OnUpdate(1.0f / 60.0f);
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
		CHECK_FALSE(audio.Play(broken));

		const glm::vec3 position(1.0f, 0.0f, 0.0f);
		CHECK(audio.PlayOneShot("Assets/Audio/Beep.wav", &position, 0.5f));
		CHECK(audio.GetActiveSoundCount() == 3);

		for (int i = 0; i < 30; i++)
			scene.OnUpdate(1.0f / 60.0f);
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
		scene.OnUpdate(1.0f / 60.0f);
		REQUIRE(scene.GetAudioSystem());
		CHECK(scene.GetAudioSystem()->IsPlaying(scene.FindEntityByName("Music")));
		CHECK_FALSE(scene.GetAudioSystem()->IsPlaying(scene.FindEntityByName("Silent")));
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
		};
		Scene scene;
		Entity entity = scene.CreateEntity("Speaker");
		entity.AddComponent<AudioSourceComponent>().Volume = 0.25f;
		for (const Case& c : cases)
		{
			CAPTURE(c.Problem);
			std::string error;
			CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "AudioSource", c.Patch, error));
			CHECK(error.find(c.Problem) != std::string::npos);
			CHECK(entity.GetComponent<AudioSourceComponent>().Volume == 0.25f);
		}
		// Equal distances are allowed (no attenuation range).
		std::string error;
		CHECK_MESSAGE(ComponentRegistry::AddOrPatch(entity, "AudioSource", { { "MinDistance", 5 }, { "MaxDistance", 5 } }, error), error);

		AudioSourceComponent invalid;
		invalid.Volume = nan;
		CHECK(ValidateAudioSource(invalid).find("Volume") == 0);
		invalid = {};
		invalid.Bus = static_cast<AudioBus>(9);
		CHECK(ValidateAudioSource(invalid) == "Bus must be one of its 4 values (got 9)");
		CHECK_FALSE(IsValidAudioVolume(nan));
		CHECK_FALSE(IsValidAudioVolume(std::numeric_limits<float>::infinity()));
		CHECK_FALSE(IsValidAudioVolume(-0.01f));
		CHECK(IsValidAudioVolume(0.0f));
		CHECK(IsValidAudioVolume(4.0f));

		// C++ code can bypass the registry; the system refuses to play such a source rather than corrupt its bus.
		BasaltTest::TempProject project("AudioValidation");
		project.WriteFile("Assets/Audio/Beep.wav", MakeWav(0.25f));
		HeadlessAudio engine;
		AudioScene audioScene;
		AudioSourceComponent negative = Source("Assets/Audio/Beep.wav");
		negative.Volume = -1.0f;
		Entity bad = audioScene.Add("Bad", negative);
		AudioSystem& audio = audioScene.Start();
		CHECK_FALSE(audio.IsPlaying(bad));
		CHECK_FALSE(audio.Play(bad));
		CHECK_FALSE(audio.PlayOneShot("Assets/Audio/Beep.wav", nullptr, nan));
		CHECK_FALSE(audio.PlayOneShot("Assets/Audio/Beep.wav", nullptr, -1.0f));
		CHECK(audio.GetActiveSoundCount() == 0);
		CHECK_FALSE(audio.SetBusVolume(AudioBus::Music, nan));
		CHECK_FALSE(audio.SetBusVolume(AudioBus::Music, -0.5f));
		CHECK(audio.GetBusVolume(AudioBus::Music) == 1.0f);
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

	TEST_CASE("A bus value outside the enum (C++ only) falls back to SFX")
	{
		BasaltTest::TempProject project("AudioBadBus");
		HeadlessAudio engine;
		AudioScene scene;
		AudioSystem& audio = scene.Start();
		CHECK(audio.SetBusVolume(static_cast<AudioBus>(7), 0.5f));
		CHECK(audio.GetBusVolume(AudioBus::SFX) == 0.5f);
		CHECK(audio.GetBusVolume(static_cast<AudioBus>(7)) == 0.5f);
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
		HeadlessAudio engine;
		Scene scene;
		scene.OnRuntimeStart();
		REQUIRE(scene.GetAudioSystem());
		CHECK(scene.GetAudioSystem()->SetBusVolume(AudioBus::Music, 0.0f));
		std::string error;
		REQUIRE_MESSAGE(scene.RequestLoad("Assets/Scenes/Next.bscene", error), error);
		scene.OnUpdate(1.0f / 60.0f);
		AudioSystem* audio = scene.GetAudioSystem();
		REQUIRE(audio);
		Entity music = scene.FindEntityByName("Music");
		CHECK(audio->GetBusVolume(AudioBus::Music) == 0.0f);
		CHECK(audio->IsPlaying(music));
		CHECK(Rms(Mix(0.1f)) == 0.0f);
		CHECK(audio->SetBusVolume(AudioBus::Music, 1.0f));
		CHECK(Rms(Mix(0.1f)) > 0.01f);
		CHECK(audio->SetBusVolume(AudioBus::Music, 0.25f));

		// Loading away from a playing stream releases it.
		REQUIRE_MESSAGE(scene.RequestLoad("Assets/Scenes/Next.bscene", error), error);
		scene.OnUpdate(1.0f / 60.0f);
		CHECK(audio->GetActiveSoundCount() == 1);
		CHECK(audio->IsPlaying(scene.FindEntityByName("Music")));
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
		CHECK(Rms(streamed.begin() + 2 * SamplesPerSecond, streamed.begin() + 2 * SamplesPerSecond + SamplesPerSecond / 10) > 0.01f);
		CHECK(Rms(streamed.end() - SamplesPerSecond / 4, streamed.end()) == 0.0f);
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
			CHECK(Rms(streamed.end() - SamplesPerSecond / 5, streamed.end()) > 0.01f);
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

		// Stopped while a page refill is pending, then replayed from the start.
		scene.Run(1.5f);
		audio.Stop(songEntity);
		CHECK(Rms(Mix(0.1f)) == 0.0f);
		REQUIRE(audio.Play(songEntity));
		CHECK(audio.GetActiveSoundCount() == 2);
		CHECK(audio.IsStreaming(songEntity));
		CHECK(Rms(Mix(0.1f)) > 0.01f);
		scene.Run(1.5f);
		scene.GetScene().DestroyEntity(songEntity);
		CHECK(audio.GetActiveSoundCount() == 1);
		CHECK(Rms(Mix(0.1f)) == 0.0f);
	}

	TEST_CASE("A streamed clip that cannot be read is reported, not played")
	{
		BasaltTest::TempProject project("AudioStreamBroken");
		project.WriteFile("Assets/Audio/Garbage.wav", "not a wav file at all");
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
		CHECK_FALSE(audio.Play(missingEntity));
		CHECK_FALSE(audio.Play(garbageEntity));
		CHECK(audio.GetActiveSoundCount() == 0);
		CHECK(Rms(Mix(0.1f)) == 0.0f);
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

	TEST_CASE("Spatial sources follow their entity, pan by direction, and one-shots fade too")
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
		scene.Run(1.0f / 60.0f);
		CHECK(nearRms / Rms(Mix(0.1f)) == doctest::Approx(10.0f).epsilon(0.1));

		// The listener looks down -Z, so +X is on its right (channel 1).
		speaker.GetComponent<TransformComponent>().Translation = glm::vec3(3.0f, 0.0f, 0.0f);
		scene.Run(1.0f / 60.0f);
		std::vector<float> mix = Mix(0.1f);
		CHECK(Rms(mix, 1, 2) > 2.0f * Rms(mix, 0, 2));
		// Turned around, the same source is on its left.
		scene.GetListener().GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
		scene.Run(1.0f / 60.0f);
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
		scene.GetScene().OnUpdate(1.0f / 60.0f);
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
		CHECK(AudioEngine::ReadDeviceless(0.01f, frames));
		CHECK(frames.size() == 2 * 480);
	}
}
