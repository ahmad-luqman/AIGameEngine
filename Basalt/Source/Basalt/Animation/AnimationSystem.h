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

	// An AnimationEvent marker that playback crossed, waiting to be delivered.
	struct AnimationEventRecord
	{
		UUID Entity = 0;
		std::string Name;
		std::string Clip;
	};

	// Per-entity playback and poses for a playing/simulating scene. Asset data stays shared and immutable.
	// Each Update advances every Animator, collects the events playback crossed (TakeEvents) and then moves
	// BoneAttachment entities onto their joints.
	class AnimationSystem
	{
	public:
		// Most events one Animator delivers per frame; the rest are dropped with a warning.
		static constexpr size_t MaxEventsPerFrame = 64;

		explicit AnimationSystem(Scene& scene);
		~AnimationSystem();
		void Update(Timestep timestep);
		bool Play(Entity entity, const std::string& clip);
		bool CrossFade(Entity entity, const std::string& clip, float seconds);
		bool Blend(Entity entity, const std::string& first, const std::string& second, float weight);
		// Plays one of the Animator's blend spaces at `parameter` (Y is ignored by 1D spaces), fading from the
		// current pose over `fadeSeconds`. Playing the running space again only moves its parameter.
		bool PlayBlendSpace(Entity entity, const std::string& space, glm::vec2 parameter, float fadeSeconds = 0.0f);
		// Moves the running blend space's parameter; fails when no blend space is playing.
		bool SetBlendSpaceParameter(Entity entity, glm::vec2 parameter);
		// The running blend space's weight per clip (summed over samples that share a clip); empty otherwise.
		std::vector<std::pair<std::string, float>> GetBlendSpaceWeights(Entity entity);
		void Pause(Entity entity, bool paused);
		void Stop(Entity entity);
		// In a blend space, seconds of its blended duration.
		bool Seek(Entity entity, float seconds);
		float GetTime(Entity entity);
		float GetDuration(Entity entity);
		bool IsPlaying(Entity entity);
		std::vector<std::string> GetClips(Entity entity);
		// Getters resolve assets/configuration lazily and may reset playback. The vector object survives
		// updates; its contents change each sample. Removal, reload or system destruction invalidates it.
		const std::vector<glm::mat4>* GetPose(Entity entity);
		// Model-space transform of the first node named `joint` in the current pose; false (with the reason in
		// outError) when the entity has no animated model or no such node.
		bool GetBoneTransform(Entity entity, const std::string& joint, glm::mat4& outTransform, std::string& outError);
		// The events crossed since the last call, in the order they were crossed.
		std::vector<AnimationEventRecord> TakeEvents();

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
			// Blend space mode while Space is not empty: Clip is the heaviest sample's clip, Time is
			// Phase * SpaceDuration, and every sample's clip is sampled at Phase of its own duration.
			std::string Space;
			glm::vec2 Parameter = glm::vec2(0);
			double Phase = 0;
			double SpaceDuration = 0;
			// Per sample of the space, refreshed every update: clip index (-1 = missing) and weight.
			std::vector<int32_t> SpaceClips;
			std::vector<float> SpaceWeights;
			// MatchVelocity: the entity's smoothed horizontal speed and where it was last update.
			double MeasuredSpeed = 0;
			glm::vec3 LastPosition = glm::vec3(0);
			bool HasLastPosition = false;
			AnimationLocalPose Local;
			AnimationLocalPose Snapshot; // Interrupted transitions start from their visible blended pose.
			bool Playing = false;
			bool Rest = false;
			bool InvalidPose = false;
			bool InvalidSpeed = false;
			// Problems already logged, so a persisting one is reported once.
			bool SpaceProblemLogged = false;
			bool EventLimitLogged = false;
			size_t EventsThisFrame = 0;
			std::string EntityName;
			std::vector<glm::mat4> Globals;
		};
		Playback* Ensure(Entity entity);
		// Refreshes SpaceClips/SpaceWeights from the component; false (logged once) when the space is gone or has
		// no playable sample.
		bool ResolveSpace(Playback& playback, const AnimatorComponent& component);
		static void Evaluate(Playback& playback);
		static void ClearTransition(Playback& playback);
		void AdvanceSpace(Playback& playback, const AnimatorComponent& component, double advance, UUID id);
		void CollectEvents(Playback& playback, const AnimatorComponent& component, int32_t clip, double from, double advance, UUID id);
		void UpdateAttachments();
		void OnDestroy(entt::registry& registry, entt::entity handle);
		Scene& m_Scene;
		std::unordered_map<UUID, Playback> m_Playback;
		std::unordered_map<entt::entity, bool> m_EmptyMeshes;
		std::vector<AnimationEventRecord> m_PendingEvents;
		// The last problem reported per BoneAttachment entity, so each is logged once.
		std::unordered_map<UUID, std::string> m_AttachmentProblems;
	};
}
