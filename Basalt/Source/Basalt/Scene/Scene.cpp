#include "Basalt/Scene/Scene.h"

#include "Basalt/Animation/AnimationSystem.h"
#include "Basalt/Audio/AudioSystem.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Input.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Particles/ParticleSystem.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Renderer/DebugDraw.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/SceneEventListener.h"
#include "Basalt/Scene/SceneSerializer.h"
#include "Basalt/Scripting/ScriptEngine.h"
#include "Basalt/UI/GameUI.h"
#include "Basalt/UI/UIInteraction.h"

#include <algorithm>
#include <atomic>
#include <unordered_set>
#include <filesystem>
#include <functional>

namespace Basalt {

	namespace {

		std::atomic<uint64_t> s_RenderRevision{ 0 };

		template<typename... Component>
		void CopyComponents(ComponentGroup<Component...>, entt::registry& destination, const entt::registry& source, const std::unordered_map<UUID, entt::entity>& destinationMap)
		{
			(
				[&]() {
					auto view = source.view<Component, IDComponent>();
					for (entt::entity sourceEntity : view)
					{
						const UUID uuid = view.template get<IDComponent>(sourceEntity).ID;
						auto it = destinationMap.find(uuid);
						if (it != destinationMap.end())
							destination.emplace_or_replace<Component>(it->second, view.template get<Component>(sourceEntity));
					}
				}(),
				...);
		}

		template<typename... Component>
		void CopyComponentsIfExists(ComponentGroup<Component...>, Entity destination, Entity source)
		{
			(
				[&]() {
					if (source.HasComponent<Component>())
						destination.AddOrReplaceComponent<Component>(source.GetComponent<Component>());
				}(),
				...);
		}

	}

	struct Scene::PendingLoad
	{
		// Project-relative, normalized.
		std::string Path;
		nlohmann::json Data;
	};

	Scene::Scene(std::string name)
		: m_Name(std::move(name))
	{
		BumpRenderRevision();
	}

	Scene::~Scene()
	{
		// Systems hold references into the registry; tear them down first.
		if (IsRunning())
		{
			if (m_State == SceneState::Play)
				OnRuntimeStop();
			else
				OnSimulationStop();
		}
	}

	Ref<Scene> Scene::Copy(const Ref<Scene>& other)
	{
		// A copy starts in Edit state; its next Play must take fresh project preferences, not the source session.
		Ref<Scene> scene = CreateRef<Scene>(other->m_Name);
		scene->m_RendererSettings = other->m_RendererSettings;
		scene->m_PhysicsSettings = other->m_PhysicsSettings;
		scene->m_PhysicsLayers = other->m_PhysicsLayers;
		scene->m_Path = other->m_Path;
		scene->m_ViewportWidth = other->m_ViewportWidth;
		scene->m_ViewportHeight = other->m_ViewportHeight;

		auto idView = other->m_Registry.view<IDComponent>();
		for (entt::entity entity : idView)
		{
			const UUID uuid = idView.get<IDComponent>(entity).ID;
			const entt::entity newEntity = scene->m_Registry.create();
			scene->m_Registry.emplace<IDComponent>(newEntity, uuid);
			scene->m_EntityMap[uuid] = newEntity;
		}

		CopyComponents(AllComponents{}, scene->m_Registry, other->m_Registry, scene->m_EntityMap);
		scene->m_RootEntities = other->m_RootEntities;
		return scene;
	}

	float Scene::GetFixedTimestep() const
	{
		if (m_PhysicsSettings.HasFixedTimestepOverride())
			return m_PhysicsSettings.FixedTimestep;
		return m_SessionFixedTimestep > 0.0f ? m_SessionFixedTimestep : Project::GetActiveFixedTimestep();
	}

	void Scene::StartSession()
	{
		m_RandomSeed = Project::GetActiveRandomSeed();
		m_SessionFixedTimestep = Project::GetActiveFixedTimestep();
		m_SessionIdentity.emplace(m_RandomSeed);
		// Authored entities never pass through the stream; reserve their IDs so a destroyed one is
		// never reissued to a later spawn by this generator.
		for (const auto& [uuid, handle] : m_EntityMap)
			m_SessionIdentity->Reserve(uuid);
	}

