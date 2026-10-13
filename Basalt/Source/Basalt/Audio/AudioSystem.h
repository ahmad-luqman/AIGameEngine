#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Core/Timestep.h"
#include "Basalt/Core/UUID.h"
#include "Basalt/Scene/Components.h"

#include <glm/glm.hpp>

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

struct ma_sound;

namespace Basalt {

	class Entity;
	class Scene;

	// Plays AudioSourceComponents of a running scene, positions the listener and owns the mixer buses.
	// The listener is the first active AudioListenerComponent, or the primary camera if there is none.
	// Bus volumes belong to the play session: each one starts at 1, survives Scene.Load (the system does) and
	// ends when play stops, so a game applies its saved settings when it starts.
	class AudioSystem
	{
	public:
		explicit AudioSystem(Scene* scene);
		~AudioSystem();

		AudioSystem(const AudioSystem&) = delete;
		AudioSystem& operator=(const AudioSystem&) = delete;

		// Starts every source with PlayOnStart.
		void Start();
		// Starts the PlayOnStart sources among entities added by a scene load, as Start does for the first scene.
		void PlayOnStart(const std::vector<Entity>& entities);
		void Update(Timestep ts);
		void OnEntityDestroyed(Entity entity);

		// (Re)starts the entity's AudioSourceComponent clip from the beginning. Returns false if it cannot play.
		bool Play(Entity entity);
		void Stop(Entity entity);
		bool IsPlaying(Entity entity) const;
		// True when the entity's current sound decodes its clip while it plays (AudioSource.Stream).
		bool IsStreaming(Entity entity) const;
		// Fire-and-forget sound at a world position (non-spatial when position is null). Always decoded whole.
		bool PlayOneShot(const std::string& clip, const glm::vec3* position, float volume = 1.0f, AudioBus bus = AudioBus::SFX);

		// Linear gain of a bus (0 = silent, 1 = unchanged). A sound's level is its own volume times its bus's
		// times Master's. volume must be finite and not negative (the Lua binding checks it).
		void SetBusVolume(AudioBus bus, float volume);
		float GetBusVolume(AudioBus bus) const;

		uint32_t GetActiveSoundCount() const;

	private:
		ma_sound* CreateSound(const std::string& clip, bool spatial, AudioBus bus, bool stream);
		static void DestroySound(ma_sound* sound);

	private:
		Scene* m_Scene = nullptr;
		std::unordered_map<UUID, ma_sound*> m_Sources;
		std::vector<ma_sound*> m_OneShots;
		// One ma_sound_group per bus, indexed by AudioBus; null without an audio engine (sounds are skipped then).
		std::array<ma_sound*, AudioBusCount> m_Buses = {};
		std::array<float, AudioBusCount> m_BusVolumes;
	};

}
