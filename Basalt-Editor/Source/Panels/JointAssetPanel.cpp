#include "Panels/JointAssetPanel.h"

#include "FileDialogs.h"

#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/Log.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>

namespace Basalt {

	void PreviewModel::Set(const std::string& path)
	{
		m_Path = path;
		m_Input = path;
		m_Error.clear();
		m_Source = path.empty() ? nullptr : AssetManager::GetMesh(path);
		if (!path.empty() && !m_Source)
			m_Error = "Cannot load the model: " + AssetManager::GetError(path);
	}

	void PreviewModel::OnFilesChanged(const std::vector<FileChange>& changes)
	{
		if (!m_Path.empty() && std::ranges::any_of(changes, [&](const FileChange& change) { return change.Path == m_Path; }))
			Set(m_Path);
	}

	void PreviewModel::OnAssetMoved(const std::string& from, const std::string& to)
	{
		if (m_Path == from)
			Set(to);
	}

	bool PreviewModel::Draw(const char* label)
	{
		ImGui::PushID(label);
		ImGui::SetNextItemWidth(-210.0f);
		ImGui::InputText("##Path", &m_Input);
		std::string chosen = m_Path;
		if (ImGui::IsItemDeactivatedAfterEdit())
			chosen = m_Input;
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(AssetPayloadType))
				chosen = static_cast<const char*>(payload->Data);
			ImGui::EndDragDropTarget();
		}
		ImGui::SameLine();
		if (FileDialogs::IsAvailable())
		{
			if (ImGui::Button("..."))
			{
				std::string error;
				const auto path = FileDialogs::Browse(FileDialogs::Kind::Open, Project::ResolvePath(m_Path), { { "glTF models", "gltf,glb" } }, error);
				if (path)
					chosen = Project::MakeRelative(*path);
				else if (!error.empty())
					m_Error = error;
			}
			ImGui::SameLine();
		}
		ImGui::TextUnformatted(label);
		ImGui::PopID();
		if (!m_Error.empty())
			ImGui::TextColored(EditorColors::Warning(), "%s", m_Error.c_str());
		if (chosen == m_Path)
			return false;
		Set(chosen);
		return true;
	}

}