	UUID Scene::SessionIdentity::Next(const Scene& scene)
	{
		for (;;)
		{
			// Explicit high/low draw order and raw outputs give the same bits on every platform.
			const uint64_t high = Stream.NextUInt32();
			const uint64_t low = Stream.NextUInt32();
			const UUID uuid = (high << 32) | low;
			if (uuid.IsValid() && !Reserved.contains(uuid) && !scene.m_EntityMap.contains(uuid))
				return uuid;
		}
	}

	UUID Scene::GenerateEntityUUID()
	{
		if (m_SessionIdentity)
			return m_SessionIdentity->Next(*this);
		UUID uuid;
		while (!uuid.IsValid() || m_EntityMap.contains(uuid))
			uuid = UUID();
		return uuid;
	}

	Entity Scene::CreateEntity(const std::string& name)
	{
		return CreateEntityInternal(GenerateEntityUUID(), name);
	}

	Entity Scene::CreateEntityWithUUID(UUID uuid, const std::string& name)
	{
		if (!uuid.IsValid() || m_EntityMap.contains(uuid))
		{
			const UUID replacement = GenerateEntityUUID();
			BS_CORE_WARN("Scene: UUID {} for '{}' is invalid or already in use; using {}", static_cast<uint64_t>(uuid), name, static_cast<uint64_t>(replacement));
			uuid = replacement;
		}
		else if (m_SessionIdentity)
			m_SessionIdentity->Reserve(uuid);
		return CreateEntityInternal(uuid, name);
	}

	Entity Scene::CreateEntityInternal(UUID uuid, const std::string& name)
	{
		Entity entity(m_Registry.create(), this);
		entity.AddComponent<IDComponent>(uuid);
		entity.AddComponent<TagComponent>(name.empty() ? std::string("Entity") : name);
		entity.AddComponent<TransformComponent>();
		entity.AddComponent<RelationshipComponent>();

		m_EntityMap[uuid] = entity;
		m_RootEntities.push_back(uuid);
		return entity;
	}

	Entity Scene::CreateChildEntity(Entity parent, const std::string& name)
	{
		Entity entity = CreateEntity(name);
		if (parent)
			SetParent(entity, parent, false);
		return entity;
	}

	void Scene::DestroyEntity(Entity entity)
	{
		if (!entity || entity.GetScene() != this)
			return;

		if (m_DeferDestruction > 0)
		{
			const UUID uuid = entity.GetUUID();
			if (std::find(m_PendingDestruction.begin(), m_PendingDestruction.end(), uuid) == m_PendingDestruction.end())
				m_PendingDestruction.push_back(uuid);
			return;
		}

		DestroyEntityImmediate(entity);
		FlushPendingDestruction();
	}

	void Scene::DestroyEntityImmediate(Entity entity)
	{
		// Callbacks below (OnDestroy) may request more destruction; queue it instead of re-entering.
		m_DeferDestruction++;

		// Children first, so systems see a consistent hierarchy while tearing down.
		const std::vector<UUID> children = entity.GetComponent<RelationshipComponent>().Children;
		for (UUID child : children)
		{
			Entity childEntity = GetEntityByUUID(child);
			if (childEntity)
				DestroyEntityImmediate(childEntity);
		}

		if (m_ScriptEngine)
			m_ScriptEngine->OnEntityDestroyed(entity);
		if (m_PhysicsWorld)
			m_PhysicsWorld->OnEntityDestroyed(entity);
		if (m_AudioSystem)
			m_AudioSystem->OnEntityDestroyed(entity);
		// ParticleSystem unregisters through its on_destroy signal when the registry destroys the entity.

		RemoveFromParent(entity);
		const UUID uuid = entity.GetUUID();
		m_PersistentEntities.erase(uuid);
		m_RootEntities.erase(std::remove(m_RootEntities.begin(), m_RootEntities.end(), uuid), m_RootEntities.end());
		m_EntityMap.erase(uuid);
		m_Registry.destroy(entity);

		m_DeferDestruction--;
	}

	void Scene::FlushPendingDestruction()
	{
		if (m_DeferDestruction > 0)
			return;
		// Destroying an entity can queue more destruction (OnDestroy callbacks), so loop until empty.
		// Requests for entities that are already gone are skipped.
		while (!m_PendingDestruction.empty())
		{
			std::vector<UUID> pending;
			pending.swap(m_PendingDestruction);
			for (UUID uuid : pending)
			{
				Entity entity = GetEntityByUUID(uuid);
				if (entity)
					DestroyEntityImmediate(entity);
			}
		}
	}

