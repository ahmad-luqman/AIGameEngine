#pragma once

#include "Basalt/Audio/AudioBus.h"
#include "Basalt/Core/UUID.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Physics/PhysicsMaterial.h"
#include "Basalt/Renderer/PostProcessSettings.h"
#include "Basalt/Scene/SceneCamera.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <string>
#include <vector>

// Components are plain data. Runtime state (physics bodies, script instances, playing sounds) lives in
// the owning systems, keyed by entity UUID, so components can be copied and serialized freely.
//
// Adding a component: define it here, add it to AllComponents below, register its JSON serializer in
// ComponentRegistry.cpp, expose it in the inspector, the Lua API, and the feature test scene.
// See .claude/skills/basalt-add-component.

namespace Basalt {

	struct IDComponent
	{
		UUID ID;
	};

	struct TagComponent
	{
		std::string Tag;
	};

	// Local transform relative to the parent entity (or the world when there is no parent).
	struct TransformComponent
	{
		glm::vec3 Translation = { 0.0f, 0.0f, 0.0f };
		glm::quat Rotation = { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale = { 1.0f, 1.0f, 1.0f };

		glm::mat4 GetTransform() const { return Math::ComposeTransform(Translation, Rotation, Scale); }
		void SetTransform(const glm::mat4& transform) { Math::DecomposeTransform(transform, Translation, Rotation, Scale); }

		// Euler angles in radians.
		glm::vec3 GetRotationEuler() const { return Math::EulerFromQuat(Rotation); }
		void SetRotationEuler(const glm::vec3& euler) { Rotation = Math::QuatFromEuler(euler); }
	};

	// Scene hierarchy. Maintained through Scene::SetParent; do not edit directly.
	struct RelationshipComponent
	{
		UUID Parent = 0;
		std::vector<UUID> Children;
	};

	// Global post-processing; runtime exposure history belongs to each renderer, never to this data.
	// Equal priorities choose the lowest randomly assigned UUID. Disabling all components restores legacy settings.
	struct PostProcessComponent
	{
		bool Enabled = true;
		uint32_t Priority = 0;
		Tonemapper Tonemap = Tonemapper::ACES;
		// Manual multiplier, also applied on top of metered auto exposure.
		float Exposure = 1.0f;
		bool AutoExposure = false;
		float MinExposure = 0.0625f;
		float MaxExposure = 16.0f;
		float ExposureCompensation = 0.0f;
		float AdaptationSpeed = 3.0f;
		bool SSAOEnabled = true;
		float SSAORadius = 0.5f;
		float SSAOIntensity = 1.0f;
		float SSAOBias = 0.025f;
		bool FXAAEnabled = false;
		bool TAAEnabled = false;
		bool BloomEnabled = false;
		float BloomThreshold = 1.0f;
		float BloomIntensity = 0.1f;
		float BloomScatter = 0.7f;
	};

	struct CameraComponent
	{
		SceneCamera Camera;
		bool Primary = true;
		bool FixedAspectRatio = false;
	};

	// Renders one mesh of a mesh asset. Mesh is a project-relative asset path ("Assets/Models/Helmet.gltf")
	// or a built-in primitive ("builtin://Cube", "builtin://Sphere", "builtin://Plane", "builtin://Cylinder",
	// "builtin://Capsule").
	struct MeshComponent
	{
		static constexpr float MaxLODDistance = 1000000;
		std::string Mesh;
		uint32_t MeshIndex = 0;
		bool CastShadows = true;
		// Imported static meshes: camera distance to each submesh's world bounds center for the first reduced level.
		// 0 keeps full detail; subsequent stored levels double the distance.
		float LODDistance = 0;
	};

	inline constexpr float MaxAnimatorSpeed = 100.0f;

	// A named marker on a clip's timeline, delivered to scripts as OnAnimationEvent(name, clip, layer) when playback
	// crosses it.
	struct AnimationEvent
	{
		// Element type name in tooling (component.types, the Lua definitions).
		static constexpr const char* TypeName = "AnimationEvent";

		// Empty marks every clip.
		std::string Clip;
		// Seconds from the clip's start.
		float Time = 0.0f;
		std::string Name = "Event";
	};

	// One clip placed on a blend space's parameter axes. Samples at the same position share its weight.
	struct AnimationBlendSample
	{
		static constexpr const char* TypeName = "AnimationBlendSample";

		// Empty selects the model's first clip.
		std::string Clip;
		float X = 0.0f;
		// 2D blend spaces only.
		float Y = 0.0f;
		// How fast the clip moves the character at playback speed 1 (units/s), for Animator MatchVelocity; 0 for
		// clips that stand still or whose speed is unknown (their weight keeps the space at the Animator's Speed).
		float Speed = 0.0f;
	};

	enum class AnimationBlendSpaceType : uint8_t
	{
		OneD = 0,
		TwoD
	};
	inline constexpr size_t AnimationBlendSpaceTypeCount = 2;

	// Clips placed on one or two parameter axes (speed, or speed x direction), weighted by where the parameter lies
	// among them and played in step.
	struct AnimationBlendSpace
	{
		static constexpr const char* TypeName = "AnimationBlendSpace";
		// Blend spaces are found by name, so an editor gives added and duplicated ones a fresh one.
		static constexpr const char* UniqueField = "Name";

		std::string Name = "BlendSpace";
		AnimationBlendSpaceType Type = AnimationBlendSpaceType::OneD;
		std::vector<AnimationBlendSample> Samples;
	};

	enum class AnimationLayerBlend : uint8_t
	{
		// Moves the pose below toward the clip by the weight, on the channels the clip animates.
		Override = 0,
		// Adds the clip's difference from its reference pose on top.
		Additive // keep last: AnimationLayerBlendCount
	};
	inline constexpr size_t AnimationLayerBlendCount = 2;

	// One entry of a layer's joint mask: the joint and its subtree take Weight, unless a deeper entry names one of
	// its descendants.
	struct AnimationMaskJoint
	{
		static constexpr const char* TypeName = "AnimationMaskJoint";

		std::string Joint;
		float Weight = 1.0f;

		bool operator==(const AnimationMaskJoint&) const = default;
	};

	// Pairs a joint of a clip library's skeleton with a joint of the Animator's model, ahead of the automatic name and
	// humanoid matching.
	struct AnimationRetargetJoint
	{
		static constexpr const char* TypeName = "AnimationRetargetJoint";

		std::string Source;
		std::string Target;
		// A reference pose without a clip: how the Source joint turns (degrees about its model's fixed axes, X, then Y, then Z)
		// so the library stands like the Animator's model at rest, e.g. lifting an A-pose arm to a T-pose. Joints below it turn
		// along; offsets lower down apply after it, in the same fixed axes. On top of ReferenceClip's pose when there is one.
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };

		// Offset by its bits, so a NaN C++ wrote (Retargeting warns about it) equals itself: clip sets compare these every
		// update, and a pair that never matched would rebuild the set and restart the Animator each time.
		bool operator==(const AnimationRetargetJoint& other) const
		{
			using Bits = std::array<uint32_t, 3>;
			return Source == other.Source && Target == other.Target && std::bit_cast<Bits>(Offset) == std::bit_cast<Bits>(other.Offset);
		}
	};

	// A model whose clips an Animator also plays, retargeted onto the Animator's own skeleton. Joints (at most MaxJoints)
	// pairs joints the automatic matching misses or gets wrong; each source and each target joint appears once, which
	// Validate enforces.
	struct AnimationClipLibrary
	{
		static constexpr const char* TypeName = "AnimationClipLibrary";
		static constexpr uint32_t MaxJoints = 256;
		// Upper bound of the reference times (seconds, one day): keeps them finite and sane; clips are far shorter.
		static constexpr float MaxReferenceTime = 86400.0f;

		std::string Mesh;
		std::vector<AnimationRetargetJoint> Joints;
		// A project-relative .bjointmap shared between Animators: its pairs follow Joints, and a map pair is dropped when a
		// complete Joints entry has the same Source or the same Target. One that cannot load makes the library add no clips.
		std::string JointMap;
		// Rest-pose matching for skeletons authored in different rest styles (an A-pose library on a T-pose model): a clip
		// of the library whose pose at ReferenceTime seconds stands like the model's rest pose, and a clip of the model
		// whose pose at ModelReferenceTime stands like the library's rest pose. Either replaces its skeleton's rest
		// orientations when joints turn (positions, and so limb lengths and the root's scale, keep the rest pose). Empty:
		// the rest pose. A clip that is not there makes the library add no clips. A library with no such frame turns its
		// joints with their pairs' Offset, which also adds to a ReferenceClip.
		std::string ReferenceClip;
		float ReferenceTime = 0.0f;
		std::string ModelReferenceClip;
		float ModelReferenceTime = 0.0f;

		bool operator==(const AnimationClipLibrary&) const = default;
	};

