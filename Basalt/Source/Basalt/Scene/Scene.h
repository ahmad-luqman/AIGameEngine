#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Core/Timestep.h"
#include "Basalt/Core/UUID.h"
#include "Basalt/Math/Random.h"
#include "Basalt/Physics/PhysicsLayers.h"
#include "Basalt/Renderer/Fsr1Settings.h"
#include "Basalt/Renderer/GraphicsSettings.h"
#include "Basalt/Renderer/PostProcessSettings.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Basalt {

	class Entity;
	class PhysicsWorld;
	class ScriptEngine;
	class AudioSystem;
	class ParticleSystem;
	class AnimationSystem;
	class UIInteraction;

	// Intermediate images the renderer can display instead of the final image (diagnostics).
	enum class RendererDebugView
	{
		None = 0,
		SSAO,
		Normals,
		Depth
	};

	// Per-scene renderer configuration (serialized with the scene).
	struct RendererSettings
	{
		float Exposure = 1.0f;
		Tonemapper Tonemap = Tonemapper::ACES;

		// FSR 1 runs only below native size, before overlays. Sharpness is RCAS attenuation in stops
		// (0 = strongest, 2 = weakest); old scenes retain bilinear upsampling.
		bool FSR1Enabled = false;
		float FSR1Sharpness = DefaultFsr1Sharpness;

		bool SSAOEnabled = true;
		float SSAORadius = 0.5f;
		float SSAOIntensity = 1.0f;
		float SSAOBias = 0.025f;

		bool ShadowsEnabled = true;
		// Distance from the camera covered by the directional light's shadow cascades.
		float ShadowDistance = 60.0f;
		float ShadowBias = 0.0015f;
		float ShadowNormalBias = 0.02f;
		// Fraction of the view distance blended between cascades (0 = hard switch).
		float CascadeSplitLambda = 0.85f;

		glm::vec3 AmbientColor = { 0.03f, 0.03f, 0.035f };
		// Not serialized: a viewing aid toggled by tools.
		RendererDebugView DebugView = RendererDebugView::None;
	};

	struct PhysicsSettings
	{
		glm::vec3 Gravity = { 0.0f, -9.81f, 0.0f };
		// Fixed simulation step in seconds overriding the project's FixedTimestep; 0 uses the project's.
		// Variable frame times are accumulated into fixed steps (see Scene::GetFixedTimestep).
		float FixedTimestep = 0.0f;
		uint32_t MaxStepsPerFrame = 8;

		bool HasFixedTimestepOverride() const { return FixedTimestep > 0.0f; }
	};

	enum class SceneState
	{
		Edit = 0,
		// Scripts, physics and audio run.
		Play,
		// Only physics runs (editor "simulate" mode).
		Simulate
	};

	// A collection of entities plus the runtime systems that animate them while playing.
	//
	// Entity destruction requested while the scene is updating (from scripts or physics callbacks), or
	// while another entity is being destroyed, is deferred until that work finishes, so handles stay
	// valid for the rest of the update and destruction never re-enters itself.
	class Scene
	{
	public:
		explicit Scene(std::string name = "Untitled");
		~Scene();

		Scene(const Scene&) = delete;
		Scene& operator=(const Scene&) = delete;

		// Deep copy of every entity and setting, preserving UUIDs (used to enter play mode).
		static Ref<Scene> Copy(const Ref<Scene>& other);

		Entity CreateEntity(const std::string& name = "Entity");
		Entity CreateEntityWithUUID(UUID uuid, const std::string& name = "Entity");
		Entity CreateChildEntity(Entity parent, const std::string& name = "Entity");
		// Destroys the entity and all its descendants (deferred while the scene is updating).
		void DestroyEntity(Entity entity);
		// Copies the entity and its descendants with new UUIDs. The copy gets the same parent.
		Entity DuplicateEntity(Entity entity);
		// Copies a forest with one reference map. Descendants already covered by an input root are skipped.
		std::vector<Entity> DuplicateEntities(const std::vector<Entity>& entities);
		// Keeps valid unique inputs whose ancestors are absent from the input, preserving order.
		std::vector<Entity> GetEntityRoots(const std::vector<Entity>& entities);

		Entity GetEntityByUUID(UUID uuid);
		Entity FindEntityByName(std::string_view name);
		std::vector<Entity> FindEntitiesByName(std::string_view name);
		bool IsEntityPendingDestruction(Entity entity) const;
		size_t GetEntityCount() const { return m_EntityMap.size(); }

		// Root entities in creation order; children are ordered by RelationshipComponent::Children.
		std::vector<Entity> GetRootEntities();
		// Every entity, depth-first in hierarchy order.
		std::vector<Entity> GetAllEntitiesOrdered();

		// Re-parents child under parent (an invalid parent makes it a root). With keepWorldTransform the
		// child's local transform is adjusted so it does not move. Returns false if it would create a cycle.
		// Fails (returning false) on cycles and when the hierarchy would exceed MaxHierarchyDepth levels.
		bool SetParent(Entity child, Entity parent, bool keepWorldTransform = true);
		// Bounds every recursive hierarchy walk (serialization, the editor tree, ...), so a malicious or
		// broken scene file cannot overflow the stack. Far deeper than any real scene needs.
		static constexpr uint32_t MaxHierarchyDepth = 256;
		// Number of ancestors (0 for a root) and number of levels below the entity (0 for a leaf).
		uint32_t GetDepth(Entity entity);
		uint32_t GetSubtreeHeight(Entity entity);
		bool IsDescendantOf(Entity entity, Entity ancestor);
		glm::mat4 GetWorldTransform(Entity entity);
		void SetWorldTransform(Entity entity, const glm::mat4& transform);

		Entity GetPrimaryCameraEntity();
		void OnViewportResize(uint32_t width, uint32_t height);
		uint32_t GetViewportWidth() const { return m_ViewportWidth; }
		uint32_t GetViewportHeight() const { return m_ViewportHeight; }

		// --- Runtime ---------------------------------------------------------------------------
		// nullopt copies the active project defaults before scripts start.
		void OnRuntimeStart(std::optional<GraphicsSettings> graphics = std::nullopt);
		void OnRuntimeStop();
		void OnSimulationStart();
		void OnSimulationStop();
		// Advances scripts, physics, particles and audio (Play) or physics and particles (Simulate). No-op in
		// Edit state.
		void OnUpdate(Timestep ts);
		SceneState GetState() const { return m_State; }
		bool IsRunning() const { return m_State != SceneState::Edit; }
		void SetPaused(bool paused) { m_Paused = paused; }
		bool IsPaused() const { return m_Paused; }
		// While paused, advance this many frames on the next updates.
		void Step(uint32_t frames = 1) { m_StepFrames += frames; }
		// Total simulated time since OnRuntimeStart/OnSimulationStart.
		float GetTime() const { return m_Time; }
		// Number of OnUpdate calls that advanced the simulation since the runtime started.
		uint64_t GetFrameCount() const { return m_FrameCount; }
		// Starting seed of the gameplay random streams of this play/simulate session (Lua, particles, runtime UUIDs;
		// scripts may reseed Lua's): the project's RandomSeed when the session started, so changing the
		// project setting mid-session (it applies from the next play) cannot split one session over two
		// seeds. Meaningful while running; outside Play/Simulate it is the last session's.
		uint32_t GetRandomSeed() const { return m_RandomSeed; }
		// The physics step this scene simulates with: its own override (PhysicsSettings::FixedTimestep), else
		// the project's. Like the seed, a session keeps the project value it started with; outside Play/Simulate this
		// is the project's current value.
		float GetFixedTimestep() const;

		// --- Scene switching (play mode, Lua Scene.Load) ---------------------------------------
		// Replaces the playing scene's contents with the scene file at `path` (project-relative, a .bscene
		// inside the project) at the end of the current update, keeping the Scene object, its systems and the
		// script state: every entity that is not persistent is destroyed (OnDestroy runs; entities OnDestroy
		// keeps spawning are kept after MaxUnloadPasses, with a script error), the file's settings (defaults for
		// what it leaves out; the debug view stays) and entities replace them, and the new entities start as at
		// play start (physics bodies, PlayOnStart audio and particles, then every script instance before any
		// OnCreate). An entity of the file whose ID is already present (a persistent manager placed in every
		// level) is skipped with its descendants in the file. Elapsed time and the frame count keep running. The
		// file is read and validated now: false (with outError, and nothing changed) when it cannot be loaded or
		// the scene is not playing. A later request in the same frame replaces an earlier one; one made while a
		// load is applied (in OnDestroy or OnCreate) runs at the end of the next update.
		bool RequestLoad(const std::string& path, std::string& outError);
		bool HasPendingLoad() const { return m_PendingLoad != nullptr; }
		// Keeps a root entity and its whole tree through scene loads (Lua DontDestroyOnLoad) for the rest of
		// the play session. False (nothing marked) when the entity is not a root of this scene. If it is later
		// parented under an entity that is not persistent, a load moves it back to the root first.
		bool SetPersistent(Entity entity);
		// Whether the entity survives scene loads: it or an ancestor was marked persistent.
		bool IsPersistent(Entity entity) const;
		// Path of the scene file this scene was loaded from or last saved to (SceneSerializer::ScenePathOf:
		// project-relative inside the project, otherwise absolute); empty for a scene never loaded from or saved
		// to a file. Scene loads update it.
		const std::string& GetPath() const { return m_Path; }

		// Set by gameplay code (Lua Game.Quit) to ask the host application to stop the game.
		void RequestQuit() { m_QuitRequested = true; }
		bool IsQuitRequested() const { return m_QuitRequested; }

		PhysicsWorld* GetPhysicsWorld() const { return m_PhysicsWorld.get(); }
		ScriptEngine* GetScriptEngine() const { return m_ScriptEngine.get(); }
		AudioSystem* GetAudioSystem() const { return m_AudioSystem.get(); }
		// Null in Edit mode; owned by the current Play/Simulate session.
		AnimationSystem* GetAnimationSystem() const { return m_AnimationSystem.get(); }
		ParticleSystem* GetParticleSystem() const { return m_ParticleSystem.get(); }
		// The widgets' pointer, keyboard and gamepad state; only while playing.
		UIInteraction* GetUIInteraction() const { return m_UIInteraction.get(); }

		const std::string& GetName() const { return m_Name; }
		void SetName(const std::string& name) { m_Name = name; }
		// Transient preferences; initialized before scripts start and preserved across Scene.Load.
		const GraphicsSettings& GetGraphicsSettings() const { return m_GraphicsSettings; }
		bool SetGraphicsSettings(const nlohmann::json& patch, std::string& error);
		// Play preferences while playing, otherwise active project defaults.
		GraphicsSettings GetEffectiveGraphicsSettings() const;
		// Unique across scene instances and increments on Play/reload; resets renderer history even after address reuse.
		uint64_t GetRenderRevision() const { return m_RenderRevision; }
		RendererSettings& GetRendererSettings() { return m_RendererSettings; }
		const RendererSettings& GetRendererSettings() const { return m_RendererSettings; }
		PhysicsSettings& GetPhysicsSettings() { return m_PhysicsSettings; }
		const PhysicsSettings& GetPhysicsSettings() const { return m_PhysicsSettings; }
		// Collision layers for this scene's physics, overriding the active project's (used by tests and
		// tools that run without a project). Empty means "use the project's". Not saved in scene files.
		void SetPhysicsLayers(std::optional<PhysicsLayers> layers) { m_PhysicsLayers = std::move(layers); }
		const std::optional<PhysicsLayers>& GetPhysicsLayers() const { return m_PhysicsLayers; }

		entt::registry& GetRegistry() { return m_Registry; }
		const entt::registry& GetRegistry() const { return m_Registry; }

		template<typename... Components>
		auto GetAllEntitiesWith()
		{
			return m_Registry.view<Components...>();
		}

	private:
		// A validated scene file waiting for the end of the update (RequestLoad).
		struct PendingLoad;
		// Unload passes for entities spawned by OnDestroy while the old scene is torn down.
		static constexpr int MaxUnloadPasses = 8;

		void ApplyPendingLoad();
		// Logs a scene-load problem and reports it as a script error.
		void ReportLoadProblem(const std::string& message);
		void DestroyEntityImmediate(Entity entity);
		void FlushPendingDestruction();
		// Session-owned identity stream. Only non-generated IDs need a permanent reservation; live
		// generated IDs are protected by the entity map without retaining every retired spawn forever.
		struct SessionIdentity
		{
			explicit SessionIdentity(uint32_t seed)
				: Stream(seed)
			{}
			UUID Next(const Scene& scene);
			void Reserve(UUID uuid) { Reserved.insert(uuid); }
			Random Stream;
			// Constrains generated IDs only: loading an explicit saved ID may reuse a retired ID.
			std::unordered_set<UUID> Reserved;
		};

		// Captures the project values a play/simulate session keeps (seed, fixed timestep) and starts its
		// random streams.
		void StartSession();
		Entity CreateEntityInternal(UUID uuid, const std::string& name);
		UUID GenerateEntityUUID();
		void RemoveFromParent(Entity entity);
		// Records source UUID -> copy UUID for every copied entity in outCopies.
		Entity CopyEntityRecursive(Entity source, Entity parent, std::unordered_map<UUID, UUID>& outCopies);

	private:
		std::string m_Name;
		entt::registry m_Registry;
		std::unordered_map<UUID, entt::entity> m_EntityMap;
		std::vector<UUID> m_RootEntities;

		SceneState m_State = SceneState::Edit;
		// While non-zero (updating, tearing down entities, stopping), DestroyEntity only queues the request.
		// This makes destruction from script callbacks (including OnDestroy) safe and non-reentrant.
		uint32_t m_DeferDestruction = 0;
		bool m_Paused = false;
		uint32_t m_StepFrames = 0;
		float m_Time = 0.0f;
		uint64_t m_FrameCount = 0;
		uint32_t m_RandomSeed = Random::DefaultSeed;
		// The project's FixedTimestep when the session started (0 outside a session).
		float m_SessionFixedTimestep = 0.0f;
		// Independent of gameplay draws; continues across Scene.Load and is released on stop.
		std::optional<SessionIdentity> m_SessionIdentity;
		bool m_QuitRequested = false;
		std::vector<UUID> m_PendingDestruction;
		Scope<PendingLoad> m_PendingLoad;
		// Roots marked with SetPersistent; runtime-only (cleared when play starts or stops, never saved).
		std::unordered_set<UUID> m_PersistentEntities;
		std::string m_Path;

		uint32_t m_ViewportWidth = 0;
		uint32_t m_ViewportHeight = 0;

		RendererSettings m_RendererSettings;
		void BumpRenderRevision();
		uint64_t m_RenderRevision = 0;
		GraphicsSettings m_GraphicsSettings;
		PhysicsSettings m_PhysicsSettings;
		std::optional<PhysicsLayers> m_PhysicsLayers;

		Scope<PhysicsWorld> m_PhysicsWorld;
		Scope<ScriptEngine> m_ScriptEngine;
		Scope<AudioSystem> m_AudioSystem;
		Scope<ParticleSystem> m_ParticleSystem;
		Scope<AnimationSystem> m_AnimationSystem;
		Scope<UIInteraction> m_UIInteraction;

		friend class Entity;
		friend class SceneSerializer;
	};

}
