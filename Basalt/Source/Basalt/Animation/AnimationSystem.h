#pragma once

#include "Basalt/Animation/AnimationPose.h"
#include "Basalt/Core/UUID.h"
#include "Basalt/Core/Timestep.h"
#include "Basalt/Scene/Components.h"

#include <entt/entt.hpp>

#include <unordered_map>

namespace Basalt {

	class Entity;
	class Scene;

	// Per-entity playback and poses for a playing/simulating scene. Asset data stays shared and immutable.
	class AnimationSystem
	{
	public:
		explicit AnimationSystem(Scene& scene);
		~AnimationSystem();
		void Update(Timestep timestep);
		bool Play(Entity entity, const std::string& clip);
		bool CrossFade(Entity entity, const std::string& clip, float seconds);
		bool Blend(Entity entity, const std::string& first, const std::string& second, float weight);
		void Pause(Entity entity, bool paused);
		void Stop(Entity entity);
		bool Seek(Entity entity, float seconds);
		float GetTime(Entity entity);
		float GetDuration(Entity entity);
		bool IsPlaying(Entity entity);
		std::vector<std::string> GetClips(Entity entity);
		// Getters resolve assets/configuration lazily and may reset playback. The vector object survives
		// updates; its contents change each sample. Removal, reload or system destruction invalidates it.
		const std::vector<glm::mat4>* GetPose(Entity entity);

	private:
		struct Playback
		{
			entt::entity Handle = entt::null;
			Ref<MeshSource> Source;
			std::string ConfiguredClip;
			int32_t Clip = -1;
			double Time = 0;
			int32_t Outgoing = -1;
			double OutgoingTime = 0;
			float BlendWeight = 1;
			double FadeTime = 0;
			float FadeDuration = 0;
			AnimationLocalPose Local;
			AnimationLocalPose Snapshot; // Interrupted transitions start from their visible blended pose.
			bool Playing = false;
			bool Rest = false;
			bool InvalidPose = false;
			bool InvalidSpeed = false;
			std::string EntityName;
			std::vector<glm::mat4> Globals;
		};
		Playback* Ensure(Entity entity);
		static void Evaluate(Playback& playback);
		static void ClearTransition(Playback& playback);
		void OnDestroy(entt::registry& registry, entt::entity handle);
		Scene& m_Scene;
		std::unordered_map<UUID, Playback> m_Playback;
		std::unordered_map<entt::entity, bool> m_EmptyMeshes;
	};
}
