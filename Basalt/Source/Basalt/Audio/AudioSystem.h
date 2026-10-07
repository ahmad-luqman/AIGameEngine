#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Core/Timestep.h"
#include "Basalt/Core/UUID.h"

#include <glm/glm.hpp>

#include <string>
#include <unordered_map>
#include <vector>

struct ma_sound;

namespace Basalt {

	class Entity;
	class Scene;

	// Plays AudioSourceComponents of a running scene and positions the listener.
	// The listener is the first active AudioListenerComponent, or the primary camera if there is none.
	class AudioSystem
	{
	public:
		explicit AudioSystem(Scene* scene);
		~AudioSystem();

		AudioSystem(const AudioSystem&) = delete;
		AudioSystem& operator=(const AudioSystem&) = delete;

		// Starts every source with PlayOnStart.
		void Start();
		void Update(Timestep ts);
		void OnEntityDestroyed(Entity entity);

		// (Re)starts the entity's AudioSourceComponent clip from the beginning. Returns false if it cannot play.
		bool Play(Entity entity);
		void Stop(Entity entity);
		bool IsPlaying(Entity entity) const;
		// Fire-and-forget sound at a world position (non-spatial when position is null).
		bool PlayOneShot(const std::string& clip, const glm::vec3* position, float volume = 1.0f);

		uint32_t GetActiveSoundCount() const;

	private:
		ma_sound* CreateSound(const std::string& clip, bool spatial);
		static void DestroySound(ma_sound* sound);

	private:
		Scene* m_Scene = nullptr;
		std::unordered_map<UUID, ma_sound*> m_Sources;
		std::vector<ma_sound*> m_OneShots;
	};

}