	bool Scene::IsEntityPendingDestruction(Entity entity) const
	{
		if (!entity)
			return false;
		const UUID uuid = entity.GetUUID();
		return std::find(m_PendingDestruction.begin(), m_PendingDestruction.end(), uuid) != m_PendingDestruction.end();
	}

	Entity Scene::CopyEntityRecursive(Entity source, Entity parent, std::unordered_map<UUID, UUID>& outCopies)
	{
		Entity copy = CreateEntity(source.GetName());
		outCopies[source.GetUUID()] = copy.GetUUID();
		CopyComponentsIfExists(AllComponents{}, copy, source);
		// The copied relationship refers to the source's hierarchy; rebuild it.
		copy.GetComponent<RelationshipComponent>() = RelationshipComponent{};
		if (parent)
			SetParent(copy, parent, false);

		const std::vector<UUID> children = source.GetComponent<RelationshipComponent>().Children;
		for (UUID child : children)
		{
			Entity childEntity = GetEntityByUUID(child);
			if (childEntity)
				CopyEntityRecursive(childEntity, copy, outCopies);
		}
		return copy;
	}

	Entity Scene::DuplicateEntity(Entity entity)
	{
		const auto copies = DuplicateEntities({ entity });
		return copies.empty() ? Entity{} : copies.front();
	}

	std::vector<Entity> Scene::DuplicateEntities(const std::vector<Entity>& entities)
	{
		for (Entity entity : entities)
			if (!entity || entity.GetScene() != this)
				return {};
		const auto roots = GetEntityRoots(entities);
		std::unordered_map<UUID, UUID> copies;
		std::vector<Entity> result;
		result.reserve(roots.size());
		for (Entity root : roots)
			result.push_back(CopyEntityRecursive(root, root.GetParent(), copies));
		// References between separate selected trees must also connect the copies to each other.
		for (const auto& [source, copied] : copies)
			ComponentRegistry::RemapEntityReferences(GetEntityByUUID(copied), copies);
		return result;
	}

	std::vector<Entity> Scene::GetEntityRoots(const std::vector<Entity>& entities)
	{
		std::unordered_set<UUID> members;
		members.reserve(entities.size());
		for (Entity entity : entities)
			if (entity && entity.GetScene() == this)
				members.insert(entity.GetUUID());
		std::unordered_set<UUID> emitted;
		std::vector<Entity> roots;
		for (Entity entity : entities)
		{
			if (!entity || entity.GetScene() != this || !emitted.insert(entity.GetUUID()).second)
				continue;
			bool inherited = false;
			// Walking ancestors avoids comparing every pair in a large, flat forest.
			for (Entity parent = entity.GetParent(); parent; parent = parent.GetParent())
				if (members.contains(parent.GetUUID()))
				{
					inherited = true;
					break;
				}
			if (!inherited)
				roots.push_back(entity);
		}
		return roots;
	}

	Entity Scene::GetEntityByUUID(UUID uuid)
	{
		auto it = m_EntityMap.find(uuid);
		if (it == m_EntityMap.end())
			return {};
		return { it->second, this };
	}

	Entity Scene::FindEntityByName(std::string_view name)
	{
		for (Entity entity : GetAllEntitiesOrdered())
		{
			if (entity.GetName() == name)
				return entity;
		}
		return {};
	}

	std::vector<Entity> Scene::FindEntitiesByName(std::string_view name)
	{
		std::vector<Entity> result;
		for (Entity entity : GetAllEntitiesOrdered())
		{
			if (entity.GetName() == name)
				result.push_back(entity);
		}
		return result;
	}

	std::vector<Entity> Scene::GetRootEntities()
	{
		std::vector<Entity> result;
		result.reserve(m_RootEntities.size());
		for (UUID uuid : m_RootEntities)
		{
			Entity entity = GetEntityByUUID(uuid);
			if (entity)
				result.push_back(entity);
		}
		return result;
	}

	std::vector<Entity> Scene::GetAllEntitiesOrdered()
	{
		std::vector<Entity> result;
		result.reserve(m_EntityMap.size());

		std::function<void(Entity)> visit = [&](Entity entity) {
			result.push_back(entity);
			for (UUID child : entity.GetComponent<RelationshipComponent>().Children)
			{
				Entity childEntity = GetEntityByUUID(child);
				if (childEntity)
					visit(childEntity);
			}
		};

		for (Entity root : GetRootEntities())
			visit(root);
		return result;
	}

