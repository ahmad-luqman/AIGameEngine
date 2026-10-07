#pragma once

#include "EditorContext.h"

#include <filesystem>
#include <functional>
#include <string>

namespace Basalt {

	// Browses the project's Assets directory. Files are drag sources (project-relative paths) for the
	// viewport, hierarchy and inspector; double-clicking a scene opens it.
	class ContentBrowserPanel
	{
	public:
		explicit ContentBrowserPanel(EditorContext& context)
			: m_Context(context)
		{
		}

		void OnImGuiRender();

		// Invoked when a file is double-clicked (the editor opens scenes, imports models, ...).
		std::function<void(const std::string& path)> OnOpenAsset;

	private:
		EditorContext& m_Context;
		std::filesystem::path m_CurrentDirectory;
		std::string m_NewItemName;
		bool m_CreatingScript = false;
		bool m_CreatingFolder = false;
		std::filesystem::path m_PendingDelete;
	};

}
