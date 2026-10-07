#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Core/Timestep.h"
#include "Basalt/Core/UUID.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Basalt {

	class Entity;
	class PhysicsWorld;
	class ScriptEngine;
	class AudioSystem;

	enum class Tonemapper
	{
		None = 0,
		Reinhard,
		ACES,
		AgX
	};

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
		// Fixed simulation step in seconds; variable frame times are accumulated into fixed steps.
		float FixedTimestep = 1.0f / 60.0f;
		uint32_t MaxStepsPerFrame = 8;
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
		void OnRuntimeStart();
		void OnRuntimeStop();
		void OnSimulationStart();
		void OnSimulationStop();
		// Advances scripts, physics and audio (Play) or physics only (Simulate). No-op in Edit state.
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

		// Set by gameplay code (Lua Game.Quit) to ask the host application to stop the game.
		void RequestQuit() { m_QuitRequested = true; }
		bool IsQuitRequested() const { return m_QuitRequested; }

		PhysicsWorld* GetPhysicsWorld() const { return m_PhysicsWorld.get(); }
		ScriptEngine* GetScriptEngine() const { return m_ScriptEngine.get(); }
		AudioSystem* GetAudioSystem() const { return m_AudioSystem.get(); }

		const std::string& GetName() const { return m_Name; }
		void SetName(const std::string& name) { m_Name = name; }
		RendererSettings& GetRendererSettings() { return m_RendererSettings; }
		const RendererSettings& GetRendererSettings() const { return m_RendererSettings; }
		PhysicsSettings& GetPhysicsSettings() { return m_PhysicsSettings; }
		const PhysicsSettings& GetPhysicsSettings() const { return m_PhysicsSettings; }

		entt::registry& GetRegistry() { return m_Registry; }

		template<typename... Components>
		auto GetAllEntitiesWith()
		{
			return m_Registry.view<Components...>();
		}

	private:
		void DestroyEntityImmediate(Entity entity);
		void FlushPendingDestruction();
		void RemoveFromParent(Entity entity);
		Entity CopyEntityRecursive(Entity source, Entity parent);

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
		bool m_QuitRequested = false;
		std::vector<UUID> m_PendingDestruction;

		uint32_t m_ViewportWidth = 0;
		uint32_t m_ViewportHeight = 0;

		RendererSettings m_RendererSettings;
		PhysicsSettings m_PhysicsSettings;

		Scope<PhysicsWorld> m_PhysicsWorld;
		Scope<ScriptEngine> m_ScriptEngine;
		Scope<AudioSystem> m_AudioSystem;

		friend class Entity;
		friend class SceneSerializer;
	};

}