	void Scene::RemoveFromParent(Entity entity)
	{
		auto& relationship = entity.GetComponent<RelationshipComponent>();
		if (!relationship.Parent.IsValid())
			return;

		Entity parent = GetEntityByUUID(relationship.Parent);
		if (parent)
		{
			auto& siblings = parent.GetComponent<RelationshipComponent>().Children;
			siblings.erase(std::remove(siblings.begin(), siblings.end(), entity.GetUUID()), siblings.end());
		}
		relationship.Parent = 0;
	}

	bool Scene::IsDescendantOf(Entity entity, Entity ancestor)
	{
		Entity current = entity.GetParent();
		while (current)
		{
			if (current == ancestor)
				return true;
			current = current.GetParent();
		}
		return false;
	}

	uint32_t Scene::GetDepth(Entity entity)
	{
		uint32_t depth = 0;
		for (Entity parent = entity.GetParent(); parent; parent = parent.GetParent())
			depth++;
		return depth;
	}

	uint32_t Scene::GetSubtreeHeight(Entity entity)
	{
		// Iterative on purpose: this runs before the depth limit is known to hold.
		uint32_t height = 0;
		std::vector<std::pair<Entity, uint32_t>> stack = { { entity, 0u } };
		while (!stack.empty())
		{
			const auto [current, level] = stack.back();
			stack.pop_back();
			height = std::max(height, level);
			for (UUID child : current.GetComponent<RelationshipComponent>().Children)
			{
				if (Entity childEntity = GetEntityByUUID(child))
					stack.emplace_back(childEntity, level + 1);
			}
		}
		return height;
	}

	bool Scene::SetParent(Entity child, Entity parent, bool keepWorldTransform)
	{
		if (!child)
			return false;
		if (parent && (parent == child || IsDescendantOf(parent, child)))
		{
			BS_CORE_WARN("Scene: cannot parent '{}' to '{}': it would create a cycle", child.GetName(), parent.GetName());
			return false;
		}
		if (parent && GetDepth(parent) + 1 + GetSubtreeHeight(child) > MaxHierarchyDepth)
		{
			BS_CORE_WARN("Scene: cannot parent '{}' to '{}': the hierarchy would be deeper than {} levels", child.GetName(), parent.GetName(), MaxHierarchyDepth);
			return false;
		}

		const glm::mat4 worldTransform = GetWorldTransform(child);
		const UUID childUUID = child.GetUUID();

		RemoveFromParent(child);
		m_RootEntities.erase(std::remove(m_RootEntities.begin(), m_RootEntities.end(), childUUID), m_RootEntities.end());

		if (parent)
		{
			child.GetComponent<RelationshipComponent>().Parent = parent.GetUUID();
			parent.GetComponent<RelationshipComponent>().Children.push_back(childUUID);
		}
		else
		{
			m_RootEntities.push_back(childUUID);
		}

		if (keepWorldTransform)
			SetWorldTransform(child, worldTransform);
		return true;
	}

	glm::mat4 Scene::GetWorldTransform(Entity entity)
	{
		glm::mat4 transform = entity.GetComponent<TransformComponent>().GetTransform();
		Entity parent = entity.GetParent();
		while (parent)
		{
			transform = parent.GetComponent<TransformComponent>().GetTransform() * transform;
			parent = parent.GetParent();
		}
		return transform;
	}

	void Scene::SetWorldTransform(Entity entity, const glm::mat4& transform)
	{
		Entity parent = entity.GetParent();
		const glm::mat4 local = parent ? glm::inverse(GetWorldTransform(parent)) * transform : transform;
		entity.GetComponent<TransformComponent>().SetTransform(local);
	}

	Entity Scene::GetPrimaryCameraEntity()
	{
		for (Entity entity : GetAllEntitiesOrdered())
		{
			const auto* camera = entity.TryGetComponent<CameraComponent>();
			if (camera && camera->Primary)
				return entity;
		}
		return {};
	}

	void Scene::OnViewportResize(uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0)
			return;
		m_ViewportWidth = width;
		m_ViewportHeight = height;

