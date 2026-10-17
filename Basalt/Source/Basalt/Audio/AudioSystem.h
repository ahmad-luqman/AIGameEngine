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
	// A stage exists only while its effect is on, so a source without effects sounds exactly as before. Each bus
	// gets its own reverb the first time one of its sources sends to it; the reverb returns into that bus, so the
	// bus volume scales the reverb too. The Doppler shift comes from velocities: an entity's own physics body (or
	// character), else its closest ancestor's (a camera on a character), else its change in world position.
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

		// The room every bus's reverb simulates. Returns false, logging why and changing nothing, when a setting is
		// outside 0..1 or not finite.
		bool SetReverb(const AudioReverbSettings& reverb);
		const AudioReverbSettings& GetReverb() const { return m_Reverb; }

		// The Doppler pitch factor miniaudio applied to the entity's sound in the last mix (1 = unshifted; also 1
		// without a sound or a spatial one). For tests and diagnostics, like IsStreaming.
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

		// The previous world position of a sound's entity or the listener, for velocities from movement.
		struct Motion
		{
			glm::vec3 LastPosition = glm::vec3(0.0f);
			bool HasLastPosition = false;
		};

		// A playing source: its sound and the effect nodes behind it (defined in the .cpp; miniaudio's node types
		// are anonymous structs that cannot be forward declared).
		struct Voice;
		struct Reverb;

		bool PlaySource(Entity entity, StartMode mode);
		SoundHandle CreateSound(const std::string& clip, bool spatial, const Output& output, bool stream);
		Output GetOutput(AudioBus bus) const;
		// Applies the live effect fields of source to voice, building or bypassing chain stages as they turn on or off.
		void ApplyEffects(Voice& voice, const AudioSourceComponent& source);
		// Routes the chain through the low-pass and, with a reverb, through the send to it.
		void RouteVoice(Voice& voice, bool lowPass, Reverb* reverb);
		// The reverb of a bus, created on first use; null (logged once per bus) when it cannot be created.
		Reverb* GetReverbNode(size_t bus);
		// The entity's velocity for the Doppler shift (see the class comment), clamped to MaxDopplerSpeed. Records
		// position in motion; with ts == 0 (the immediate updates of Start and PlayOnStart) it only seeds motion.
		glm::vec3 TrackVelocity(Entity entity, const glm::vec3& position, Motion& motion, float seconds) const;

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
		// The listener's entity and its movement, for its velocity; a new listener starts with no history.
		UUID m_ListenerID = 0;
		Motion m_ListenerMotion;
		glm::vec3 m_ListenerVelocity = glm::vec3(0.0f);
	};

}
