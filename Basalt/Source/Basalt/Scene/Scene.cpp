#include "Basalt/Scene/Scene.h"

#include "Basalt/Audio/AudioSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Renderer/DebugDraw.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scripting/ScriptEngine.h"

#include <algorithm>
#include <functional>

namespace Basalt {

	namespace {

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

	Scene::Scene(std::string name)
		: m_Name(std::move(name))
	{
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
		Ref<Scene> scene = CreateRef<Scene>(other->m_Name);
		scene->m_RendererSettings = other->m_RendererSettings;
		scene->m_PhysicsSettings = other->m_PhysicsSettings;
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

	Entity Scene::CreateEntity(const std::string& name)
	{
		return CreateEntityWithUUID(UUID(), name);
	}

	Entity Scene::CreateEntityWithUUID(UUID uuid, const std::string& name)
	{
		if (!uuid.IsValid() || m_EntityMap.contains(uuid))
		{
			BS_CORE_WARN("Scene: UUID {} is invalid or already in use; generating a new one", static_cast<uint64_t>(uuid));
			uuid = UUID();
		}

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

		RemoveFromParent(entity);
		const UUID uuid = entity.GetUUID();
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

	Entity Scene::CopyEntityRecursive(Entity source, Entity parent)
	{
		Entity copy = CreateEntity(source.GetName());
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
				CopyEntityRecursive(childEntity, copy);
		}
		return copy;
	}

	Entity Scene::DuplicateEntity(Entity entity)
	{
		if (!entity || entity.GetScene() != this)
			return {};

		Entity copy = CopyEntityRecursive(entity, entity.GetParent());
		return copy;
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

	bool Scene::SetParent(Entity child, Entity parent, bool keepWorldTransform)
	{
		if (!child)
			return false;
		if (parent && (parent == child || IsDescendantOf(parent, child)))
		{
			BS_CORE_WARN("Scene: cannot parent '{}' to '{}': it would create a cycle", child.GetName(), parent.GetName());
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

	void Scene::OnRuntimeStart()
	{
		if (IsRunning())
			return;

		m_State = SceneState::Play;
		m_Time = 0.0f;
		m_FrameCount = 0;
		m_QuitRequested = false;
		m_Paused = false;
		m_StepFrames = 0;

		m_PhysicsWorld = CreateScope<PhysicsWorld>(this);
		m_AudioSystem = CreateScope<AudioSystem>(this);
		m_ScriptEngine = CreateScope<ScriptEngine>(this);

		m_PhysicsWorld->Start();
		m_AudioSystem->Start();

		// Scripts may destroy entities in OnCreate.
		m_DeferDestruction++;
		m_ScriptEngine->Start();
		m_DeferDestruction--;
		FlushPendingDestruction();
	}

	void Scene::OnRuntimeStop()
	{
		if (m_State != SceneState::Play)
			return;

		// OnDestroy callbacks may destroy entities; apply those once every system is shut down.
		m_DeferDestruction++;
		if (m_ScriptEngine)
			m_ScriptEngine->Stop();
		m_DeferDestruction--;
		m_ScriptEngine.reset();
		m_AudioSystem.reset();
		m_PhysicsWorld.reset();
		m_State = SceneState::Edit;
		FlushPendingDestruction();
	}

	void Scene::OnSimulationStart()
	{
		if (IsRunning())
			return;

		m_State = SceneState::Simulate;
		m_Time = 0.0f;
		m_FrameCount = 0;
		m_Paused = false;
		m_StepFrames = 0;
		m_PhysicsWorld = CreateScope<PhysicsWorld>(this);
		m_PhysicsWorld->Start();
	}

	void Scene::OnSimulationStop()
	{
		if (m_State != SceneState::Simulate)
			return;

		m_PhysicsWorld.reset();
		m_PendingDestruction.clear();
		m_State = SceneState::Edit;
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

		// Debug lines describe the current frame only.
		DebugDraw::Clear();

		m_DeferDestruction++;
		if (m_State == SceneState::Play)
			m_ScriptEngine->Update(ts);
		m_PhysicsWorld->Step(ts);
		if (m_State == SceneState::Play)
		{
			m_ScriptEngine->LateUpdate(ts);
			m_AudioSystem->Update(ts);
		}
		m_DeferDestruction--;

		FlushPendingDestruction();
		m_Time += ts.GetSeconds();
		m_FrameCount++;
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
