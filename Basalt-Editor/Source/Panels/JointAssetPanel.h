#pragma once

#include "EditorColors.h"
#include "EditorContext.h"
#include "JointAssetDraft.h"

#include "Basalt/Automation/AutomationSession.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/FileWatcher.h"
#include "Basalt/Project/Project.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace Basalt {

	class MeshSource;

	// A model named in a joint asset's window, to show its joints: it belongs to the window (the file holds names only).
	class PreviewModel
	{
	public:
		// Loads `path` (empty clears it); the reason stays in GetError when it cannot load.
		void Set(const std::string& path);
		// Reloads the model when one of `changes` is its file.
		void OnFilesChanged(const std::vector<FileChange>& changes);
		void OnAssetMoved(const std::string& from, const std::string& to);
		// A path field that also takes a model dropped from the Content Browser; true when the model changed.
		bool Draw(const char* label);

		const std::string& GetPath() const { return m_Path; }
		const std::string& GetError() const { return m_Error; }
		const Ref<MeshSource>& GetSource() const { return m_Source; }

	private:
		std::string m_Path;
		std::string m_Input;
		std::string m_Error;
		Ref<MeshSource> m_Source;
	};

	// One window over one joint list file (see JointAssetDraft): the same file handling for the mask and joint map editors,
	// which draw its body. It shows only while a file is open. Save writes through asset.write, so a save is validated like
	// an agent's write; closing with unsaved edits asks first.
	template<typename Asset>
	class JointAssetPanel
	{
	public:
		JointAssetPanel(EditorContext& context, const char* title)
			: m_Context(context)
			, m_Title(title)
		{
		}
		virtual ~JointAssetPanel() = default;
		JointAssetPanel(const JointAssetPanel&) = delete;
		JointAssetPanel& operator=(const JointAssetPanel&) = delete;

		bool IsOpen() const { return m_Draft.IsOpen(); }
		const JointAssetDraft<Asset>& GetDraft() const { return m_Draft; }
		// The problem shown in the window (a failed save, an unreadable change on disk), or empty.
		const std::string& GetError() const { return m_Error; }

		// Shows the file; fails while another file has unsaved edits.
		bool Open(const std::string& requested, std::string& outError)
		{
			const std::filesystem::path given = FileSystem::PathFromUtf8(requested);
			const std::string path = FileSystem::NormalizeGeneric(given.is_absolute() ? Project::MakeRelative(given) : requested);
			if (!Project::GetActive() || !path.starts_with("Assets/") || path.find("..") != std::string::npos || FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(path)) != Asset::Extension)
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
			m_Project = Project::GetActive();
			m_Error.clear();
			OnOpened();
			return true;
		}

		void Reset()
		{
			m_Draft.Close();
			m_Project.reset();
			m_Error.clear();
			m_PromptDiscard = false;
			OnReset();
		}

		void OnFilesChanged(const std::vector<FileChange>& changes)
		{
			OnFilesChangedInBody(changes);
			const auto change = std::find_if(changes.begin(), changes.end(), [&](const FileChange& candidate) { return candidate.Path == m_Draft.GetPath(); });
			if (!IsOpen() || change == changes.end())
				return;
			std::string error;
			if (m_Draft.OnFileChanged(error))
				m_Error.clear();
			else if (change->Type == FileChangeType::Removed)
				m_Error = "The file was deleted: Save writes a new file with a new identity, and references to the old one stay unresolved.";
			else if (!error.empty())
				m_Error = "The file changed on disk and cannot be read: " + error;
		}

		void OnAssetMoved(const std::string& from, const std::string& to)
		{
			m_Draft.OnAssetMoved(from, to);
			OnAssetMovedInBody(from, to);
		}

		void OnImGuiRender()
		{
			if (IsOpen() && m_Project.lock() != Project::GetActive())
				Reset();
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
				DrawDiscardPrompt();
			}
			ImGui::End();
			if (!keepOpen)
			{
				if (dirty)
					m_PromptDiscard = true;
				else
					Reset();
			}
		}

		// What editor.animation_mask and editor.joint_map return, on top of DescribeBody.
		nlohmann::json Describe() const
		{
			std::string problem;
			const bool valid = IsOpen() && m_Draft.Validate(problem);
			nlohmann::json state = { { "open", IsOpen() }, { "path", m_Draft.GetPath() }, { "dirty", m_Draft.IsDirty() }, { "valid", valid }, { "problem", problem }, { "externalChange", m_Draft.HasExternalChange() }, { "error", m_Error }, { "joints", m_Draft.GetAsset().Joints.size() } };
			state.update(DescribeBody());
			return state;
		}

	protected:
		virtual void DrawBody() = 0;
		virtual nlohmann::json DescribeBody() const = 0;
		// The hooks of what the body keeps beside the draft (its preview models).
		virtual void OnOpened() {}
		virtual void OnReset() {}
		virtual void OnFilesChangedInBody(const std::vector<FileChange>&) {}
		virtual void OnAssetMovedInBody(const std::string&, const std::string&) {}

		EditorContext& m_Context;
		JointAssetDraft<Asset> m_Draft;

	private:
		bool Save()
		{
			std::string problem;
			if (!m_Draft.Validate(problem))
			{
				m_Error = "Not saved: " + problem;
				return false;
			}
			if (!m_Context.Execute("asset.write", { { "path", m_Draft.GetPath() }, { "content", m_Draft.MakeFileText() } }))
			{
				// Execute reported the command's error in the status bar.
				m_Error = "Not saved: " + m_Context.StatusMessage;
				return false;
			}
			m_Draft.MarkSaved();
			m_Error.clear();
			m_Context.SetStatus("Saved " + m_Draft.GetPath(), false);
			return true;
		}

		void Revert()
		{
			if (std::string error; m_Draft.Revert(error))
				m_Error.clear();
			else
				m_Error = "Cannot read the file: " + error;
		}

		void DrawToolbar(bool dirty)
		{
			std::string problem;
			const bool valid = m_Draft.Validate(problem);
			const bool changed = dirty || m_Draft.HasExternalChange();
			ImGui::BeginDisabled(!changed || !valid);
			if (ImGui::Button("Save"))
				Save();
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!changed);
			if (ImGui::Button("Revert"))
				Revert();
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!m_Draft.CanUndo());
			if (ImGui::Button("Undo"))
				m_Draft.Undo();
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!m_Draft.CanRedo());
			if (ImGui::Button("Redo"))
				m_Draft.Redo();
			ImGui::EndDisabled();
			if (m_Draft.HasExternalChange())
				ImGui::TextColored(EditorColors::Warning(), "The file changed on disk: Save replaces it, Revert loads it.");
			if (!m_Error.empty())
				ImGui::TextColored(EditorColors::Error(), "%s", m_Error.c_str());
			if (!valid)
				ImGui::TextColored(EditorColors::Warning(), "%s", problem.c_str());
		}

		void DrawDiscardPrompt()
		{
			if (m_PromptDiscard)
				ImGui::OpenPopup("Discard edits?");
			m_PromptDiscard = false;
			if (ImGui::BeginPopupModal("Discard edits?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
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

		const char* m_Title;
		// The project the draft belongs to; another one closes it.
		std::weak_ptr<Project> m_Project;
		std::string m_Error;
		bool m_PromptDiscard = false;
	};

}
