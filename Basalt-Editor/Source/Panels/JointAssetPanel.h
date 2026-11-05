#pragma once

#include "EditorContext.h"
#include "JointAssetDraft.h"

#include "Basalt/Core/FileWatcher.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
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
		// Why the model cannot show (it did not load), else why its file dialog failed.
		const std::string& GetError() const { return m_Error.empty() ? m_DialogError : m_Error; }
		const Ref<MeshSource>& GetSource() const { return m_Source; }

	private:
		std::string m_Path;
		std::string m_Input;
		std::string m_Error;
		std::string m_DialogError;
		Ref<MeshSource> m_Source;
	};

	// One window over one joint list file (see JointAssetDraft): the same file handling for the mask and joint map editors,
	// which draw its body. It shows only while a file is open. Save writes through asset.write, so a save is validated like
	// an agent's write; closing with unsaved edits asks first.
	template<typename Asset>
	class JointAssetPanel
	{
	public:
		JointAssetPanel(EditorContext& context, const char* title, std::vector<PreviewModel*> models);
		virtual ~JointAssetPanel() = default;
		JointAssetPanel(const JointAssetPanel&) = delete;
		JointAssetPanel& operator=(const JointAssetPanel&) = delete;

		bool IsOpen() const { return m_Draft.IsOpen(); }
		const JointAssetDraft<Asset>& GetDraft() const { return m_Draft; }
		// The problem shown in the window (a failed save, an unreadable change on disk), or empty.
		const std::string& GetError() const { return m_Error; }

		// Shows the file; fails while another file has unsaved edits.
		bool Open(const std::string& requested, std::string& outError);
		void Reset();
		void OnFilesChanged(const std::vector<FileChange>& changes);
		void OnAssetMoved(const std::string& from, const std::string& to);
		void OnImGuiRender();
		// What editor.animation_mask and editor.joint_map return, on top of DescribeBody.
		nlohmann::json Describe() const;

	protected:
		virtual void DrawBody() = 0;
		virtual nlohmann::json DescribeBody() const = 0;
		// A file was opened: the body names its models.
		virtual void OnOpened() = 0;
		// Records the finished edit of the item drawn last.
		void RecordFinishedEdit();
		// The rest of a list after its rows: drops the row whose Remove was pressed, then draws its Add button.
		void DrawListFooter(std::optional<size_t> removed, const char* addLabel, uint32_t maxEntries);

		EditorContext& m_Context;
		JointAssetDraft<Asset> m_Draft;

	private:
		// Another project closes the draft, and with it any unsaved edits: says so. The same project reloaded keeps them.
		void CloseIfProjectChanged();
		void Save();
		void Revert();
		void DrawToolbar(bool dirty);
		void DrawDiscardPrompt();

		const char* m_Title;
		std::vector<PreviewModel*> m_Models;
		// Where the draft's project lives: its path means nothing in another one.
		std::filesystem::path m_ProjectDirectory;
		std::string m_Error;
		// Two panels draw their prompts side by side, outside their windows, so each needs a popup of its own.
		std::string m_DiscardPopup;
	};

}
