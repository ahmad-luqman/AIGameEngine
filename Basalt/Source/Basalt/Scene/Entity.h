#pragma once

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/UUID.h"
#include "Basalt/Scene/Components.h"
#include "Basalt/Scene/Scene.h"

#include <entt/entt.hpp>

namespace Basalt {

	// Lightweight handle to an entity in a Scene. Copy freely; it does not own anything.
	// A default-constructed Entity is invalid. Using an Entity after its scene is destroyed is undefined.
	class Entity
	{
	public:
		Entity() = default;
		Entity(entt::entity handle, Scene* scene)
			: m_EntityHandle(handle)
			, m_Scene(scene)
		{
		}

		template<typename T, typename... Args>
		T& AddComponent(Args&&... args)
		{
			BS_CORE_ASSERT(!HasComponent<T>(), "Entity already has component");
			return m_Scene->m_Registry.emplace<T>(m_EntityHandle, std::forward<Args>(args)...);
		}

		template<typename T, typename... Args>
		T& AddOrReplaceComponent(Args&&... args)
		{
			return m_Scene->m_Registry.emplace_or_replace<T>(m_EntityHandle, std::forward<Args>(args)...);
		}

		template<typename T>
		T& GetComponent() const
		{
			BS_CORE_ASSERT(HasComponent<T>(), "Entity does not have component");
			return m_Scene->m_Registry.get<T>(m_EntityHandle);
		}

		template<typename T>
		T* TryGetComponent() const
		{
			return m_Scene->m_Registry.try_get<T>(m_EntityHandle);
		}

		template<typename T>
		bool HasComponent() const
		{
			return m_Scene->m_Registry.all_of<T>(m_EntityHandle);
		}

		template<typename T>
		void RemoveComponent()
		{
			m_Scene->m_Registry.remove<T>(m_EntityHandle);
		}

		// True if this handle refers to a live entity.
		bool IsValid() const { return m_Scene && m_Scene->m_Registry.valid(m_EntityHandle); }
		explicit operator bool() const { return IsValid(); }
		operator entt::entity() const { return m_EntityHandle; }

		UUID GetUUID() const { return GetComponent<IDComponent>().ID; }
		const std::string& GetName() const { return GetComponent<TagComponent>().Tag; }
		TransformComponent& GetTransform() const { return GetComponent<TransformComponent>(); }

		Entity GetParent() const;
		std::vector<Entity> GetChildren() const;
		Scene* GetScene() const { return m_Scene; }

		bool operator==(const Entity& other) const { return m_EntityHandle == other.m_EntityHandle && m_Scene == other.m_Scene; }
		bool operator!=(const Entity& other) const { return !(*this == other); }

	private:
		entt::entity m_EntityHandle = entt::null;
		Scene* m_Scene = nullptr;
	};

}
