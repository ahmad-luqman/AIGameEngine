#include <doctest/doctest.h>

#include <Basalt/Audio/AudioEngine.h>
#include <Basalt/Audio/AudioSystem.h>
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
}