	// A clip or state machine played on top of the Animator's base playback (clip, blend, blend space or state machine),
	// in list order: e.g. an upper-body wave masked to the arms, an upper-body graph, or additive breathing.
	struct AnimationLayer
	{
		static constexpr const char* TypeName = "AnimationLayer";
		// Layers are found by name.
		static constexpr const char* UniqueField = "Name";
		static constexpr uint32_t MaxMaskJoints = 64;

		std::string Name = "Layer";
		// Empty: the layer shows nothing until a script plays a clip on it.
		std::string Clip;
		// A project-relative .banimgraph the layer runs on its own (an upper-body graph), replacing Clip; empty plays Clip.
		// Its parameters and triggers share the Animator's names: setting one reaches every graph that declares it, unless
		// OwnParameters.
		std::string StateMachine;
		// StateMachine layers: SetParameter and SetTrigger pass this layer by, so its graph holds its own values of the
		// names it shares with the others and only SetLayerParameter and SetLayerTrigger reach it.
		bool OwnParameters = false;
		// Animator.Events fire for this layer's clips too, while it shows (a weight above 0), naming it in the event. Off by
		// default: a marker without a Clip would fire once for the base and again for the layer.
		bool Events = false;
		AnimationLayerBlend Blend = AnimationLayerBlend::Override;
		// 0..1. The starting weight of the layer; a changed value applies at once.
		float Weight = 1.0f;
		// 0..100; the layer's own clock, not scaled by Animator.Speed.
		float Speed = 1.0f;
		// Clip layers only: a StateMachine's states loop as each of them says.
		bool Looping = true;
		// Additive only: the pose the clip is a difference from, ReferenceClip (empty: the layer's own clip; required with
		// a StateMachine) sampled at ReferenceTime seconds.
		std::string ReferenceClip;
		float ReferenceTime = 0.0f;
		// Empty (or only entries without a Joint): every joint at weight 1. Otherwise joints under no entry take 0.
		std::vector<AnimationMaskJoint> Mask;
		// A project-relative .banimmask shared between Animators; when set, its joints replace Mask, and one that cannot load
		// hides the layer (rather than showing it on the whole body, as an empty Mask would).
		std::string MaskAsset;
	};

	// One normalized-time sample. Number, integer and boolean fields use Value's lanes (capture zeros unused lanes; sampling
	// ignores them); string and enum fields use Text.
	struct PropertyAnimationKey
	{
		static constexpr const char* TypeName = "PropertyAnimationKey";
		static constexpr const char* NormalizedTimeField = "Time";
		// Longest Text, in bytes.
		static constexpr uint32_t MaxTextLength = 1024;
		float Time = 0;
		glm::vec4 Value{ 0.0f };
		std::string Text;
	};
	// A field on this entity, addressed through ComponentRegistry rather than a memory offset. Field is a top-level field
	// ("Translation") or a path into list elements ("Layers[Upper].Weight", "Chains[0].Stiffness"; see
	// PropertyAnimation::ResolveField).
	struct PropertyAnimationTrack
	{
		static constexpr const char* TypeName = "PropertyAnimationTrack";
		std::string Component = "Transform";
		std::string Field = "Translation";
		// Holds each key until the next one. Integer, boolean, string and enum fields always step.
		bool Stepped = false;
		std::vector<PropertyAnimationKey> Keys;
	};
	// Authored property timelines. Playback clocks belong to PropertyAnimationSystem in Play/Simulate.
	struct PropertyAnimationComponent
	{
		static constexpr uint32_t MaxTracks = 64;
		static constexpr uint32_t MaxKeys = 256;
		float Duration = 1;
		float Speed = 1;
		bool Looping = true;
		bool PlayOnStart = true;
		std::vector<PropertyAnimationTrack> Tracks;
	};

	// Draws an entire glTF model with independent skeletal/TRS animation playback.
	struct AnimatorComponent
	{
		static constexpr uint32_t MaxEvents = 256;
		static constexpr uint32_t MaxBlendSpaces = 32;
		static constexpr uint32_t MaxBlendSamples = 64;
		static constexpr uint32_t MaxLayers = 16;
		static constexpr uint32_t MaxLODInterval = 60;
		static constexpr uint32_t MaxClipLibraries = 64;

		std::string Mesh;
		// Empty selects the first clip. May name a blend space (a clip of the same name wins).
		std::string Clip;
		float Speed = 1.0f;
		bool Looping = true;
		bool PlayOnStart = true;
		bool CastShadows = true;
		// Scales a blend space's playback rate by the entity's measured horizontal speed over the moving samples'
		// weighted Speed (for their share of the weight; samples with Speed 0 keep the plain Speed), so feet do not
		// slide. A space of only moving samples freezes while the entity stands still.
		bool MatchVelocity = false;
		std::vector<AnimationEvent> Events;
		std::vector<AnimationBlendSpace> BlendSpaces;
		// Reusable project-relative .banimgraph. Empty keeps direct playback.
		std::string StateMachine;
		// Master gate; graph states must also opt in. RootJoint explicitly names the extraction node.
		bool RootMotion = false;
		std::string RootJoint;
		std::vector<AnimationLayer> Layers;
		// Opt-in pose LOD: beyond this distance from the primary camera evaluate every LODInterval
		// updates. Zero distance disables LOD. Clocks, events and root motion still advance each update.
		float LODDistance = 0.0f;
		uint32_t LODInterval = 1;
		// Models whose clips this Animator also plays (at most MaxClipLibraries), such as a shared Mixamo library,
		// retargeted onto Mesh's skeleton. Clip names (Clip, blend space samples, layers, events, graph states) resolve in
		// Mesh first, then in each library in order. Joints match by the library's Joints, then by name (ignoring
		// "prefix:" and "prefix|"), then as humanoid bones; skeletons in different rest pose styles (T-pose and A-pose)
		// need a reference pose (AnimationClipLibrary::ReferenceClip or ModelReferenceClip).
		std::vector<AnimationClipLibrary> ClipLibraries;
		// Edit mode only: plays the Animator in the scene view (its clip or state machine, layers, inverse kinematics and
		// spring bones), regardless of PlayOnStart, without moving the entity. Feet and spring bones meet the scene's static
		// bodies; events, root-motion displacement, bodies that move, ragdolls and attachments wait for Play.
		bool Preview = false;
	};

	// Keeps the entity at a joint (any node) of an Animator's model while the scene plays or simulates: its
	// world transform becomes the target's bone transform times this offset. Edit mode keeps the authored
	// transform.
	struct BoneAttachmentComponent
	{
		// The entity with the Animator; 0 = this entity's parent.
		UUID Target = 0;
		// Node name in the target's model; the first node with this name is used.
		std::string Joint;
		// Offset in the joint's space.
		glm::vec3 Translation = { 0.0f, 0.0f, 0.0f };
		glm::quat Rotation = { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale = { 1.0f, 1.0f, 1.0f };
	};

	enum class IKChainType : uint8_t
	{
		// Bends Joint's parent and turns its grandparent so Joint reaches the target (legs, arms); Joint itself turns to a
		// target rotation or the ground.
		TwoBone = 0,
		// Turns Joint and ChainLength - 1 of its ancestors so Joint's Axis points at the target (head, spine, aim).
		LookAt,
		// Bends Joint's ChainLength ancestors (FABRIK) so Joint reaches the target (tails, tentacles, three-bone legs);
		// Joint itself turns to a target rotation or the ground.
		Fabrik // keep last: IKChainTypeCount
	};
	inline constexpr size_t IKChainTypeCount = 3;
	// TwoBone and Fabrik chains reach a target with Joint (and can stand on the ground or lock a foot); LookAt chains aim.
	inline constexpr bool IKChainReaches(IKChainType type)
	{
		return type == IKChainType::TwoBone || type == IKChainType::Fabrik;
	}

	// Replaces a Fabrik chain's limits for one of its joints (see IKChain::SwingAngle), all three: what it leaves out is free,
	// not inherited from the chain.
	struct IKJointLimit
	{
		static constexpr const char* TypeName = "IKJointLimit";

		// A joint the chain turns: an ancestor of the chain's Joint. Entries without a name are the inspector's additions, and
		// ignored.
		std::string Joint;
		float SwingAngle = 180.0f;
		float TwistMin = -180.0f;
		float TwistMax = 180.0f;
	};

	// One inverse kinematics chain of an InverseKinematics component. Targets come from scripts (SetIKTarget), then from
	// the component's Target entity (FollowTarget chains), then for Ground chains from the surface under the joint.
	struct IKChain
	{
		static constexpr const char* TypeName = "IKChain";
		// Chains are found by name.
		static constexpr const char* UniqueField = "Name";
		static constexpr uint32_t MaxChainLength = 16;
		static constexpr float MaxSwingAngle = 180.0f;
		static constexpr float MaxTwistAngle = 180.0f;

