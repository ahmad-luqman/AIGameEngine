#include "EditorContext.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Scene/SceneSerializer.h"

#include <imgui.h>

namespace Basalt {

	EditorContext::EditorContext(CommandRegistry& commands, AutomationSession& session, EditorHistory& history)
		: Commands(commands)
		, Session(session)
		, History(history)
	{
		Session.BeforeCommand = [this](const std::string& command, const nlohmann::json& params) {
			const std::string name = EditorHistory::CommandName(command, params);
			if (!m_FieldName.empty() && (!m_UICommand || m_FieldName != name))
				FinishFieldEdit();
			if (!m_UICommand || name != m_PendingName)
				CommitHistory();
			if (History.IsActive() && (command == "scene.new" || command == "scene.open" || command == "project.open" || command == "project.create" || command == "play.start" || command == "play.simulate" || command == "scene.save" || command == "scene.save_as"))
				throw CommandError("finish or cancel the active transaction first");
		};
		Session.AfterCommand = [this](const std::string& command, const nlohmann::json& params) {
			Session.PruneSelection();
			m_PendingName = EditorHistory::CommandName(command, params);
			if (!m_UICommand)
				CommitHistory();
		};
	}

	EditorContext::~EditorContext()
	{
		Session.BeforeCommand = nullptr;
		Session.AfterCommand = nullptr;
	}

	bool EditorContext::BeginTransaction(const std::string& name)
	{
		if (Session.IsPlaying())
			return false;
		CommitHistory();
		if (!History.Begin(name))
			return false;
		m_TransactionWasDirty = Session.IsDirty();
		return true;
	}

	void EditorContext::EndTransaction()
	{
		History.End(SceneSerializer::SerializeScene(*Session.GetEditScene()));
		m_PendingName.clear();
	}

	bool EditorContext::CancelTransaction(const EditorHistory::Restore& restore)
	{
		if (!History.Cancel(restore))
			return false;
		if (!m_TransactionWasDirty)
			Session.ClearDirty();
		m_FieldName.clear();
		m_PendingName.clear();
		return true;
	}

	void EditorContext::FinishFieldEdit()
	{
		if (!m_FieldName.empty())
		{
			if (History.IsActive())
				EndTransaction();
			m_FieldName.clear();
		}
	}

	void EditorContext::EditField(const std::string& command, const nlohmann::json& params, bool changed, bool committed)
	{
		const std::string name = EditorHistory::CommandName(command, params);
		if (changed)
		{
			if (m_FieldName != name)
				FinishFieldEdit();
			if (!History.IsActive() && BeginTransaction(name))
				m_FieldName = name;
			Execute(command, params);
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
		if (!Session.IsPlaying() && !History.IsActive())
		{
			History.Commit(SceneSerializer::SerializeScene(*Session.GetEditScene()), m_PendingName);
			m_PendingName.clear();
		}
	}

	void EditorContext::SetStatus(const std::string& message, bool isError)
	{
		StatusMessage = message;
		StatusIsError = isError;
		StatusTime = ImGui::GetTime();
	}

}
