#include <doctest/doctest.h>

#include <Basalt/Audio/AudioEngine.h>
#include <Basalt/Audio/AudioSystem.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

#include "TestUtils.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace Basalt;

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

	// Mixes the given duration and returns the interleaved stereo output.
	std::vector<float> Mix(float seconds)
	{
		std::vector<float> frames;
		REQUIRE(AudioEngine::ReadDeviceless(seconds, frames));
		return frames;
	}

	float Rms(const std::vector<float>& samples)
	{
		if (samples.empty())
			return 0.0f;
		double sum = 0.0;
		for (float sample : samples)
			sum += static_cast<double>(sample) * sample;
		return static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
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

	// A running scene with a listener at the origin (looking down -Z) and the given sources.
	class AudioScene
	{
	public:
		explicit AudioScene(const glm::vec3& listenerPosition = glm::vec3(0.0f))
		{
			Entity listener = m_Scene.CreateEntity("Listener");
			listener.GetComponent<TransformComponent>().Translation = listenerPosition;
			listener.AddComponent<AudioListenerComponent>();
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

	private:
		Scene m_Scene;
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

}

TEST_SUITE("Audio")
{
	TEST_CASE("Audio sources play, finish and are positioned without an output device")
	{
		BasaltTest::TempProject project("Audio");
		project.WriteFile("Assets/Audio/Beep.wav", MakeWav(0.25f));
		REQUIRE(AudioEngine::Init(true));
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
		AudioEngine::Shutdown();
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
		REQUIRE(AudioEngine::Init(true));

		Scene scene;
		scene.OnRuntimeStart();
		std::string error;
		REQUIRE_MESSAGE(scene.RequestLoad("Assets/Scenes/Next.bscene", error), error);
		scene.OnUpdate(1.0f / 60.0f);
		REQUIRE(scene.GetAudioSystem());
		CHECK(scene.GetAudioSystem()->IsPlaying(scene.FindEntityByName("Music")));
		CHECK_FALSE(scene.GetAudioSystem()->IsPlaying(scene.FindEntityByName("Silent")));
		scene.OnRuntimeStop();
		AudioEngine::Shutdown();
	}

	TEST_CASE("AudioSource Bus and Stream fields round-trip through the component registry")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Speaker");
		std::string error;
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, "AudioSource", { { "Clip", "Music.ogg" } }, error), error);
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

	TEST_CASE("Mixer buses scale the sounds routed to them, and Master scales every bus")
	{
		BasaltTest::TempProject project("AudioBuses");
		project.WriteFile("Assets/Audio/Tone.wav", MakeWav(1.0f));
		HeadlessAudio engine;
		AudioScene scene;
		scene.Add("Music", Source("Assets/Audio/Tone.wav", AudioBus::Music));
		scene.Add("Effect", Source("Assets/Audio/Tone.wav", AudioBus::SFX));
		AudioSystem& audio = scene.Start();
		for (size_t i = 0; i < AudioBusCount; i++)
			CHECK(audio.GetBusVolume(static_cast<AudioBus>(i)) == 1.0f);

		audio.SetBusVolume(AudioBus::SFX, 0.0f);
		const float musicOnly = Rms(Mix(0.1f));
		CHECK(musicOnly > 0.01f);
		audio.SetBusVolume(AudioBus::Music, 0.5f);
		CHECK(audio.GetBusVolume(AudioBus::Music) == 0.5f);
		CHECK(Rms(Mix(0.1f)) == doctest::Approx(musicOnly * 0.5f).epsilon(0.05));

		audio.SetBusVolume(AudioBus::Music, 0.0f);
		CHECK(Rms(Mix(0.1f)) == 0.0f);
		audio.SetBusVolume(AudioBus::SFX, 1.0f);
		CHECK(Rms(Mix(0.1f)) == doctest::Approx(musicOnly).epsilon(0.05));

		audio.SetBusVolume(AudioBus::Music, 1.0f);
		audio.SetBusVolume(AudioBus::Master, 0.0f);
		CHECK(Rms(Mix(0.1f)) == 0.0f);
		audio.SetBusVolume(AudioBus::Master, 0.5f);
		CHECK(Rms(Mix(0.1f)) == doctest::Approx(musicOnly).epsilon(0.05));

		// One-shots take a bus too (SFX by default).
		audio.SetBusVolume(AudioBus::Master, 1.0f);
		audio.SetBusVolume(AudioBus::Music, 0.0f);
		audio.SetBusVolume(AudioBus::SFX, 0.0f);
		audio.SetBusVolume(AudioBus::UI, 0.0f);
		REQUIRE(audio.PlayOneShot("Assets/Audio/Tone.wav", nullptr, 1.0f, AudioBus::UI));
		REQUIRE(audio.PlayOneShot("Assets/Audio/Tone.wav", nullptr));
		CHECK(Rms(Mix(0.1f)) == 0.0f);
		audio.SetBusVolume(AudioBus::UI, 1.0f);
		const float clickOnly = Rms(Mix(0.1f));
		CHECK(clickOnly == doctest::Approx(musicOnly).epsilon(0.05));
		audio.SetBusVolume(AudioBus::UI, 0.0f);
		audio.SetBusVolume(AudioBus::SFX, 1.0f);
		CHECK(Rms(Mix(0.1f)) > 1.5f * clickOnly);
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
		scene.GetAudioSystem()->SetBusVolume(AudioBus::Music, 0.0f);
		std::string error;
		REQUIRE_MESSAGE(scene.RequestLoad("Assets/Scenes/Next.bscene", error), error);
		scene.OnUpdate(1.0f / 60.0f);
		AudioSystem* audio = scene.GetAudioSystem();
		REQUIRE(audio);
		Entity music = scene.FindEntityByName("Music");
		CHECK(audio->GetBusVolume(AudioBus::Music) == 0.0f);
		CHECK(audio->IsPlaying(music));
		CHECK(Rms(Mix(0.1f)) == 0.0f);
		audio->SetBusVolume(AudioBus::Music, 1.0f);
		CHECK(Rms(Mix(0.1f)) > 0.01f);
		audio->SetBusVolume(AudioBus::Music, 0.25f);

		scene.OnRuntimeStop();
		scene.OnRuntimeStart();
		CHECK(scene.GetAudioSystem()->GetBusVolume(AudioBus::Music) == 1.0f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A streamed clip plays exactly the samples of the decoded clip, across its pages")
	{
		BasaltTest::TempProject project("AudioStream");
		// 2.5 s at the engine's rate: three one-second stream pages, no resampling.
		project.WriteFile("Assets/Audio/Song.wav", MakeWav(2.5f, 48000));
		HeadlessAudio engine;

		auto play = [](bool stream) {
			AudioScene scene;
			AudioSourceComponent source = Source("Assets/Audio/Song.wav", AudioBus::Music);
			source.Loop = false;
			source.Stream = stream;
			Entity song = scene.Add("Song", source);
			AudioSystem& audio = scene.Start();
			REQUIRE(audio.IsPlaying(song));
			CHECK(audio.IsStreaming(song) == stream);
			std::vector<float> output = Mix(3.0f);
			// Updating the scene by 0 s mixes nothing but keeps the source registered.
			scene.Run(0.0f);
			CHECK_FALSE(audio.IsPlaying(song));
			return output;
		};
		const std::vector<float> decoded = play(false);
		const std::vector<float> streamed = play(true);
		REQUIRE(decoded.size() == streamed.size());
		size_t mismatches = 0;
		for (size_t i = 0; i < decoded.size(); i++)
		{
			if (std::abs(decoded[i] - streamed[i]) > 1e-6f)
				mismatches++;
		}
		CHECK(mismatches == 0);
		// The last page is heard, and the clip ends on time.
		// Interleaved stereo: two samples per frame.
		constexpr size_t samplesPerSecond = size_t{ 2 } * 48000;
		CHECK(Rms({ streamed.begin() + 2 * samplesPerSecond, streamed.begin() + 2 * samplesPerSecond + samplesPerSecond / 10 }) > 0.01f);
		CHECK(Rms({ streamed.end() - samplesPerSecond / 4, streamed.end() }) == 0.0f);
	}

	TEST_CASE("A looping stream keeps playing past its end")
	{
		BasaltTest::TempProject project("AudioStreamLoop");
		project.WriteFile("Assets/Audio/Ambience.wav", MakeWav(1.2f));
		HeadlessAudio engine;
		AudioScene scene;
		AudioSourceComponent source = Source("Assets/Audio/Ambience.wav", AudioBus::Music);
		source.Stream = true;
		Entity ambience = scene.Add("Ambience", source);
		AudioSystem& audio = scene.Start();
		scene.Run(3.0f);
		CHECK(audio.IsPlaying(ambience));
		CHECK(Rms(Mix(0.2f)) > 0.01f);
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
		const float near = MeasureRms(spatial, glm::vec3(0.0f, 0.0f, -2.0f));
		const float far = MeasureRms(spatial, glm::vec3(0.0f, 0.0f, -20.0f));
		CHECK(near > 0.01f);
		// Inverse distance attenuation: gain = MinDistance / distance.
		CHECK(near / far == doctest::Approx(10.0f).epsilon(0.1));
		// Beyond MaxDistance the gain stops falling.
		CHECK(MeasureRms(spatial, glm::vec3(0.0f, 0.0f, -100.0f)) == doctest::Approx(MeasureRms(spatial, glm::vec3(0.0f, 0.0f, -50.0f))).epsilon(0.02));

		const AudioSourceComponent flat = Source("Assets/Audio/Tone.wav");
		CHECK(MeasureRms(flat, glm::vec3(0.0f, 0.0f, -20.0f)) == doctest::Approx(MeasureRms(flat, glm::vec3(0.0f, 0.0f, -2.0f))).epsilon(0.02));
		CHECK(MeasureRms(flat, glm::vec3(0.0f, 0.0f, -20.0f)) > near);
		// Nor are the buses they play through: a bus is not a point in the world.
		const glm::vec3 farListener(0.0f, 0.0f, 100.0f);
		CHECK(MeasureRms(flat, glm::vec3(0.0f), farListener) == doctest::Approx(MeasureRms(flat, glm::vec3(0.0f))).epsilon(0.02));
		// A spatial source next to a listener far from the origin is as loud as next to one at the origin.
		CHECK(MeasureRms(spatial, farListener - glm::vec3(0.0f, 0.0f, 2.0f), farListener) == doctest::Approx(near).epsilon(0.02));
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

		scene.Run(0.3f);
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
		// A finished source stays registered until it is stopped or replayed.
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

		AudioScene scene;
		Entity beep = scene.Add("Beep", Source("Assets/Audio/Beep.wav"));
		AudioSystem& audio = scene.Start();
		CHECK_FALSE(audio.IsPlaying(beep));
		CHECK_FALSE(audio.Play(beep));
		CHECK_FALSE(audio.PlayOneShot("Assets/Audio/Beep.wav", nullptr));
		CHECK(audio.GetActiveSoundCount() == 0);
		audio.SetBusVolume(AudioBus::Music, 0.3f);
		CHECK(audio.GetBusVolume(AudioBus::Music) == doctest::Approx(0.3f));
		scene.Run(0.1f);
	}
}
