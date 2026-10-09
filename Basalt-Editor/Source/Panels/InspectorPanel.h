#pragma once

#include "EditorContext.h"

#include "Basalt/Scene/Entity.h"

#include <filesystem>
#include <functional>
#include <unordered_map>

namespace Basalt {

	// Shows and edits the selected entity's components. The UI is generated from each component's JSON
	// (ComponentRegistry), so every component — including future ones — is editable without custom code.
	class InspectorPanel
	{
	public:
		explicit InspectorPanel(EditorContext& context)
			: m_Context(context)
		{
		}

		void OnImGuiRender();

	private:
		void DrawScriptProperties(Entity entity, const nlohmann::json& component);
		// Picker for a component field that holds an entity UUID; noneLabel names 0 (e.g. "World" for a
		// joint's ConnectedEntity). filter, when set, limits the listed entities.
		void DrawEntityReference(Entity entity, const std::string& component, const std::string& field, uint64_t current, const std::function<bool(Entity)>& filter, const char* noneLabel);
		const nlohmann::json* GetScriptDefaults(const std::string& scriptPath);

	private:
		EditorContext& m_Context;

		struct ScriptCacheEntry
		{
			std::filesystem::file_time_type WriteTime;
			nlohmann::json Properties;
			std::string Error;
		};
		std::unordered_map<std::string, ScriptCacheEntry> m_ScriptCache;
	};

}
