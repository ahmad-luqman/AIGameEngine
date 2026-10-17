#include "Basalt/Audio/AudioSystem.h"

#include "Basalt/Audio/AudioEngine.h"
#include "Basalt/Audio/AudioRules.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <miniaudio.h>

#include <extras/nodes/ma_reverb_node/ma_reverb_node.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
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

		// Second order (12 dB per octave): steep enough to muffle, gentle enough to keep the sound recognizable.
		constexpr ma_uint32 LowPassOrder = 2;

		// The live effect values of a source, made safe: the registry rejects values outside the AudioRules ranges,
		// but C++ code can write them directly and Update applies them every frame, so out-of-range values turn
		// the effect off quietly instead of logging every frame.
		struct EffectValues
		{
			float Doppler = 0.0f;
			float LowPass = 0.0f;
			float ReverbSend = 0.0f;
		};

		EffectValues SanitizeEffects(const AudioSourceComponent& source, float sampleRate)
		{
			auto inRange = [](float value, float min, float max) { return std::isfinite(value) && value >= min && value <= max; };
			EffectValues values;
			if (inRange(source.Doppler, 0.0f, MaxAudioDoppler))
				values.Doppler = source.Doppler;
			// The filter is only defined below the Nyquist frequency; keep a margin under it.
			if (inRange(source.LowPass, MinAudioLowPass, MaxAudioLowPass))
				values.LowPass = std::min(source.LowPass, 0.45f * sampleRate);
			if (inRange(source.ReverbSend, 0.0f, 1.0f))
				values.ReverbSend = source.ReverbSend;
			return values;
		}

		void ApplyReverbSettings(ma_reverb_node& node, const AudioReverbSettings& settings)
		{
			verblib_set_room_size(&node.reverb, settings.RoomSize);
			verblib_set_damping(&node.reverb, settings.Damping);
			verblib_set_width(&node.reverb, settings.Width);
		}

	}

	struct AudioSystem::Voice
	{
		using LowPassHandle = std::unique_ptr<ma_lpf_node, NodeDeleter<ma_lpf_node, ma_lpf_node_uninit>>;
		using SplitterHandle = std::unique_ptr<ma_splitter_node, NodeDeleter<ma_splitter_node, ma_splitter_node_uninit>>;

		// Declared before the sound so the sound (upstream) is destroyed first (do not reorder).
		SplitterHandle Send;
		LowPassHandle LowPass;
		SoundHandle Sound;
		Output Out;
		// The cutoff the low-pass node was last built with, and which stages the chain currently routes through.
		float LowPassCutoff = 0.0f;
		bool LowPassRouted = false;
		bool SendRouted = false;
		// A stage that failed to create is not retried (that would log every frame); the sound plays without it.
		bool LowPassFailed = false;
		bool SendFailed = false;
		Motion Movement;
	};

	struct AudioSystem::Reverb
	{
		ma_reverb_node Node;
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

	// Members do the teardown in order: one-shots and sources, then the buses, children before Master.
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
		// Attached to the endpoint first (a null group), then to the output: the endpoint is a valid target even when
		// output is a bus group.
		if (const ma_result result = ma_sound_init_from_file(engine, path.c_str(), flags, nullptr, nullptr, sound.get()); result != MA_SUCCESS)
		{
			BS_CORE_ERROR("Audio: cannot load clip '{}' ({}, {}): {}", clip, path, stream ? "streamed" : "decoded", ma_result_description(result));
			return nullptr;
		}
		ma_node_attach_output_bus(sound.get(), 0, output.Node, 0);
		// miniaudio's default Doppler factor is 1; sources opt in through AudioSource.Doppler (ApplyEffects).
		ma_sound_set_doppler_factor(sound.get(), 0.0f);
		return SoundHandle(sound.release());
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
				ma_sound_start(it->second->Sound.get());
		}
		m_PendingStarts.clear();
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
			const glm::vec3 position = glm::vec3(m_Scene->GetWorldTransform(entity)[3]);
			ma_sound_set_position(sound.get(), position.x, position.y, position.z);
		}
		auto voice = std::make_unique<Voice>();
		voice->Sound = std::move(sound);
		voice->Out = output;
		ApplyEffects(*voice, *source);
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
		if (it == m_Sources.end() || ma_sound_is_spatialization_enabled(it->second->Sound.get()) == MA_FALSE)
			return 1.0f;
		return it->second->Sound->engineNode.spatializer.dopplerPitch;
	}

	bool AudioSystem::PlayOneShot(const std::string& clip, const glm::vec3* position, float volume, AudioBus bus)
	{
		if (!IsValidAudioVolume(volume))
		{
			BS_CORE_ERROR("Audio: cannot play '{}' at volume {}: it must be a finite number >= 0", clip, volume);
			return false;
		}
		SoundHandle sound = CreateSound(clip, position != nullptr, GetOutput(bus), false);
		if (!sound)
			return false;
		ma_sound_set_volume(sound.get(), volume);
		if (position)
			ma_sound_set_position(sound.get(), position->x, position->y, position->z);
		ma_sound_start(sound.get());
		m_OneShots.push_back(std::move(sound));
		return true;
	}

	void AudioSystem::OnEntityDestroyed(Entity entity)
	{
		Stop(entity);
	}

	void AudioSystem::ApplyEffects(Voice& voice, const AudioSourceComponent& source)
	{
		ma_engine* engine = AudioEngine::GetEngine();
		ma_sound* sound = voice.Sound.get();
		const auto sampleRate = static_cast<float>(ma_engine_get_sample_rate(engine));
		const EffectValues effects = SanitizeEffects(source, sampleRate);

		ma_sound_set_doppler_factor(sound, effects.Doppler);

		const bool lowPass = effects.LowPass > 0.0f && !voice.LowPassFailed;
		if (lowPass && (!voice.LowPass || effects.LowPass != voice.LowPassCutoff))
		{
			const ma_uint32 channels = ma_node_get_output_channels(sound, 0);
			const ma_lpf_node_config config = ma_lpf_node_config_init(channels, ma_engine_get_sample_rate(engine), effects.LowPass, LowPassOrder);
			if (!voice.LowPass)
			{
				auto node = std::make_unique<ma_lpf_node>();
				if (const ma_result result = ma_lpf_node_init(ma_engine_get_node_graph(engine), &config, nullptr, node.get()); result != MA_SUCCESS)
				{
					BS_CORE_ERROR("Audio: cannot create a low-pass filter ({}); the sound plays unfiltered", ma_result_description(result));
					voice.LowPassFailed = true;
				}
				else
				{
					voice.LowPass.reset(node.release());
				}
			}
			// Same channels and order: miniaudio keeps the filter's state, so the cutoff glides without a click.
			// With a device the audio thread may be mid-block; it then filters that block with a mix of the two
			// coefficient sets, which is inaudible.
			else if (ma_lpf_node_reinit(&config.lpf, voice.LowPass.get()) != MA_SUCCESS)
			{
				BS_CORE_ERROR("Audio: cannot change a low-pass cutoff to {} Hz", effects.LowPass);
			}
			voice.LowPassCutoff = effects.LowPass;
		}

		Reverb* reverb = effects.ReverbSend > 0.0f && !voice.SendFailed && voice.Out.Bus ? GetReverbNode(*voice.Out.Bus) : nullptr;
		if (reverb && !voice.Send)
		{
			const ma_uint32 channels = ma_node_get_output_channels(sound, 0);
			const ma_splitter_node_config config = ma_splitter_node_config_init(channels);
			auto node = std::make_unique<ma_splitter_node>();
			if (const ma_result result = ma_splitter_node_init(ma_engine_get_node_graph(engine), &config, nullptr, node.get()); result != MA_SUCCESS)
			{
				BS_CORE_ERROR("Audio: cannot create a reverb send ({}); the sound plays dry", ma_result_description(result));
				voice.SendFailed = true;
			}
			else
			{
				voice.Send.reset(node.release());
			}
		}
		if (!voice.Send)
			reverb = nullptr;
		if (reverb)
			ma_node_set_output_bus_volume(voice.Send.get(), 1, effects.ReverbSend);

		RouteVoice(voice, lowPass && voice.LowPass, reverb);
	}

	void AudioSystem::RouteVoice(Voice& voice, bool lowPass, Reverb* reverb)
	{
		const bool send = reverb != nullptr;
		if (lowPass == voice.LowPassRouted && send == voice.SendRouted)
			return;
		// Built from the bus backwards. Attaching an output replaces its previous connection; stages that drop out
		// are detached so the graph stops pulling them (a dropped send lets the reverb's tail ring out).
		ma_node* next = voice.Out.Node;
		if (send)
		{
			ma_node_attach_output_bus(voice.Send.get(), 0, next, 0);
			ma_node_attach_output_bus(voice.Send.get(), 1, &reverb->Node, 0);
			next = voice.Send.get();
		}
		else if (voice.Send)
		{
			ma_node_detach_all_output_buses(voice.Send.get());
		}
		if (lowPass)
		{
			ma_node_attach_output_bus(voice.LowPass.get(), 0, next, 0);
			next = voice.LowPass.get();
		}
		else if (voice.LowPass)
		{
			ma_node_detach_all_output_buses(voice.LowPass.get());
		}
		ma_node_attach_output_bus(voice.Sound.get(), 0, next, 0);
		voice.LowPassRouted = lowPass;
		voice.SendRouted = send;
	}

	AudioSystem::Reverb* AudioSystem::GetReverbNode(size_t bus)
	{
		if (m_Reverbs[bus])
			return m_Reverbs[bus].get();
		ma_engine* engine = AudioEngine::GetEngine();
		if (m_ReverbFailed[bus] || !engine || !m_Buses[bus])
			return nullptr;

		// The reverb feeds the bus, so it has the bus's (the engine's) channel count. verblib handles mono and stereo
		// only: on a surround device the sends are dropped and the sounds still play dry.
		const ma_uint32 channels = ma_engine_get_channels(engine);
		const ma_reverb_node_config config = ma_reverb_node_config_init(channels, ma_engine_get_sample_rate(engine));
		auto reverb = std::make_unique<Reverb>();
		if (const ma_result result = ma_reverb_node_init(ma_engine_get_node_graph(engine), &config, nullptr, &reverb->Node); result != MA_SUCCESS)
		{
			m_ReverbFailed[bus] = true;
			BS_CORE_ERROR("Audio: cannot create the {} bus reverb for {} channels ({}); its sounds play without reverb", AudioBusNames[bus], channels, ma_result_description(result));
			return nullptr;
		}
		// The node starts with verblib's defaults: wet only (the dry sound reaches the bus directly) at unity gain.
		ApplyReverbSettings(reverb->Node, m_Reverb);
		ma_node_attach_output_bus(&reverb->Node, 0, m_Buses[bus].get(), 0);
		m_Reverbs[bus] = std::move(reverb);
		return m_Reverbs[bus].get();
	}

	glm::vec3 AudioSystem::TrackVelocity(Entity entity, const glm::vec3& position, Motion& motion, float seconds) const
	{
		glm::vec3 velocity(0.0f);
		bool fromBody = false;
		// A body knows its velocity exactly. A transform delta would jitter whenever frames and fixed physics steps
		// do not line up (some frames see no step, others two), which a Doppler shift turns into a warble.
		if (const PhysicsWorld* physics = m_Scene->GetPhysicsWorld())
		{
			for (Entity e = entity; e; e = e.GetParent())
			{
				if (physics->HasBody(e) || physics->HasCharacter(e))
				{
					velocity = physics->GetLinearVelocity(e);
					fromBody = true;
					break;
				}
			}
		}
		if (!fromBody && motion.HasLastPosition && seconds > 0.0f)
			velocity = (position - motion.LastPosition) / seconds;
		if (seconds > 0.0f || !motion.HasLastPosition)
		{
			motion.LastPosition = position;
			motion.HasLastPosition = true;
		}

		const float speed = glm::length(velocity);
		if (!std::isfinite(speed))
			return glm::vec3(0.0f);
		return speed > MaxDopplerSpeed ? velocity * (MaxDopplerSpeed / speed) : velocity;
	}

	void AudioSystem::Update(Timestep ts)
	{
		const float seconds = ts.GetSeconds();

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
			if (listener.GetUUID() != m_ListenerID)
			{
				m_ListenerID = listener.GetUUID();
				m_ListenerMotion = {};
				m_ListenerVelocity = glm::vec3(0.0f);
			}
			const glm::mat4 transform = m_Scene->GetWorldTransform(listener);
			const glm::vec3 position(transform[3]);
			// The immediate updates (ts 0) of Start and PlayOnStart have no movement to measure; the velocity of the
			// last real frame stands.
			const glm::vec3 velocity = TrackVelocity(listener, position, m_ListenerMotion, seconds);
			if (seconds > 0.0f)
				m_ListenerVelocity = velocity;
			AudioEngine::SetListener(position, -glm::normalize(glm::vec3(transform[2])), glm::normalize(glm::vec3(transform[1])), m_ListenerVelocity);
		}

		for (auto& [uuid, voice] : m_Sources)
		{
			Entity entity = m_Scene->GetEntityByUUID(uuid);
			if (!entity)
				continue;
			if (const auto* source = entity.TryGetComponent<AudioSourceComponent>())
				ApplyEffects(*voice, *source);
			ma_sound* sound = voice->Sound.get();
			if (ma_sound_is_spatialization_enabled(sound) == MA_FALSE)
				continue;
			const glm::vec3 position = glm::vec3(m_Scene->GetWorldTransform(entity)[3]);
			ma_sound_set_position(sound, position.x, position.y, position.z);
			const glm::vec3 velocity = TrackVelocity(entity, position, voice->Movement, seconds);
			if (seconds > 0.0f)
				ma_sound_set_velocity(sound, velocity.x, velocity.y, velocity.z);
		}

		AudioEngine::AdvanceDeviceless(ts.GetSeconds());

		// Release finished one-shots. Sources stay registered (and report !IsPlaying) until stopped.
		std::erase_if(m_OneShots, [](const SoundHandle& sound) { return ma_sound_at_end(sound.get()); });
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
		m_Reverb = reverb;
		for (const std::unique_ptr<Reverb>& node : m_Reverbs)
		{
			if (node)
				ApplyReverbSettings(node->Node, m_Reverb);
		}
		return true;
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