		std::string Name = "Chain";
		IKChainType Type = IKChainType::TwoBone;
		// TwoBone, Fabrik: the end of the chain (a foot, hand or tail tip). LookAt: the joint that aims (a head, or a hand
		// holding a gun).
		std::string Joint;
		// 1..MaxChainLength. LookAt: how many joints share the turn, Joint first, then its ancestors. Fabrik: how many
		// ancestors of Joint bend (the chain's bones).
		uint32_t ChainLength = 1;
		// LookAt: the aiming direction in Joint's local space.
		glm::vec3 Axis = { 0.0f, 0.0f, 1.0f };
		// LookAt: the largest turn, in degrees (0..180), away from the animated aim.
		float MaxAngle = 60.0f;
		// TwoBone: the direction (not a point) the middle joint bends toward, in the entity's space; zero keeps the
		// animated bend. Fabrik: the side a straight chain is nudged toward when its target lies within reach (zero: +Z, else
		// X or Y).
		glm::vec3 Pole = { 0.0f, 0.0f, 0.0f };
		// TwoBone, Fabrik: place Joint on the ground below it (foot placement) while no other target is set.
		bool Ground = false;
		// Reach for (TwoBone, Fabrik) or aim at (LookAt) the component's Target entity while no script target is set.
		bool FollowTarget = false;
		// TwoBone, Fabrik: hold Joint where it was planted while its animation stands on it, so a planted foot does not
		// slide (see InverseKinematicsComponent::LockSpeed).
		bool FootLock = false;
		// 0..1. The starting weight of the chain; a changed value applies at once.
		float Weight = 1.0f;
		// Fabrik: what each joint the chain turns may do, measured from its rest rotation (the model's, not the animated one).
		// SwingAngle (degrees, 0..180) bounds how far its bone swings away from where the rest pose points it, as seen from
		// its parent; TwistMin..TwistMax (degrees, -180..180) bound the turn about the bone itself. 180 and -180..180 leave
		// it free. At full weight a solved chain keeps them even where that leaves the target out of reach, and turns an
		// animated pose outside them back inside; while the chain has no target the animation plays untouched. The chain's
		// Joint is its end, not a turned joint: its target rotation stays exact.
		float SwingAngle = MaxSwingAngle;
		float TwistMin = -MaxTwistAngle;
		float TwistMax = MaxTwistAngle;
		// Fabrik: what replaces the three above for single joints, at most MaxChainLength.
		std::vector<IKJointLimit> JointLimits;
	};

	// Whether a chain's settings limit any joint, whatever its Type does with them.
	inline bool HasIKJointLimits(const IKChain& chain)
	{
		return chain.SwingAngle != IKChain::MaxSwingAngle || chain.TwistMin != -IKChain::MaxTwistAngle || chain.TwistMax != IKChain::MaxTwistAngle || std::ranges::any_of(chain.JointLimits, [](const IKJointLimit& limit) { return !limit.Joint.empty(); });
	}

	// Inverse kinematics applied to the Animator on the same entity after physics and OnLateUpdate, before
	// attachments and drawing. Chains reach for script targets, the Target entity (FollowTarget chains) or the ground:
	// foot placement casts a ray under each Ground chain's joint, lowers Pelvis by the largest downward offset, and
	// aligns the feet to the surface. FootLock chains hold planted feet in place.
	struct InverseKinematicsComponent
	{
		static constexpr uint32_t MaxChains = 16;

		std::vector<IKChain> Chains;
		// Lowered (never raised) by the largest downward foot offset, each scaled by its chain's weight, so the lower
		// foot can reach the ground; empty: no pelvis adjustment.
		std::string Pelvis;
		// Ground rays run under each foot from RayHeight above the plane through the entity's origin to RayDepth below
		// it (meters, 0..100).
		float RayHeight = 0.5f;
		float RayDepth = 0.5f;
		// Time constant (seconds, 0..100) of the exponential easing of foot offsets and ground normals (the pelvis
		// follows the eased offsets) and of foot locks engaging and releasing; 0 snaps.
		float Smoothing = 0.1f;
		// The entity FollowTarget chains reach for or aim at (its position); 0: none.
		UUID Target = 0;
		// Foot locking (meters, 0..100, in world space): a FootLock chain's joint locks once its target (the animated joint,
		// or its ground point) moves slower than LockSpeed (per second), and holds its position across the entity's up axis
		// (its height follows the animation and the ground) until the animation lifts it LockHeight above its height when
		// locked, the character leaves the ground, or (at once) its target is LockDistance away across the up axis.
		float LockSpeed = 0.5f;
		float LockHeight = 0.05f;
		float LockDistance = 0.3f;
	};

	// Names the colliders (SpringCollider::Name) a spring chain collides with.
	struct SpringChainCollider
	{
		static constexpr const char* TypeName = "SpringChainCollider";

		std::string Name;
	};

	// One chain of a SpringBone component: Joint and the joints below it swing behind the animated pose. Each turned
	// joint aims at its child, which is simulated as a point mass, so the last turned joint needs a child (the tip).
	struct SpringChain
	{
		static constexpr const char* TypeName = "SpringChain";
		// Chains are found by name.
		static constexpr const char* UniqueField = "Name";
		static constexpr uint32_t MaxChainLength = 16;
		// Upper bounds of the settings below. MaxStiffness keeps the integration stable at the simulation's
		// substep (see SpringStepRate in Animation/SpringBones.h).
		static constexpr float MaxStiffness = 10000.0f;
		static constexpr float MaxDamping = 1000.0f;
		static constexpr float MaxGravityFactor = 100.0f;
		static constexpr float MaxRadius = 100.0f;
		static constexpr float MaxAngleLimit = 180.0f;

		std::string Name = "Chain";
		// The first joint that swings (hair root, cape top, tail base); its parent follows the animation.
		std::string Joint;
		// How many joints swing (0..MaxChainLength), Joint first, each the first child of the one before; 0 follows the
		// first children down to the last joint that still has a child, at most MaxChainLength joints.
		uint32_t ChainLength = 0;
		// Pull (1/s², 0..MaxStiffness) of each point toward where the animated shape puts it; 0 hangs freely.
		float Stiffness = 40.0f;
		// Velocity decay (1/s, 0..MaxDamping) of the world-space motion, like air drag; higher settles sooner.
		float Damping = 4.0f;
		// Scales the physics gravity (-MaxGravityFactor..MaxGravityFactor); 0 ignores gravity.
		float GravityFactor = 1.0f;
		// Radius of each simulated point against the colliders and, with CollidePhysics, physics bodies (0..MaxRadius, in
		// the entity's units: it scales with the entity).
		float Radius = 0.02f;
		// 0..1. Blend between the animated pose (0) and the simulation (1); the simulation keeps running at 0.
		float Weight = 1.0f;
		// Degrees (0..MaxAngleLimit): how far each bone may swing away from where the animated shape (turned with the
		// swung bones before it) points it; MaxAngleLimit leaves it free, 0 keeps the animated shape. Colliders and physics
		// bodies win over the limit.
		float MaxAngle = MaxAngleLimit;
		// The component's colliders this chain stays outside: only those whose Name is listed (at most
		// SpringBoneComponent::MaxColliders entries), or every collider when no entry has a Name (entries without one are
		// the inspector's unnamed additions). Listed names that match no collider are warned about; when none matches, the
		// chain collides with no collider.
		std::vector<SpringChainCollider> Colliders;
		// Also stays outside physics bodies (not triggers, nor the entity, its descendants or its character controller's
		// entity and descendants). One way: the points never push the bodies. In Play and Simulate that is every body; the
		// editor's preview, which has no physics world, sees the static ones.
		bool CollidePhysics = false;
		// With CollidePhysics, the physics layer the chain counts as: it meets the bodies on layers the collision matrix
		// lets this one collide with. Empty: every layer. A name the project does not define counts as Default, as for a
		// body.
		std::string Layer;
		// Each substep also checks the way a point moved, not only where it ended, so a fast point stops at a collider or
		// body too thin to catch it there. Costs up to three rounds of sweeps per point and substep; a point that an obstacle would part
		// from its bone by more than half the bone's length lets go, so a chain whose head went past cannot hang behind.
		bool Continuous = false;
	};

	// A sphere or capsule attached to a joint that keeps the chains of its SpringBone component outside it (a head under
	// hair, legs in front of a cape): every chain without a Colliders list, and those that list its Name. It follows the
	// pose before any chain swings, so a collider on a swinging joint does not move with the swing.
	struct SpringCollider
	{
		static constexpr const char* TypeName = "SpringCollider";
		static constexpr float MaxSize = 100.0f;

		// Chains name it in their Colliders; several colliders may share a name, which then names them all.
		std::string Name;
		// The joint the collider moves with; empty: the entity itself.
		std::string Joint;
		// Center in the joint's (or the entity's) space.
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
		// 0..MaxSize, in the joint's (or the entity's) space: it scales with that world transform, by its average axis
		// scale.
		float Radius = 0.1f;
		// 0: a sphere; otherwise a capsule along the joint's local Y axis, HalfHeight (0..MaxSize) to each side of
		// Offset.
		float HalfHeight = 0.0f;
	};

