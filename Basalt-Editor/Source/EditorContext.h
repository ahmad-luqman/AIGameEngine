#pragma once

#include "Basalt/Automation/AutomationSession.h"
#include "Basalt/Automation/CommandRegistry.h"
#include "Basalt/Automation/EditorHistory.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace Basalt {

	// Shares validated command edits between panels and automation. Owns the session's command observer
	// and transaction ownership, capturing edits on successful and failed command completion.
	class EditorContext : public CommandObserver
	{
	public:
		enum class TransactionKind
		{
			Automation,
			Field,
			Gizmo
		};

		EditorContext(CommandRegistry& commands, AutomationSession& session, EditorHistory& history);
		~EditorContext() override;
		EditorContext(const EditorContext&) = delete;
		EditorContext& operator=(const EditorContext&) = delete;
		bool BeginTransaction(const std::string& name, TransactionKind kind = TransactionKind::Automation);
		bool EndTransaction();
		bool CancelTransaction(const EditorHistory::Restore& restore);
		// A failed gesture cancel falls back to a committed edit so Undo/save remain recoverable.
		void FinishGizmoTransaction(bool cancel, const EditorHistory::Restore& restore);
		bool HasAutomationTransaction() const;
		std::string TransactionError() const;
		bool CanStartPlay(std::string& error) override;
		void OnSceneChanged();
		void EditField(const std::string& command, const nlohmann::json& params, bool changed, bool committed);
		void FinishFieldEdit();

		// Runs a GUI command. On failure the error is logged/shown and nullopt returned.
		std::optional<nlohmann::json> Execute(const std::string& command, const nlohmann::json& params = nlohmann::json::object());
		// Captures a pending authored GUI edit once, outside transactions; otherwise a no-op.
		void CommitHistory();
		// Records a scene edit made outside any command (a prefab merge after a file change) as its own undo step,
		// so undoing the next command does not also undo it.
		void RecordExternalEdit(const std::string& name);
		void SetStatus(const std::string& message, bool isError);
		// A command result's "warnings" joined with "; " (empty when it has none), for the status bar and panels.
		static std::string JoinWarnings(const nlohmann::json& result);

		CommandRegistry& Commands;
		AutomationSession& Session;
		EditorHistory& History;
		std::string StatusMessage;
		bool StatusIsError = false;
		double StatusTime = 0.0;

	private:
		void BeforeCommand(const CommandInfo& command, const nlohmann::json& params) override;
		void AfterCommand(const CommandInfo& command, const nlohmann::json& params) noexcept override;
		void ReportHistory(bool success);
		struct Transaction
		{
			TransactionKind Kind;
			bool WasDirty;
		};
		std::optional<Transaction> m_Transaction;
		bool m_UICommand = false;
		uint64_t m_CommandRevision = 0;
		std::string m_PendingName;
		std::string m_FailedField;
	};

	// Drag-and-drop payload type carrying a project-relative asset path.
	inline constexpr const char* AssetPayloadType = "BASALT_ASSET";
	// Drag-and-drop payload type carrying an entity ID (uint64_t).
	inline constexpr const char* EntityPayloadType = "BASALT_ENTITY";

}
