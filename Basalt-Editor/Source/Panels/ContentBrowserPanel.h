#pragma once

#include "EditorContext.h"
#include "ThumbnailManager.h"

#include <filesystem>
#include <functional>
#include <string>

namespace Basalt {

	// Browses the project's Assets directory. Files are drag sources (project-relative paths) for the
	// viewport, hierarchy and inspector; double-clicking a scene opens it (a material opens in the Material Editor).
	// Images, environments, models, prefabs and materials show previews (ThumbnailManager), with a larger one and
	// details on hover. New scripts, materials and folders are made from the context menu; a model's materials can be
	// extracted to material files.
	class ContentBrowserPanel
	{
	public:
		explicit ContentBrowserPanel(EditorContext& context)
			: m_Context(context)
		{
		}

		void OnImGuiRender();
		// Previews come from here once the renderer exists; without it the browser shows type tags only.
		void SetThumbnails(ThumbnailManager* thumbnails) { m_Thumbnails = thumbnails; }

		// Invoked when a file is double-clicked (the editor opens scenes, imports models, ...).
		std::function<void(const std::string& path)> OnOpenAsset;

	private:
		void DrawEntry(const std::filesystem::directory_entry& entry, float rowHeight);
		void DrawPreviewTooltip(const ThumbnailManager::Preview& preview, const std::string& relative, const std::string& fileName);

		EditorContext& m_Context;
		ThumbnailManager* m_Thumbnails = nullptr;
		float m_IconSize = 32.0f;
		std::filesystem::path m_CurrentDirectory;
		std::string m_NewItemName;
		bool m_CreatingScript = false;
		bool m_CreatingMaterial = false;
		bool m_CreatingFolder = false;
		std::filesystem::path m_PendingDelete;
	};

}