	// Secondary motion for hair, capes, tails and ears, applied to the Animator on the same entity after inverse
	// kinematics and before attachments: each chain's joints follow simulated points that lag behind the
	// character's motion, sag under gravity and wind, and spring back to the animated pose.
	struct SpringBoneComponent
	{
		static constexpr uint32_t MaxChains = 16;
		static constexpr uint32_t MaxColliders = 32;

		std::vector<SpringChain> Chains;
		std::vector<SpringCollider> Colliders;
		// World-space acceleration (m/s²) added to every point, for wind or gusts set from scripts.
		glm::vec3 Wind = { 0.0f, 0.0f, 0.0f };
	};

	// Optional override for a skin joint's generated ragdoll body and its constraint to its parent.
	struct RagdollBone
	{
		static constexpr const char* TypeName = "RagdollBone";
		static constexpr const char* UniqueField = "Joint";
		std::string Joint = "Joint";
		// Joint-local units, including the joint/armature world scale; zero derives size from the rest skeleton.
		// Length includes the round ends.
		float Radius = 0.0f;
		float Length = 0.0f;
		// Requested kilograms; zero uses Mass / body count. Stabilization redistributes extreme mass ratios.
		float Mass = 0.0f;
		// Degrees from the rest pose; twist is around the bone's length axis.
		float SwingAngle = 45.0f;
		float TwistMin = -30.0f;
		float TwistMax = 30.0f;
		// 0..1: scales the rig's motor torque for the joint above this bone, so one limb can go limp while the others hold
		// their pose (the rig's root has no joint above it). Applies live, without rebuilding the rig.
		float MotorStrength = 1.0f;
		bool operator==(const RagdollBone&) const = default;
	};

	// Skeleton physics for the Animator on this entity: passive, or powered by motors that track the animated pose.
	// Runtime bodies/poses belong to PhysicsWorld.
	struct RagdollComponent
	{
		static constexpr uint32_t MaxBones = 128;
		static constexpr float MaxRecoveryTime = 60.0f;
		static constexpr float MinMotorFrequency = 0.1f;
		static constexpr float MaxMotorFrequency = 30.0f;
		static constexpr float MaxMotorDamping = 10.0f;
		static constexpr float MaxMotorTorque = 1000000.0f;
		static constexpr float MaxTiltLimit = 180.0f;
		bool Enabled = false;
		// Empty selects all skin joints; otherwise selects this skin joint and its descendants.
		std::string RootJoint;
		// RootJoint's body follows the animation instead of falling, so only what hangs from it is physical: an upper body
		// reacting to hits while the legs keep walking. The entity keeps its rigid body or character controller, root
		// motion and inverse kinematics, and recovery leaves it where it is. The limbs never collide with those bodies (or
		// a character controller on an ancestor). Needs a skin joint below the root.
		bool KinematicRoot = false;
		float Mass = 70.0f;
		// Generated radius as a fraction of the rest bone length (0.01..0.5).
		float RadiusRatio = 0.15f;
		std::string Layer = "Default";
		// Seconds to blend the last physical pose back to animation when Enabled becomes false.
		float RecoveryTime = 0.5f;
		std::vector<RagdollBone> Bones;
		// 0..1; 0 keeps the ragdoll passive. While active, each joint's motor turns it toward the animated pose (stumbles,
		// hit reactions) with up to MotorTorque * MotorStrength. The starting strength: a changed value applies at once,
		// and SetRagdollMotorStrength fades it. Motor settings never rebuild the ragdoll.
		float MotorStrength = 0.0f;
		// Hz (MinMotorFrequency..MaxMotorFrequency): how stiffly the motors spring toward the pose.
		float MotorFrequency = 8.0f;
		// 0..MaxMotorDamping: the motors' damping ratio (1 settles without overshoot).
		float MotorDamping = 1.0f;
		// N·m (0..MaxMotorTorque): the largest torque each joint's motor applies at strength 1.
		float MotorTorque = 500.0f;
		// 0..1; 0 leaves balance off. While the motors run and a part below the rig's center of mass stands on something, the
		// whole rig is turned back toward the animated root's uprightness about its center of mass, so no joint has to
		// carry it, and pushed back up to the animated pose's height with a brake for the grip that takes off the feet. Scaled
		// by the motor strength, so a fade to 0 lets the rig fall. Has no effect with KinematicRoot. Balance settings never
		// rebuild the ragdoll.
		float BalanceStrength = 0.0f;
		// Hz (MinMotorFrequency..MaxMotorFrequency): how stiffly the rig is turned upright and lifted.
		float BalanceFrequency = 3.0f;
		// 0..MaxMotorDamping: the damping ratio of that turn and lift (1 settles without overshoot).
		float BalanceDamping = 1.0f;
		// Degrees (0..MaxTiltLimit) away from the animation's uprightness: past this the rig has lost its balance and falls,
		// until something rights it. At 0 it needs to be exactly upright, which it never quite is.
		float BalanceMaxTilt = 60.0f;
		bool operator==(const RagdollComponent&) const = default;
	};

	// Replaces the whole imported material of every submesh. Material names a material asset: a .bmaterial file or a
	// model's imported material ("<model>#material/<n>"), which carry the full model (alpha mode and cutoff, sidedness,
	// normal/occlusion strengths, per-map UV transforms and sets). While Material is set the inline values below are
	// unused, except as the fallback while the asset has not loaded (a model's material while the model streams in) or
	// when it cannot load (logged once).
	// The inline values are a PBR material with clearcoat/transmission, single-sided with strengths of 1: the one UV
	// transform applies to every map on UV0. They blend when AlbedoColor.a < 1 (alpha is then coverage) or
	// Transmission > 0 (full coverage, see-through). Texture paths are project-relative; empty means "no texture".
	struct MaterialComponent
	{
		std::string Material;
		glm::vec4 AlbedoColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		glm::vec3 EmissiveColor = { 0.0f, 0.0f, 0.0f };
		float EmissiveIntensity = 1.0f;
		std::string AlbedoMap;
		std::string NormalMap;
		std::string MetallicRoughnessMap;
		std::string OcclusionMap;
		std::string EmissiveMap;
		float Clearcoat = 0.0f;
		float ClearcoatRoughness = 0.0f;
		float Transmission = 0.0f;
		std::string ClearcoatMap;
		std::string ClearcoatRoughnessMap;
		std::string ClearcoatNormalMap;
		std::string TransmissionMap;
		glm::vec2 UVOffset = glm::vec2(0);
		glm::vec2 UVScale = glm::vec2(1);
		float UVRotation = 0.0f; // Radians; shared by this override's textures.
	};

	// Light shining along the entity's forward axis (-Z). A scene may have several; only the primary one
	// (see CollectDirectionalLights) renders shadows, the others light the scene unshadowed.
	struct DirectionalLightComponent
	{
		glm::vec3 Color = { 1.0f, 1.0f, 1.0f };
		float Intensity = 3.0f;
		bool CastShadows = true;
		// Penumbra size for soft shadows (light source angular size); 0 gives hard PCF edges.
		float ShadowSoftness = 1.0f;
	};

	// Omnidirectional local light; optional shadows use six atlas tiles.
	struct PointLightComponent
	{
		glm::vec3 Color = { 1.0f, 1.0f, 1.0f };
		float Intensity = 10.0f;
		float Radius = 10.0f;
		bool CastShadows = false;
		// Texel radius (unlike directional angular softness), [0, MaxLocalShadowSoftness].
		// Zero keeps hardware comparison filtering.
		float ShadowSoftness = 1.0f;
	};

	// Cone light along the entity's forward axis (-Z). Angles are in degrees.
	struct SpotLightComponent
	{
		glm::vec3 Color = { 1.0f, 1.0f, 1.0f };
		float Intensity = 20.0f;
		float Range = 15.0f;
		float InnerConeAngle = 20.0f;
		float OuterConeAngle = 30.0f;
		bool CastShadows = false;
		// Texel radius (unlike directional angular softness), [0, MaxLocalShadowSoftness].
		// Zero keeps hardware comparison filtering.
		float ShadowSoftness = 1.0f;
	};

	// Image-based lighting and background from an equirectangular HDRI (.hdr).
	// Without an environment map, a procedural sky gradient is used.
	struct SkyLightComponent
	{
		std::string EnvironmentMap;
		float Intensity = 1.0f;
		// Rotation of the environment around the world Y axis, in degrees.
		float Rotation = 0.0f;
		bool ShowBackground = true;
	};

	enum class RigidBodyType
	{
		Static = 0,
		Dynamic,
		Kinematic
	};

