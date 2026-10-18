#pragma once

#include "Basalt/Animation/AnimationPose.h"
#include "Basalt/Core/UUID.h"
#include "Basalt/Core/Timestep.h"
#include "Basalt/Scene/Components.h"

#include <entt/entt.hpp>

#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Basalt {

	class Entity;
	class Scene;

	// An AnimationEvent marker that playback crossed, waiting to be delivered.
	struct AnimationEventRecord
	{
		// May be gone by the time the event is delivered.
		UUID Entity = 0;
		std::string Name;
		// The clip whose playback crossed the marker (never empty, unlike AnimationEvent::Clip).
		std::string Clip;
	};

	// Per-entity playback and poses for a playing/simulating scene. Asset data stays shared and immutable.
	// Each Update advances every Animator, collects the events playback crossed (TakeEvents) and then moves
	// BoneAttachment entities onto their joints.
	class AnimationSystem
	{
	public:
		// Most events one Animator delivers per frame; the rest are dropped (warned once per playback started).
		static constexpr size_t MaxEventsPerFrame = 64;

		explicit AnimationSystem(Scene& scene);
		~AnimationSystem();
		void Update(Timestep timestep);
		bool Play(Entity entity, const std::string& clip);
		bool CrossFade(Entity entity, const std::string& clip, float seconds);
		bool Blend(Entity entity, const std::string& first, const std::string& second, float weight);
		// Plays one of the Animator's blend spaces at `parameter` (Y is ignored by 1D spaces), fading from the
		// current pose over `fadeSeconds`. While that space is already playing it only moves the parameter (the fade
		// is ignored); a finished or paused one restarts.
		bool PlayBlendSpace(Entity entity, const std::string& space, glm::vec2 parameter, float fadeSeconds = 0.0f);
		// Moves the running blend space's parameter; false when no blend space is running or it cannot play there.
		bool SetBlendSpaceParameter(Entity entity, glm::vec2 parameter);
		// The running blend space's weight per clip (summed over samples that share a clip); empty when no blend
		// space is running or it cannot play.
		std::vector<std::pair<std::string, float>> GetBlendSpaceWeights(Entity entity);
		void Pause(Entity entity, bool paused);
		void Stop(Entity entity);
		// In a blend space, seconds of its blended cycle.
		bool Seek(Entity entity, float seconds);
		float GetTime(Entity entity);
		float GetDuration(Entity entity);
		// False while a blend space cannot play (missing space or clips); it resumes once they exist.
		bool IsPlaying(Entity entity);
		std::vector<std::string> GetClips(Entity entity);
		// Getters resolve assets/configuration lazily and may reset playback. The vector object survives
		// updates; its contents change each sample. Removal, reload or system destruction invalidates it.
		const std::vector<glm::mat4>* GetPose(Entity entity);
		// Model-space transform of the first node named `joint` in the current pose; false (with the reason in
		// outError) when the entity has no playable Animator or its model no such node.
		bool GetBoneTransform(Entity entity, const std::string& joint, glm::mat4& outTransform, std::string& outError);
		// Logs a GetBoneTransform failure once per entity, joint and reason, so a script polling a misspelled joint
		// every frame reports it once.
		void ReportBoneProblem(Entity entity, const std::string& joint, const std::string& error);
		// The events crossed since the last call, per Animator in update order: each clip's in timeline order, the
		// current clip's before the outgoing clip's.
		std::vector<AnimationEventRecord> TakeEvents();

	private:
		// One sample of the running blend space, resolved against the model.
		struct SpaceSample
		{
			int32_t Clip = -1; // -1: the sample's clip is missing (weight 0)
			float Weight = 0;
			float Speed = 0;
		};
		// The running blend space. It is looked up by name every update, so edits during play apply at once.
		struct SpaceState
		{
			std::string Name;
			glm::vec2 Parameter = glm::vec2(0);
			// Fraction of the synchronized cycle, 0..1: every clip is sampled at Phase of its own duration.
			double Phase = 0;
			// Blended cycle length: the weighted sum of the sample clips' durations.
			double Duration = 0;
			std::vector<SpaceSample> Samples;
			// False while the space or every sample clip is missing: playback holds its pose and resumes once they
			// exist.
			bool Resolved = false;
			// The problem last logged, so each one is reported once.
			std::string Problem;
		};
		struct Playback
		{
			entt::entity Handle = entt::null;
			Ref<MeshSource> Source;
			std::string ConfiguredClip;
			// In a blend space: the heaviest sample's clip.
			int32_t Clip = -1;
			// In a blend space: Phase * Duration.
			double Time = 0;
			int32_t Outgoing = -1;
			double OutgoingTime = 0;
			float BlendWeight = 1;
			double FadeTime = 0;
			float FadeDuration = 0;
			std::optional<SpaceState> Space;
			// MatchVelocity: the entity's smoothed horizontal speed and where it was at the last update.
			double MeasuredSpeed = 0;
			std::optional<glm::vec3> LastPosition;
			AnimationLocalPose Local;
			AnimationLocalPose Snapshot; // Interrupted transitions start from their visible blended pose.
			bool Playing = false;
			bool Rest = false;
			bool InvalidPose = false;
			bool InvalidSpeed = false;
			bool EventLimitLogged = false;
			std::string EntityName;
			std::vector<glm::mat4> Globals;
		};
		Playback* Ensure(Entity entity);
		// Why Ensure returns no playback for the entity.
		static std::string DescribeUnplayable(Entity entity);
		// Fills the state's samples, duration and dominant clip from the component; false when the space or every
		// sample clip is missing. `outProblem` also names missing sample clips when the others can play.
		bool ResolveSpaceState(const MeshSource& source, const AnimatorComponent& component, SpaceState& state, int32_t& outDominant, std::string& outProblem);
		// Re-resolves the running space and logs a new problem; false when it cannot play.
		bool ResolveSpace(Playback& playback, SpaceState& state, const AnimatorComponent& component);
		static void Evaluate(Playback& playback);
		static void ClearTransition(Playback& playback);
		// Starts fading from the visible pose; the current clip keeps advancing as the outgoing one when playback is
		// a single clip.
		static void BeginTransition(Playback& playback, float seconds);
		void AdvanceSpace(Playback& playback, SpaceState& state, const AnimatorComponent& component, double advance, UUID id, size_t& budget);
		// Queues the markers of `clip` that a clock moving from `from` by `advance` crosses, within `budget`.
		void CollectEvents(Playback& playback, const AnimatorComponent& component, int32_t clip, double from, double advance, UUID id, size_t& budget);
		void UpdateAttachments();
		void OnDestroy(entt::registry& registry, entt::entity handle);
		Scene& m_Scene;
		std::unordered_map<UUID, Playback> m_Playback;
		std::unordered_map<entt::entity, bool> m_EmptyMeshes;
		std::vector<AnimationEventRecord> m_PendingEvents;
		// The last problem reported per BoneAttachment entity, so each is logged once.
		std::unordered_map<UUID, std::string> m_AttachmentProblems;
		std::unordered_set<std::string> m_ReportedBoneProblems;
		// Scratch storage reused every update.
		std::vector<const AnimationEvent*> m_Markers;
		std::vector<float> m_Weights;
	};
}
