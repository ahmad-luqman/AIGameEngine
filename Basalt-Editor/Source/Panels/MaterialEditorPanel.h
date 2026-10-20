#pragma once

#include "EditorContext.h"
#include "MaterialAssetDraft.h"

#include "Basalt/Core/FileWatcher.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	class Project;
	class ThumbnailManager;

	// Edits one material file (.bmaterial) in its own window: every field of the full material model with a live
	// preview of the unsaved draft. Save writes it through asset.write (so it is validated like an agent's write);
	// Revert reads the file again. Opening another material or closing the window with unsaved edits asks first.
	class MaterialEditorPanel
	{
	public:
		explicit MaterialEditorPanel(EditorContext& context)
			: m_Context(context)
		{
		}

		static constexpr const char* Title = "Material Editor";

		void OnImGuiRender();
		void SetThumbnails(ThumbnailManager* thumbnails) { m_Thumbnails = thumbnails; }

		// Shows the material (asking about unsaved edits to the current one first).
		void Open(const std::string& path);
		// Shows the material without asking (editor.material_editor): fails while another material has unsaved edits.
		bool TryOpen(const std::string& path, std::string& outError);
		bool IsOpen() const { return m_Draft.IsOpen(); }
		// The problem shown in the window (a failed save, an unreadable change on disk), or empty.
		const std::string& GetError() const { return m_Error; }
		// The last save's warnings, joined (empty when it had none).
		const std::string& GetWarning() const { return m_Warning; }
		// Files changed on disk: an open clean draft follows its file, a dirty one reports the conflict.
		void OnFilesChanged(const std::vector<FileChange>& changes);
		// asset.move/asset.rename moved a file: the draft follows its own file and the maps that name the moved one.
		void OnAssetMoved(const std::string& from, const std::string& to);
		// The project closed or changed: the draft belongs to the old one.
		void Reset();

		const MaterialAssetDraft& GetDraft() const { return m_Draft; }

	private:
		bool HasOtherUnsavedDraft(const std::string& path) const;
		bool Save();
		void Revert();
		void DrawFields();
		void DrawDiscardPrompt();

		EditorContext& m_Context;
		ThumbnailManager* m_Thumbnails = nullptr;
		MaterialAssetDraft m_Draft;
		// The project the draft belongs to; another one closes it.
		std::weak_ptr<Project> m_Project;
		// The discard prompt to open, for opening this path (or closing the window when it has none).
		bool m_PromptDiscard = false;
		std::optional<std::string> m_PendingOpen;
		std::string m_Error;
		// The last save's warnings (the texture problems asset.write reports, an adopted sidecar).
		std::string m_Warning;
	};

}