	struct RigidBodyComponent
	{
		RigidBodyType Type = RigidBodyType::Static;
		float Mass = 1.0f;
		float LinearDamping = 0.05f;
		float AngularDamping = 0.05f;
		float GravityFactor = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		PhysicsCombineMode FrictionCombine = PhysicsCombineMode::Default;
		PhysicsCombineMode RestitutionCombine = PhysicsCombineMode::Default;
		// Triggers report overlaps to scripts but do not collide.
		bool IsTrigger = false;
		// Prevents the body from rotating (character controllers, upright objects).
		bool FixedRotation = false;
		// Name of a project physics layer; the project's collision matrix decides which layers collide.
		std::string Layer = "Default";
		// Sweeps the body between steps so fast objects (projectiles) do not tunnel through thin geometry.
		// Costs more per step; only affects dynamic bodies that are not triggers.
		bool Continuous = false;
		// Lets the physics engine stop simulating the body after it has barely moved for about half a second; it
		// wakes when something touches, pushes or moves it. Turn it off for bodies that must never stall, such as a
		// ball slowly turning around on a slope (sleeping freezes it there); dynamic bodies resting on or jointed to such
		// a dynamic body stay awake with it (Jolt does not link kinematic bodies into islands). Only affects dynamic and
		// kinematic bodies that are not triggers (triggers never sleep).
		bool AllowSleeping = true;
	};

	// Collider shapes are sized in local space and scaled by the entity's world scale. A scale change during
	// play (the entity's or a parent's) resizes the body's shape in place: its velocity and contacts carry on,
	// and its joints are rebuilt from the current poses (their anchors scale with the body).
	// Every collider can set OverrideMaterial to touch with its own Friction and Restitution instead of the
	// RigidBody's (still combined with the other body by the RigidBody's combine modes and clamped the same way),
	// e.g. an icy runner under a grippy cart. Characters ignore it (with a warning).
	struct BoxColliderComponent
	{
		glm::vec3 HalfExtents = { 0.5f, 0.5f, 0.5f };
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
		bool OverrideMaterial = false;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		bool operator==(const BoxColliderComponent&) const = default;
	};

	struct SphereColliderComponent
	{
		float Radius = 0.5f;
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
		bool OverrideMaterial = false;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		bool operator==(const SphereColliderComponent&) const = default;
	};

	// Capsule aligned with the local Y axis. Total height = 2 * (HalfHeight + Radius).
	struct CapsuleColliderComponent
	{
		float Radius = 0.5f;
		float HalfHeight = 0.5f;
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
		bool OverrideMaterial = false;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		bool operator==(const CapsuleColliderComponent&) const = default;
	};

	// Collides by the triangles of a mesh asset (the entity also needs a RigidBodyComponent). Static and
	// kinematic bodies use the exact triangle mesh. Two of those never meet: Jolt skips pairs without a dynamic
	// body unless one opts in, which Basalt lets only triggers do, and triggers, dynamic bodies and characters
	// are always convex.
	// Bodies with Convex set use the convex hull of the mesh's drawn vertices; so do dynamic bodies (Jolt
	// cannot simulate a triangle mesh; a warning asks to set Convex) and triggers (a triangle mesh has no
	// inside). An empty Mesh uses the entity's MeshComponent (its Mesh and MeshIndex; this MeshIndex is then
	// ignored), so an imported node's mesh needs no path. Scaled by the entity's world scale, mirroring
	// included; cooked shapes are shared by every entity using the same mesh.
	struct MeshColliderComponent
	{
		std::string Mesh;
		uint32_t MeshIndex = 0;
		bool Convex = false;
		bool OverrideMaterial = false;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		bool operator==(const MeshColliderComponent&) const = default;
	};

	enum class JointType
	{
		// Locks the relative position and rotation.
		Fixed = 0,
		// Ball-and-socket: shares the anchor point, rotates freely.
		Point,
		// Rotates around Axis through the anchor (doors, wheels, pendulums).
		Hinge,
		// Translates along Axis without rotating (pistons, rails).
		Slider,
		// Keeps the anchors at a fixed distance, or within [LimitMin, LimitMax] (ropes).
		Distance,
		// Ball-and-socket whose Axis stays within LimitMax degrees of where it started (simple ragdoll limbs,
		// hanging lamps).
		Cone,
		// Each translation and rotation of the joint frame locked or limited on its own (ragdolls).
		SixDOF
	};

	enum class JointMotorMode
	{
		Off = 0,
		// Drives toward its target as a speed (degrees/s for hinges, m/s for sliders).
		Velocity,
		// Drives toward its target as a position (degrees for hinges, meters for sliders).
		Position
	};

	// Constrains the rigid body of BodyEntity (this entity when 0) to the body of ConnectedEntity, or to the
	// world when that is 0. The joint is built from the bodies' poses when physics starts, when either body
	// is rebuilt, or when a structural field changes (Type, an entity, an anchor, an axis or UseLimits): that
	// relative pose is the joint's rest position, so hinge angles, slider offsets and six-DOF limits are
	// measured from it. The other fields update the live joint in place.
	// An entity holds one JointComponent. To give a body several joints (a ladder rung held by two ropes),
	// put each joint on its own entity, usually a child of the body, with BodyEntity set to the body.
	// Which fields each type reads is decided in one place, JointFieldApplies (Basalt/Scene/JointFields.h).
	struct JointComponent
	{
		JointType Type = JointType::Hinge;
		// The entity whose body this joint moves; 0 = the entity holding the component. Anchor and the axes
		// are in this body's local space; a separate holder's own transform plays no part.
		UUID BodyEntity = 0;
		UUID ConnectedEntity = 0;
		// Attachment point in the body's local space.
		glm::vec3 Anchor = { 0.0f, 0.0f, 0.0f };
		// Distance joints only: attachment point on the connected entity (its local space), or a world
		// position when connected to the world.
		glm::vec3 ConnectedAnchor = { 0.0f, 0.0f, 0.0f };
		// Hinge rotation axis, slider direction, cone axis or six-DOF twist axis (X of the joint frame), in
		// the body's local space.
		glm::vec3 Axis = { 0.0f, 1.0f, 0.0f };
		// Six-DOF only: Y of the joint frame (made perpendicular to Axis); Z is Axis x SecondaryAxis.
		glm::vec3 SecondaryAxis = { 1.0f, 0.0f, 0.0f };
		// Hinge: angles in degrees with -180 <= LimitMin <= 0 <= LimitMax <= 180; slider: offsets in meters
		// with LimitMin <= 0 <= LimitMax; distance: lengths in meters with 0 <= LimitMin <= LimitMax; cone:
		// LimitMax is the cone's half angle in degrees (0..180) and LimitMin is unused. Six-DOF joints use
		// the Linear/Angular limits below instead. Fixed and point joints have no limits. Without limits a
		// distance joint keeps its starting length and a cone joint swings freely.
		bool UseLimits = false;
		float LimitMin = 0.0f;
		float LimitMax = 0.0f;
		// Softens the limits into a spring (hinge, slider, distance and six-DOF translation): 0 Hz = rigid.
		// Damping is a ratio, 1 = critically damped; lower values bounce. A spring on a distance joint without
		// limits pulls it back to its starting length (a bungee).
		float LimitSpringFrequency = 0.0f;
		float LimitSpringDamping = 1.0f;
		// Six-DOF limits along/around the joint frame's X (Axis), Y and Z, used when UseLimits is set (without
		// limits a six-DOF joint locks translation and rotates freely). Translation in meters, rotation in
		// degrees; every range must contain 0 (the rest pose), and min == max locks that axis. X rotation
		// (twist) lies within [-180, 180]; Y and Z rotation (swing) form a symmetric cone whose half angles are
		// AngularLimitMax.y and .z (0..180): AngularLimitMin.y/.z of 0 means -max (Jolt's cone swing cannot be
		// one-sided). Jolt locks an angle limit within 0.5 degrees of 0 and frees one within 0.5 of 180. Limit
		// springs soften only the limited (not locked or free) translation axes; Jolt has no soft rotation limits.
		glm::vec3 LinearLimitMin = { 0.0f, 0.0f, 0.0f };
		glm::vec3 LinearLimitMax = { 0.0f, 0.0f, 0.0f };
		glm::vec3 AngularLimitMin = { 0.0f, 0.0f, 0.0f };
		glm::vec3 AngularLimitMax = { 0.0f, 0.0f, 0.0f };
		// Six-DOF only: translation axes of the joint frame that move freely, with or without UseLimits (their
		// LinearLimitMin/Max are ignored). Rotation needs no flag: a +-180 degree limit already frees it.
		glm::bvec3 FreeLinearAxes = { false, false, false };
		// Hinge and slider joints only.
		JointMotorMode MotorMode = JointMotorMode::Off;
		float MotorTarget = 0.0f;
		// Six-DOF only: a motor per axis of the joint frame (X = Axis), so animation can drive a ragdoll. Each
		// axis reads its own component of the target: LinearMotorTarget in m/s (Velocity) or meters from the
		// rest pose (Position); AngularMotorTarget in degrees/s around that axis as it turns with this body
		// (Velocity; Jolt's convention, which differs from the connected body's frame once the joint is
		// bent) or, for the
		// Position axes together, the target orientation relative to the rest pose as Euler angles in degrees
		// (the same convention as Transform Rotation and GetJointRotation; components of non-Position axes are
		// treated as 0). Position targets beyond the limits are clamped to them. A motor on a locked axis does
		// nothing (without UseLimits translation is locked).
		std::array<JointMotorMode, 3> LinearMotorMode = { JointMotorMode::Off, JointMotorMode::Off, JointMotorMode::Off };
		std::array<JointMotorMode, 3> AngularMotorMode = { JointMotorMode::Off, JointMotorMode::Off, JointMotorMode::Off };
		glm::vec3 LinearMotorTarget = { 0.0f, 0.0f, 0.0f };
		glm::vec3 AngularMotorTarget = { 0.0f, 0.0f, 0.0f };
		// Strongest force (N) a slider or six-DOF translation motor may apply.
		float MotorMaxForce = 1000.0f;
		// Strongest torque (N·m) a hinge or six-DOF rotation motor may apply. 0 (the default) uses MotorMaxForce,
		// which capped both before this field existed; so does a negative value, with a warning. Separate because
		// one six-DOF joint drives in two units: a strong translation drive with a gentle rotation one.
		float MotorMaxTorque = 0.0f;
		// The spring a Position motor pulls toward its target with (hinge, slider, six-DOF): a higher frequency
		// (Hz, > 0) is stiffer, still capped by the motor's force or torque. Damping is a ratio, 1 = critically
		// damped.
		float MotorSpringFrequency = 2.0f;
		float MotorSpringDamping = 1.0f;
		// The joint breaks when its constraint force (N) or torque (N·m), motor and limit effort included,
		// exceeds these; 0 never breaks. Point and distance joints hold no torque, so BreakTorque does not
		// apply to them.
		float BreakForce = 0.0f;
		float BreakTorque = 0.0f;
		// Whether the two connected bodies collide with each other.
		bool EnableCollision = false;

