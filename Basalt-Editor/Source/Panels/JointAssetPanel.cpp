#include "Panels/JointAssetPanel.h"

#include "EditorColors.h"
#include "FileDialogs.h"

#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Project/Project.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>

namespace Basalt {

	namespace {
		constexpr const char* FileDeletedMessage = "The file was deleted: Save writes it again.";
	}

	void PreviewModel::Set(const std::string& path)
	{
		// File changes and moves name files in this form.
		m_Path = path.empty() ? std::string() : FileSystem::NormalizeGeneric(path);
		m_Input = m_Path;
		m_Error.clear();
		m_DialogError.clear();
		m_Source = m_Path.empty() ? nullptr : AssetManager::GetMesh(m_Path);
		if (!m_Path.empty() && !m_Source)
			m_Error = "Cannot load the model: " + AssetManager::GetError(m_Path);
	}

	void PreviewModel::OnFilesChanged(const std::vector<FileChange>& changes)
	{
		// A model reloads with its buffers and images too.
		const auto affected = [&](const FileChange& change) { return change.Path == m_Path || (m_Source && std::ranges::find(m_Source->ExternalFiles, change.Path) != m_Source->ExternalFiles.end()); };
		if (!m_Path.empty() && std::ranges::any_of(changes, affected))
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
				const auto path = FileDialogs::Browse(FileDialogs::Kind::Open, Project::ResolvePath(m_Path), { { "glTF models", "gltf,glb" } }, m_DialogError);
				if (path)
					chosen = Project::MakeRelative(*path);
			}
			ImGui::SameLine();
		}
		ImGui::TextUnformatted(label);
		ImGui::PopID();
		if (!GetError().empty())
			ImGui::TextColored(EditorColors::Warning(), "%s", GetError().c_str());
		if (chosen == m_Path)
			return false;
		Set(chosen);
		return true;
	}

	template<typename Asset>
	JointAssetPanel<Asset>::JointAssetPanel(EditorContext& context, const char* title, std::vector<PreviewModel*> models)
		: m_Context(context)
		, m_Title(title)
		, m_Models(std::move(models))
		, m_DiscardPopup(std::string("Discard edits?##") + title)
	{
	}

	template<typename Asset>
	bool JointAssetPanel<Asset>::Open(const std::string& requested, std::string& outError)
	{
		// Before the checks below: they read a draft that may belong to the project just left.
		CloseIfProjectChanged();
		const std::filesystem::path given = FileSystem::PathFromUtf8(requested);
		const std::string path = FileSystem::NormalizeGeneric(given.is_absolute() ? Project::MakeRelative(given) : requested);
		if (!Project::GetActive() || !path.starts_with("Assets/") || FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(path)) != Asset::Extension)
		{
			outError = "'" + requested + "' is not a " + Asset::Extension + " file under Assets/";
			return false;
		}
		if (m_Draft.GetPath() == path)
			return true;
		if (m_Draft.IsDirty())
		{
			outError = "Save or Revert '" + m_Draft.GetPath() + "' before opening another file";
			return false;
		}
		if (!m_Draft.Load(path, outError))
			return false;
		m_ProjectDirectory = Project::GetActive()->GetDirectory();
		m_Error.clear();
		OnOpened();
		return true;
	}

	template<typename Asset>
	void JointAssetPanel<Asset>::Reset()
	{
		m_Draft.Close();
		m_ProjectDirectory.clear();
		m_Error.clear();
		for (PreviewModel* model : m_Models)
			model->Set({});
	}

	template<typename Asset>
	void JointAssetPanel<Asset>::CloseIfProjectChanged()
	{
		const Ref<Project>& active = Project::GetActive();
		if (!IsOpen() || (active && active->GetDirectory() == m_ProjectDirectory))
			return;
		if (m_Draft.IsDirty())
		{
			BS_WARN("Discarded the unsaved edits to '{}': its project closed", m_Draft.GetPath());
			m_Context.SetStatus("Discarded the unsaved edits to " + m_Draft.GetPath() + ": its project closed", true);
		}
		Reset();
	}

	template<typename Asset>
	void JointAssetPanel<Asset>::OnFilesChanged(const std::vector<FileChange>& changes)
	{
		for (PreviewModel* model : m_Models)
			model->OnFilesChanged(changes);
		const auto change = std::ranges::find(changes, m_Draft.GetPath(), &FileChange::Path);
		if (!IsOpen() || change == changes.end())
			return;
		std::string error;
		const bool followed = m_Draft.OnFileChanged(error);
		const bool removed = change->Type == FileChangeType::Removed;
		// A dirty draft reads nothing, so what it said stays, unless the file it called deleted is back.
		if (followed || (m_Error == FileDeletedMessage && !removed && error.empty()))
			m_Error.clear();
		else if (removed)
			m_Error = FileDeletedMessage;
		else if (!error.empty())
			m_Error = "The file changed on disk and cannot be read: " + error;
	}

	template<typename Asset>
	void JointAssetPanel<Asset>::OnAssetMoved(const std::string& from, const std::string& to)
	{
		m_Draft.OnAssetMoved(from, to);
		for (PreviewModel* model : m_Models)
			model->OnAssetMoved(from, to);
	}

	template<typename Asset>
	void JointAssetPanel<Asset>::OnImGuiRender()
	{
		CloseIfProjectChanged();
		if (!IsOpen())
			return;
		ImGui::SetNextWindowSize(ImVec2(520.0f, 640.0f), ImGuiCond_FirstUseEver);
		bool keepOpen = true;
		const bool dirty = m_Draft.IsDirty();
		if (ImGui::Begin(m_Title, &keepOpen, dirty ? ImGuiWindowFlags_UnsavedDocument : ImGuiWindowFlags_None))
		{
			ImGui::TextUnformatted(m_Draft.GetPath().c_str());
			DrawToolbar(dirty);
			ImGui::Separator();
			DrawBody();
		}
		ImGui::End();
		if (!keepOpen)
		{
			if (dirty)
				ImGui::OpenPopup(m_DiscardPopup.c_str());
			else
				Reset();
		}
		// Outside the window: Begin returns false for a tab behind another, whose X closes it all the same.
		DrawDiscardPrompt();
	}

	template<typename Asset>
	nlohmann::json JointAssetPanel<Asset>::Describe() const
	{
		std::string problem;
		const bool valid = IsOpen() && m_Draft.Validate(problem);
		nlohmann::json state = { { "open", IsOpen() }, { "path", m_Draft.GetPath() }, { "dirty", m_Draft.IsDirty() }, { "valid", valid }, { "problem", problem }, { "externalChange", m_Draft.HasExternalChange() }, { "error", m_Error }, { "joints", m_Draft.GetAsset().Joints.size() } };
		state.update(DescribeBody());
		return state;
	}

	template<typename Asset>
	void JointAssetPanel<Asset>::RecordFinishedEdit()
	{
		if (ImGui::IsItemDeactivatedAfterEdit())
			m_Draft.Record();
	}

	template<typename Asset>
	void JointAssetPanel<Asset>::DrawListFooter(std::optional<size_t> removed, const char* addLabel, uint32_t maxEntries)
	{
		auto& joints = m_Draft.GetAsset().Joints;
		if (removed)
		{
			joints.erase(joints.begin() + static_cast<std::ptrdiff_t>(*removed));
			m_Draft.Record();
		}
		ImGui::BeginDisabled(joints.size() >= maxEntries);
		if (ImGui::Button(addLabel))
		{
			joints.emplace_back();
			m_Draft.Record();
		}
		ImGui::EndDisabled();
	}

	template<typename Asset>
	void JointAssetPanel<Asset>::Save()
	{
		std::string problem;
		if (!m_Draft.Validate(problem))
		{
			m_Error = "Not saved: " + problem;
			return;
		}
		const auto result = m_Context.Execute("asset.write", { { "path", m_Draft.GetPath() }, { "content", m_Draft.MakeFileText() } });
		if (!result)
		{
			// Execute reported the command's error in the status bar.
			m_Error = "Not saved: " + m_Context.StatusMessage;
			return;
		}
		m_Draft.MarkSaved();
		m_Error.clear();
		const std::string warnings = EditorContext::JoinWarnings(*result);
		m_Context.SetStatus(warnings.empty() ? "Saved " + m_Draft.GetPath() : "Saved with warnings: " + warnings, !warnings.empty());
	}

	template<typename Asset>
	void JointAssetPanel<Asset>::Revert()
	{
		if (std::string error; m_Draft.Revert(error))
			m_Error.clear();
		else
			m_Error = "Cannot read the file: " + error;
	}

	template<typename Asset>
	void JointAssetPanel<Asset>::DrawToolbar(bool dirty)
	{
		const auto button = [](const char* label, bool enabled) {
			ImGui::BeginDisabled(!enabled);
			const bool pressed = ImGui::Button(label);
			ImGui::EndDisabled();
			return pressed;
		};
		std::string problem;
		const bool valid = m_Draft.Validate(problem);
		const bool changed = dirty || m_Draft.HasExternalChange();
		if (button("Save", changed && valid))
			Save();
		ImGui::SameLine();
		if (button("Revert", changed))
			Revert();
		ImGui::SameLine();
		if (button("Undo", m_Draft.CanUndo()))
			m_Draft.Undo();
		ImGui::SameLine();
		if (button("Redo", m_Draft.CanRedo()))
			m_Draft.Redo();
		if (m_Draft.HasExternalChange())
			ImGui::TextColored(EditorColors::Warning(), "The file changed on disk: Save replaces it, Revert loads it.");
		if (!m_Error.empty())
			ImGui::TextColored(EditorColors::Error(), "%s", m_Error.c_str());
		if (!valid)
			ImGui::TextColored(EditorColors::Warning(), "%s", problem.c_str());
	}

	template<typename Asset>
	void JointAssetPanel<Asset>::DrawDiscardPrompt()
	{
		if (ImGui::BeginPopupModal(m_DiscardPopup.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Discard the unsaved edits to %s?", m_Draft.GetPath().c_str());
			if (ImGui::Button("Discard"))
			{
				ImGui::CloseCurrentPopup();
				Reset();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	template class JointAssetPanel<AnimationMask>;
	template class JointAssetPanel<AnimationJointMap>;

}
