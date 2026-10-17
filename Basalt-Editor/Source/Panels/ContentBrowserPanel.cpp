#include "Panels/ContentBrowserPanel.h"

#include "EditorColors.h"
#include "Panels/PreviewImage.h"
#include "ThumbnailManager.h"

#include "Basalt/Automation/EditorCommandSchemas.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Asset/MeshSource.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Renderer/PreviewRenderer.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <vector>

namespace Basalt {

	namespace {

		const char* s_ScriptTemplate = R"(-- %s: attach to an entity with a Script component. Code-editor types (lua-language-server): Basalt's Docs/basalt.d.lua.
---@class %s: BasaltScript
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

		const char* KindName(ThumbnailKind kind)
		{
			switch (kind)
			{
				case ThumbnailKind::Image:
					return "Image";
				case ThumbnailKind::Environment:
					return "HDR environment";
				case ThumbnailKind::Model:
					return "Model";
				case ThumbnailKind::Prefab:
					return "Prefab";
				default:
					return "File";
			}
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
		ImGui::Begin(EditorCommandSchemas::ContentBrowserTitle);
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
		ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f, ImGui::GetContentRegionMax().x - 160.0f));
		ImGui::SetNextItemWidth(150.0f);
		ImGui::SliderFloat("##IconSize", &m_IconSize, 16.0f, 96.0f, "Icons %.0f px");
		ImGui::Separator();

		std::vector<std::filesystem::directory_entry> entries;
		for (auto it = std::filesystem::directory_iterator(m_CurrentDirectory, error); !error && it != std::filesystem::directory_iterator(); it.increment(error))
			entries.push_back(*it);
		std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
			if (a.is_directory() != b.is_directory())
				return a.is_directory();
			return a.path().filename() < b.path().filename();
		});

		const float rowHeight = std::max(m_IconSize, ImGui::GetTextLineHeight());
		// Only rows on screen ask for previews: a folder with more files than the preview cache holds must not
		// cycle the cache every frame.
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(entries.size()), rowHeight + ImGui::GetStyle().ItemSpacing.y);
		while (clipper.Step())
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
				DrawEntry(entries[static_cast<size_t>(row)], rowHeight);

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
			ImGui::Separator();
			if (ImGui::MenuItem("Clear Thumbnail Cache", nullptr, false, m_Thumbnails != nullptr))
			{
				// Done before the next frame's ImGui pass: this frame may already draw the textures it frees.
				m_Thumbnails->RequestClear(true);
				m_Context.SetStatus("Clearing the thumbnail cache; previews are made again", false);
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

	void ContentBrowserPanel::DrawEntry(const std::filesystem::directory_entry& entry, float rowHeight)
	{
		std::error_code error;
		const bool directory = entry.is_directory(error);
		const std::string fileName = FileSystem::PathToUtf8(entry.path().filename());
		const std::string relative = Project::MakeRelative(entry.path());
		ImGui::PushID(relative.c_str());

		// One row per file: the preview (or a type tag) in an icon-sized square, then the name.
		const ThumbnailManager::Preview* preview = !directory && m_Thumbnails ? m_Thumbnails->Get(relative) : nullptr;
		const ImVec2 rowMin = ImGui::GetCursorScreenPos();
		ImGui::Selectable("##Item", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, rowHeight));
		const bool hovered = ImGui::IsItemHovered();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const float textY = rowMin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f;
		if (preview && preview->Texture)
			DrawPreviewImage(drawList, *preview, rowMin, m_IconSize);
		else
			drawList->AddText(ImVec2(rowMin.x, textY), ImGui::GetColorU32(ImGuiCol_TextDisabled), IconFor(entry.path(), directory));
		const float iconWidth = std::max(m_IconSize, ImGui::CalcTextSize("[FILE]").x);
		drawList->AddText(ImVec2(rowMin.x + iconWidth + 6.0f, textY), ImGui::GetColorU32(ImGuiCol_Text), fileName.c_str());

		if (!directory && ImGui::BeginDragDropSource())
		{
			ImGui::SetDragDropPayload(AssetPayloadType, relative.c_str(), relative.size() + 1);
			ImGui::TextUnformatted(relative.c_str());
			ImGui::EndDragDropSource();
		}
		if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			if (directory)
				m_CurrentDirectory = entry.path();
			else if (OnOpenAsset)
				OnOpenAsset(relative);
		}
		if (preview && hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
			DrawPreviewTooltip(*preview, relative, fileName);
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

	void ContentBrowserPanel::DrawPreviewTooltip(const ThumbnailManager::Preview& preview, const std::string& relative, const std::string& fileName)
	{
		if (!ImGui::BeginTooltip())
			return;
		ImGui::TextUnformatted(fileName.c_str());
		ImGui::TextDisabled("%s", KindName(preview.Kind));
		if (preview.Texture)
			DrawPreviewSquare(preview, static_cast<float>(PreviewRenderer::OutputSize));
		if (preview.Status == ThumbnailManager::State::Pending)
			ImGui::TextDisabled("Making a preview...");
		else if (preview.Status == ThumbnailManager::State::Failed)
			ImGui::TextColored(EditorColors::Error(), "No preview: %s", preview.Error.c_str());
		for (const std::string& line : preview.Details)
			ImGui::TextUnformatted(line.c_str());
		// A model's materials, each on a sphere: those its preview read when it was made, or the model's when the open
		// scene has it loaded. Never loaded here: a whole glTF would stall the frame.
		std::vector<ThumbnailManager::Preview::Material> loaded;
		if (preview.Materials.empty() && preview.Kind == ThumbnailKind::Model)
		{
			if (const Ref<MeshSource> mesh = AssetManager::FindLoadedMesh(relative))
				loaded = ThumbnailManager::ListMaterials(*mesh);
		}
		const std::vector<ThumbnailManager::Preview::Material>& materials = preview.Materials.empty() ? loaded : preview.Materials;
		if (!materials.empty())
		{
			static constexpr size_t ShownMaterials = 8;
			static constexpr float Sphere = 48.0f;
			ImGui::Separator();
			for (size_t i = 0; i < std::min(materials.size(), ShownMaterials); i++)
			{
				DrawPreviewSquare(*m_Thumbnails->GetMaterial(relative + "#material/" + std::to_string(i), materials[i].Values), Sphere);
				ImGui::SameLine();
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(materials[i].Name.c_str());
			}
			if (materials.size() > ShownMaterials)
				ImGui::TextDisabled("and %zu more", materials.size() - ShownMaterials);
		}
		ImGui::EndTooltip();
	}

}