		bool operator==(const JointComponent&) const = default;
	};

	// Moves the entity as a game character (Jolt's CharacterVirtual) instead of a rigid body: it slides along
	// walls, walks up slopes and steps within its limits, rides moving platforms and pushes dynamic bodies.
	// Its shape is the entity's colliders. Scripts drive it with Move(velocity); while airborne, gravity
	// (scaled by GravityFactor) takes over the vertical speed. A RigidBodyComponent on the same entity is
	// ignored. Other bodies, queries and triggers see the character as a kinematic body at 90% of the
	// colliders' size. Characters meet each other's full shapes instead and push each other with their
	// velocity (when their layers collide); they raise no collision events with each other.
	struct CharacterControllerComponent
	{
		static constexpr float MaxJumpGrace = 10.0f;

		// Steepest ground (degrees from horizontal, 1..90) the character stands on and walks up; it slides
		// down anything steeper.
		float SlopeLimit = 45.0f;
		// Tallest step (m) the character climbs while walking; it also stays on the ground over drops this
		// small when walking down stairs or slopes. 0 turns both off.
		float StepHeight = 0.3f;
		// Strongest force (N) the character pushes dynamic bodies with.
		float MaxStrength = 100.0f;
		// Mass (kg) pressing down on dynamic bodies the character stands on.
		float Mass = 70.0f;
		// Scales the scene gravity; 0 makes the Move velocity apply in full on every axis (flying, ladders).
		float GravityFactor = 1.0f;
		// Seconds (0..MaxJumpGrace, counted in physics steps) after leaving walkable ground without jumping during which
		// an upward Move still jumps once (coyote time); 0 jumps only from the ground.
		float JumpGrace = 0.0f;
		// Name of a project physics layer, as on RigidBodyComponent.
		std::string Layer = "Default";

		bool operator==(const CharacterControllerComponent&) const = default;
	};

	// Lua behaviour. Script is a project-relative .lua path; Properties overrides the defaults the script
	// declares in its Properties table (numbers, booleans, strings, vectors as [x, y, z]).
	struct ScriptComponent
	{
		std::string Script;
		nlohmann::json Properties = nlohmann::json::object();
	};

	struct AudioSourceComponent
	{
		std::string Clip;
		float Volume = 1.0f;
		float Pitch = 1.0f;
		bool Loop = false;
		bool PlayOnStart = false;
		// Spatial sources are attenuated and panned relative to the active listener.
		bool Spatial = true;
		float MinDistance = 1.0f;
		float MaxDistance = 50.0f;
		// SFX by default: routing straight to Master would put the sound outside every category volume.
		AudioBus Bus = AudioBus::SFX;
		// Decodes the clip while it plays instead of loading it whole: for long music and ambience.
		bool Stream = false;
		// The effects below apply while the sound plays (the AudioSystem re-reads them every frame), so scripts can
		// drive them: occlusion, entering a hall. The fields above apply on the next Play.
		// Strength of the Doppler pitch shift from the source's and the listener's velocities: 0 = off, 1 =
		// physical. Spatial sources only.
		float Doppler = 0.0f;
		// Low-pass cutoff in Hz (muffles the sound, e.g. behind a wall); 0 = off.
		float LowPass = 0.0f;
		// Share of the sound fed to its bus's reverb, 0..1; the dry sound is unchanged.
		float ReverbSend = 0.0f;
		// High-pass cutoff in Hz, 0 = off. Echo wet gain and soft distortion drive: 0..1.
		float HighPass = 0.0f;
		float Echo = 0.0f;
		float Distortion = 0.0f;
	};

	// Spherical room centered at the entity's world position. Radius and BlendDistance are world units,
	// unaffected by scale. Zones blend inward from their boundary over lower priorities; lowest UUID
	// blends last on ties. Room settings are each 0..1 and containment follows the listener.
	struct AudioReverbZoneComponent
	{
		bool Enabled = true;
		uint32_t Priority = 0;
		float Radius = 5.0f;
		float BlendDistance = 1.0f;
		float RoomSize = 0.5f;
		float Damping = 0.25f;
		float Width = 1.0f;
	};

	struct AudioListenerComponent
	{
		bool Active = true;
	};

	// Each particle enum's value count follows it: name tables, the emitter rules and the renderer's
	// pipelines size themselves from it, so a new value cannot be half supported.
	enum class ParticleEmitterShape
	{
		Point = 0,
		Sphere,
		Box,
		Cone
	};
	constexpr size_t ParticleEmitterShapeCount = 4;

	enum class ParticleSimulationSpace
	{
		World = 0,
		Local
	};
	constexpr size_t ParticleSimulationSpaceCount = 2;

	enum class ParticleBlendMode
	{
		Alpha = 0,
		Additive
	};
	constexpr size_t ParticleBlendModeCount = 2;

	// A normalized lifetime key. Curves interpolate linearly and clamp outside their key range.
	struct ParticleSizeKey
	{
		static constexpr const char* TypeName = "ParticleSizeKey";
		static constexpr const char* NormalizedTimeField = "Time";
		float Time = 0.0f;
		float Value = 1.0f;
		bool operator==(const ParticleSizeKey&) const = default;
	};
	struct ParticleColorKey
	{
		static constexpr const char* TypeName = "ParticleColorKey";
		static constexpr const char* NormalizedTimeField = "Time";
		float Time = 0.0f;
		glm::vec4 Value{ 1.0f };
		bool operator==(const ParticleColorKey&) const = default;
	};

	// Emits camera-facing particles while the scene plays/simulates or edit Preview is on (ParticleSystem uses the
	// CPU, the renderer draws them in the transparent pass). Shapes and Velocity are in the emitter's
	// local space; Box and Cone emit along local +Y, Point and Sphere in every direction. Each emission cycle
	// lasts Duration seconds and starts with BurstCount particles; Rate emits continuously in between.
	// Limit constants are below; the full per-field rules are in Particles/ParticleEmitterRules.cpp.
	struct ParticleEmitterComponent
	{
		// Largest MaxParticles and BurstCount.
		static constexpr uint32_t MaxParticlesLimit = 10000;
		static constexpr uint32_t MaxCurveKeys = 16;
		// Shortest and longest Duration and lifetime (s). The longest keeps a particle's age advancing at
		// small time steps (float precision) and its motion far from overflow.
		static constexpr float MinTime = 1e-3f;
		static constexpr float MaxTime = 1e5f;
		// Largest Rate (particles/s).
		static constexpr float MaxRate = 1e6f;
		// Largest magnitude of speeds, Velocity, GravityScale, Drag, shape and particle sizes and color
		// channels: with MaxTime, keeps positions, sizes and colors finite through a particle's whole life.
		static constexpr float MaxMagnitude = 1e4f;
		// Largest ConeAngle (degrees): a full sphere.
		static constexpr float MaxConeAngle = 180.0f;

