#include "EditorContext.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Scene/SceneSerializer.h"

#include <imgui.h>

namespace Basalt {

	std::optional<nlohmann::json> EditorContext::Execute(const std::string& command, const nlohmann::json& params)
	{
		const nlohmann::json response = Commands.HandleRequest(Session, { { "command", command }, { "params", params } });
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
		if (!Session.IsPlaying())
			History.Commit(SceneSerializer::SerializeScene(*Session.GetEditScene()));
	}

	void EditorContext::SetStatus(const std::string& message, bool isError)
	{
		StatusMessage = message;
		StatusIsError = isError;
		StatusTime = ImGui::GetTime();
	}

}
