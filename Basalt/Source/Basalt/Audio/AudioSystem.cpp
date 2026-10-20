#include "Basalt/Audio/AudioSystem.h"

#include "Basalt/Audio/AudioEngine.h"
#include "Basalt/Audio/AudioRules.h"
#include "Basalt/Audio/LowPassNode.h"
#include "Basalt/Audio/ReverbNode.h"
#include "Basalt/Audio/SourceEffectsNode.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace Basalt {

	namespace {

		// Created first and destroyed last: every other bus feeds it.
		constexpr auto MasterIndex = static_cast<size_t>(AudioBus::Master);
		static_assert(MasterIndex == 0);

		// The bus's index into the bus arrays; nullopt (logged) for a value outside the enum, which only C++ code
		// can pass (the registry and Lua reject unknown names, and sources are validated before they play).
		std::optional<size_t> BusIndex(AudioBus bus)
		{
			const auto index = static_cast<size_t>(bus);
			if (index < AudioBusCount)
				return index;
			BS_CORE_ERROR("Audio: bus value {} is out of range", index);
			return std::nullopt;
		}

		// Uninitializes and frees a miniaudio node with its own uninit function.
		template<typename T, void (*Uninit)(T*, const ma_allocation_callbacks*)>
		struct NodeDeleter
		{
			void operator()(T* node) const
			{
				Uninit(node, nullptr);
				delete node;
			}
		};

		using SourceEffectsHandle = std::unique_ptr<SourceEffectsNode, NodeDeleter<SourceEffectsNode, SourceEffectsNodeUninit>>;
		using LowPassHandle = std::unique_ptr<LowPassNode, NodeDeleter<LowPassNode, LowPassNodeUninit>>;
		using SplitterHandle = std::unique_ptr<ma_splitter_node, NodeDeleter<ma_splitter_node, ma_splitter_node_uninit>>;
		using ReverbHandle = std::unique_ptr<ReverbNode, NodeDeleter<ReverbNode, ReverbNodeUninit>>;

		// Creates a node through init(node); null, logging what failed and what plays instead, when init fails. A
		// plain unique_ptr until init succeeds: a handle's deleter must only see initialized nodes.
		template<typename Handle, typename Init>
		Handle CreateNode(Init init, std::string_view what, std::string_view fallback)
		{
			auto node = std::make_unique<typename Handle::element_type>();
			if (const ma_result result = init(node.get()); result != MA_SUCCESS)
			{
				BS_CORE_ERROR("Audio: cannot create {} ({}); {}", what, ma_result_description(result), fallback);
				return nullptr;
			}
			return Handle(node.release());
		}

		// With the largest factor and speed, both terms of miniaudio's pitch ratio stay positive, so the pitch stays
		// finite and above 0: the 0.26..3.8 range MaxAudioDoppler documents. At the speed of sound it would fall to 0
		// (silence) for a listener moving away and grow without bound for an approaching source. (miniaudio's
		// MA_DEFAULT_SPEED_OF_SOUND is only visible to its implementation, so AudioSpeedOfSound restates it.)
		static_assert(MaxAudioDoppler * AudioSystem::MaxDopplerSpeed < AudioSpeedOfSound);

		// The live effect values of a source, each one turned off when it is outside its AudioRules range (only C++
		// code can store such values: the registry rejects them).
		struct EffectValues
		{
			float Doppler = 0.0f;
			float LowPass = 0.0f;
			float ReverbSend = 0.0f;
			float HighPass = 0.0f, Echo = 0.0f, Distortion = 0.0f;
		};

		EffectValues SanitizeEffects(const AudioSourceComponent& source, float sampleRate)
		{
			EffectValues values;
			if (IsValidAudioDoppler(source.Doppler))
				values.Doppler = source.Doppler;
			// The filter is only defined below the Nyquist frequency; keep a margin under it (a 15 kHz cutoff becomes
			// 9.9 kHz on a 22.05 kHz output).
			if (IsValidAudioLowPass(source.LowPass))
				values.LowPass = std::min(source.LowPass, MaxAudioLowPassRateFraction * sampleRate);
			if (IsValidAudioReverbSend(source.ReverbSend))
				values.ReverbSend = source.ReverbSend;
			if (IsValidAudioLowPass(source.HighPass))
				values.HighPass = std::min(source.HighPass, MaxAudioLowPassRateFraction * sampleRate);
			if (IsFiniteInRange(source.Echo, 0, 1))
				values.Echo = source.Echo;
			if (IsFiniteInRange(source.Distortion, 0, 1))
				values.Distortion = source.Distortion;
			return values;
		}

		bool IsFinite(const glm::vec3& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}

		// A position miniaudio can work with: finite, and within MaxAudioCoordinate of the origin on every axis, so the
		// spatializer's distances and their squares stay far from float's range (beyond it they overflow into NaN).
		bool IsUsablePosition(const glm::vec3& v)
		{
			return IsFinite(v) && glm::all(glm::lessThanEqual(glm::abs(v), glm::vec3(AudioSystem::MaxAudioCoordinate)));
		}

		// The linear velocity of the entity's own dynamic or kinematic body or character, else of its closest
		// ancestor's (a camera on a character); nullopt when none has one, or when a static body comes first: a script
		// can move a static body, which then reports no velocity, so its movement is measured instead.
		std::optional<glm::vec3> FindBodyVelocity(const PhysicsWorld* physics, Entity entity)
		{
			if (!physics)
				return std::nullopt;
			for (Entity e = entity; e; e = e.GetParent())
			{
				if (physics->HasCharacter(e))
					return physics->GetLinearVelocity(e);
				if (physics->HasBody(e))
				{
					const auto* body = e.TryGetComponent<RigidBodyComponent>();
					if (!body || body->Type == RigidBodyType::Static)
						return std::nullopt;
					return physics->GetLinearVelocity(e);
				}
			}
			return std::nullopt;
		}

		// The pitch factor miniaudio's spatializer gives a sound at soundPosition (ma_doppler_pitch). Computed here from
		// what the system hands miniaudio, so the main thread never reads the value the audio thread writes. In double:
		// finite but extreme positions (near FLT_MAX, from bad scene data) would overflow the difference in float.
		float DopplerPitch(const glm::vec3& listenerPosition, const glm::vec3& soundPosition, const glm::vec3& soundVelocity, const glm::vec3& listenerVelocity, float factor)
		{
			const glm::dvec3 toListener = glm::dvec3(listenerPosition) - glm::dvec3(soundPosition);
			const double distance = glm::length(toListener);
			if (factor <= 0.0f || distance == 0.0)
				return 1.0f;
			const double speedOfSound = AudioSpeedOfSound;
			const double limit = speedOfSound / factor;
			const double listenerSpeed = std::min(glm::dot(toListener, glm::dvec3(listenerVelocity)) / distance, limit);
			const double soundSpeed = std::min(glm::dot(toListener, glm::dvec3(soundVelocity)) / distance, limit);
			return static_cast<float>((speedOfSound - factor * listenerSpeed) / (speedOfSound - factor * soundSpeed));
		}

		// True when a problem seen every frame should be logged: when it appears, and again after it has cleared.
		bool ShouldReport(bool& reported, bool hasProblem)
		{
			const bool report = hasProblem && !reported;
			reported = hasProblem;
			return report;
		}

		// The entity's velocity for the Doppler shift, clamped to MaxDopplerSpeed, and records position in
		// lastPosition. A body knows its velocity exactly; a position delta would jitter whenever frames and fixed
		// physics steps do not line up (some frames see no step, others two), which a Doppler shift turns into a
		// warble, so the delta is only the fallback. nullopt when seconds is 0 (the immediate updates of Start and
		// PlayOnStart), which have no movement to measure and only seed lastPosition. Positions are finite (the caller
		// checks); a body's velocity can still blow up with its simulation, which gives no shift (reported once).
		std::optional<glm::vec3> TrackVelocity(const PhysicsWorld* physics, Entity entity, const glm::vec3& position, std::optional<glm::vec3>& lastPosition, float seconds, bool& reportedInvalidVelocity)
		{
			if (!(seconds > 0.0f))
			{
				if (!lastPosition)
					lastPosition = position;
				return std::nullopt;
			}
			glm::vec3 velocity(0.0f);
			if (const std::optional<glm::vec3> bodyVelocity = FindBodyVelocity(physics, entity))
				velocity = *bodyVelocity;
			else if (lastPosition)
				velocity = (position - *lastPosition) / seconds;
			lastPosition = position;

			if (ShouldReport(reportedInvalidVelocity, !IsFinite(velocity)))
				BS_CORE_ERROR("Audio: the physics velocity of '{}' is not finite; its sound gets no Doppler shift", entity.GetName());
			if (!IsFinite(velocity))
				return glm::vec3(0.0f);
			// Bounded first so the length cannot overflow.
			velocity = glm::clamp(velocity, glm::vec3(-1e6f), glm::vec3(1e6f));
			const float speed = glm::length(velocity);
			return speed > AudioSystem::MaxDopplerSpeed ? velocity * (AudioSystem::MaxDopplerSpeed / speed) : velocity;
		}

		// One effect stage of a voice: its node (created on first use), whether the chain runs through it, and whether
		// creating or routing it failed (not retried, which would log every frame; the sound plays without it).
		template<typename Handle>
		struct EffectStage
		{
			Handle Node;
			bool Routed = false;
			bool Failed = false;

			// Drops the node (its uninit detaches it from the graph) and gives up on the stage.
			void Disable()
			{
				Node.reset();
				Routed = false;
				Failed = true;
			}
		};

	}

	struct AudioSystem::Voice
	{
		Voice(SoundHandle sound, const Output& out)
			: Sound(std::move(sound))
			, Out(out)
		{
			BS_CORE_ASSERT(Sound && Out.Node, "a voice has a sound and somewhere to send it");
		}

		// Declared before the sound so the sound (upstream) is destroyed first (do not reorder).
		EffectStage<SplitterHandle> Send;
		EffectStage<LowPassHandle> LowPass;
		EffectStage<SourceEffectsHandle> Effects;
		float TailTime = 0.0f;
		bool EchoTail = false;
		SoundHandle Sound;
		// Fixed for the voice's life, which Route's early return relies on.
		const Output Out;
		// Problems reported once, until they clear, rather than every frame (the effects: the problem last reported).
		std::string ReportedEffectsProblem;
		bool ReportedInvalidPosition = false;
		bool ReportedInvalidVelocity = false;
		// The entity's previous world position, for its velocity.
		std::optional<glm::vec3> LastPosition;
		// What the sound was last given, for GetDopplerPitch.
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Velocity = glm::vec3(0.0f);
		float DopplerFactor = 0.0f;

		void SetPosition(const glm::vec3& position)
		{
			ma_sound_set_position(Sound.get(), position.x, position.y, position.z);
			Position = position;
		}

		void SetVelocity(const glm::vec3& velocity)
		{
			ma_sound_set_velocity(Sound.get(), velocity.x, velocity.y, velocity.z);
			Velocity = velocity;
		}

		void SetDopplerFactor(float factor)
		{
			ma_sound_set_doppler_factor(Sound.get(), factor);
			DopplerFactor = factor;
		}

		// Creates the low-pass on first use (at cutoff) and otherwise moves it to cutoff: it glides there while in the
		// chain, and restarts there, with its memory cleared, when it goes back in after a bypass. False when it does
		// not exist (creation failed, logged naming the source).
		bool UpdateLowPass(ma_engine* engine, float cutoff, std::string_view name)
		{
			if (LowPass.Failed)
				return false;
			if (!LowPass.Node)
			{
				ma_node_graph* graph = ma_engine_get_node_graph(engine);
				const ma_uint32 channels = ma_node_get_output_channels(Sound.get(), 0);
				const ma_uint32 sampleRate = ma_engine_get_sample_rate(engine);
				LowPass.Node = CreateNode<LowPassHandle>([&](LowPassNode* node) { return LowPassNodeInit(graph, channels, sampleRate, cutoff, node); }, fmt::format("a low-pass filter for '{}'", name), "the sound plays unfiltered");
				LowPass.Failed = !LowPass.Node;
				return !LowPass.Failed;
			}
			if (LowPass.Routed)
				LowPassNodeSetCutoff(LowPass.Node.get(), cutoff);
			else
				LowPassNodeRestart(LowPass.Node.get(), cutoff);
			return true;
		}

		// Creates the reverb send on first use and sets how much it sends; false when it does not exist.
		bool UpdateSend(ma_engine* engine, float send, std::string_view name)
		{
			if (Send.Failed)
				return false;
			if (!Send.Node)
			{
				ma_node_graph* graph = ma_engine_get_node_graph(engine);
				const ma_splitter_node_config config = ma_splitter_node_config_init(ma_node_get_output_channels(Sound.get(), 0));
				Send.Node = CreateNode<SplitterHandle>([&](ma_splitter_node* node) { return ma_splitter_node_init(graph, &config, nullptr, node); }, fmt::format("a reverb send for '{}'", name), "the sound plays dry");
				Send.Failed = !Send.Node;
				if (Send.Failed)
					return false;
			}
			// Only fails for an output bus the splitter does not have (it has two).
			ma_node_set_output_bus_volume(Send.Node.get(), 1, send);
			return true;
		}

		// Routes the sound through the combined effects stage, low-pass and reverb send. The chain is built from the bus
		// backwards, and only connections that change are made: re-attaching an output briefly unlinks it in
		// miniaudio, which could cost the voice a block. Attaching replaces an output's previous connection, so the
		// old path plays until the new one exists; stages that drop out are detached afterwards so the graph stops
		// pulling them (a dropped send lets the reverb's tail ring out). When miniaudio refuses a connection the
		// sound is wired straight to its output (logged) and plays without effects from then on.
		void Route(bool lowPass, ma_node* reverb, std::string_view name)
		{
			const bool send = reverb != nullptr;
			if (lowPass == LowPass.Routed && send == Send.Routed && Effects.Routed == bool(Effects.Node))
				return;
			ma_result result = MA_SUCCESS;
			const auto previousFilter = LowPass.Routed ? static_cast<ma_node*>(LowPass.Node.get()) : (Send.Routed ? static_cast<ma_node*>(Send.Node.get()) : Out.Node);
			const auto previousSound = Effects.Routed ? static_cast<ma_node*>(Effects.Node.get()) : previousFilter;
			auto attach = [&](ma_node* from, ma_uint32 output, ma_node* to, ma_node* was) { if (result == MA_SUCCESS && to != was) result = ma_node_attach_output_bus(from, output, to, 0); };
			ma_node* next = Out.Node;
			if (send)
			{
				attach(Send.Node.get(), 0, next, Send.Routed ? Out.Node : nullptr);
				attach(Send.Node.get(), 1, reverb, Send.Routed ? reverb : nullptr);
				next = Send.Node.get();
			}
			if (lowPass)
			{
				attach(LowPass.Node.get(), 0, next, LowPass.Routed ? (Send.Routed ? static_cast<ma_node*>(Send.Node.get()) : Out.Node) : nullptr);
				next = LowPass.Node.get();
			}
			if (Effects.Node)
			{
				attach(Effects.Node.get(), 0, next, Effects.Routed ? previousFilter : nullptr);
				next = Effects.Node.get();
			}
			attach(Sound.get(), 0, next, previousSound);
			if (result != MA_SUCCESS)
			{
				BS_CORE_ERROR("Audio: cannot route '{}' through effects ({}); playing dry", name, ma_result_description(result));
				if (const ma_result direct = ma_node_attach_output_bus(Sound.get(), 0, Out.Node, 0); direct != MA_SUCCESS)
					BS_CORE_ERROR("Audio: cannot reconnect '{}' to its bus ({})", name, ma_result_description(direct));
				Effects.Disable();
				LowPass.Disable();
				Send.Disable();
				return;
			}
			if (!send && Send.Node)
				ma_node_detach_all_output_buses(Send.Node.get());
			if (!lowPass && LowPass.Node)
				ma_node_detach_all_output_buses(LowPass.Node.get());
			LowPass.Routed = lowPass;
			Send.Routed = send;
			Effects.Routed = bool(Effects.Node);
		}
	};

	struct AudioSystem::Reverb
	{
		ReverbHandle Node;
	};

	void AudioSystem::SoundDeleter::operator()(ma_sound* sound) const
	{
		ma_sound_uninit(sound);
		delete sound;
	}

	AudioSystem::AudioSystem(Scene* scene)
		: m_Scene(scene)
	{
		ma_engine* engine = AudioEngine::GetEngine();
		if (!engine)
			return;

		// Master feeds the engine's endpoint; every other bus feeds Master. Buses only mix: they are neither
		// spatialized (a group has no position, so spatializing one would attenuate it by the listener's distance
		// from the origin; miniaudio already disables it for groups, the flag makes that explicit) nor pitched.
		constexpr ma_uint32 BusFlags = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH;
		for (size_t i = 0; i < AudioBusCount; i++)
		{
			ma_sound* parent = i == MasterIndex ? nullptr : m_Buses[MasterIndex].get();
			// A plain unique_ptr until init succeeds: SoundDeleter must only see initialized sounds.
			auto group = std::make_unique<ma_sound>();
			if (const ma_result result = ma_sound_group_init(engine, BusFlags, parent, group.get()); result != MA_SUCCESS)
			{
				// CreateSound routes the bus's sounds into Master instead (or the endpoint, without Master), and
				// SetBusVolume refuses the bus.
				BS_CORE_ERROR("Audio: cannot create the {} bus ({}); its sounds play through Master and ignore its volume", AudioBusNames[i], ma_result_description(result));
				continue;
			}
			m_Buses[i] = SoundHandle(group.release());
		}
	}

	// Members do the teardown in order: one-shots and sources, then the bus reverbs, then the buses, children before
	// Master.
	AudioSystem::~AudioSystem() = default;

	AudioSystem::Output AudioSystem::GetOutput(AudioBus bus) const
	{
		ma_engine* engine = AudioEngine::GetEngine();
		if (!engine)
			return {};
		const std::optional<size_t> index = BusIndex(bus);
		if (!index)
			return {};
		// A bus that failed to create falls back to Master, so Master's volume still applies.
		if (m_Buses[*index])
			return { m_Buses[*index].get(), index };
		if (m_Buses[MasterIndex])
			return { m_Buses[MasterIndex].get(), MasterIndex };
		return { ma_engine_get_endpoint(engine), std::nullopt };
	}

	AudioSystem::SoundHandle AudioSystem::CreateSound(const std::string& clip, bool spatial, const Output& output, bool stream)
	{
		ma_engine* engine = AudioEngine::GetEngine();
		if (!engine || !output.Node)
			return nullptr;
		if (clip.empty())
		{
			BS_CORE_ERROR("Audio: cannot play a sound without a clip");
			return nullptr;
		}

		const std::string path = Project::ResolvePath(clip).string();
		// A plain unique_ptr until init succeeds: SoundDeleter must only see initialized sounds.
		auto sound = std::make_unique<ma_sound>();
		// Streams decode a page at a time as they play (the resource manager keeps two one-second pages);
		// everything else is decoded whole on load and shared while any sound uses the same file.
		ma_uint32 flags = stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
		if (!spatial)
			flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
		// Initialized on the endpoint (a null group) and then attached to output, because output may be the endpoint,
		// which is not an ma_sound_group.
		if (const ma_result result = ma_sound_init_from_file(engine, path.c_str(), flags, nullptr, nullptr, sound.get()); result != MA_SUCCESS)
		{
			BS_CORE_ERROR("Audio: cannot load clip '{}' ({}, {}): {}", clip, path, stream ? "streamed" : "decoded", ma_result_description(result));
			return nullptr;
		}
		SoundHandle handle(sound.release());
		if (const ma_result result = ma_node_attach_output_bus(handle.get(), 0, output.Node, 0); result != MA_SUCCESS)
		{
			BS_CORE_ERROR("Audio: cannot connect clip '{}' to its bus: {}", clip, ma_result_description(result));
			return nullptr;
		}
		// miniaudio's default Doppler factor is 1; sources opt in through AudioSource.Doppler (ApplyEffects).
		ma_sound_set_doppler_factor(handle.get(), 0.0f);
		return handle;
	}

	void AudioSystem::Start()
	{
		BS_CORE_ASSERT(m_PendingStarts.empty(), "Start runs once, before BeginPlayback");
		auto view = m_Scene->GetAllEntitiesWith<AudioSourceComponent>();
		for (entt::entity handle : view)
		{
			if (view.get<AudioSourceComponent>(handle).PlayOnStart)
				PlaySource({ handle, m_Scene }, StartMode::OnBeginPlayback);
		}
		Update(0.0f);
	}

	void AudioSystem::BeginPlayback()
	{
		for (UUID uuid : m_PendingStarts)
		{
			if (auto it = m_Sources.find(uuid); it != m_Sources.end())
			{
				ma_sound_start(it->second->Sound.get());
				// The scripts' OnCreate may have just moved the entity (to a spawn point): measuring from where Start
				// found it would read that as a teleport on the first frame.
				it->second->LastPosition.reset();
			}
		}
		m_PendingStarts.clear();
		m_Listener.LastPosition.reset();
	}

	void AudioSystem::PlayOnStart(const std::vector<Entity>& entities)
	{
		for (Entity entity : entities)
		{
			const auto* source = entity ? entity.TryGetComponent<AudioSourceComponent>() : nullptr;
			if (source && source->PlayOnStart)
				PlaySource(entity, StartMode::OnBeginPlayback);
		}
		// Positions the new sources and the listener right away, as Start does.
		Update(0.0f);
	}

	bool AudioSystem::Play(Entity entity)
	{
		return PlaySource(entity, StartMode::Now);
	}

	bool AudioSystem::PlaySource(Entity entity, StartMode mode)
	{
		const auto* source = entity.TryGetComponent<AudioSourceComponent>();
		if (!source)
			return false;

		Stop(entity);
		// The registry rejects such values; C++ code can still write them directly.
		if (const std::string problem = ValidateAudioSource(*source); !problem.empty())
		{
			BS_CORE_ERROR("Audio: cannot play the AudioSource of '{}': {}", entity.GetName(), problem);
			return false;
		}
		// A NaN position would reach the bus and its reverb (whose feedback keeps it until play stops).
		const glm::vec3 position = glm::vec3(m_Scene->GetWorldTransform(entity)[3]);
		if (source->Spatial && !IsUsablePosition(position))
		{
			BS_CORE_ERROR("Audio: cannot play the AudioSource of '{}': its world position is not finite or is beyond {} units", entity.GetName(), MaxAudioCoordinate);
			return false;
		}
		const Output output = GetOutput(source->Bus);
		SoundHandle sound = CreateSound(source->Clip, source->Spatial, output, source->Stream);
		if (!sound)
			return false;

		ma_sound_set_volume(sound.get(), source->Volume);
		ma_sound_set_pitch(sound.get(), source->Pitch);
		ma_sound_set_looping(sound.get(), source->Loop ? MA_TRUE : MA_FALSE);
		if (source->Spatial)
		{
			ma_sound_set_min_distance(sound.get(), source->MinDistance);
			ma_sound_set_max_distance(sound.get(), source->MaxDistance);
		}
		auto voice = std::make_unique<Voice>(std::move(sound), output);
		if (source->Spatial)
			voice->SetPosition(position);
		ApplyEffects(*voice, entity, *source);
		const UUID uuid = entity.GetUUID();
		if (mode == StartMode::Now)
			ma_sound_start(voice->Sound.get());
		else
			m_PendingStarts.insert(uuid);
		m_Sources[uuid] = std::move(voice);
		return true;
	}

	void AudioSystem::Stop(Entity entity)
	{
		const UUID uuid = entity.GetUUID();
		m_PendingStarts.erase(uuid);
		m_Sources.erase(uuid);
	}

	bool AudioSystem::IsPlaying(Entity entity) const
	{
		const UUID uuid = entity.GetUUID();
		auto it = m_Sources.find(uuid);
		if (it == m_Sources.end())
			return false;
		// A pending source (loaded, BeginPlayback not run yet) already counts as playing.
		return m_PendingStarts.contains(uuid) || ma_sound_is_playing(it->second->Sound.get());
	}

	bool AudioSystem::IsStreaming(Entity entity) const
	{
		auto it = m_Sources.find(entity.GetUUID());
		if (it == m_Sources.end() || !it->second->Sound->pResourceManagerDataSource)
			return false;
		return (it->second->Sound->pResourceManagerDataSource->flags & MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM) != 0;
	}

	float AudioSystem::GetDopplerPitch(Entity entity) const
	{
		auto it = m_Sources.find(entity.GetUUID());
		if (it == m_Sources.end())
			return 1.0f;
		const Voice& voice = *it->second;
		ma_sound* sound = voice.Sound.get();
		// Nothing is shifted while nothing plays (a clip that played to its end included).
		if (ma_sound_is_spatialization_enabled(sound) == MA_FALSE || ma_sound_is_playing(sound) == MA_FALSE)
			return 1.0f;
		return DopplerPitch(m_ListenerPosition, voice.Position, voice.Velocity, m_ListenerVelocity, voice.DopplerFactor);
	}

	bool AudioSystem::PlayOneShot(const std::string& clip, const glm::vec3* position, float volume, AudioBus bus, const AudioOneShotOptions& options)
	{
		AudioSourceComponent source;
		source.LowPass = options.LowPass;
		source.ReverbSend = options.ReverbSend;
		source.HighPass = options.HighPass;
		source.Echo = options.Echo;
		source.Distortion = options.Distortion;
		if (const auto problem = ValidateAudioEffects(source); !problem.empty())
		{
			BS_CORE_ERROR("Audio: cannot play one-shot '{}': {}", clip, problem);
			return false;
		}
		if (!IsValidAudioVolume(volume))
		{
			BS_CORE_ERROR("Audio: cannot play '{}' at volume {}: it must be a finite number >= 0", clip, volume);
			return false;
		}
		// Never updated after this, so a NaN would sound for the one-shot's whole life.
		if (position && !IsUsablePosition(*position))
		{
			BS_CORE_ERROR("Audio: cannot play '{}' at a position that is not finite or is beyond {} units", clip, MaxAudioCoordinate);
			return false;
		}
		SoundHandle sound = CreateSound(clip, position != nullptr, GetOutput(bus), false);
		if (!sound)
			return false;
		ma_sound_set_volume(sound.get(), volume);
		if (position)
			ma_sound_set_position(sound.get(), position->x, position->y, position->z);
		auto voice = std::make_unique<Voice>(std::move(sound), GetOutput(bus));
		ConfigureEffects(*voice, source, clip);
		if (const auto result = ma_sound_start(voice->Sound.get()); result != MA_SUCCESS)
		{
			BS_CORE_ERROR("Audio: cannot start one-shot '{}' ({})", clip, ma_result_description(result));
			return false;
		}
		m_OneShots.push_back(std::move(voice));
		return true;
	}

	void AudioSystem::OnEntityDestroyed(Entity entity)
	{
		Stop(entity);
	}

	void AudioSystem::ApplyEffects(Voice& voice, Entity entity, const AudioSourceComponent& source)
	{
		// Out-of-range values turn their effect off; say so once per problem (a different one is reported too), until
		// they are valid again.
		if (std::string problem = ValidateAudioEffects(source); problem != voice.ReportedEffectsProblem)
		{
			if (!problem.empty())
				BS_CORE_ERROR("Audio: an effect of the AudioSource of '{}' is off: {}", entity.GetName(), problem);
			voice.ReportedEffectsProblem = std::move(problem);
		}
		ConfigureEffects(voice, source, entity.GetName());
	}

	void AudioSystem::ConfigureEffects(Voice& voice, const AudioSourceComponent& source, std::string_view name)
	{
		ma_engine* engine = AudioEngine::GetEngine();
		const EffectValues effects = SanitizeEffects(source, static_cast<float>(ma_engine_get_sample_rate(engine)));
		voice.SetDopplerFactor(effects.Doppler);

		const float highPass = effects.HighPass;
		const float echo = effects.Echo;
		const float distortion = effects.Distortion;
		if (!voice.Effects.Node && !voice.Effects.Failed && (highPass > 0.0f || echo > 0.0f || distortion > 0.0f))
		{
			voice.Effects.Node = CreateNode<SourceEffectsHandle>([&](SourceEffectsNode* node) { return SourceEffectsNodeInit(ma_engine_get_node_graph(engine), ma_node_get_output_channels(voice.Sound.get(), 0), ma_engine_get_sample_rate(engine), node); }, fmt::format("source effects for '{}'", name), "the sound plays dry");
			voice.Effects.Failed = !voice.Effects.Node;
		}
		if (voice.Effects.Node)
			SourceEffectsNodeSet(voice.Effects.Node.get(), highPass, echo, distortion);
		voice.EchoTail = echo > 0.0f && voice.Effects.Node;
		const bool lowPass = effects.LowPass > 0.0f && voice.UpdateLowPass(engine, effects.LowPass, name);
		// A failed send never creates its bus's reverb.
		Reverb* reverb = effects.ReverbSend > 0.0f && !voice.Send.Failed && voice.Out.Bus ? GetReverbNode(*voice.Out.Bus) : nullptr;
		if (reverb && !voice.UpdateSend(engine, effects.ReverbSend, name))
			reverb = nullptr;
		voice.Route(lowPass, reverb ? reverb->Node.get() : nullptr, name);
	}

	AudioSystem::Reverb* AudioSystem::GetReverbNode(size_t bus)
	{
		if (m_Reverbs[bus])
			return m_Reverbs[bus].get();
		ma_engine* engine = AudioEngine::GetEngine();
		if (m_ReverbFailed[bus] || !engine || !m_Buses[bus])
			return nullptr;

		// The reverb feeds the bus, so it has the bus's (the engine's) channel count. verblib handles mono and stereo
		// at 22.05 to 176.4 kHz only: elsewhere the sends are dropped and the sounds still play dry.
		const ma_uint32 channels = ma_engine_get_channels(engine);
		const ma_uint32 sampleRate = ma_engine_get_sample_rate(engine);
		ma_node_graph* graph = ma_engine_get_node_graph(engine);
		const std::string what = fmt::format("the {} bus reverb for {} channels at {} Hz", AudioBusNames[bus], channels, sampleRate);
		ReverbHandle node = CreateNode<ReverbHandle>([&](ReverbNode* reverb) { return ReverbNodeInit(graph, channels, sampleRate, m_Reverb, reverb); }, what, "its sounds play without reverb");
		if (!node)
		{
			m_ReverbFailed[bus] = true;
			return nullptr;
		}
		if (const ma_result result = ma_node_attach_output_bus(node.get(), 0, m_Buses[bus].get(), 0); result != MA_SUCCESS)
		{
			m_ReverbFailed[bus] = true;
			BS_CORE_ERROR("Audio: cannot connect {} ({}); its sounds play without reverb", what, ma_result_description(result));
			return nullptr;
		}
		m_Reverbs[bus] = std::make_unique<Reverb>(Reverb{ std::move(node) });
		return m_Reverbs[bus].get();
	}

	void AudioSystem::UpdateReverbZones()
	{
		struct ZoneBlend
		{
			const AudioReverbZoneComponent* Zone;
			uint64_t ID;
			float Weight;
		};
		std::vector<ZoneBlend> blends;
		std::unordered_set<UUID> seen;
		auto zones = m_Scene->GetAllEntitiesWith<AudioReverbZoneComponent, IDComponent>();
		for (auto handle : zones)
		{
			const auto& zone = zones.get<AudioReverbZoneComponent>(handle);
			const UUID id = zones.get<IDComponent>(handle).ID;
			if (!zone.Enabled)
				continue;
			seen.insert(id);
			const glm::vec3 center(m_Scene->GetWorldTransform(Entity(handle, m_Scene))[3]);
			std::string problem = ValidateAudioReverbZone(zone);
			if (problem.empty() && !IsUsablePosition(center))
				problem = "world position is not finite or exceeds the audio coordinate limit";
			if (!problem.empty())
			{
				if (m_ReportedZoneProblems[id] != problem)
					BS_CORE_ERROR("Audio: the AudioReverbZone of '{}' is ignored: {}", Entity(handle, m_Scene).GetName(), problem);
				m_ReportedZoneProblems[id] = std::move(problem);
				continue;
			}
			m_ReportedZoneProblems.erase(id);
			const float distance = glm::length(center - m_ListenerPosition);
			if (distance >= zone.Radius)
				continue;
			const float weight = zone.BlendDistance > 0.0f ? std::clamp((zone.Radius - distance) / zone.BlendDistance, 0.0f, 1.0f) : 1.0f;
			blends.push_back({ &zone, static_cast<uint64_t>(id), weight });
		}
		std::erase_if(m_ReportedZoneProblems, [&](const auto& entry) { return !seen.contains(entry.first); });
		// Higher priorities fade over the room below them, so entering their outer edge cannot jump to the baseline.
		std::sort(blends.begin(), blends.end(), [](const ZoneBlend& a, const ZoneBlend& b) { return a.Zone->Priority != b.Zone->Priority ? a.Zone->Priority < b.Zone->Priority : a.ID > b.ID; });
		AudioReverbSettings result = m_BaseReverb;
		for (const auto& blend : blends)
		{
			result.RoomSize = glm::mix(result.RoomSize, blend.Zone->RoomSize, blend.Weight);
			result.Damping = glm::mix(result.Damping, blend.Zone->Damping, blend.Weight);
			result.Width = glm::mix(result.Width, blend.Zone->Width, blend.Weight);
		}
		if (result != m_Reverb)
			ApplyReverb(result);
	}

	void AudioSystem::PlaceListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up, const glm::vec3& velocity)
	{
		AudioEngine::SetListener(position, forward, up, velocity);
		m_ListenerPosition = position;
		m_ListenerForward = forward;
		m_ListenerUp = up;
		m_ListenerVelocity = velocity;
	}

	void AudioSystem::Update(Timestep ts)
	{
		const float seconds = ts.GetSeconds();
		const PhysicsWorld* physics = m_Scene->GetPhysicsWorld();

		// Listener: explicit listener component first, primary camera otherwise.
		Entity listener;
		auto listeners = m_Scene->GetAllEntitiesWith<AudioListenerComponent>();
		for (entt::entity handle : listeners)
		{
			if (listeners.get<AudioListenerComponent>(handle).Active)
			{
				listener = { handle, m_Scene };
				break;
			}
		}
		if (!listener)
			listener = m_Scene->GetPrimaryCameraEntity();
		if (listener)
		{
			// A new listener starts without movement history.
			if (listener.GetUUID() != m_Listener.ID)
				m_Listener = { listener.GetUUID() };
			const glm::mat4 transform = m_Scene->GetWorldTransform(listener);
			const glm::vec3 position(transform[3]);
			const glm::vec3 forward = -glm::normalize(glm::vec3(transform[2]));
			const glm::vec3 up = glm::normalize(glm::vec3(transform[1]));
			// A NaN would reach every spatial sound, and through them the reverbs, whose feedback would keep it
			// until play stops. Such a listener (a broken transform, a zero scale) keeps its last placement, and its
			// movement is measured afresh once it is finite again.
			const bool finite = IsUsablePosition(position) && IsFinite(forward) && IsFinite(up);
			if (ShouldReport(m_Listener.ReportedInvalidTransform, !finite))
				BS_CORE_ERROR("Audio: the listener '{}' has a transform that is not finite (or is beyond {} units); it keeps its last placement", listener.GetName(), MaxAudioCoordinate);
			if (finite)
			{
				if (const std::optional<glm::vec3> velocity = TrackVelocity(physics, listener, position, m_Listener.LastPosition, seconds, m_Listener.ReportedInvalidVelocity))
					m_Listener.Velocity = *velocity;
				PlaceListener(position, forward, up, m_Listener.Velocity);
			}
			else
			{
				m_Listener.LastPosition.reset();
			}
		}
		else
		{
			// Whichever listener comes next (also this one again) starts without movement history.
			m_Listener = {};
		}
		// Without a valid listener this frame, the last placement stands still: a velocity left behind would keep
		// shifting every Doppler source.
		if (!listener || m_Listener.ReportedInvalidTransform)
			PlaceListener(m_ListenerPosition, m_ListenerForward, m_ListenerUp, glm::vec3(0.0f));

		UpdateReverbZones();

		for (auto& [uuid, voice] : m_Sources)
		{
			Entity entity = m_Scene->GetEntityByUUID(uuid);
			if (!entity)
				continue;
			if (const auto* source = entity.TryGetComponent<AudioSourceComponent>())
				ApplyEffects(*voice, entity, *source);
			ma_sound* sound = voice->Sound.get();
			if (ma_sound_is_spatialization_enabled(sound) == MA_FALSE)
				continue;
			const glm::vec3 position = glm::vec3(m_Scene->GetWorldTransform(entity)[3]);
			const bool finite = IsUsablePosition(position);
			if (ShouldReport(voice->ReportedInvalidPosition, !finite))
				BS_CORE_ERROR("Audio: '{}' has a world position that is not finite (or is beyond {} units); its sound keeps its last position, unmoving", entity.GetName(), MaxAudioCoordinate);
			if (!finite)
			{
				// Standing still, and measured afresh once it is finite again (no teleport across the gap).
				voice->SetVelocity(glm::vec3(0.0f));
				voice->LastPosition.reset();
				continue;
			}
			voice->SetPosition(position);
			if (const std::optional<glm::vec3> velocity = TrackVelocity(physics, entity, position, voice->LastPosition, seconds, voice->ReportedInvalidVelocity))
				voice->SetVelocity(*velocity);
		}

		AudioEngine::AdvanceDeviceless(seconds);

		// Echo one-shots retain a three-second tail: 12 repeats at 0.35 feedback fall below -100 dB.
		// Sources stay registered (and report !IsPlaying) until stopped.
		std::erase_if(m_OneShots, [seconds](const std::unique_ptr<Voice>& voice) {
			if (!ma_sound_at_end(voice->Sound.get()))
				return false;
			if (std::isfinite(seconds) && seconds > 0.0f)
				voice->TailTime += seconds;
			return !voice->EchoTail || voice->TailTime >= 3.0f;
		});
	}

	bool AudioSystem::SetBusVolume(AudioBus bus, float volume)
	{
		const std::optional<size_t> index = BusIndex(bus);
		if (!index)
			return false;
		if (!IsValidAudioVolume(volume))
		{
			BS_CORE_ERROR("Audio: the {} bus volume must be a finite number >= 0 (got {})", AudioBusNames[*index], volume);
			return false;
		}
		// Without an engine there is nothing to scale and the volume is only kept; with one, a bus that failed to
		// create cannot apply it.
		if (AudioEngine::GetEngine() && !m_Buses[*index])
		{
			BS_CORE_ERROR("Audio: the {} bus failed to create; its volume cannot change", AudioBusNames[*index]);
			return false;
		}
		m_BusVolumes[*index] = volume;
		if (m_Buses[*index])
			ma_sound_group_set_volume(m_Buses[*index].get(), volume);
		return true;
	}

	bool AudioSystem::SetReverb(const AudioReverbSettings& reverb)
	{
		if (const std::string problem = ValidateAudioReverb(reverb); !problem.empty())
		{
			BS_CORE_ERROR("Audio: cannot set the reverb: {}", problem);
			return false;
		}
		m_BaseReverb = reverb;
		UpdateReverbZones();
		return true;
	}

	void AudioSystem::ApplyReverb(const AudioReverbSettings& reverb)
	{
		m_Reverb = reverb;
		for (const std::unique_ptr<Reverb>& node : m_Reverbs)
		{
			if (node)
				ReverbNodeSetSettings(node->Node.get(), m_Reverb);
		}
	}

	bool AudioSystem::HasReverb(AudioBus bus) const
	{
		const std::optional<size_t> index = BusIndex(bus);
		return index && m_Reverbs[*index] != nullptr;
	}

	float AudioSystem::GetBusVolume(AudioBus bus) const
	{
		const std::optional<size_t> index = BusIndex(bus);
		return index ? m_BusVolumes[*index] : 0.0f;
	}

	uint32_t AudioSystem::GetActiveSoundCount() const
	{
		return static_cast<uint32_t>(m_Sources.size() + m_OneShots.size());
	}

}
