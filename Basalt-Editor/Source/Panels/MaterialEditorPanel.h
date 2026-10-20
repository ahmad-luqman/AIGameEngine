#pragma once

#include "EditorContext.h"
#include "MaterialAssetDraft.h"

#include "Basalt/Core/FileWatcher.h"

#include <memory>
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
		bool IsOpen() const { return m_Open; }
		// The problem shown in the window (a failed save, an unreadable change on disk), or empty.
		const std::string& GetError() const { return m_Error; }
		// Files changed on disk: an open clean draft follows its file, a dirty one reports the conflict.
		void OnFilesChanged(const std::vector<FileChange>& changes);
		// The project closed or changed: the draft belongs to the old one.
		void Reset();

		const MaterialAssetDraft& GetDraft() const { return m_Draft; }

	private:
		bool Save();
		void Revert();
		void DrawFields();
		void DrawDiscardPrompt();

		EditorContext& m_Context;
		ThumbnailManager* m_Thumbnails = nullptr;
		MaterialAssetDraft m_Draft;
		// The project the draft belongs to; another one closes it.
		std::weak_ptr<Project> m_Project;
		bool m_Open = false;
		// What a discard prompt is for: opening this path, or closing the window when empty.
		std::string m_PendingOpen;
		bool m_PromptDiscard = false;
		std::string m_Error;
	};

}