		auto view = m_Registry.view<CameraComponent>();
		for (entt::entity entity : view)
		{
			auto& camera = view.get<CameraComponent>(entity);
			if (!camera.FixedAspectRatio)
				camera.Camera.SetViewportSize(width, height);
		}
	}

	void Scene::BumpRenderRevision()
	{
		m_RenderRevision = ++s_RenderRevision;
	}

	GraphicsSettings Scene::GetEffectiveGraphicsSettings() const
	{
		return m_State == SceneState::Play ? m_GraphicsSettings : Project::GetActiveGraphicsSettings();
	}

	bool Scene::SetGraphicsSettings(const nlohmann::json& patch, std::string& error)
	{
		if (m_State != SceneState::Play)
		{
			error = "graphics settings require Play mode";
			return false;
		}
		if (!PatchGraphicsSettings(patch, m_GraphicsSettings, error, UnreachableTargetFrameRate))
			return false;
		const auto state = GetDynamicResolutionState(m_GraphicsSettings.DynamicResolution, m_GraphicsSettings.RenderScale, true);
		if (state == DynamicResolutionState::EmptyRange)
			BS_CORE_WARN("Graphics.SetSettings: dynamic resolution {}", DescribeDynamicResolutionState(state, m_GraphicsSettings.DynamicResolution, m_GraphicsSettings.RenderScale));
		return true;
	}

	bool Scene::RequirePlayForBindings(std::string& outError) const
	{
		if (m_State == SceneState::Play)
			return true;
		outError = "input bindings require Play mode";
		return false;
	}

	bool Scene::SetInputBindings(std::string_view action, std::vector<InputBinding> bindings, std::string& outError)
	{
		if (!RequirePlayForBindings(outError))
			return false;
		return m_InputActions.SetBindings(action, std::move(bindings), outError);
	}

	bool Scene::ResetInputBindings(std::string_view action, std::string& outError)
	{
		if (!RequirePlayForBindings(outError))
			return false;
		const InputAction* start = m_StartInputActions.Find(action);
		if (!start)
		{
			outError = fmt::format("unknown input action '{}'", action);
			return false;
		}
		return m_InputActions.SetBindings(action, start->Bindings, outError);
	}

	bool Scene::ResetAllInputBindings(std::string& outError)
	{
		if (!RequirePlayForBindings(outError))
			return false;
		m_InputActions = m_StartInputActions;
		return true;
	}

	void Scene::OnRuntimeStart(std::optional<GraphicsSettings> graphics)
	{
		if (IsRunning())
			return;

		m_GraphicsSettings = graphics.value_or(Project::GetActiveGraphicsSettings());
		m_StartInputActions = Project::GetActiveInputActions();
		m_InputActions = m_StartInputActions;
		// The cursor belongs to the session: a game that locked it and stopped must not keep it locked.
		Input::SetCursorMode(CursorMode::Normal);
		m_StoppedSessionScriptErrors.clear();
		m_State = SceneState::Play;
		BumpRenderRevision();
		m_Time = 0.0f;
		m_FrameCount = 0;
		// A play session never shows the previous session's immediate-mode UI (e.g. when it starts paused).
		GameUI::BeginFrame();
		StartSession();
		m_QuitRequested = false;
		m_Paused = false;
		m_StepFrames = 0;
		m_PendingLoad.reset();
		m_PersistentEntities.clear();

		m_PhysicsWorld = CreateScope<PhysicsWorld>(this);
		m_AudioSystem = CreateScope<AudioSystem>(this);
		m_ParticleSystem = CreateScope<ParticleSystem>(this);
		m_AnimationSystem = CreateScope<AnimationSystem>(*this);
		m_ScriptEngine = CreateScope<ScriptEngine>(this);
		m_UIInteraction = CreateScope<UIInteraction>();

		m_PhysicsWorld->Start();
		m_AudioSystem->Start();
		// Before scripts, so OnCreate can play and emit particles.
		m_ParticleSystem->Start();

		// Scripts may destroy entities in OnCreate.
		m_DeferDestruction++;
		m_ScriptEngine->Start();
		m_DeferDestruction--;
		FlushPendingDestruction();
		// After OnCreate, so bus volumes it sets (e.g. the player's saved settings) apply before the PlayOnStart
		// sources sound.
		m_AudioSystem->BeginPlayback();
	}

	void Scene::OnRuntimeStop()
	{
		if (m_State != SceneState::Play)
			return;

		GameUI::BeginFrame();
		// OnDestroy callbacks may destroy entities; apply those once every system is shut down.
		m_DeferDestruction++;
		if (m_ScriptEngine)
			m_ScriptEngine->Stop();
		m_DeferDestruction--;
		m_StoppedSessionScriptErrors = m_ScriptEngine ? m_ScriptEngine->GetErrors() : std::vector<std::string>{};
		m_ScriptEngine.reset();
		m_UIInteraction.reset();
		m_ParticleSystem.reset();
		m_AnimationSystem.reset();
		m_AudioSystem.reset();
		m_PhysicsWorld.reset();
		m_InputActions = {};
		m_StartInputActions = {};
		Input::SetCursorMode(CursorMode::Normal);
		m_State = SceneState::Edit;
		m_SessionIdentity.reset();
		m_SessionFixedTimestep = 0.0f;
		m_PendingLoad.reset();
		m_PersistentEntities.clear();
		FlushPendingDestruction();
	}

	void Scene::OnSimulationStart()
	{
		if (IsRunning())
			return;

		m_State = SceneState::Simulate;
		m_Time = 0.0f;
		m_FrameCount = 0;
		StartSession();
		m_Paused = false;
		m_StepFrames = 0;
		m_PhysicsWorld = CreateScope<PhysicsWorld>(this);
		m_PhysicsWorld->Start();
		// Particles preview in simulation too (PlayOnStart emitters); nothing else can play them there.
		m_ParticleSystem = CreateScope<ParticleSystem>(this);
		m_AnimationSystem = CreateScope<AnimationSystem>(*this);
		m_ParticleSystem->Start();
	}

	void Scene::OnSimulationStop()
	{
		if (m_State != SceneState::Simulate)
			return;

		m_ParticleSystem.reset();
		m_AnimationSystem.reset();
		m_PhysicsWorld.reset();
		m_PendingDestruction.clear();
		m_State = SceneState::Edit;
		m_SessionIdentity.reset();
		m_SessionFixedTimestep = 0.0f;
	}

	void Scene::OnUpdate(Timestep ts)
	{
		if (!IsRunning())
			return;

		if (m_Paused)
		{
			if (m_StepFrames == 0)
				return;
			m_StepFrames--;
		}

		// Debug lines and game UI describe the current frame only.
		DebugDraw::Clear();
		GameUI::BeginFrame();

		m_DeferDestruction++;
		if (m_State == SceneState::Play)
		{
			// Before the scripts: widget callbacks run before OnUpdate, and immediate-mode widgets drawn in
			// OnUpdate read this frame's input.
			m_UIInteraction->Update(*this, ts.GetSeconds());
			m_ScriptEngine->Update(ts, m_UIInteraction->TakeEvents());
		}
		m_PhysicsWorld->Step(ts);
		if (m_State == SceneState::Play)
			m_ScriptEngine->LateUpdate(ts);
		// Animation moves BoneAttachment entities, so it runs before particles.
		m_AnimationSystem->Update(ts);
		DispatchAnimationEvents();
		// After every system that moves entities, so particles leave from where the emitter is this frame.
		m_ParticleSystem->Update(ts);
		if (m_State == SceneState::Play)
			m_AudioSystem->Update(ts);
		m_DeferDestruction--;

		FlushPendingDestruction();
		m_Time += ts.GetSeconds();
		m_FrameCount++;

		if (m_State == SceneState::Play && m_PendingLoad)
			ApplyPendingLoad();
	}

	void Scene::DispatchAnimationEvents()
	{
		for (const AnimationEventRecord& record : m_AnimationSystem->TakeEvents())
		{
			// An earlier callback may have destroyed the entity.
			Entity entity = GetEntityByUUID(record.Entity);
			if (!entity || IsEntityPendingDestruction(entity))
				continue;
			if (m_EventListener)
				m_EventListener->OnAnimationEvent(*this, entity, record.Name, record.Clip);
			if (m_ScriptEngine)
				m_ScriptEngine->OnAnimationEvent(entity, record.Name, record.Clip);
		}
	}

	bool Scene::RequestLoad(const std::string& path, std::string& outError)
	{
		if (m_State != SceneState::Play)
		{
			outError = "scenes can only be loaded while playing";
			return false;
		}

		// Only scenes inside the project (stricter than Scene.Instantiate): a scene switch replaces the whole
		// game, so it must not come from outside it.
		const std::filesystem::path relative = std::filesystem::path(path).lexically_normal();
		if (relative.empty() || relative.has_root_name() || relative.has_root_directory() || *relative.begin() == "..")
		{
			outError = "'" + path + "' must be a project-relative path inside the project";
			return false;
		}
		if (relative.extension() != ".bscene")
		{
			outError = "'" + path + "' is not a scene file (.bscene)";
			return false;
		}

		std::optional<nlohmann::json> data = SceneSerializer::ReadSceneFile(Project::ResolvePath(relative), path, outError);
		if (!data)
			return false;
		// Fully validated now (a scratch copy is built and discarded), so applying it at the end of the frame
		// cannot fail halfway with the old scene already gone.
		Scene scratch;
		if (!SceneSerializer::DeserializeScene(scratch, *data, outError))
		{
			outError = relative.filename().string() + ": " + outError;
			return false;
		}

		if (m_PendingLoad)
			BS_CORE_WARN("Scene.Load: '{}' replaces the load of '{}' requested earlier this frame", relative.generic_string(), m_PendingLoad->Path);
		m_PendingLoad = CreateScope<PendingLoad>(PendingLoad{ SceneSerializer::ScenePathOf(Project::ResolvePath(relative)), std::move(*data) });
		return true;
	}

	bool Scene::SetPersistent(Entity entity)
	{
		if (!entity || entity.GetScene() != this || entity.GetParent())
			return false;
		m_PersistentEntities.insert(entity.GetUUID());
		return true;
	}

	bool Scene::IsPersistent(Entity entity) const
	{
		if (!entity || entity.GetScene() != this)
			return false;
		for (Entity current = entity; current; current = current.GetParent())
		{
			if (m_PersistentEntities.contains(current.GetUUID()))
				return true;
		}
		return false;
	}

	void Scene::ReportLoadProblem(const std::string& message)
	{
		// Caused by scripts (or an engine bug): make it visible to them and to play.step's scriptErrors (which
		// logs it too).
		if (m_ScriptEngine)
			m_ScriptEngine->ReportError(message);
		else
			BS_CORE_ERROR("{}", message);
	}

	void Scene::ApplyPendingLoad()
	{
		const Scope<PendingLoad> load = std::move(m_PendingLoad);

		// Unload: OnDestroy may spawn entities, which a further pass destroys too.
		const std::unordered_set<UUID> persistentBefore = m_PersistentEntities;
		for (int pass = 0;; pass++)
		{
			// A persistent entity that was parented under one that is not leaves that tree first (keeping its
			// world transform), so it survives as promised. Sorted: roots are appended in this order, which must
			// not depend on the standard library's hash order.
			std::vector<UUID> persistent(m_PersistentEntities.begin(), m_PersistentEntities.end());
			std::sort(persistent.begin(), persistent.end(), [](UUID a, UUID b) { return static_cast<uint64_t>(a) < static_cast<uint64_t>(b); });
			for (UUID uuid : persistent)
			{
				Entity entity = GetEntityByUUID(uuid);
				if (entity && entity.GetParent() && !IsPersistent(entity.GetParent()))
					SetParent(entity, {}, true);
			}

			std::vector<UUID> doomed;
			for (UUID root : m_RootEntities)
			{
				if (!m_PersistentEntities.contains(root))
					doomed.push_back(root);
			}
			if (doomed.empty())
				break;
			if (pass == MaxUnloadPasses)
			{
				std::string names;
				for (UUID root : doomed)
					names += (names.empty() ? "'" : ", '") + GetEntityByUUID(root).GetName() + "'";
				ReportLoadProblem("Scene.Load: entities that OnDestroy keeps spawning while '" + m_Name + "' unloads are kept in '" + load->Path + "': " + names);
				break;
			}
			m_DeferDestruction++;
			for (UUID root : doomed)
				DestroyEntity(GetEntityByUUID(root));
			m_DeferDestruction--;
			FlushPendingDestruction();
		}
		// OnDestroy may also have destroyed a persistent entity, or parented it under one being destroyed.
		for (UUID uuid : persistentBefore)
		{
			if (!m_EntityMap.contains(uuid))
				BS_CORE_WARN("Scene.Load: persistent entity {} was destroyed while '{}' unloaded", static_cast<uint64_t>(uuid), m_Name);
		}
		// Nothing of the old scene's UI stays hovered, held or focused (a held button must not click the new scene).
		// After the unload: its OnDestroy callbacks may still draw widgets or set the focus.
		if (m_UIInteraction)
			m_UIInteraction->OnSceneLoaded();

		// An entity of the file that is already here (normally the persistent copy of a manager every level
		// contains) is skipped together with all its descendants in the file, whether or not the kept entity has
		// them: the kept tree stays exactly as it is.
		nlohmann::json data = std::move(load->Data);
		size_t skipped = 0;
		if (auto entities = data.find("Entities"); entities != data.end() && entities->is_array())
		{
			auto idOf = [](const nlohmann::json& entity, const char* key) -> uint64_t {
				const auto it = entity.find(key);
				return it != entity.end() && it->is_number_unsigned() ? it->get<uint64_t>() : 0;
			};
			std::unordered_set<uint64_t> skippedIDs;
			// Repeated until stable: files list parents first, but nothing requires it.
			for (bool changed = true; changed;)
			{
				changed = false;
				for (const nlohmann::json& entity : *entities)
				{
					const uint64_t id = idOf(entity, "ID");
					if (id == 0 || skippedIDs.contains(id))
						continue;
					if (m_EntityMap.contains(UUID(id)) || skippedIDs.contains(idOf(entity, "Parent")))
					{
						skippedIDs.insert(id);
						changed = true;
					}
				}
			}
			if (!skippedIDs.empty())
			{
				nlohmann::json kept = nlohmann::json::array();
				for (nlohmann::json& entity : *entities)
				{
					if (!skippedIDs.contains(idOf(entity, "ID")))
						kept.push_back(std::move(entity));
				}
				*entities = std::move(kept);
				skipped = skippedIDs.size();
			}
		}

		// Settings the file leaves out take their defaults, not the previous scene's values (the debug view is a
		// viewing aid, not scene data, so it stays).
		const RendererDebugView debugView = m_RendererSettings.DebugView;
		m_RendererSettings = RendererSettings{};
		BumpRenderRevision();
		m_RendererSettings.DebugView = debugView;
		m_PhysicsSettings = PhysicsSettings{};
		m_Name = std::filesystem::path(load->Path).stem().string();
		m_Path = load->Path;

		std::unordered_set<UUID> existing;
		for (const auto& [uuid, handle] : m_EntityMap)
			existing.insert(uuid);
		std::string error;
		const bool loaded = SceneSerializer::DeserializeScene(*this, data, error);
		// RequestLoad validated the file, so only an engine bug gets here; keep and start whatever was created.
		BS_CORE_ASSERT(loaded, "a validated scene failed to load");
		if (!loaded)
			ReportLoadProblem("Scene.Load: '" + load->Path + "' failed to load after validation (an engine bug): " + error);

		std::vector<Entity> created;
		for (Entity entity : GetAllEntitiesOrdered())
		{
			if (!existing.contains(entity.GetUUID()))
				created.push_back(entity);
		}

		// Start the new entities as OnRuntimeStart starts a scene: bodies first, so OnCreate can use them.
		OnViewportResize(m_ViewportWidth, m_ViewportHeight);
		if (m_PhysicsWorld)
		{
			m_PhysicsWorld->SetGravity(m_PhysicsSettings.Gravity);
			m_PhysicsWorld->OnSceneLoaded();
		}
		m_DeferDestruction++;
		if (m_AudioSystem)
			m_AudioSystem->PlayOnStart(created);
		if (m_ScriptEngine)
			m_ScriptEngine->EnsureInstances(created);
		m_DeferDestruction--;
		FlushPendingDestruction();
		// After OnCreate, as on play start: the new scene's scripts can stop or re-bus its music first.
		if (m_AudioSystem)
			m_AudioSystem->BeginPlayback();

		if (loaded)
			BS_CORE_INFO("Scene: loaded '{}' ({} entities; {} kept from the previous scene, {} of the file's skipped as already present)", load->Path, created.size(), existing.size(), skipped);
	}

	Entity Entity::GetParent() const
	{
		const UUID parent = GetComponent<RelationshipComponent>().Parent;
		return parent.IsValid() ? m_Scene->GetEntityByUUID(parent) : Entity{};
	}

	std::vector<Entity> Entity::GetChildren() const
	{
		std::vector<Entity> children;
		for (UUID child : GetComponent<RelationshipComponent>().Children)
		{
			Entity entity = m_Scene->GetEntityByUUID(child);
			if (entity)
				children.push_back(entity);
		}
		return children;
	}

}
