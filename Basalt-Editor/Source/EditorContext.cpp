#include "EditorContext.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Scene/SceneSerializer.h"

#include <imgui.h>

namespace Basalt {

	EditorContext::EditorContext(CommandRegistry& commands, AutomationSession& session, EditorHistory& history)
		: Commands(commands)
		, Session(session)
		, History(history)
	{
		const bool attached = Session.AttachCommandObserver(*this);
		BS_CORE_ASSERT(attached, "EditorContext requires exclusive command observation");
		if (!attached)
			BS_CORE_ERROR("EditorContext could not attach its command observer");
	}

	EditorContext::~EditorContext()
	{
		Session.DetachCommandObserver(*this);
	}

	void EditorContext::BeforeCommand(const CommandInfo& command, const nlohmann::json& params)
	{
		// Duplication replaces the selection. Finish its drag first so viewport cancellation cannot
		// roll back both the movement and the newly created copies.
		if (m_Transaction && m_Transaction->Kind == TransactionKind::Gizmo && command.Name == "editor.duplicate")
			if (!EndTransaction())
				throw CommandError(StatusMessage);
		const std::string name = EditorHistory::CommandName(command.Name, params);
		const bool finishTransaction = command.Name == "editor.transaction" && params.value("action", std::string()) != "begin";
		if (!finishTransaction && m_Transaction && m_Transaction->Kind == TransactionKind::Field && (!m_UICommand || History.TransactionName() != name))
			FinishFieldEdit();
		if (!m_UICommand || name != m_PendingName)
			CommitHistory();
		if (History.IsActive() && command.Policy == CommandPolicy::EditBoundary)
			throw CommandError(TransactionError());
		if (m_UICommand && HasAutomationTransaction() && command.Policy != CommandPolicy::HistoryControl && command.Name != "editor.select" && command.Name != "editor.pick" && command.Name != "editor.box_select" && command.Name != "editor.viewport")
			throw CommandError(TransactionError());
		m_CommandRevision = Session.GetEditRevision();
	}

	void EditorContext::AfterCommand(const CommandInfo& command, const nlohmann::json& params) noexcept
	{
		try
		{
			Session.PruneSelection();
			if (command.Policy != CommandPolicy::HistoryControl && Session.GetEditRevision() != m_CommandRevision)
			{
				m_PendingName = EditorHistory::CommandName(command.Name, params);
				if (!m_UICommand)
					CommitHistory();
			}
		}
		catch (const std::exception& error)
		{
			BS_CORE_ERROR("Editor history capture after {} failed: {}", command.Name, error.what());
		}
	}

	bool EditorContext::HasAutomationTransaction() const
	{
		return m_Transaction && m_Transaction->Kind == TransactionKind::Automation;
	}

	std::string EditorContext::TransactionError() const
	{
		return "finish or cancel active transaction '" + History.TransactionName() + "' first (Edit menu or editor.transaction)";
	}

	bool EditorContext::CanStartPlay(std::string& error)
	{
		if (!History.IsActive())
			return true;
		error = TransactionError();
		return false;
	}

	void EditorContext::OnSceneChanged()
	{
		m_Transaction.reset();
		m_PendingName.clear();
		m_FailedField.clear();
		m_CommandRevision = Session.GetEditRevision();
	}

	bool EditorContext::BeginTransaction(const std::string& name, TransactionKind kind)
	{
		if (Session.IsPlaying())
			return false;
		CommitHistory();
		if (!History.Begin(name))
		{
			SetStatus(TransactionError(), true);
			return false;
		}
		m_Transaction = Transaction{ kind, Session.IsDirty() };
		return true;
	}

	void EditorContext::ReportHistory(bool success)
	{
		if (!success)
			SetStatus(History.GetError(), true);
		else if (!History.GetWarning().empty())
			SetStatus(History.GetWarning(), true);
	}

	bool EditorContext::EndTransaction()
	{
		if (!History.IsActive())
			return false;
		try
		{
			const bool success = History.End(SceneSerializer::SerializeScene(*Session.GetEditScene()));
			ReportHistory(success);
			if (!success)
				return false;
			m_Transaction.reset();
			m_PendingName.clear();
			return true;
		}
		catch (const std::exception& error)
		{
			BS_CORE_ERROR("History commit failed: {}", error.what());
			SetStatus(std::string("Commit failed: ") + error.what(), true);
			return false;
		}
	}

