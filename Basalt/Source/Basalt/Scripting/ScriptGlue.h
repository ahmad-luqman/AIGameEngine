#pragma once

#include "Basalt/Core/UUID.h"

#include <sol/sol.hpp>

#include <functional>
#include <random>

namespace Basalt {

	class Entity;
	class Scene;
	class ScriptEngine;

	// Lua-side entity handle. Stores the UUID, so it stays safe to hold after the entity is destroyed:
	// every method checks that the entity still exists and raises a Lua error otherwise.
	struct ScriptEntity
	{
		UUID ID = 0;
		Scene* SceneContext = nullptr;

		Entity Resolve() const;
		Entity ResolveOrThrow() const;
		bool operator==(const ScriptEntity& other) const { return ID == other.ID && SceneContext == other.SceneContext; }
	};

	// State shared between the bindings and the ScriptEngine.
	struct ScriptContext
	{
		// Null when scripts are loaded outside a running scene (property discovery).
		Scene* SceneContext = nullptr;
		ScriptEngine* Engine = nullptr;
		// Returns the script instance table of an entity, or nil.
		std::function<sol::object(UUID)> GetInstance;
		float DeltaTime = 0.0f;
		std::mt19937 Random{ 0x8A5A17u };
	};

	// Registers the complete Basalt Lua API (see Docs/ScriptingAPI.md) into the state.
	void RegisterScriptBindings(sol::state& lua, ScriptContext& context);

}
