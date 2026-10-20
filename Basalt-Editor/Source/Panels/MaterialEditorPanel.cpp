#include "Panels/MaterialEditorPanel.h"

#include "EditorColors.h"
#include "Panels/JsonWidgets.h"
#include "Panels/PreviewImage.h"
#include "ThumbnailManager.h"

#include "Basalt/Asset/MaterialAsset.h"
#include "Basalt/Project/Project.h"

#include <imgui.h>

#include <algorithm>
#include <array>

namespace Basalt {

	namespace {

		// The scalar fields, in the order the file lists them.
		constexpr std::array<const char*, 14> ScalarFields = { "AlbedoColor", "Metallic", "Roughness", "EmissiveColor", "EmissiveIntensity", "AlphaMode", "AlphaCutoff", "DoubleSided", "NormalScale", "OcclusionStrength", "Clearcoat", "ClearcoatRoughness", "ClearcoatNormalScale", "Transmission" };
		constexpr std::array<const char*, 4> TransformFields = { "Offset", "Scale", "Rotation", "TexCoord" };

	}

	void MaterialEditorPanel::Open(const std::string& path)
	{
		if (m_Open && m_Draft.IsDirty() && m_Draft.GetPath() != path)
		{
			m_PendingOpen = path;
			m_PromptDiscard = true;
			return;
		}
		std::string error;
		if (!TryOpen(path, error))
		{
			m_Context.SetStatus("Cannot edit '" + path + "': " + error, true);
			return;
		}
		ImGui::SetWindowFocus(Title);
	}

	bool MaterialEditorPanel::TryOpen(const std::string& path, std::string& outError)
	{
		if (m_Open && m_Draft.IsDirty() && m_Draft.GetPath() != path)
		{
			outError = "'" + m_Draft.GetPath() + "' has unsaved edits (close the Material Editor first to discard them)";
			return false;
		}
		if (m_Open && m_Draft.GetPath() == path)
			return true;
		if (!m_Draft.Load(path, outError))
			return false;
		m_Open = true;
		m_Project = Project::GetActive();
		m_Error.clear();
		return true;
	}

	void MaterialEditorPanel::OnFilesChanged(const std::vector<FileChange>& changes)
	{
		if (!m_Open || std::none_of(changes.begin(), changes.end(), [&](const FileChange& change) { return change.Path == m_Draft.GetPath(); }))
			return;
		std::string error;
		if (!m_Draft.OnFileChanged(error) && !error.empty())
			m_Error = "The file changed on disk and cannot be read: " + error;
	}

	void MaterialEditorPanel::Reset()
	{
		m_Draft.Close();
		m_Open = false;
		m_PromptDiscard = false;
		m_PendingOpen.clear();
		m_Error.clear();
	}

	bool MaterialEditorPanel::Save()
	{
		std::string error;
		const auto text = m_Draft.MakeFileText(error);
		if (!text)
		{
			m_Error = "Not saved: " + error;
			return false;
		}
		// asset.write validates it again, reloads it everywhere it is used and acknowledges the write to the watcher.
		if (!m_Context.Execute("asset.write", { { "path", m_Draft.GetPath() }, { "content", *text } }))
		{
			m_Error = "Not saved: see the Console";
			return false;
		}
		m_Draft.MarkSaved();
		m_Error.clear();
		m_Context.SetStatus("Saved " + m_Draft.GetPath(), false);
		return true;
	}

	void MaterialEditorPanel::Revert()
	{
		std::string error;
		if (m_Draft.Revert(error))
			m_Error.clear();
		else
			m_Error = "Cannot read the file: " + error;
	}