	bool EditorContext::CancelTransaction(const EditorHistory::Restore& restore)
	{
		const bool success = History.Cancel(restore);
		ReportHistory(success);
		if (!success)
			return false;
		if (m_Transaction && !m_Transaction->WasDirty)
			Session.ClearDirty();
		m_Transaction.reset();
		m_PendingName.clear();
		m_FailedField.clear();
		return true;
	}

	void EditorContext::FinishGizmoTransaction(bool cancel, const EditorHistory::Restore& restore)
	{
		if (!m_Transaction || m_Transaction->Kind != TransactionKind::Gizmo)
			return;
		if (!cancel)
		{
			EndTransaction();
			return;
		}
		if (CancelTransaction(restore))
			return;
		const std::string error = History.GetError();
		const bool committed = EndTransaction();
		SetStatus(error + (committed ? "; gesture committed instead, use Undo to retry" : "; use Edit > Commit/Cancel Transaction to recover"), true);
	}

	void EditorContext::FinishFieldEdit()
	{
		if (m_Transaction && m_Transaction->Kind == TransactionKind::Field)
			EndTransaction();
		m_FailedField.clear();
	}

	void EditorContext::EditField(const std::string& command, const nlohmann::json& params, bool changed, bool committed)
	{
		const std::string name = EditorHistory::CommandName(command, params);
		if (changed && m_FailedField != name)
		{
			if (HasAutomationTransaction())
			{
				SetStatus(TransactionError(), true);
				m_FailedField = name;
			}
			else
			{
				if (History.TransactionName() != name)
					FinishFieldEdit();
				if (!History.IsActive())
					BeginTransaction(name, TransactionKind::Field);
				if (!Execute(command, params))
				{
					FinishFieldEdit();
					m_FailedField = name;
				}
			}
		}
		if (committed)
			FinishFieldEdit();
	}

	std::optional<nlohmann::json> EditorContext::Execute(const std::string& command, const nlohmann::json& params)
	{
		m_UICommand = true;
		const nlohmann::json response = Commands.HandleRequest(Session, { { "command", command }, { "params", params } });
		m_UICommand = false;
		if (!response.value("ok", false))
		{
			const std::string error = response.value("error", std::string("unknown error"));
			SetStatus(command + ": " + error, true);
			BS_CORE_WARN("Editor: {}: {}", command, error);
			return std::nullopt;
		}
		return response["result"];
	}

	void EditorContext::CommitHistory()
	{
		if (m_PendingName.empty() || Session.IsPlaying() || History.IsActive())
			return;
		try
		{
			const bool success = History.Commit(SceneSerializer::SerializeScene(*Session.GetEditScene()), m_PendingName);
			ReportHistory(success);
			if (success)
				m_PendingName.clear();
		}
		catch (const std::exception& error)
		{
			BS_CORE_ERROR("History capture failed: {}", error.what());
			SetStatus(std::string("Capture failed: ") + error.what(), true);
		}
	}

	void EditorContext::RecordExternalEdit(const std::string& name)
	{
		if (Session.IsExecutingCommand() || Session.GetEditRevision() == m_CommandRevision)
			return;
		// An unfinished GUI edit is captured together with the merge: the scene holds both by now.
		FinishFieldEdit();
		if (m_PendingName.empty())
			m_PendingName = name;
		CommitHistory();
		m_CommandRevision = Session.GetEditRevision();
	}

	std::string EditorContext::JoinWarnings(const nlohmann::json& result)
	{
		std::string joined;
		const auto warnings = result.is_object() ? result.find("warnings") : result.end();
		if (warnings == result.end() || !warnings->is_array())
			return joined;
		for (const auto& warning : *warnings)
			joined += (joined.empty() ? "" : "; ") + (warning.is_string() ? warning.get<std::string>() : warning.dump());
		return joined;
	}

	void EditorContext::SetStatus(const std::string& message, bool isError)
	{
		StatusMessage = message;
		StatusIsError = isError;
		StatusTime = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0;
	}

}
