#include "Panels/ContentBrowserPanel.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Project/Project.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <vector>

namespace Basalt {

	namespace {

		const char* s_ScriptTemplate = R"(-- %s: attach to an entity with a Script component.
local %s = {}

-- Editable per entity in the inspector.
%s.Properties = {
	Speed = 5.0,
}

function %s:OnCreate()
end

function %s:OnUpdate(dt)
end

return %s
)";

		std::string MakeScript(const std::string& name)
		{
			std::string text = s_ScriptTemplate;
			size_t position;
			while ((position = text.find("%s")) != std::string::npos)
				text.replace(position, 2, name);
			return text;
		}

		const char* IconFor(const std::filesystem::path& path, bool directory)
		{
			if (directory)
				return "[DIR]";
			const std::string extension = path.extension().string();
			if (extension == ".bscene")
				return "[SCN]";
			if (extension == ".bprefab")
				return "[PFB]";
			if (extension == ".lua")
				return "[LUA]";
			if (extension == ".gltf" || extension == ".glb")
				return "[MDL]";
			if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".hdr" || extension == ".tga")
				return "[IMG]";
			if (extension == ".wav" || extension == ".mp3" || extension == ".ogg" || extension == ".flac")
				return "[SND]";
			return "[FILE]";
		}

	}

	void ContentBrowserPanel::OnImGuiRender()
	{
		ImGui::Begin("Content Browser");
		const Ref<Project>& project = Project::GetActive();
		if (!project)
		{
			ImGui::TextDisabled("No project open.");
			ImGui::End();
			return;
		}

		const std::filesystem::path root = project->GetAssetDirectory();
		std::error_code error;
		if (m_CurrentDirectory.empty() || !std::filesystem::is_directory(m_CurrentDirectory, error) || Project::MakeRelative(m_CurrentDirectory).rfind("Assets", 0) != 0)
			m_CurrentDirectory = root;

		if (m_CurrentDirectory != root && ImGui::Button("<- Back"))
			m_CurrentDirectory = m_CurrentDirectory.parent_path();
		ImGui::SameLine();
		ImGui::TextUnformatted(Project::MakeRelative(m_CurrentDirectory).c_str());
		ImGui::Separator();

		std::vector<std::filesystem::directory_entry> entries;
		for (auto it = std::filesystem::directory_iterator(m_CurrentDirectory, error); !error && it != std::filesystem::directory_iterator(); it.increment(error))
			entries.push_back(*it);
		std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
			if (a.is_directory() != b.is_directory())
				return a.is_directory();
			return a.path().filename() < b.path().filename();
		});

		for (const auto& entry : entries)
		{
			const bool directory = entry.is_directory(error);
			const std::string fileName = entry.path().filename().string();
			const std::string relative = Project::MakeRelative(entry.path());
			ImGui::PushID(relative.c_str());

			const std::string label = std::string(IconFor(entry.path(), directory)) + " " + fileName;
			ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
			if (!directory && ImGui::BeginDragDropSource())
			{
				ImGui::SetDragDropPayload(AssetPayloadType, relative.c_str(), relative.size() + 1);
				ImGui::TextUnformatted(relative.c_str());
				ImGui::EndDragDropSource();
			}
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				if (directory)
					m_CurrentDirectory = entry.path();
				else if (OnOpenAsset)
					OnOpenAsset(relative);
			}
			if (ImGui::BeginPopupContextItem())
			{
				if (ImGui::MenuItem("Copy path"))
					ImGui::SetClipboardText(relative.c_str());
				if (ImGui::MenuItem("Delete"))
					m_PendingDelete = entry.path();
				ImGui::EndPopup();
			}
			ImGui::PopID();
		}

		if (ImGui::BeginPopupContextWindow("ContentContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		{
			if (ImGui::MenuItem("New Script..."))
			{
				m_CreatingScript = true;
				m_NewItemName = "NewScript";
			}
			if (ImGui::MenuItem("New Folder..."))
			{
				m_CreatingFolder = true;
				m_NewItemName = "NewFolder";
			}
			ImGui::EndPopup();
		}

		if (m_CreatingScript || m_CreatingFolder)
			ImGui::OpenPopup("NewItem");
		if (ImGui::BeginPopupModal("NewItem", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted(m_CreatingScript ? "Script name:" : "Folder name:");
			ImGui::InputText("##ItemName", &m_NewItemName);
			const bool valid = !m_NewItemName.empty() && m_NewItemName.find_first_of("/\\.:") == std::string::npos;
			ImGui::BeginDisabled(!valid);
			if (ImGui::Button("Create"))
			{
				if (m_CreatingScript)
				{
					const std::string path = Project::MakeRelative(m_CurrentDirectory / (m_NewItemName + ".lua"));
					m_Context.Execute("asset.write", { { "path", path }, { "content", MakeScript(m_NewItemName) } });
				}
				else
				{
					std::filesystem::create_directories(m_CurrentDirectory / m_NewItemName, error);
				}
				m_CreatingScript = m_CreatingFolder = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				m_CreatingScript = m_CreatingFolder = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		if (!m_PendingDelete.empty())
			ImGui::OpenPopup("ConfirmDelete");
		if (ImGui::BeginPopupModal("ConfirmDelete", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Delete '%s'? This cannot be undone.", Project::MakeRelative(m_PendingDelete).c_str());
			if (ImGui::Button("Delete"))
			{
				std::filesystem::remove_all(m_PendingDelete, error);
				if (error)
					m_Context.SetStatus("Delete failed: " + error.message(), true);
				m_PendingDelete.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				m_PendingDelete.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		ImGui::End();
	}

}