		bool PlayOnStart = true;
		// Restarts the cycle (and its burst) every Duration seconds; otherwise emission stops after one cycle.
		bool Looping = true;
		float Duration = 5.0f;
		// Cap on live particles; emission beyond it is dropped, not deferred. Lowering it during play removes
		// the excess.
		uint32_t MaxParticles = 1000;

		// Particles per second.
		float Rate = 10.0f;
		uint32_t BurstCount = 0;
		ParticleEmitterShape Shape = ParticleEmitterShape::Point;
		// Sphere radius, or the cone's base radius.
		float ShapeRadius = 0.5f;
		// Half-extents of the Box shape.
		glm::vec3 BoxExtents = { 0.5f, 0.5f, 0.5f };
		// Half-angle of the Cone shape in degrees (0..180; 180 emits in every direction).
		float ConeAngle = 25.0f;

		// Seconds a particle lives, picked uniformly between the two (in either order).
		float LifetimeMin = 1.0f;
		float LifetimeMax = 2.0f;
		// Initial speed along the shape's emission direction (m/s), picked uniformly between the two (in
		// either order); negative speeds emit against that direction.
		float SpeedMin = 1.0f;
		float SpeedMax = 2.0f;
		// Added to every particle's initial velocity (m/s, along the emitter's rotated axes; not scaled).
		glm::vec3 Velocity = { 0.0f, 0.0f, 0.0f };
		// Scales the scene's physics gravity.
		float GravityScale = 0.0f;
		// Linear drag rate (1/s, >= 0): speed decays by about e^-Drag per second (1 leaves ~37% after a second).
		float Drag = 0.0f;
		// World: particles stay where they were emitted when the emitter moves. Local: they move (and turn)
		// with it. Either way the shape scales with the emitter and speeds are in m/s.
		ParticleSimulationSpace SimulationSpace = ParticleSimulationSpace::World;

		// Linear HDR color and opacity (0..1) at birth and at death, interpolated over the particle's life.
		glm::vec4 StartColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		glm::vec4 EndColor = { 1.0f, 1.0f, 1.0f, 0.0f };
		// Billboard width and height (m) at birth and at death.
		float StartSize = 0.2f;
		float EndSize = 0.2f;
		// Multipliers sampled once at birth; equal endpoints draw no random value and defaults (1) leave
		// size/color unchanged. Color channels 0..1 only darken or fade.
		float StartSizeMin = 1.0f;
		float StartSizeMax = 1.0f;
		glm::vec4 StartColorMin{ 1.0f };
		glm::vec4 StartColorMax{ 1.0f };
		// Roll about the view axis: radians and radians/s in C++, degrees and degrees/s in JSON/Lua.
		float RotationMin = 0.0f;
		float RotationMax = 0.0f;
		float AngularVelocityMin = 0.0f;
		float AngularVelocityMax = 0.0f;
		// Nonempty curves replace Start/End interpolation; random birth multipliers still apply.
		std::vector<ParticleSizeKey> SizeCurve;
		std::vector<ParticleColorKey> ColorCurve;
		// Fade distance against opaque depth in world units; 0 disables soft intersections.
		float SoftDistance = 0.0f;
		// Two-sided diffuse ambient/IBL and direct lighting, without shadows or specular highlights.
		bool Lit = false;
		// Editor-only playback independent of PlayOnStart; toggle off/on to restart a completed cycle.
		bool Preview = false;
		ParticleBlendMode Blend = ParticleBlendMode::Alpha;
		// Project-relative texture (sRGB color, alpha = coverage); empty uses a built-in soft disc.
		std::string Texture;

		bool operator==(const ParticleEmitterComponent&) const = default;
	};

	// Screen-space game UI. Everything is laid out on the canvas UI.* uses: 1080 units tall, origin at the
	// top-left, +Y down, width following the aspect ratio. Draw order follows the scene hierarchy: parents
	// before their children, later siblings on top; immediate-mode UI.* output draws over all of it.
	// Value rules are in UI/UIRules.cpp.

	// Places a rectangle relative to its parent's rect: the rect of the nearest ancestor with UI components,
	// or the whole canvas. AnchorMin/AnchorMax are fractions of the parent rect (0,0 = top-left): equal
	// anchors pin the rect to a point, different anchors stretch it with the parent. Size is added to the
	// anchors' span (so with equal anchors it is the size), Position moves the Pivot (a fraction of this rect)
	// away from its anchored spot. Draws nothing itself; UIImage and UIText on the entity fill it.
	struct UIRectComponent
	{
		// Largest magnitude of Position and Size: far beyond any canvas, small enough to stay exact in float.
		static constexpr float MaxMagnitude = 1e6f;

		// false hides this entity and its whole subtree (layout, drawing and UI.GetRect).
		bool Visible = true;
		// Clips descendants to this rect's transformed axis-aligned bounds.
		bool ClipChildren = false;
		// Clockwise about Pivot (+Y down): radians in C++, degrees in JSON/Lua, within +/-1000 turns.
		float Rotation = 0.0f;
		// Positive per-axis scale about Pivot, each axis 0.001..1000; inherited by descendants.
		glm::vec2 Scale = { 1.0f, 1.0f };
		glm::vec2 AnchorMin = { 0.5f, 0.5f };
		glm::vec2 AnchorMax = { 0.5f, 0.5f };
		glm::vec2 Pivot = { 0.5f, 0.5f };
		glm::vec2 Position = { 0.0f, 0.0f };
		glm::vec2 Size = { 200.0f, 100.0f };

		bool operator==(const UIRectComponent&) const = default;
	};

	// Fills the entity's UI rect with a color, an image, or a nine-slice panel.
	struct UIImageComponent
	{
		// Largest Slice border (texels), and the SliceScale range: a zero scale would collapse the borders, and
		// 1/1000 of a canvas unit per texel is already invisible.
		static constexpr float MaxSlice = 1e4f;
		static constexpr float MinSliceScale = 1e-3f;
		static constexpr float MaxSliceScale = 100.0f;

		// Project-relative image, drawn as stored (display colors, no sRGB decoding); empty draws Color.
		std::string Texture;
		// Tint multiplied with the image (display color and opacity, 0..1).
		glm::vec4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };
		// Nine-slice borders in texels (left, top, right, bottom): corners keep their size, edges stretch
		// along one axis and the center along both. All zero stretches the whole image.
		glm::vec4 Slice = { 0.0f, 0.0f, 0.0f, 0.0f };
		// Canvas units per border texel; opposite borders shrink proportionally when they would overlap, in the
		// rect or in the texture.
		float SliceScale = 1.0f;
		// Row-major sprite grid; frame zero is the top-left cell.
		glm::vec2 UVMin = { 0.0f, 0.0f };
		glm::vec2 UVMax = { 1.0f, 1.0f };
		uint32_t SpriteColumns = 1;
		uint32_t SpriteRows = 1;
		uint32_t SpriteFrame = 0;

		bool operator==(const UIImageComponent&) const = default;
	};

	enum class UIHorizontalAlign
	{
		Left = 0,
		Center,
		Right
	};
	constexpr size_t UIHorizontalAlignCount = 3;

	enum class UIVerticalAlign
	{
		Top = 0,
		Middle,
		Bottom
	};
	constexpr size_t UIVerticalAlignCount = 3;

	// Draws text inside the entity's UI rect, each line aligned on its own.
	struct UITextComponent
	{
		static constexpr float MinFontSize = 4.0f;
		static constexpr float MaxFontSize = 512.0f;
		static constexpr float DefaultFontSize = 32.0f;

		std::string Text = "Text";
		// Project-relative TrueType font (.ttf); empty uses the built-in font. A font that fails to load falls
		// back to the built-in one (the error is logged once per change of the file).
		std::string Font;
		// Line height in canvas units.
		float FontSize = DefaultFontSize;
		glm::vec4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };
		UIHorizontalAlign HorizontalAlign = UIHorizontalAlign::Left;
		UIVerticalAlign VerticalAlign = UIVerticalAlign::Top;
		// Breaks lines at the rect's width, between words (a word wider than the rect is split across lines).
		// '\n' always breaks.
		bool Wrap = true;
		// An offset drop shadow that keeps text readable over bright scenes.
		bool Shadow = true;

