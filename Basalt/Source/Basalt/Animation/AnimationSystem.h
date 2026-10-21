#pragma once

#include "Basalt/Animation/AnimationClipSet.h"
#include "Basalt/Animation/AnimationPose.h"
#include "Basalt/Animation/AnimationGraph.h"
#include "Basalt/Animation/FadingWeight.h"
#include "Basalt/Animation/SpringBones.h"
#include "Basalt/Core/UUID.h"
#include "Basalt/Core/Timestep.h"
#include "Basalt/Scene/Components.h"

#include <entt/entt.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

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
	// Each Update advances every Animator, samples due poses (optionally distance-throttled), and collects the events
	// playback crossed (TakeEvents). The scene runs LateUpdate (inverse kinematics, spring bones, ragdoll physics, then attachments) after
	// physics and OnLateUpdate; standalone callers get it at the end of Update by default.
	class AnimationSystem
	{
	public:
		// Most events one Animator delivers per frame; the rest are dropped (warned once per playback started).
		static constexpr size_t MaxEventsPerFrame = 64;

		explicit AnimationSystem(Scene& scene);
		~AnimationSystem();
		// runLateUpdate: also run LateUpdate at the end; the scene passes false and calls LateUpdate after physics.
		void Update(Timestep timestep, bool runLateUpdate = true);
		// Solves IK and spring bones, applies physical ragdoll poses, then refreshes attachments after physics and
		// OnLateUpdate. Invalid steps (reported by Update) skip IK/springs; physical overlays and attachments still refresh.
		void LateUpdate(Timestep timestep);
		// Set a declared Float/Bool parameter on an active graph; false with a deduplicated diagnostic on failure.
		bool SetParameter(Entity entity, const std::string& name, float value);
		bool SetParameter(Entity entity, const std::string& name, bool value);
		// Latch a declared trigger until a successful transition consumes it. False for an inactive graph or wrong name/type.
		bool SetTrigger(Entity entity, const std::string& name);
		// Current graph state; empty after Stop, direct playback, or a graph load failure.
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
		// Layers (Animator.Layers, found by name). PlayLayer restarts the layer at 0, on `clip` when given (until the
		// configured Clip changes), otherwise on its configured Clip (dropping an earlier script clip); false when the
		// layer would show nothing (the call then changes nothing). SetLayerWeight fades linearly over `seconds` (0: at
		// once). False for an unknown layer or clip or an invalid value, logged as ReportCallProblem describes.
		bool PlayLayer(Entity entity, const std::string& layer, const std::optional<std::string>& clip);
		bool SetLayerWeight(Entity entity, const std::string& layer, float weight, float seconds);
		std::optional<float> GetLayerWeight(Entity entity, const std::string& layer);
		// Inverse kinematics chains (InverseKinematics.Chains, found by name). A target is a world position (with an
		// optional world rotation for a TwoBone chain's end joint) or an entity followed every frame; it replaces
		// ground placement until ClearIKTarget. False for a missing component or chain, an invalid value, or a rotation
		// for a LookAt chain, logged as ReportCallProblem describes.
		bool SetIKTarget(Entity entity, const std::string& chain, const glm::vec3& position, const std::optional<glm::quat>& rotation);
		bool SetIKTarget(Entity entity, const std::string& chain, Entity target, bool matchRotation);
		bool ClearIKTarget(Entity entity, const std::string& chain);
		bool SetIKWeight(Entity entity, const std::string& chain, float weight, float seconds);
		std::optional<float> GetIKWeight(Entity entity, const std::string& chain);
		// Morph targets, found by name in every mesh node of the model with that target (a name shared by several meshes,
		// such as a head and its teeth, sets them all). A script weight (0..1) replaces the animated one, fading over
		// `seconds` from the weight shown, until ClearMorphWeight; it survives playback resets. False for an unknown
		// target or an invalid value, logged as ReportCallProblem describes.
		bool SetMorphWeight(Entity entity, const std::string& target, float weight, float seconds);
		// The weight shown (of the first node with the target).
		std::optional<float> GetMorphWeight(Entity entity, const std::string& target);
		// Returns the target (without one, every target) to its animated weight at once. False when no script weight was
		// set; an unknown target is logged.
		bool ClearMorphWeight(Entity entity, const std::optional<std::string>& target);
		// Unique target names in node index (glTF) order, each mesh's in declaration order.
		std::vector<std::string> GetMorphTargets(Entity entity);
		// Weights shown, at MeshNode::MorphWeightOffset (Source's MorphWeightCount of them); the same lifetime as GetPose.
		const std::vector<float>* GetMorphWeights(Entity entity);
		// The playback's model with its pose and morph weights, resolved once (for drawing); the same lifetime as GetPose.
		struct PoseView
		{
			Ref<MeshSource> Source;
			std::span<const glm::mat4> Globals;
			std::span<const float> MorphWeights;
		};
		std::optional<PoseView> GetPoseView(Entity entity);
		// Clip sets kept for the Animators (diagnostics and tests): one per model and clip library combination in use.
		size_t GetClipSetCount() const { return m_ClipSets.size(); }
		// Restarts every spring chain of the entity on its animated pose, at rest, at the next spring solve (after a
		// teleport too short for the automatic detection, or a cutscene cut). False without a SpringBone component.
		bool ResetSpringBones(Entity entity);
		// Physics removed/rebuilt a rig or finished recovery; discard its cached overlay immediately.
		void ClearRagdollPose(Entity entity);
		// Sampled/layered animation before overlays; an optional clip previews a direct Play at time zero without mutation.
		bool GetAnimatedPose(Entity entity, const MeshSource& source, std::vector<glm::mat4>& globals, const std::string& clip = {});
		// Refresh physical overlays after an immediate control call, without advancing clocks or attachments.
		void RefreshRagdollPoses();
		// The events crossed since the last call, per Animator in update order: each clip's in the order playback
		// crossed them, the current clip's before the outgoing one's (for a two-clip blend, the second clip's first).
		std::vector<AnimationEventRecord> TakeEvents();

	private:
		// A move faster than this (units/s) between two updates is a teleport (respawn, Scene.Load, a script placing the
		// entity), not movement, so it does not spike MatchVelocity or swing spring bones. MatchVelocity checks the
		// entity's horizontal move, spring bones each chain's first joint in 3D; both compare the move against
		// TeleportSpeed times the longer of the frame and one fixed step, because physics moves bodies once per step: at
		// high frame rates most frames see no move.
		static constexpr double TeleportSpeed = 100.0;
		// Why graph evaluation/parameter writes are enabled or disabled, independent of pose and pause state.
		enum class GraphControl
		{
			Active,
			Direct,
			Stopped
		};
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
		// Outgoing fade or first two-clip Blend contribution; Clip and Space are mutually exclusive.
		// RootMotion implies a source. Interrupted fades retain only the frozen visible pose snapshot.
		struct OutgoingState
		{
			int32_t Clip = -1;
			double Time = 0;
			bool Looping = true;
			bool RootMotion = false;
			// Only populated for a fade out of a root-motion blend space; its pose remains a snapshot.
			std::optional<SpaceState> Space;
		};
		struct RootFrames
		{
			// End * inverse(start), cached per source clip and extraction node.
			glm::mat4 Cycle{ 1 };
			bool Valid = false;
			// A failed interval disables stripping for this clip until the next frame, restart or seek.
			bool IntervalValid = true;
			// Retained across good frames: each clip/node failure is reported once per playback/cache bind.
			std::string Problem;
		};
		// What a layer's resolution depends on: it is resolved again only when this changes.
		struct LayerKey
		{
			std::string Clip;
			AnimationLayerBlend Blend = AnimationLayerBlend::Override;
			std::string ReferenceClip;
			float ReferenceTime = 0;
			std::vector<AnimationMaskJoint> Mask;
			// Compares without copying the configuration; NaN values written by C++ compare equal to themselves, so they
			// cannot force a resolution every update.
			bool Matches(const AnimationLayer& config, const std::string& clip) const;
		};
		// A layer's clip, mask and reference pose, resolved against the model.
		struct LayerResolution
		{
			LayerKey Key;
			// -1 shows nothing: no clip, or a missing clip or reference clip.
			int32_t Clip = -1;
			AnimationLayerMask Mask;
			// Additive layers only.
			AnimationLocalPose Reference;
			// Mask with the root-motion joint and the nodes above it left out, for RootMaskNode (-1: not built yet).
			AnimationLayerMask RootMask;
			int32_t RootMaskNode = -1;
		};
		// One node's local transform as an IK, spring bone or ragdoll pass left it.
		struct OverlayEntry
		{
			uint32_t Node = 0;
			AnimationNodePose Local;
		};
		// A script's morph weight, carried across playback resets: Slots are the pose weights its target names in the
		// playback's Source, looked up again whenever the playback is rebuilt.
		struct MorphOverride
		{
			std::string Target;
			FadingWeight Weight;
			std::vector<uint32_t> Slots;
		};
		// Runtime playback of one Animator layer, matched to its configuration by name every update. It belongs to
		// the playback and resets with it.
		struct LayerState
		{
			std::string Name;
			// Set by PlayLayer; nullopt plays the configured Clip.
			std::optional<std::string> ScriptClip;
			// The configured Clip last seen: a changed one replaces the script clip and restarts.
			std::string ConfiguredClip;
			FadingWeight Weight;
			double Time = 0;
			// Cached by ResolveLayer; nullopt until the first resolution.
			std::optional<LayerResolution> Resolved;
			// The last resolution, configuration and root-motion problems, each logged when it changes. The root-motion one
			// is kept while root motion is off, so states switching it on and off do not repeat it.
			std::string Problem;
			std::string ConfigProblem;
			std::string RootProblem;
		};
		struct Playback
		{
			entt::entity Handle = entt::null;
			Ref<MeshSource> Source;
			// Source's clips and the Animator's retargeted library clips; every clip index refers to it.
			Ref<const AnimationClipSet> Clips;
			std::string ConfiguredClip;
			std::string ConfiguredGraph;
			Ref<const AnimationGraph> Graph;
			std::optional<size_t> StateIndex;
			std::vector<float> Parameters;
			// Unwrapped cycle phase: blend-space duration changes must not move an exit threshold.
			double StatePhase = 0;
			// Active permits parameter writes/transitions. Seek preserves Stopped control while leaving rest.
			GraphControl Control = GraphControl::Direct;
			// Paused freezes the graph; Playing advances clocks and may be false at a non-looping end.
			bool Paused = false;
			bool Looping = true;
			bool RootMotion = false;
			int32_t RootNode = -1;
			OutgoingState Outgoing;
			// Rest horizontal frame and clip cycles depend only on Source and RootNode, not state policy.
			std::optional<glm::mat4> RootReference;
			std::unordered_map<int32_t, RootFrames> RootClips;
			// One diagnostic per declared parameter and supplied type; caller names cannot grow this storage.
			std::vector<std::array<std::string, 3>> ParameterProblems;
			// Undeclared names retain only the most recent diagnostic per supplied type.
			std::array<std::string, 3> UnknownParameterProblems;
			// Last failure per graph state, so competing unplayable targets cannot alternate logs.
			std::vector<std::string> StateProblems;
			std::string RootProblem;
			std::string RootApplyProblem;
			// In a blend space: the heaviest sample's clip.
			int32_t Clip = -1;
			// In a blend space: Phase * Duration.
			double Time = 0;
			float BlendWeight = 1;
			double FadeTime = 0;
			float FadeDuration = 0;
			std::optional<SpaceState> Space;
			// MatchVelocity: the entity's smoothed horizontal speed and where it was at the last update.
			double MeasuredSpeed = 0;
			std::optional<glm::vec3> LastPosition;
			// The base pose (clip, blend, fade or blend space). Layers and IK never touch it; only a non-finite result
			// replaces it with the rest pose.
			AnimationLocalPose Local;
			AnimationLocalPose Snapshot; // Interrupted transitions start from their visible blended pose.
			std::vector<LayerState> Layers;
			// The layered pose: Local with the layers applied, never IK. IK solves from it.
			AnimationLocalPose Pose;
			// The last IK solution's local transforms of the nodes it moved. Each evaluation composes Globals from a copy
			// of Pose with these on top, so a pose evaluated again after the IK pass (e.g. Play in OnAnimationEvent)
			// keeps the solution until the next solve.
			std::vector<OverlayEntry> IKOverlay;
			// The last spring bone solution's local transforms of the nodes it turned, applied after IKOverlay the same
			// way. Spring bones solve from Pose with IKOverlay, never from this.
			std::vector<OverlayEntry> SpringOverlay;
			// Physical pose, applied last so callbacks that resample animation keep the ragdoll.
			std::vector<OverlayEntry> RagdollOverlay;
			// The overlays did not compose on a finite pose and were dropped; warned once until an overlay composes again.
			bool OverlayDropped = false;
			// 0 evaluates next update; otherwise the number of updates since the last evaluation.
			uint32_t LODFrame = 0;
			bool Playing = false;
			bool Rest = false;
			bool InvalidPose = false;
			bool PoseJobFailed = false;
			bool InvalidSpeed = false;
			bool InvalidLOD = false;
			bool EventLimitLogged = false;
			// Animator.Clip when it named neither a clip nor a blend space: a blend space of that name added later
			// still starts (until something else plays).
			std::string PendingStart;
			// The last failed PlayBlendSpace's problem, so a script retrying every frame logs it once.
			std::string PlayProblem;
			std::string EntityName;
			std::vector<glm::mat4> Globals;
			std::vector<MorphOverride> MorphOverrides;
			bool InvalidMorphWeights = false;
			// Pose.MorphWeights with the script weights on top (Finish).
			std::vector<float> MorphWeights;
		};
		// Runtime state of one IK chain, matched to its configuration by name every update.
		struct IKChainState
		{
			// A script's world-space target.
			struct PositionTarget
			{
				glm::vec3 Position = glm::vec3(0);
				std::optional<glm::quat> Rotation;
			};
			// An entity followed every frame.
			struct EntityTarget
			{
				UUID Entity = 0;
				bool MatchRotation = false;
			};
			// Foot placement's smoothed ground below the joint: the offset from the entity's origin height along its up
			// axis, and the normal (world space). Placed is false until the first solve with time (seconds > 0), which
			// snaps instead of easing; ClearIKTarget resets it.
			struct GroundContact
			{
				float Offset = 0;
				glm::vec3 Normal = glm::vec3(0, 1, 0);
				bool Placed = false;
			};
			std::string Name;
			FadingWeight Weight;
			// monostate: no script target (ground placement for Ground chains, else idle).
			std::variant<std::monostate, PositionTarget, EntityTarget> Target;
			GroundContact Ground;
			// The last problem solving this chain, logged when it changes.
			std::string Problem;
		};
		// The IK runtime state of one entity. It survives playback resets (targets and weights are gameplay state) and
		// ends with the component.
		struct IKState
		{
			entt::entity Handle = entt::null;
			std::vector<IKChainState> Chains;
			// The last problem with the entity as a whole (its Animator, pose or transform, and out-of-range settings,
			// including chains' Weight and MaxAngle), and the last one lowering the pelvis, each logged when it changes.
			std::string Problem;
			std::string PelvisProblem;
		};
		// Runtime state of one spring chain, matched to its configuration by name every update.
		struct SpringChainState
		{
			std::string Name;
			// The chain resolved against the model: its joints, then the tip, or why it cannot be resolved. It is
			// resolved again (and the chain restarts) when the Joint, the ChainLength or the model changes.
			struct Resolution
			{
				Ref<MeshSource> Source;
				std::string Joint;
				uint32_t Length = 0;
				std::vector<uint32_t> Joints;
				std::string Problem;
				bool Matches(const Ref<MeshSource>& source, const SpringChain& config) const { return Source == source && Joint == config.Joint && Length == config.ChainLength; }
			};
			Resolution Resolved;
			// The simulated children of the joints and where the animation put the joints and the tip at the last solve
			// (world space). Absent until the chain starts on the animated pose: before its first solve, after
			// ResetSpringBones, a teleport, a changed resolution, a frame its entity could not be solved, or points that
			// went non-finite.
			struct Simulation
			{
				std::vector<SpringPoint> Points;
				std::vector<glm::vec3> Animated;
			};
			std::optional<Simulation> Sim;
			// The last problem with this chain, logged when it changes. HoldProblem keeps it through the frame the chain
			// restarts on (when nothing is simulated, so nothing could go wrong yet), so a problem that comes back right
			// after is not logged again.
			std::string Problem;
			bool HoldProblem = false;
		};
		// The spring bone runtime state of one entity. It survives playback resets (the points are where the chains hang
		// in the world) and ends with the component.
		struct SpringState
		{
			// ColliderBinding::Node for a collider whose joint the model does not have; -1 is the entity itself.
			static constexpr int32_t NoNode = -2;
			struct ColliderBinding
			{
				std::string Joint;
				int32_t Node = -1;
			};
			entt::entity Handle = entt::null;
			std::vector<SpringChainState> Chains;
			// Each collider's joint resolved against ColliderSource, resolved again when the joint names or the model
			// change.
			std::vector<ColliderBinding> Colliders;
			Ref<MeshSource> ColliderSource;
			// The last problem with the entity as a whole (its Animator, pose, transform, gravity, wind and colliders),
			// logged when it changes; HoldProblem as in SpringChainState.
			std::string Problem;
			bool HoldProblem = false;
		};
		// A chain's state and configuration, as FindChain returns them.
		struct ChainLookup
		{
			IKChainState* State = nullptr;
			const IKChain* Config = nullptr;
		};
		// Main-thread asset handles resolved once per key during an update. Direct operations resolve afresh.
		struct ResolvedAssets
		{
			Ref<MeshSource> Source;
			Ref<const AnimationGraph> Graph;
		};
		Playback* Ensure(Entity entity, const ResolvedAssets* assets = nullptr);
		// Ensure for an operation: logs why the entity cannot animate (once per entity and reason).
		Playback* EnsurePlayable(Entity entity);
		// Why Ensure returns no playback for the entity.
		static std::string DescribeUnplayable(Entity entity);
		// Fills the state's samples, duration and Resolved from the component and returns the dominant clip; -1
		// (Resolved false) when the space cannot play (see SpaceState::Resolved). `outProblem` also names missing
		// sample clips when the others can play.
		int32_t ResolveSpaceState(const AnimationClipSet& clips, const AnimatorComponent& component, SpaceState& state, std::string& outProblem);
		// Re-resolves the running space and logs a new problem; false when it cannot play.
		bool ResolveSpace(Playback& playback, SpaceState& state, const AnimatorComponent& component);
		static void Evaluate(Playback& playback);
		// Builds Pose from Local and the layers, then composes Globals from Pose with IK, spring and ragdoll overlays on top. Layers
		// leave out the root-motion joint and the nodes above it while root motion applies (also while it fades out).
		static void Finish(Playback& playback);
		// "'Model.glb'" or "'Model.glb' or its N clip libraries", for clip-not-found messages.
		static std::string DescribeClipSources(const Playback& playback);
		// Erases a playback, keeping its script morph weights for the next one.
		void DropPlayback(UUID id);
		// The pose weight slots of a morph target, logging an unknown target for `operation`.
		std::vector<uint32_t> FindMorphTarget(Entity entity, const Playback& playback, const std::string& target, const char* operation);
		// The Animator's clip set for `model`, built (and its problems logged) when the model, ClipLibraries or their
		// loaded sources changed.
		Ref<const AnimationClipSet> ResolveClips(Entity entity, const AnimatorComponent& component, const Ref<MeshSource>& model);
		// Requires Local/Pose/Globals sized by Ensure (which retries incomplete initialization) and immutable source topology.
		// Rewrites those buffers in place, so queued-job recovery needs no pose allocation.
		static void RestoreRestPose(Playback& playback);
		// Matches layer states to the configuration (by name), resolves them and advances their clocks and fades.
		static void AdvanceLayers(Playback& playback, const AnimatorComponent& component, double seconds);
		// Re-resolves the layer's clip, mask and reference pose when what they depend on changed; returns them.
		static const LayerResolution& ResolveLayer(Playback& playback, const AnimationLayer& config, LayerState& state);
		// The layer named `layer` after syncing, or null (reported for `operation`).
		LayerState* FindLayer(Entity entity, Playback& playback, const std::string& layer, const char* operation);
		// Moves BoneAttachment entities to their joints once the rigs are placed (part of LateUpdate).
		void UpdateAttachments();
		// Reconstructs physical local transforms from world-space bodies and blends them over the layered animation.
		void ApplyRagdolls();
		void SolveIK(float seconds);
		void SolveEntityIK(Entity entity, Playback& playback, const InverseKinematicsComponent& component, IKState& state, float seconds);
		IKState& SyncIK(Entity entity, const InverseKinematicsComponent& component);
		void SolveSprings(float seconds);
		void SolveEntitySprings(Entity entity, Playback& playback, const SpringBoneComponent& component, SpringState& state, float seconds);
		SpringState& SyncSprings(Entity entity, const SpringBoneComponent& component);
		// Fills `out` with the colliders in world space from the pose's globals, resolving their joints when needed.
		static void ResolveSpringColliders(const Ref<MeshSource>& source, const SpringBoneComponent& component, SpringState& state, const glm::mat4& world, const std::vector<glm::mat4>& globals, std::vector<SpringCollisionShape>& out, std::vector<std::string>& problems);
		void OnSpringDestroy(entt::registry& registry, entt::entity handle);
		// A pass's overlay: &Playback::IKOverlay, &Playback::SpringOverlay or &Playback::RagdollOverlay.
		using Overlay = std::vector<OverlayEntry> Playback::*;
		// Drops a pass's overlay and composes the pose again from what the other passes left.
		static void DropOverlay(Playback& playback, Overlay overlay);
		// Keeps the local transforms of `nodes` (sorted and deduplicated here) as a pass's overlay and its composed globals
		// as the pose: Pose with the overlays of this pass and the passes before it (IK, springs, then ragdoll). A later pass then
		// stores or drops its own.
		static void StoreOverlay(Playback& playback, Overlay overlay, std::vector<uint32_t>& nodes, const AnimationLocalPose& pose, const std::vector<glm::mat4>& globals);
		// Logs `problem` for `owner` (a component name) when it changed since `reported`, naming `chain` when given.
		static void ReportProblem(std::string& reported, std::string problem, Entity entity, const char* owner, const std::string* chain = nullptr);
		// The problems joined into one message ("; " between them).
		static std::string JoinProblems(const std::vector<std::string>& problems);
		// The states matched to `configs` by Name, in configuration order. Each state moves to at most one configuration
		// (even if C++ code wrote duplicate names past Validate); unmatched configurations get create(config).
		template<typename State, typename Config, typename Create>
		static std::vector<State> MatchByName(std::vector<State> states, const std::vector<Config>& configs, Create create)
		{
			std::vector<State> matched;
			matched.reserve(configs.size());
			std::vector<bool> taken(states.size(), false);
			for (const Config& config : configs)
			{
				const auto found = std::ranges::find_if(states, [&](const State& old) { return !taken[static_cast<size_t>(&old - states.data())] && old.Name == config.Name; });
				if (found != states.end())
				{
					taken[static_cast<size_t>(found - states.begin())] = true;
					matched.push_back(std::move(*found));
				}
				else
					matched.push_back(create(config));
			}
			return matched;
		}
		// The chain named `chain` after syncing, or nulls (reported for `operation`).
		ChainLookup FindChain(Entity entity, const std::string& chain, const char* operation);
		// Logs `problem` (plus `detail`, e.g. the offending values) for one entity, operation and layer, chain or target name
		// when the problem differs from the last one reported there; an empty problem records that operation's success.
		// A script repeating a failing call every frame, between other calls, logs it once. Unknown names share one
		// slot per operation, so scripts cannot grow the record without bound.
		static void ReportCallProblem(std::map<std::pair<UUID, std::string>, std::string>& reported, Entity entity, const char* owner, const char* operation, const std::string& name, const std::string& problem, const std::string& detail = {});
		void ReportLayerCall(Entity entity, const char* operation, const std::string& layer, const std::string& problem, const std::string& detail = {});
		void ReportIKCall(Entity entity, const char* operation, const std::string& chain, const std::string& problem, const std::string& detail = {});
		// A component value C++ code wrote outside [low, high] (Validate rejects it from data): clamped (infinities to the
		// bound on their side), or `fallback` (the component's default) for NaN; the problem (value-free, so a changing
		// value logs once) is appended to `problems`.
		static float ClampSetting(float value, float low, float high, float fallback, const std::string& name, std::vector<std::string>& problems);
		void OnIKDestroy(entt::registry& registry, entt::entity handle);
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
		bool EnterState(Playback& playback, size_t stateIndex, const AnimatorComponent& component, float fade);
		void UpdateGraph(Playback& playback, const AnimatorComponent& component);
		bool SetParameterValue(Entity entity, const std::string& name, float value, AnimationParameterType type);
		static double CycleDuration(const Playback& playback);
		static bool ResolveRootNode(Playback& playback, const AnimatorComponent& component);
		static void ApplyStateAxes(const Playback& playback, const AnimationState& state, SpaceState& space);
		static bool CanExtractRoot(Playback& playback, int32_t clip);
		static void ReportRootClipProblem(Playback& playback, int32_t clip, const std::string& problem);
		static void ResetRootIntervals(Playback& playback);
		static std::optional<glm::mat4> SourceRootDelta(Playback& playback, int32_t clip, double time, const std::optional<SpaceState>& space, double advance, bool looping, bool enabled);
		// A frame's root motion: the extracted delta, and how much of the entity's horizontal movement it owns (0..1):
		// the root-motion side of a fade by its weight, so a character's gameplay movement fills in the rest.
		struct RootMotionFrame
		{
			glm::mat4 Delta = glm::mat4(1);
			float Share = 1.0f;
		};
		static std::optional<RootMotionFrame> FrameRootMotion(Playback& playback, double advance, double shown, double seconds, double plainSpeed);
		static void StripRoot(Playback& playback, AnimationLocalPose& pose, int32_t clip);
		static std::optional<glm::mat4> SpaceRootDelta(Playback& playback, const SpaceState& space, double advance, bool looping);
		static std::optional<glm::mat4> RootDelta(Playback& playback, int32_t clip, double from, double advance, bool looping);
		void ApplyRootMotion(Entity entity, Playback& playback, const RootMotionFrame& motion, float seconds);
		void OnDestroy(entt::registry& registry, entt::entity handle);
		Scene& m_Scene;
		std::unordered_map<UUID, Playback> m_Playback;
		std::unordered_map<entt::entity, bool> m_EmptyMeshes;
		// Built clip sets, found again by model and loaded libraries (see ResolveClips); dropped once no playback uses them.
		std::vector<Ref<const AnimationClipSet>> m_ClipSets;
		// Script morph weights of playbacks dropped while their model cannot load, restored when it can again.
		std::unordered_map<UUID, std::vector<MorphOverride>> m_DetachedMorphOverrides;
		// Script morph weights waiting for a model with their target, warned once each.
		std::set<std::pair<UUID, std::string>> m_WaitingMorphOverrides;
		// Clip library sources by key, resolved once per update.
		std::unordered_map<std::string, Ref<MeshSource>> m_LibrarySources;
		std::vector<AnimationEventRecord> m_PendingEvents;
		// The last problem reported per BoneAttachment entity, so each is logged once.
		std::unordered_map<UUID, std::string> m_AttachmentProblems;
		// The last GetBoneTransform problem reported per entity and joint, and the last unplayable reason per entity.
		std::map<std::pair<UUID, std::string>, std::string> m_BoneProblems;
		std::unordered_map<UUID, std::string> m_UnplayableReported;
		std::unordered_map<UUID, IKState> m_IK;
		std::unordered_map<UUID, SpringState> m_Springs;
		// The last failed script call's problem per entity and operation and layer, chain or morph target name (see
		// ReportCallProblem).
		std::map<std::pair<UUID, std::string>, std::string> m_LayerProblems;
		std::map<std::pair<UUID, std::string>, std::string> m_IKCallProblems;
		std::map<std::pair<UUID, std::string>, std::string> m_MorphProblems;
		// Borrowed, disjoint playback outputs; sampling and composition touch only Target.
		struct PoseJob
		{
			Playback* Target = nullptr;
			bool Sample = true; // true: sample clips and compose; false: sample layers and compose overlays over the held base.
		};
		// Queued during entity updates, executed after that loop and joined before LateUpdate. Addresses survive
		// unordered_map rehashes; no queued playback is erased or externally mutated before the join.
		std::vector<PoseJob> m_PoseJobs;
		// Scratch storage reused every update.
		std::vector<const AnimationEvent*> m_Markers;
		std::vector<float> m_Weights;
		std::vector<glm::mat4> m_OverlayGlobals;
		std::vector<SpringCollisionShape> m_SpringColliders;
		std::vector<glm::vec3> m_SpringAnimated;
		std::vector<glm::vec3> m_SpringTargets;
		// The nodes a pass moved (IK, then springs).
		std::vector<uint32_t> m_TouchedNodes;
	};
}
