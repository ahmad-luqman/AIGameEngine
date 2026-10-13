#pragma once

#include "Basalt/Core/UUID.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Physics/PhysicsMaterial.h"
#include "Basalt/Scene/SceneCamera.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <array>
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
		std::string Mesh;
		uint32_t MeshIndex = 0;
		bool CastShadows = true;
	};

	// PBR metallic-roughness material. When present it overrides the material imported with the mesh.
	// Texture paths are project-relative; empty means "no texture".
	struct MaterialComponent
	{
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

	struct PointLightComponent
	{
		glm::vec3 Color = { 1.0f, 1.0f, 1.0f };
		float Intensity = 10.0f;
		float Radius = 10.0f;
	};

	// Cone light along the entity's forward axis (-Z). Angles are in degrees.
	struct SpotLightComponent
	{
		glm::vec3 Color = { 1.0f, 1.0f, 1.0f };
		float Intensity = 20.0f;
		float Range = 15.0f;
		float InnerConeAngle = 20.0f;
		float OuterConeAngle = 30.0f;
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
	};

	struct SphereColliderComponent
	{
		float Radius = 0.5f;
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
		bool OverrideMaterial = false;
		float Friction = 0.5f;
		float Restitution = 0.0f;
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

	// Emits camera-facing particles while the scene plays or simulates (ParticleSystem simulates them on the
	// CPU, the renderer draws them unlit in the transparent pass). Shapes and Velocity are in the emitter's
	// local space; Box and Cone emit along local +Y, Point and Sphere in every direction. Each emission cycle
	// lasts Duration seconds and starts with BurstCount particles; Rate emits continuously in between.
	// Limit constants are below; the full per-field rules are in Particles/ParticleEmitterRules.cpp.
	struct ParticleEmitterComponent
	{
		// Largest MaxParticles and BurstCount.
		static constexpr uint32_t MaxParticlesLimit = 10000;
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

	// A horizontal slider: a track filling the rect, a fill from the left edge to the value and a handle (tinted)
	// at the value. Dragging, or Left/Right while focused, changes Value within [Min, Max] in steps of Step, Max
	// always being a stop (0 = continuous; Left/Right then move a twentieth of the range).
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
		// Canvas units; a handle wider than the rect shrinks to its width.
		float HandleWidth = 24.0f;
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

	// Marks the root entity of a prefab instance and remembers the prefab it came from.
	struct PrefabComponent
	{
		std::string Prefab;
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
		MaterialComponent,
		DirectionalLightComponent,
		PointLightComponent,
		SpotLightComponent,
		SkyLightComponent,
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
		ParticleEmitterComponent,
		UIRectComponent,
		UIImageComponent,
		UITextComponent,
		UIButtonComponent,
		UISliderComponent,
		UICheckboxComponent,
		PrefabComponent>;

}
