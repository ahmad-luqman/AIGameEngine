#pragma once

#include "Basalt/Audio/AudioBus.h"
#include "Basalt/Core/Timestep.h"
#include "Basalt/Core/UUID.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

		// Loads every source with PlayOnStart; they start sounding in BeginPlayback. IsPlaying reports them as
		// playing from here on, so OnCreate sees the scene as it will be.
		void Start();
		// Starts the sources Start or PlayOnStart loaded. The scene calls it after the scripts' OnCreate: with an
		// output device the mix runs all the time, and music must not sound before a script applies the player's
		// saved bus volumes or stops it.
		void BeginPlayback();
		// Loads the PlayOnStart sources among entities added by a scene load, as Start does for the first scene;
		// the scene calls BeginPlayback after their OnCreate.
		void PlayOnStart(const std::vector<Entity>& entities);
		void Update(Timestep ts);
		void OnEntityDestroyed(Entity entity);

		// (Re)starts the entity's AudioSourceComponent clip from the beginning, reading every field once (later
		// changes apply on the next Play). Stops the current sound first, also when the new one cannot play.
		// Returns false when the entity has no AudioSourceComponent or there is no audio engine, and false,
		// logging why, when the source cannot play (no clip, invalid values, a clip that does not load).
		bool Play(Entity entity);
		void Stop(Entity entity);
		bool IsPlaying(Entity entity) const;
		// True when the entity's current sound decodes its clip while it plays (AudioSource.Stream). Asks
		// miniaudio what it did rather than echoing the field, for tests and diagnostics.
		bool IsStreaming(Entity entity) const;
		// Fire-and-forget sound at a world position (non-spatial when position is null). Always decoded whole.
		// False without an audio engine, and false, logging why, when the clip cannot load, volume is not a
		// finite number >= 0 or bus is outside the enum.
		bool PlayOneShot(const std::string& clip, const glm::vec3* position, float volume = 1.0f, AudioBus bus = AudioBus::SFX);

		// Linear gain of a bus (0 = silent, 1 = unchanged, above 1 amplifies). A sound's level is its own volume
		// times its bus's times Master's. Returns false, logging why and changing nothing, when volume is not a
		// finite number >= 0, bus is outside the enum or the bus failed to create.
		bool SetBusVolume(AudioBus bus, float volume);
		// 0 (logged) for a bus outside the enum.
		float GetBusVolume(AudioBus bus) const;

		uint32_t GetActiveSoundCount() const;

	private:
		// Uninitializes and frees a sound or a bus (an ma_sound_group is an ma_sound).
		struct SoundDeleter
		{
			void operator()(ma_sound* sound) const;
		};
		using SoundHandle = std::unique_ptr<ma_sound, SoundDeleter>;

		static constexpr std::array<float, AudioBusCount> UnitBusVolumes = [] {
			std::array<float, AudioBusCount> volumes = {};
			volumes.fill(1.0f);
			return volumes;
		}();

		enum class StartMode
		{
			Now,
			// Loaded now, started by BeginPlayback.
			OnBeginPlayback
		};

		bool PlaySource(Entity entity, StartMode mode);
		SoundHandle CreateSound(const std::string& clip, bool spatial, AudioBus bus, bool stream);

	private:
		Scene* m_Scene = nullptr;
		// One ma_sound_group per bus, indexed by AudioBus. All null without an audio engine (nothing plays then);
		// one is null if its group failed to create (its sounds then play through Master). Declared before the
		// sounds so it is destroyed after them (do not reorder), and elements go in reverse order: Master (index 0)
		// last.
		std::array<SoundHandle, AudioBusCount> m_Buses;
		std::array<float, AudioBusCount> m_BusVolumes = UnitBusVolumes;
		std::unordered_map<UUID, SoundHandle> m_Sources;
		// Sources loaded by Start that BeginPlayback has not started yet (see BeginPlayback).
		std::unordered_set<UUID> m_PendingStarts;
		std::vector<SoundHandle> m_OneShots;
	};

}
