#pragma once

#include "Basalt/Core/UUID.h"
#include "Basalt/Scene/ComponentRegistry.h"

#include <entt/entt.hpp>

#include <map>
#include <string>
#include <unordered_map>

namespace Basalt {
	class Scene;
	class Entity;
	struct PropertyAnimationComponent;
	std::string ValidatePropertyAnimation(const PropertyAnimationComponent& animation);

	// Resolves numeric timeline targets and applies samples through the registry's validated patch path.
	class PropertyAnimation
	{
	public:
		static const ComponentFieldType* FindField(const std::string& component, const std::string& field);
		// Samples normalized time in [0,1]. Missing targets fail; already-applied components remain changed.
		static bool Sample(Entity entity, const PropertyAnimationComponent& animation, float time, std::string& outError);
		// Captures a field's current serialized value, inserting or replacing a key on its timeline.
		static bool Key(Entity entity, const std::string& component, const std::string& field, float time, std::string& outError);
	};

	// Session-owned property clocks. Registry edits restart a timeline; runtime state never enters scene files.
	class PropertyAnimationSystem
	{
	public:
		explicit PropertyAnimationSystem(Scene& scene);
		~PropertyAnimationSystem();
		void Update(float deltaTime);
		// Play restarts, Pause/Resume preserve time, Stop samples zero, Seek samples normalized time.
		bool Control(Entity entity, const std::string& action, float time, std::string& outError);
		nlohmann::json Status(Entity entity);

	private:
		void Reset(entt::registry& registry, entt::entity entity);
		struct Clock
		{
			double Time = 0;
			bool Failed = false;
			bool Playing = true;
		};
		Scene& m_Scene;
		std::unordered_map<UUID, Clock> m_Clocks;
	};
}
