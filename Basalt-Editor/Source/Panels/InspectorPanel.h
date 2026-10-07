#pragma once

#include "EditorContext.h"

#include "Basalt/Scene/Entity.h"

#include <filesystem>
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
