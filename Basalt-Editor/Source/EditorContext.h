#pragma once

#include "Basalt/Automation/EditorHistory.h"

#include "Basalt/Automation/AutomationSession.h"
#include "Basalt/Automation/CommandRegistry.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace Basalt {

	// State shared by editor panels. All scene edits go through the automation commands, so the GUI and
	// AI agents (TCP / CLI) use exactly the same, validated code paths.
	class EditorContext
	{
	public:
		EditorContext(CommandRegistry& commands, AutomationSession& session, EditorHistory& history);
		~EditorContext();
		EditorContext(const EditorContext&) = delete;
		EditorContext& operator=(const EditorContext&) = delete;
		bool BeginTransaction(const std::string& name);
		void EndTransaction();
		bool CancelTransaction(const EditorHistory::Restore& restore);
		void EditField(const std::string& command, const nlohmann::json& params, bool changed, bool committed);
		void FinishFieldEdit();

		// Runs a command. On failure the error is shown in the status bar and nullopt returned.
		std::optional<nlohmann::json> Execute(const std::string& command, const nlohmann::json& params = nlohmann::json::object());
		// Records the edit scene in the undo history (call when an edit is complete, e.g. a drag ends).
		void CommitHistory();

		void SetStatus(const std::string& message, bool isError);

		CommandRegistry& Commands;
		AutomationSession& Session;
		EditorHistory& History;

		std::string StatusMessage;
		bool StatusIsError = false;
		double StatusTime = 0.0;

	private:
		bool m_UICommand = false;
		bool m_TransactionWasDirty = false;
		std::string m_PendingName;
		std::string m_FieldName;
	};

	// Drag-and-drop payload type carrying a project-relative asset path.
	inline constexpr const char* AssetPayloadType = "BASALT_ASSET";
	// Drag-and-drop payload type carrying an entity ID (uint64_t).
	inline constexpr const char* EntityPayloadType = "BASALT_ENTITY";

}