		bool operator==(const UITextComponent&) const = default;
	};

	// Interactive widgets: a widget component on a UI entity makes its laid-out rect (its UIRect's, or its parent's)
	// respond to the mouse, the keyboard and gamepads while the scene plays (UI/UIInteraction.h). The tints multiply
	// one part of the widget by state (the button's UIImage, the slider's handle, the checkbox's box): Highlight
	// while hovered or focused, Pressed while held with the pointer over it (a dragged slider stays pressed) or by
	// the accept key, Disabled when not Interactable. A user interaction calls Target's script method (Target 0 =
	// the widget's own entity) named by OnClick / OnValueChanged, with the widget entity (and the new value); an
	// empty name calls nothing. Setting values from code or files does not call it.

	// A clickable rect. Draws nothing itself: it tints the entity's UIImage, and its UIText is the label.
	struct UIButtonComponent
	{
		bool Interactable = true;
		glm::vec4 NormalTint = { 0.85f, 0.85f, 0.85f, 1.0f };
		glm::vec4 HighlightTint = { 1.0f, 1.0f, 1.0f, 1.0f };
		glm::vec4 PressedTint = { 0.65f, 0.65f, 0.65f, 1.0f };
		glm::vec4 DisabledTint = { 0.5f, 0.5f, 0.5f, 0.5f };
		UUID Target = 0;
		std::string OnClick = "OnClick";

		bool operator==(const UIButtonComponent&) const = default;
	};

	// A slider: horizontal values increase rightwards; Vertical values increase upwards. The track fills the
	// rect, with a fill and a tinted handle at Value. Dragging, or Left/Right (Up/Down when Vertical) while
	// focused, changes Value within [Min, Max] in steps of Step, with Max always a stop (0 = continuous;
	// navigation then moves a twentieth of the range).
	struct UISliderComponent
	{
		bool Interactable = true;
		float Value = 0.0f;
		float Min = 0.0f;
		float Max = 1.0f;
		float Step = 0.0f;
		glm::vec4 TrackColor = { 0.12f, 0.12f, 0.15f, 0.9f };
		glm::vec4 FillColor = { 0.3f, 0.6f, 1.0f, 1.0f };
		glm::vec4 HandleColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		// Canvas units along the slider axis: width horizontally, height when Vertical; at most that rect extent.
		float HandleWidth = 24.0f;
		bool Vertical = false;
		glm::vec4 NormalTint = { 0.85f, 0.85f, 0.85f, 1.0f };
		glm::vec4 HighlightTint = { 1.0f, 1.0f, 1.0f, 1.0f };
		glm::vec4 PressedTint = { 0.65f, 0.65f, 0.65f, 1.0f };
		glm::vec4 DisabledTint = { 0.5f, 0.5f, 0.5f, 0.5f };
		UUID Target = 0;
		std::string OnValueChanged = "OnValueChanged";

		bool operator==(const UISliderComponent&) const = default;
	};

	// A check box with its label: a square box (tinted) as tall as the rect at its left edge, a check mark inside
	// while Checked, and the entity's UIText laid out in the rest of the rect. Clicking anywhere in the rect toggles
	// it.
	struct UICheckboxComponent
	{
		bool Interactable = true;
		bool Checked = false;
		glm::vec4 BoxColor = { 0.12f, 0.12f, 0.15f, 0.9f };
		glm::vec4 CheckColor = { 0.3f, 0.6f, 1.0f, 1.0f };
		glm::vec4 NormalTint = { 0.85f, 0.85f, 0.85f, 1.0f };
		glm::vec4 HighlightTint = { 1.0f, 1.0f, 1.0f, 1.0f };
		glm::vec4 PressedTint = { 0.65f, 0.65f, 0.65f, 1.0f };
		glm::vec4 DisabledTint = { 0.5f, 0.5f, 0.5f, 0.5f };
		UUID Target = 0;
		std::string OnValueChanged = "OnValueChanged";

		bool operator==(const UICheckboxComponent&) const = default;
	};

	// A single-line UTF-8 editor. Keyboard editing acts on the focused widget; MaxLength counts code points.
	struct UITextInputComponent
	{
		bool Interactable = true;
		bool ReadOnly = false;
		std::string Text;
		std::string Placeholder;
		uint32_t MaxLength = 256;

		glm::vec4 NormalTint = { 0.85f, 0.85f, 0.85f, 1.0f };
		glm::vec4 HighlightTint = { 1.0f, 1.0f, 1.0f, 1.0f };
		glm::vec4 PressedTint = { 0.65f, 0.65f, 0.65f, 1.0f };
		glm::vec4 DisabledTint = { 0.5f, 0.5f, 0.5f, 0.5f };
		UUID Target = 0;
		std::string OnValueChanged = "OnValueChanged";

		bool operator==(const UITextInputComponent&) const = default;
	};

	// A choice menu. Options are newline-separated labels; Selected and its callback use zero-based indices.
	struct UIDropdownComponent
	{
		bool Interactable = true;
		std::string Options = "Option";
		uint32_t Selected = 0;

		glm::vec4 NormalTint = { 0.85f, 0.85f, 0.85f, 1.0f };
		glm::vec4 HighlightTint = { 1.0f, 1.0f, 1.0f, 1.0f };
		glm::vec4 PressedTint = { 0.65f, 0.65f, 0.65f, 1.0f };
		glm::vec4 DisabledTint = { 0.5f, 0.5f, 0.5f, 0.5f };
		UUID Target = 0;
		std::string OnValueChanged = "OnValueChanged";

		bool operator==(const UIDropdownComponent&) const = default;
	};

	// A clipped viewport whose children occupy ContentSize and move opposite Offset. Wheel units use ScrollSpeed.
	struct UIScrollViewComponent
	{
		bool Interactable = true;
		glm::vec2 ContentSize = { 0.0f, 0.0f };
		glm::vec2 Offset = { 0.0f, 0.0f };
		float ScrollSpeed = 32.0f;

		bool operator==(const UIScrollViewComponent&) const = default;
	};

	// A panel can consume pointer input without acting as a focusable widget; its children remain interactive.
	struct UIPanelComponent
	{
		bool BlockInput = true;

		bool operator==(const UIPanelComponent&) const = default;
	};

	// Optional directional focus targets; a missing direction uses spatial navigation. Invalid targets do not move focus.
	struct UINavigationComponent
	{
		UUID Up = 0;
		UUID Down = 0;
		UUID Left = 0;
		UUID Right = 0;

		bool operator==(const UINavigationComponent&) const = default;
	};

	// Draws an inset border while this entity's widget has keyboard/gamepad focus.
	struct UIFocusRingComponent
	{
		glm::vec4 Color = { 0.3f, 0.6f, 1.0f, 1.0f };
		float Thickness = 2.0f;

		bool operator==(const UIFocusRingComponent&) const = default;
	};

	// Marks a prefab instance root: the prefab it came from, which entities belong to the instance, and the source as
	// last merged, so later prefab edits merge without losing the instance's own edits (Scene/Prefab). Overrides are
	// derived from these (Prefab::ComputeOverrides, prefab.overrides), never stored. What it is follows from its fields and
	// its place, so copies and unpacking need no bookkeeping:
	// - Members and Baseline: a tracked instance. Listed by an instance around it that holds this prefab there, it is a
	//   nested root that was on its own for a while, which belongs to that instance again until its next merge clears this.
	// - Members alone: created during Play; it records membership only.
	// - Neither, listed by an instance around it: a nested root, which that instance tracks with its entities.
	// - Neither, anywhere else: untracked (saved before tracking existed, a copy of a nested root), adopted on merge.
	// - Empty Prefab: no link; prefab writes ignore it.
	struct PrefabComponent
	{
		std::string Prefab;
		// Source entity ID (decimal key) -> instance UUID, keeping the IDs of entities the instance deleted so Revert
		// recreates them with the same IDs.
		nlohmann::json Members = nlohmann::json::object();
		// The source's entity records as last merged: keyed by source ID, with source-space entity references and asset
		// paths (UUID/path objects on disk). Pointing the component at another prefab without new tracking clears both.
		nlohmann::json Baseline = nlohmann::json::object();
	};

	template<typename... Component>
	struct ComponentGroup
	{
	};

	// Every component except IDComponent, in a stable order. Used for copying scenes and entities.
	using AllComponents = ComponentGroup<
		TagComponent,
		TransformComponent,
		RelationshipComponent,
		CameraComponent,
		MeshComponent,
		AnimatorComponent,
		PropertyAnimationComponent,
		BoneAttachmentComponent,
		InverseKinematicsComponent,
		SpringBoneComponent,
		RagdollComponent,
		MaterialComponent,
		DirectionalLightComponent,
		PointLightComponent,
		SpotLightComponent,
		SkyLightComponent,
		PostProcessComponent,
		RigidBodyComponent,
		BoxColliderComponent,
		SphereColliderComponent,
		CapsuleColliderComponent,
		MeshColliderComponent,
		JointComponent,
		CharacterControllerComponent,
		ScriptComponent,
		AudioSourceComponent,
		AudioListenerComponent,
		AudioReverbZoneComponent,
		ParticleEmitterComponent,
		UIRectComponent,
		UIImageComponent,
		UITextComponent,
		UIButtonComponent,
		UISliderComponent,
		UICheckboxComponent,
		UITextInputComponent,
		UIDropdownComponent,
		UIScrollViewComponent,
		UIPanelComponent,
		UINavigationComponent,
		UIFocusRingComponent,
		PrefabComponent>;

}
