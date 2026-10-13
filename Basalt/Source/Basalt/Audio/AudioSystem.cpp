#include "Basalt/Audio/AudioSystem.h"

#include "Basalt/Audio/AudioEngine.h"
#include "Basalt/Audio/AudioRules.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <miniaudio.h>

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

	}

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

	AudioSystem::SoundHandle AudioSystem::CreateSound(const std::string& clip, bool spatial, AudioBus bus, bool stream)
	{
		ma_engine* engine = AudioEngine::GetEngine();
		if (!engine)
			return nullptr;
		if (clip.empty())
		{
			BS_CORE_ERROR("Audio: cannot play a sound without a clip");
			return nullptr;
		}
		const std::optional<size_t> busIndex = BusIndex(bus);
		if (!busIndex)
			return nullptr;

		const std::string path = Project::ResolvePath(clip).string();
		// A plain unique_ptr until init succeeds: SoundDeleter must only see initialized sounds.
		auto sound = std::make_unique<ma_sound>();
		// Streams decode a page at a time as they play (the resource manager keeps two one-second pages);
		// everything else is decoded whole on load and shared while any sound uses the same file.
		ma_uint32 flags = stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
		if (!spatial)
			flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
		// A bus that failed to create falls back to Master, so Master's volume still applies.
		const SoundHandle& busGroup = m_Buses[*busIndex];
		ma_sound* group = busGroup ? busGroup.get() : m_Buses[MasterIndex].get();
		if (const ma_result result = ma_sound_init_from_file(engine, path.c_str(), flags, group, nullptr, sound.get()); result != MA_SUCCESS)
		{
			BS_CORE_ERROR("Audio: cannot load clip '{}' ({}, {}): {}", clip, path, stream ? "streamed" : "decoded", ma_result_description(result));
			return nullptr;
		}
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
				ma_sound_start(it->second.get());
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
		SoundHandle sound = CreateSound(source->Clip, source->Spatial, source->Bus, source->Stream);
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
		const UUID uuid = entity.GetUUID();
		if (mode == StartMode::Now)
			ma_sound_start(sound.get());
		else
			m_PendingStarts.insert(uuid);
		m_Sources[uuid] = std::move(sound);
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
		return m_PendingStarts.contains(uuid) || ma_sound_is_playing(it->second.get());
	}

	bool AudioSystem::IsStreaming(Entity entity) const
	{
		auto it = m_Sources.find(entity.GetUUID());
		if (it == m_Sources.end() || !it->second->pResourceManagerDataSource)
			return false;
		return (it->second->pResourceManagerDataSource->flags & MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM) != 0;
	}

	bool AudioSystem::PlayOneShot(const std::string& clip, const glm::vec3* position, float volume, AudioBus bus)
	{
		if (!IsValidAudioVolume(volume))
		{
			BS_CORE_ERROR("Audio: cannot play '{}' at volume {}: it must be a finite number >= 0", clip, volume);
			return false;
		}
		SoundHandle sound = CreateSound(clip, position != nullptr, bus, false);
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

	void AudioSystem::Update(Timestep ts)
	{
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
			const glm::mat4 transform = m_Scene->GetWorldTransform(listener);
			AudioEngine::SetListener(glm::vec3(transform[3]), -glm::normalize(glm::vec3(transform[2])), glm::normalize(glm::vec3(transform[1])));
		}

		for (auto& [uuid, sound] : m_Sources)
		{
			Entity entity = m_Scene->GetEntityByUUID(uuid);
			if (!entity || ma_sound_is_spatialization_enabled(sound.get()) == MA_FALSE)
				continue;
			const glm::vec3 position = glm::vec3(m_Scene->GetWorldTransform(entity)[3]);
			ma_sound_set_position(sound.get(), position.x, position.y, position.z);
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
