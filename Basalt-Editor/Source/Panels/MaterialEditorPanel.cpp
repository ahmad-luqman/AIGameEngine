#include "Panels/MaterialEditorPanel.h"

#include "EditorColors.h"
#include "Panels/JsonWidgets.h"
#include "Panels/PreviewImage.h"
#include "ThumbnailManager.h"

#include "Basalt/Asset/MaterialAsset.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Project/Project.h"

#include <imgui.h>

#include <algorithm>
#include <iterator>

namespace Basalt {

	bool MaterialEditorPanel::HasOtherUnsavedDraft(const std::string& path) const
	{
		return IsOpen() && m_Draft.IsDirty() && m_Draft.GetPath() != path;
	}

	void MaterialEditorPanel::Open(const std::string& path)
	{
		if (HasOtherUnsavedDraft(FileSystem::NormalizeGeneric(path)))
		{
			m_PendingOpen = FileSystem::NormalizeGeneric(path);
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

	bool MaterialEditorPanel::TryOpen(const std::string& requested, std::string& outError)
	{
		// Normalized like file-change paths, which the draft's path must match to follow its file.
		const std::string path = FileSystem::NormalizeGeneric(requested);
		if (HasOtherUnsavedDraft(path))
		{
			outError = "'" + m_Draft.GetPath() + "' has unsaved edits (close the Material Editor first to discard them)";
			return false;
		}
		if (IsOpen() && m_Draft.GetPath() == path)
			return true;
		if (!m_Draft.Load(path, outError))
			return false;
		m_Project = Project::GetActive();
		m_Error.clear();
		m_Warning.clear();
		return true;
	}

	void MaterialEditorPanel::OnFilesChanged(const std::vector<FileChange>& changes)
	{
		const auto change = std::find_if(changes.begin(), changes.end(), [&](const FileChange& c) { return c.Path == m_Draft.GetPath(); });
		if (!IsOpen() || change == changes.end())
			return;
		std::string error;
		if (m_Draft.OnFileChanged(error))
		{
			m_Error.clear();
			m_Warning.clear();
		}
		else if (change->Type == FileChangeType::Removed)
			m_Error = "The file was deleted: Save writes a new file with a new identity, and references to the old one stay unresolved.";
		else if (!error.empty())
			m_Error = "The file changed on disk and cannot be read: " + error;
	}

	void MaterialEditorPanel::OnAssetMoved(const std::string& from, const std::string& to)
	{
		if (m_PendingOpen && *m_PendingOpen == from)
			m_PendingOpen = to;
		m_Draft.OnAssetMoved(from, to);
	}

	void MaterialEditorPanel::Reset()
	{
		m_Draft.Close();
		m_PromptDiscard = false;
		m_PendingOpen.reset();
		m_Error.clear();
		m_Warning.clear();
	}

	bool MaterialEditorPanel::Save()
	{
		std::string error;
		const auto text = m_Draft.MakeFileText(error);
		if (!text)
		{
			m_Error = "Not saved: " + error;
			m_Warning.clear();
			return false;
		}
		// asset.write validates it again, reloads it everywhere it is used and acknowledges the write to the watcher.
		const auto result = m_Context.Execute("asset.write", { { "path", m_Draft.GetPath() }, { "content", *text } });
		if (!result)
		{
			// Execute reported the command's error in the status bar.
			m_Error = "Not saved: " + m_Context.StatusMessage;
			m_Warning.clear();
			return false;
		}
		// Textures with problems render without that texture (a flat normal for normal maps): say which.
		const std::string warnings = EditorContext::JoinWarnings(*result);
		m_Warning = warnings.empty() ? std::string() : "Saved with warnings: " + warnings;
		if (!m_Draft.MarkSaved(*text, error))
		{
			m_Error = "Saved, but the written file does not read back: " + error;
			m_Context.SetStatus(m_Error, true);
			return true;
		}
		m_Error.clear();
		m_Context.SetStatus("Saved " + m_Draft.GetPath(), false);
		return true;
	}

	void MaterialEditorPanel::Revert()
	{
		std::string error;
		if (m_Draft.Revert(error))
		{
			m_Error.clear();
			m_Warning.clear();
		}
		else
			m_Error = "Cannot read the file: " + error;
	}

	void MaterialEditorPanel::OnImGuiRender()
	{
		if (IsOpen() && m_Project.lock() != Project::GetActive())
			Reset();
		if (!IsOpen())
			return;
		ImGui::SetNextWindowSize(ImVec2(420.0f, 640.0f), ImGuiCond_FirstUseEver);
		bool keepOpen = true;
		// Parsed once per frame: dirtiness and the preview both use it.
		std::string problem;
		const auto material = m_Draft.Parse(problem);
		const bool dirty = m_Draft.IsDirty(material);
		if (ImGui::Begin(Title, &keepOpen, dirty ? ImGuiWindowFlags_UnsavedDocument : ImGuiWindowFlags_None))
		{
			ImGui::TextUnformatted(m_Draft.GetPath().c_str());
			const bool canSave = dirty || m_Draft.HasExternalChange();
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
			// The last save's warnings include what loading reported (unresolved references); show one or the other.
			if (!m_Warning.empty())
				ImGui::TextColored(EditorColors::Warning(), "%s", m_Warning.c_str());
			else
				for (const std::string& warning : m_Draft.GetWarnings())
					ImGui::TextColored(EditorColors::Warning(), "%s", warning.c_str());

			if (material)
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
			if (dirty)
			{
				m_PendingOpen.reset();
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
		for (const char* field : MaterialAsset::ScalarFields)
			if (data.contains(field))
				DrawJsonField(field, data[field], std::string_view(field) == "AlphaMode" ? &AlphaModes : nullptr);
		ImGui::SeparatorText("Maps");
		ImGui::TextDisabled("Drop textures from the Content Browser onto a map.");
		auto& transforms = data["TextureTransforms"];
		for (const MaterialAsset::SlotInfo& slot : MaterialAsset::Slots)
		{
			ImGui::PushID(slot.MapField);
			if (data.contains(slot.MapField))
				DrawJsonField(slot.MapField, data[slot.MapField]);
			if (transforms.is_object() && transforms.contains(slot.Name) && ImGui::TreeNode("UV transform"))
			{
				auto& transform = transforms[slot.Name];
				for (const char* key : MaterialAsset::TransformFields)
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
			const std::optional<std::string> next = std::move(m_PendingOpen);
			Reset();
			if (next)
				Open(*next);
			ImGui::CloseCurrentPopup();
		}
		else if (cancel)
		{
			m_PendingOpen.reset();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

}
