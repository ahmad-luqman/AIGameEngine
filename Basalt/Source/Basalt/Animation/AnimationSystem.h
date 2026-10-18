#pragma once

#include "Basalt/Animation/AnimationPose.h"
#include "Basalt/Animation/AnimationGraph.h"
#include "Basalt/Core/UUID.h"
#include "Basalt/Core/Timestep.h"
#include "Basalt/Scene/Components.h"

#include <entt/entt.hpp>

#include <map>
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
	// Each Update advances every Animator and collects the events playback crossed (TakeEvents). The scene
	// updates attachments after physics; standalone callers get attachments at the end of Update by default.
	class AnimationSystem
	{
	public:
		// Most events one Animator delivers per frame; the rest are dropped (warned once per playback started).
		static constexpr size_t MaxEventsPerFrame = 64;

		explicit AnimationSystem(Scene& scene);
		~AnimationSystem();
		void Update(Timestep timestep, bool updateAttachments = true);
		void UpdateAttachments();
		bool SetParameter(Entity entity, const std::string& name, float value);
		bool SetParameter(Entity entity, const std::string& name, bool value);
		bool SetTrigger(Entity entity, const std::string& name);
		std::string GetState(Entity entity);
		bool Play(Entity entity, const std::string& clip);
		bool CrossFade(Entity entity, const std::string& clip, float seconds);
		bool Blend(Entity entity, const std::string& first, const std::string& second, float weight);
		// Plays one of the Animator's blend spaces at `parameter` (Y is ignored by 1D spaces), fading from the
		// current pose over `fadeSeconds`. While that space is already playing it only moves the parameter (the fade
		// is ignored); a finished or paused one restarts.
		bool PlayBlendSpace(Entity entity, const std::string& space, glm::vec2 parameter, float fadeSeconds = 0.0f);
		// Moves the running blend space's parameter; false when no blend space is running or it cannot play there.
		bool SetBlendSpaceParameter(Entity entity, glm::vec2 parameter);
		// The running blend space's parameter, if one is running.
		std::optional<glm::vec2> GetBlendSpaceParameter(Entity entity);
		// The running blend space's weight per clip (summed over samples that share a clip, 0 for clips without
		// weight at the parameter); empty when no blend space is running or it cannot play.
		std::vector<std::pair<std::string, float>> GetBlendSpaceWeights(Entity entity);
		void Pause(Entity entity, bool paused);
		void Stop(Entity entity);
		// In a blend space, seconds of its blended cycle; false while the space cannot play.
		bool Seek(Entity entity, float seconds);
		// In a blend space that cannot play: its last playable cycle.
		float GetTime(Entity entity);
		float GetDuration(Entity entity);
		// False while a blend space cannot play (see SpaceState::Resolved); it resumes once it can.
		bool IsPlaying(Entity entity);
		std::vector<std::string> GetClips(Entity entity);
		// Getters resolve assets/configuration lazily and may reset playback. The vector object survives
		// updates; its contents change each sample. Removal, reload or system destruction invalidates it.
		const std::vector<glm::mat4>* GetPose(Entity entity);
		// Model-space transform of the first node named `joint` in the current pose; false (with the reason in
		// outError) when the entity has no playable Animator or its model no such node.
		bool GetBoneTransform(Entity entity, const std::string& joint, glm::mat4& outTransform, std::string& outError);
		// Logs a GetBoneTransform failure for a script when it appears or changes (an empty `error` records success),
		// so a script polling a misspelled joint every frame reports it once.
		void ReportBoneProblem(Entity entity, const std::string& joint, const std::string& error);
		// The events crossed since the last call, per Animator in update order: each clip's in the order playback
		// crossed them, the current clip's before the outgoing one's (for a two-clip blend, the second clip's first).
		std::vector<AnimationEventRecord> TakeEvents();

	private:
		// One sample of the running blend space, resolved against the model.
		struct SpaceSample
		{
			int32_t Clip = -1; // -1: the sample's clip is missing (weight 0)
			float Weight = 0;
			float Speed = 0;
		};
		// The running blend space. It is looked up by name every update while playing, so edits during play apply at
		// once.
		struct SpaceState
		{
			std::string Name;
			glm::vec2 Parameter = glm::vec2(0);
			// Fraction of the synchronized cycle, 0..1: every clip is sampled at Phase of its own duration.
			double Phase = 0;
			// Blended cycle length: the weighted sum of the sample clips' durations.
			double Duration = 0;
			std::vector<SpaceSample> Samples;
			// False while the space is missing, has no samples, or no sample weighted at the parameter has a clip:
			// playback holds its pose and resumes once it can play.
			bool Resolved = false;
			// The problem last logged, so a problem is logged when it appears or changes, not every update.
			std::string Problem;
		};
		struct Playback
		{
			entt::entity Handle = entt::null;
			Ref<MeshSource> Source;
			std::string ConfiguredClip;
			std::string ConfiguredGraph;
			Ref<AnimationGraph> Graph;
			std::string State;
			std::vector<float> Parameters;
			// Unwrapped cycle phase: blend-space duration changes must not move an exit threshold.
			double StatePhase = 0;
			bool GraphActive = false;
			bool Paused = false;
			bool Looping = true;
			bool OutgoingLooping = true;
			bool RootMotion = false;
			bool OutgoingRootMotion = false;
			int32_t RootNode = -1;
			std::string RootProblem;
			std::string RootApplyProblem;
			bool InvalidRootPose = false;
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
			// Animator.Clip when it named neither a clip nor a blend space: a blend space of that name added later
			// still starts (until something else plays).
			std::string PendingStart;
			// The last failed PlayBlendSpace's problem, so a script retrying every frame logs it once.
			std::string PlayProblem;
			std::string EntityName;
			std::vector<glm::mat4> Globals;
		};
		Playback* Ensure(Entity entity);
		// Ensure for an operation: logs why the entity cannot animate (once per entity and reason).
		Playback* EnsurePlayable(Entity entity);
		// Why Ensure returns no playback for the entity.
		static std::string DescribeUnplayable(Entity entity);
		// Fills the state's samples, duration and Resolved from the component and returns the dominant clip; -1
		// (Resolved false) when the space cannot play (see SpaceState::Resolved). `outProblem` also names missing
		// sample clips when the others can play.
		int32_t ResolveSpaceState(const MeshSource& source, const AnimatorComponent& component, SpaceState& state, std::string& outProblem);
		// Re-resolves the running space and logs a new problem; false when it cannot play.
		bool ResolveSpace(Playback& playback, SpaceState& state, const AnimatorComponent& component);
		static void Evaluate(Playback& playback);
		static void ClearTransition(Playback& playback);
		// Starts fading from the visible pose; the current clip keeps advancing as the outgoing one when playback is
		// a single clip.
		static void BeginTransition(Playback& playback, float seconds);
		// Starts `clip` (or a blend space whose dominant clip it is) from 0 after the caller set up the transition.
		static void Start(Playback& playback, int32_t clip);
		static void TakeDirectControl(Playback& playback, const AnimatorComponent& component);
		void AdvanceSpace(Playback& playback, SpaceState& state, const AnimatorComponent& component, double advance, UUID id, size_t& budget);
		// Queues the markers of `clip` that a clock moving from `from` by `advance` crosses, within `budget`.
		void CollectEvents(Playback& playback, const AnimatorComponent& component, int32_t clip, double from, double advance, UUID id, size_t& budget, bool looping);
		bool EnterState(Playback& playback, const AnimationState& state, const AnimatorComponent& component, float fade);
		void UpdateGraph(Playback& playback, const AnimatorComponent& component);
		bool SetParameterValue(Entity entity, const std::string& name, float value, AnimationParameterType type);
		static bool StripRoot(Playback& playback, AnimationLocalPose& pose, bool enabled);
		static glm::mat4 CurrentRootDelta(const Playback& playback, double advance);
		static glm::mat4 RootDelta(const Playback& playback, int32_t clip, double from, double advance, bool looping);
		void ApplyRootMotion(Entity entity, Playback& playback, const glm::mat4& delta, float seconds);
		void OnDestroy(entt::registry& registry, entt::entity handle);
		Scene& m_Scene;
		std::unordered_map<UUID, Playback> m_Playback;
		std::unordered_map<entt::entity, bool> m_EmptyMeshes;
		std::vector<AnimationEventRecord> m_PendingEvents;
		// The last problem reported per BoneAttachment entity, so each is logged once.
		std::unordered_map<UUID, std::string> m_AttachmentProblems;
		// The last GetBoneTransform problem reported per entity and joint, and the last unplayable reason per entity.
		std::map<std::pair<UUID, std::string>, std::string> m_BoneProblems;
		std::unordered_map<UUID, std::string> m_UnplayableReported;
		// Scratch storage reused every update.
		std::vector<const AnimationEvent*> m_Markers;
		std::vector<float> m_Weights;
	};
}