	void MaterialEditorPanel::OnImGuiRender()
	{
		if (m_Open && m_Project.lock() != Project::GetActive())
			Reset();
		if (!m_Open)
			return;
		ImGui::SetNextWindowSize(ImVec2(420.0f, 640.0f), ImGuiCond_FirstUseEver);
		bool keepOpen = true;
		if (ImGui::Begin(Title, &keepOpen, m_Draft.IsDirty() ? ImGuiWindowFlags_UnsavedDocument : ImGuiWindowFlags_None))
		{
			ImGui::TextUnformatted(m_Draft.GetPath().c_str());
			const bool canSave = m_Draft.IsDirty() || m_Draft.HasExternalChange();
			ImGui::BeginDisabled(!canSave);
			if (ImGui::Button("Save"))
				Save();
			ImGui::SameLine();
			if (ImGui::Button("Revert"))
				Revert();
			ImGui::EndDisabled();
			ImGui::SameLine();
			const UUID selection = m_Context.Session.GetSelection();
			ImGui::BeginDisabled(!selection.IsValid());
			if (ImGui::Button("Apply to Selection") && m_Context.Execute("component.set", { { "entity", static_cast<uint64_t>(selection) }, { "component", "Material" }, { "data", { { "Material", m_Draft.GetPath() } } } }))
				m_Context.CommitHistory();
			ImGui::EndDisabled();
			if (m_Draft.HasExternalChange())
				ImGui::TextColored(EditorColors::Warning(), "The file changed on disk: Save replaces it, Revert loads it.");
			if (!m_Error.empty())
				ImGui::TextColored(EditorColors::Error(), "%s", m_Error.c_str());

			std::string problem;
			if (const auto material = m_Draft.Parse(problem))
			{
				if (m_Thumbnails)
					DrawPreviewSquare(*m_Thumbnails->GetMaterial("editor:" + m_Draft.GetPath(), *material), static_cast<float>(PreviewRenderer::OutputSize));
			}
			else
				ImGui::TextColored(EditorColors::Error(), "Invalid: %s", problem.c_str());
			ImGui::Separator();
			DrawFields();
		}
		ImGui::End();
		if (!keepOpen)
		{
			if (m_Draft.IsDirty())
			{
				m_PendingOpen.clear();
				m_PromptDiscard = true;
			}
			else
				Reset();
		}
		DrawDiscardPrompt();
	}

	void MaterialEditorPanel::DrawFields()
	{
		static const std::vector<std::string> AlphaModes(std::begin(MaterialAsset::AlphaModeNames), std::end(MaterialAsset::AlphaModeNames));
		nlohmann::json& data = m_Draft.GetData();
		for (const char* field : ScalarFields)
			if (data.contains(field))
				DrawJsonField(field, data[field], std::string_view(field) == "AlphaMode" ? &AlphaModes : nullptr);
		ImGui::SeparatorText("Maps");
		ImGui::TextDisabled("Drop textures from the Content Browser onto a map.");
		auto& transforms = data["TextureTransforms"];
		for (size_t slot = 0; slot < MaterialTextureSlotCount; slot++)
		{
			const char* field = MaterialAsset::MapFields[slot];
			ImGui::PushID(field);
			if (data.contains(field))
				DrawJsonField(field, data[field]);
			if (transforms.is_object() && transforms.contains(MaterialAsset::SlotNames[slot]) && ImGui::TreeNode("UV transform"))
			{
				auto& transform = transforms[MaterialAsset::SlotNames[slot]];
				for (const char* key : TransformFields)
					if (transform.contains(key))
						DrawJsonField(key, transform[key]);
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
	}

	void MaterialEditorPanel::DrawDiscardPrompt()
	{
		if (m_PromptDiscard)
		{
			ImGui::OpenPopup("Unsaved Material");
			m_PromptDiscard = false;
		}
		if (!ImGui::BeginPopupModal("Unsaved Material", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			return;
		ImGui::Text("'%s' has unsaved changes.", m_Draft.GetPath().c_str());
		const bool save = ImGui::Button("Save");
		ImGui::SameLine();
		const bool discard = ImGui::Button("Discard");
		ImGui::SameLine();
		const bool cancel = ImGui::Button("Cancel");
		if ((save && Save()) || discard)
		{
			const std::string next = std::move(m_PendingOpen);
			Reset();
			if (!next.empty())
				Open(next);
			ImGui::CloseCurrentPopup();
		}
		else if (cancel)
		{
			m_PendingOpen.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

}
