#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace Basalt::EditorCommandSchemas {

	// Editor tool contracts live in the headless core so discovery and validation are testable without a GPU.
	nlohmann::json Pick();
	nlohmann::json VertexSnap();
	nlohmann::json Select();
	nlohmann::json Camera();
	nlohmann::json Transform();
	nlohmann::json ComponentSet();
	nlohmann::json BoxSelect();
	nlohmann::json Transaction();
	nlohmann::json Undo();
	nlohmann::json Redo();
	nlohmann::json WindowScreenshot();
	nlohmann::json Screenshot();
	nlohmann::json Panel();
	nlohmann::json Thumbnail();
	nlohmann::json MaterialPreview();
	nlohmann::json MaterialEditor();

	// Editor window titles (ImGui window names; editor.panel takes them).
	inline constexpr const char* ViewportTitle = "Viewport";
	inline constexpr const char* GameTitle = "Game";
	inline constexpr const char* HierarchyTitle = "Hierarchy";
	inline constexpr const char* InspectorTitle = "Inspector";
	inline constexpr const char* SceneTitle = "Scene";
	inline constexpr const char* ContentBrowserTitle = "Content Browser";
	inline constexpr const char* ConsoleTitle = "Console";
	inline constexpr const char* ProjectSettingsTitle = "Project Settings";

	// An editor window editor.panel can show and focus, by its ImGui window title.
	struct PanelInfo
	{
		const char* Title;
		// Only these can be closed; the others are always open.
		bool Closable;
	};
	inline constexpr PanelInfo Panels[] = { { ViewportTitle, false }, { GameTitle, false }, { HierarchyTitle, false }, { InspectorTitle, false }, { SceneTitle, false }, { ContentBrowserTitle, false }, { ConsoleTitle, false }, { ProjectSettingsTitle, true } };

	bool IsClosablePanel(const std::string& title);

	// The editor keeps one open flag for closable windows: Project Settings must be the only one.
	constexpr bool ProjectSettingsIsTheOnlyClosablePanel()
	{
		size_t closable = 0;
		for (const PanelInfo& panel : Panels)
			if (panel.Closable)
			{
				if (panel.Title != ProjectSettingsTitle)
					return false;
				closable++;
			}
		return closable == 1;
	}

}
