#include "Basalt/Audio/AudioSystem.h"

#include "Basalt/Audio/AudioEngine.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <miniaudio.h>

#include <algorithm>
#include <cmath>

namespace Basalt {

	namespace {

		// C++ code can store a value outside the enum (the registry and Lua never do); it plays on SFX, the
		// default bus, rather than indexing past the bus arrays.
		size_t BusIndex(AudioBus bus)
		{
			const auto index = static_cast<size_t>(bus);
			if (index < AudioBusCount)
				return index;
			BS_CORE_ERROR("Audio: bus value {} is out of range; using SFX", index);
			return static_cast<size_t>(AudioBus::SFX);
		}

	}

	AudioSystem::AudioSystem(Scene* scene)
		: m_Scene(scene)
	{
		m_BusVolumes.fill(1.0f);
		ma_engine* engine = AudioEngine::GetEngine();
		if (!engine)
			return;

		// Master feeds the engine's endpoint; every other bus feeds Master. Buses only mix: miniaudio never
		// spatializes a group, and they are not pitched either.
		constexpr ma_uint32 BusFlags = MA_SOUND_FLAG_NO_PITCH;
		for (size_t i = 0; i < AudioBusCount; i++)
		{
			ma_sound* parent = i == static_cast<size_t>(AudioBus::Master) ? nullptr : m_Buses[static_cast<size_t>(AudioBus::Master)];
			auto* group = new ma_sound();
			if (ma_sound_group_init(engine, BusFlags, parent, group) != MA_SUCCESS)
			{
				// Its sounds then play unscaled by this bus (straight into Master, or the endpoint).
				BS_CORE_ERROR("Audio: cannot create the {} bus; its volume has no effect", AudioBusNames[i]);
				delete group;
				continue;
			}
			m_Buses[i] = group;
		}
	}

	AudioSystem::~AudioSystem()
	{
		for (auto& [uuid, sound] : m_Sources)
			DestroySound(sound);
		for (ma_sound* sound : m_OneShots)
			DestroySound(sound);
		// Children before Master: a group must not be uninitialized while others still feed it.
		for (size_t i = AudioBusCount; i-- > 0;)
		{
			if (!m_Buses[i])
				continue;
			ma_sound_group_uninit(m_Buses[i]);
			delete m_Buses[i];
		}
	}

	ma_sound* AudioSystem::CreateSound(const std::string& clip, bool spatial, AudioBus bus, bool stream)
	{
		ma_engine* engine = AudioEngine::GetEngine();
		if (!engine || clip.empty())
			return nullptr;

		const std::string path = Project::ResolvePath(clip).string();
		auto* sound = new ma_sound();
		// Streams decode a page at a time as they play (the resource manager keeps two one-second pages);
		// everything else is decoded whole on load and shared between sounds of the same file.
		ma_uint32 flags = stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
		if (!spatial)
			flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
		if (ma_sound_init_from_file(engine, path.c_str(), flags, m_Buses[BusIndex(bus)], nullptr, sound) != MA_SUCCESS)
		{
			BS_CORE_ERROR("Audio: cannot load clip '{}'", clip);
			delete sound;
			return nullptr;
		}
		return sound;
	}

	void AudioSystem::DestroySound(ma_sound* sound)
	{
		if (!sound)
			return;
		ma_sound_uninit(sound);
		delete sound;
	}

	void AudioSystem::Start()
	{
		auto view = m_Scene->GetAllEntitiesWith<AudioSourceComponent>();
		for (entt::entity handle : view)
		{
			if (view.get<AudioSourceComponent>(handle).PlayOnStart)
				Play({ handle, m_Scene });
		}
		Update(0.0f);
	}

	void AudioSystem::PlayOnStart(const std::vector<Entity>& entities)
	{
		for (Entity entity : entities)
		{
			const auto* source = entity ? entity.TryGetComponent<AudioSourceComponent>() : nullptr;
			if (source && source->PlayOnStart)
				Play(entity);
		}
		// Positions the new sources and the listener right away, as Start does.
		Update(0.0f);
	}

	bool AudioSystem::Play(Entity entity)
	{
		const auto* source = entity.TryGetComponent<AudioSourceComponent>();
		if (!source)
			return false;

		Stop(entity);
		ma_sound* sound = CreateSound(source->Clip, source->Spatial, source->Bus, source->Stream);
		if (!sound)
			return false;

		ma_sound_set_volume(sound, source->Volume);
		ma_sound_set_pitch(sound, source->Pitch);
		ma_sound_set_looping(sound, source->Loop ? MA_TRUE : MA_FALSE);
		if (source->Spatial)
		{
			ma_sound_set_min_distance(sound, source->MinDistance);
			ma_sound_set_max_distance(sound, source->MaxDistance);
			const glm::vec3 position = glm::vec3(m_Scene->GetWorldTransform(entity)[3]);
			ma_sound_set_position(sound, position.x, position.y, position.z);
		}
		ma_sound_start(sound);
		m_Sources[entity.GetUUID()] = sound;
		return true;
	}

	void AudioSystem::Stop(Entity entity)
	{
		auto it = m_Sources.find(entity.GetUUID());
		if (it == m_Sources.end())
			return;
		DestroySound(it->second);
		m_Sources.erase(it);
	}

	bool AudioSystem::IsPlaying(Entity entity) const
	{
		auto it = m_Sources.find(entity.GetUUID());
		return it != m_Sources.end() && ma_sound_is_playing(it->second);
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
		ma_sound* sound = CreateSound(clip, position != nullptr, bus, false);
		if (!sound)
			return false;
		ma_sound_set_volume(sound, volume);
		if (position)
			ma_sound_set_position(sound, position->x, position->y, position->z);
		ma_sound_start(sound);
		m_OneShots.push_back(sound);
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
			if (!entity || ma_sound_is_spatialization_enabled(sound) == MA_FALSE)
				continue;
			const glm::vec3 position = glm::vec3(m_Scene->GetWorldTransform(entity)[3]);
			ma_sound_set_position(sound, position.x, position.y, position.z);
		}

		AudioEngine::AdvanceDeviceless(ts.GetSeconds());

		// Release finished one-shots. Sources stay registered (and report !IsPlaying) until stopped.
		m_OneShots.erase(std::remove_if(m_OneShots.begin(), m_OneShots.end(), [](ma_sound* sound) {
			if (!ma_sound_at_end(sound))
				return false;
			DestroySound(sound);
			return true; }),
						 m_OneShots.end());
	}

	void AudioSystem::SetBusVolume(AudioBus bus, float volume)
	{
		BS_CORE_ASSERT(std::isfinite(volume) && volume >= 0.0f, "bus volume must be finite and not negative");
		const size_t index = BusIndex(bus);
		m_BusVolumes[index] = volume;
		if (m_Buses[index])
			ma_sound_group_set_volume(m_Buses[index], volume);
	}

	float AudioSystem::GetBusVolume(AudioBus bus) const
	{
		return m_BusVolumes[BusIndex(bus)];
	}

	uint32_t AudioSystem::GetActiveSoundCount() const
	{
		return static_cast<uint32_t>(m_Sources.size() + m_OneShots.size());
	}

}
