#include "Basalt/Audio/AudioSystem.h"

#include "Basalt/Audio/AudioEngine.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <miniaudio.h>

#include <algorithm>

namespace Basalt {

	AudioSystem::AudioSystem(Scene* scene)
		: m_Scene(scene)
	{
	}

	AudioSystem::~AudioSystem()
	{
		for (auto& [uuid, sound] : m_Sources)
			DestroySound(sound);
		for (ma_sound* sound : m_OneShots)
			DestroySound(sound);
	}

	ma_sound* AudioSystem::CreateSound(const std::string& clip, bool spatial)
	{
		ma_engine* engine = AudioEngine::GetEngine();
		if (!engine || clip.empty())
			return nullptr;

		const std::string path = Project::ResolvePath(clip).string();
		auto* sound = new ma_sound();
		const ma_uint32 flags = MA_SOUND_FLAG_DECODE | (spatial ? 0u : static_cast<ma_uint32>(MA_SOUND_FLAG_NO_SPATIALIZATION));
		if (ma_sound_init_from_file(engine, path.c_str(), flags, nullptr, nullptr, sound) != MA_SUCCESS)
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

	bool AudioSystem::Play(Entity entity)
	{
		const auto* source = entity.TryGetComponent<AudioSourceComponent>();
		if (!source)
			return false;

		Stop(entity);
		ma_sound* sound = CreateSound(source->Clip, source->Spatial);
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

	bool AudioSystem::PlayOneShot(const std::string& clip, const glm::vec3* position, float volume)
	{
		ma_sound* sound = CreateSound(clip, position != nullptr);
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

	uint32_t AudioSystem::GetActiveSoundCount() const
	{
		return static_cast<uint32_t>(m_Sources.size() + m_OneShots.size());
	}

}
