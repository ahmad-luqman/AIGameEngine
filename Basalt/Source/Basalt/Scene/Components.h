#pragma once

#include "Basalt/Core/UUID.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Scene/SceneCamera.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

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

	// Light shining along the entity's forward axis (-Z).
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
		// Triggers report overlaps to scripts but do not collide.
		bool IsTrigger = false;
		// Prevents the body from rotating (character controllers, upright objects).
		bool FixedRotation = false;
		// Bodies only collide with bodies whose layer is set in CollisionMask.
		uint32_t Layer = 0;
		uint32_t CollisionMask = 0xFFFFFFFF;
	};

	// Collider shapes are sized in local space and scaled by the entity's world scale.
	struct BoxColliderComponent
	{
		glm::vec3 HalfExtents = { 0.5f, 0.5f, 0.5f };
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
	};

	struct SphereColliderComponent
	{
		float Radius = 0.5f;
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
	};

	// Capsule aligned with the local Y axis. Total height = 2 * (HalfHeight + Radius).
	struct CapsuleColliderComponent
	{
		float Radius = 0.5f;
		float HalfHeight = 0.5f;
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
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
		Distance
	};

	enum class JointMotorMode
	{
		Off = 0,
		// Drives toward MotorTarget as a speed (degrees/s for hinges, m/s for sliders).
		Velocity,
		// Drives toward MotorTarget as a position (degrees for hinges, meters for sliders).
		Position
	};

	// Constrains this entity's rigid body to the body of ConnectedEntity, or to the world when it is 0.
	// The joint is built from the bodies' poses when physics starts (or when the joint changes): that
	// relative pose is the joint's rest position, so hinge angles and slider offsets are measured from it.
	// One joint per entity; a chain puts a joint on every link, connected to the previous one.
	struct JointComponent
	{
		JointType Type = JointType::Hinge;
		UUID ConnectedEntity = 0;
		// Attachment point in this entity's local space.
		glm::vec3 Anchor = { 0.0f, 0.0f, 0.0f };
		// Distance joints only: attachment point on the connected entity (its local space), or a world
		// position when connected to the world.
		glm::vec3 ConnectedAnchor = { 0.0f, 0.0f, 0.0f };
		// Hinge rotation axis or slider direction, in this entity's local space.
		glm::vec3 Axis = { 0.0f, 1.0f, 0.0f };
		// Hinge: angles in degrees within [-180, 180]; slider: offsets in meters; distance: lengths in
		// meters. Without limits a distance joint keeps the distance it starts with.
		bool UseLimits = false;
		float LimitMin = 0.0f;
		float LimitMax = 0.0f;
		// Hinge and slider joints only.
		JointMotorMode MotorMode = JointMotorMode::Off;
		float MotorTarget = 0.0f;
		// Strongest torque (N·m, hinges) or force (N, sliders) the motor may apply.
		float MotorMaxForce = 1000.0f;
		// The joint breaks when its constraint force (N) or torque (N·m) exceeds these; 0 never breaks.
		float BreakForce = 0.0f;
		float BreakTorque = 0.0f;
		// Whether the two connected bodies collide with each other.
		bool EnableCollision = false;
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
		JointComponent,
		ScriptComponent,
		AudioSourceComponent,
		AudioListenerComponent,
		PrefabComponent>;

}
