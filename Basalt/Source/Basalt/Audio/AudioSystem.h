#pragma once

#include "Basalt/Audio/AudioBus.h"
#include "Basalt/Audio/AudioReverbSettings.h"
#include "Basalt/Core/Timestep.h"
#include "Basalt/Core/UUID.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ma_sound;

namespace Basalt {

	class Entity;
	class Scene;
	struct AudioSourceComponent;

	// Plays AudioSourceComponents of a running scene, positions the listener and owns the mixer buses and their
	// effects. The listener is the first active AudioListenerComponent, or the primary camera if there is none.
	// Bus volumes and the reverb settings belong to the play session: they start at their defaults, survive
	// Scene.Load (the system does) and end when play stops, so a game applies its saved settings when it starts.
	//
	// Each source's sound runs through a chain built from its live effect fields:
	//   sound -> [low-pass] -> [splitter: dry -> bus, ReverbSend -> the bus's reverb] -> bus
	// A stage is in the chain only while its effect is on (built on first use, bypassed when it turns off), so a
	// source without effects plays straight into its bus. Each bus gets its own reverb the first time one of its
	// sources sends to it; the reverb returns into that bus, so the bus volume scales the reverb too. The Doppler shift comes from velocities: an entity's own dynamic or kinematic
	// body (or character), else its closest ancestor's (a camera on a character; only the linear velocity), else its
	// change in world position (also for static bodies, which report none).
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
		// Their movement history and the listener's start over here, so an OnCreate that places them is no teleport.
		void BeginPlayback();
		// Loads the PlayOnStart sources among entities added by a scene load, as Start does for the first scene;
		// the scene calls BeginPlayback after their OnCreate.
		void PlayOnStart(const std::vector<Entity>& entities);
		void Update(Timestep ts);
		void OnEntityDestroyed(Entity entity);

		// (Re)starts the entity's AudioSourceComponent clip from the beginning, reading every field except the live
		// effects (Doppler, LowPass, ReverbSend, which Update re-applies every frame) once: changes to the others
		// apply on the next Play. Stops the current sound first, also when the new one cannot play.
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

		// The room every bus's reverb simulates. Returns false, logging why and changing nothing, when a setting is
		// outside 0..1 or not finite.
		bool SetReverb(const AudioReverbSettings& reverb);
		const AudioReverbSettings& GetReverb() const { return m_Reverb; }

		// The Doppler pitch factor of the entity's sound, from the positions, velocities and factor the last update gave
		// miniaudio (the shift its next mix applies): 1 = unshifted, also while it is not playing (never played,
		// stopped, or played to its end) or when its sound is not spatial. For tests and diagnostics.
		float GetDopplerPitch(Entity entity) const;
		// Whether the reverb of the bus exists (created when one of its sources first sends to it).
		bool HasReverb(AudioBus bus) const;

		uint32_t GetActiveSoundCount() const;

		// Speeds above this (world units per second) are clamped before they reach the Doppler shift: a teleport
		// would otherwise read as a speed past the speed of sound, which silences or shrieks for a moment.
		static constexpr float MaxDopplerSpeed = 100.0f;

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

		// Where a source's chain ends: the bus's group, Master if the bus failed to create, the engine's endpoint
		// if Master did too.
		struct Output
		{
			// An ma_node*, which miniaudio declares as void*.
			void* Node = nullptr;
			// The bus whose reverb the source sends to; nullopt for the endpoint (no reverb there).
			std::optional<size_t> Bus;
		};

		// The listener's entity, its previous world position (for its velocity) and its last velocity. A new
		// listener starts over.
		struct ListenerState
		{
			UUID ID = 0;
			std::optional<glm::vec3> LastPosition;
			glm::vec3 Velocity = glm::vec3(0.0f);
			// A transform or velocity that is not finite is reported once, until it is finite again.
			bool ReportedInvalidTransform = false;
			bool ReportedInvalidVelocity = false;
		};

		// A source's sound (until it is stopped, also after a clip ends) and the effect nodes downstream of it
		// (defined in the .cpp; miniaudio's node types are anonymous structs that cannot be forward declared).
		struct Voice;
		struct Reverb;

		bool PlaySource(Entity entity, StartMode mode);
		SoundHandle CreateSound(const std::string& clip, bool spatial, const Output& output, bool stream);
		Output GetOutput(AudioBus bus) const;
		// Applies the live effect fields of the entity's source to its voice, building or bypassing chain stages as
		// they turn on or off.
		void ApplyEffects(Voice& voice, Entity entity, const AudioSourceComponent& source);
		// The reverb of a bus, created on first use; null when it cannot be created (logged once per bus), and
		// without a log when the bus itself failed or there is no engine.
		Reverb* GetReverbNode(size_t bus);

	private:
		Scene* m_Scene = nullptr;
		// One ma_sound_group per bus, indexed by AudioBus. All null without an audio engine (nothing plays then);
		// one is null if its group failed to create (its sounds then play through Master). Declared before the
		// sounds so it is destroyed after them (do not reorder), and elements go in reverse order: Master (index 0)
		// last.
		std::array<SoundHandle, AudioBusCount> m_Buses;
		std::array<float, AudioBusCount> m_BusVolumes = UnitBusVolumes;
		// One reverb per bus, created on first use (see GetReverbNode), returning into the bus. Destroyed after the
		// sources that send to them and before the buses they feed (do not reorder).
		std::array<std::unique_ptr<Reverb>, AudioBusCount> m_Reverbs;
		// Buses whose reverb failed to create, so the error is logged once.
		std::array<bool, AudioBusCount> m_ReverbFailed = {};
		AudioReverbSettings m_Reverb;
		std::unordered_map<UUID, std::unique_ptr<Voice>> m_Sources;
		// Sources loaded by Start that BeginPlayback has not started yet (see BeginPlayback).
		std::unordered_set<UUID> m_PendingStarts;
		std::vector<SoundHandle> m_OneShots;
		ListenerState m_Listener;
		// What miniaudio's listener was last given (it keeps it while there is no listener), for GetDopplerPitch.
		glm::vec3 m_ListenerPosition = glm::vec3(0.0f);
		glm::vec3 m_ListenerVelocity = glm::vec3(0.0f);
	};

}
