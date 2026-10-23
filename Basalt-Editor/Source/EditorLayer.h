#pragma once

#include "EditorCamera.h"
#include "EditorContext.h"
#include "ThumbnailManager.h"
#include "Basalt/Automation/EditorHistory.h"
#include "Panels/ConsolePanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/InspectorPanel.h"
#include "Panels/MaterialEditorPanel.h"
#include "Panels/AnimationGraphPanel.h"
#include "Panels/PropertyAnimationPanel.h"
#include "Panels/ProjectSettingsPanel.h"
#include "Panels/SceneHierarchyPanel.h"

#include "Basalt/Automation/AutomationServer.h"
#include "Basalt/Automation/EditorPreferences.h"
#include "Basalt/Automation/GameView.h"
#include "Basalt/Automation/ViewportSettings.h"
#include "Basalt/Automation/AutomationSession.h"
#include "Basalt/Automation/CommandRegistry.h"
#include "Basalt/Automation/AutomationConnection.h"
#include "Basalt/Core/Layer.h"
#include "Basalt/Core/Timer.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Renderer/DebugDraw.h"
#include "Basalt/Renderer/SceneRenderer.h"
#include "Basalt/UI/UIRenderer.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	struct PlayStatistics;

	struct EditorOptions
	{
		std::string ProjectPath;
		// Automation server port; 0 disables the server.
		uint16_t AutomationPort = AutomationServer::DefaultPort;
	};

	// The editor: dockable panels around a scene viewport, play mode, undo, and the automation server
	// that lets AI agents drive the running editor over TCP.
	class EditorLayer : public Layer
	{
	public:
		explicit EditorLayer(EditorOptions options);
		~EditorLayer() override;

		void OnAttach() override;
		void OnDetach() override;
		void OnUpdate(Timestep ts) override;
		void OnRender() override;
		void OnImGuiRender() override;

	private:
		// Frame delta for exposure adaptation; captures explicitly use zero after resetting history.
		float m_RenderDeltaTime = 0.0f;
		// Restore the editor presentation preference after discarding the Play session.
		std::optional<bool> m_PrePlayVSync;
		void RegisterEditorCommands();
		void OnSceneChanged();

		// Reads the user's preferences (an invalid file is set aside, an unreadable one left alone for this session) and
		// points ImGui's layout file next to them (first run: a copy of ./imgui.ini).
		void LoadPreferences();
		// Gives the camera, theme and file watching their preferred values.
		void ApplyPreferences();
		void MarkPreferencesDirty();
		// Writes changed preferences: at once with `now`, otherwise once PreferencesSaveDelay has passed since the last
		// change.
		void SavePreferences(bool now);
		// Applies the changed files the background scans found (they run while autoReload is on), on the main thread.
		void UpdateFileWatching();
		// Records the active project as recent and the wheel's camera speed, and saves the preferences when due.
		void UpdatePreferences();
		// Stops play and opens the project; false (the error on the status bar) when it did not open.
		bool OpenRecentProject(const std::string& directory);
		// File > Open Recent Project: a menu item per project (greyed out when its folder is gone) and Clear.
		void DrawRecentProjectsMenu();
		// The Open Project prompt's list: a click fills in the path, a double click opens the project.
		void DrawRecentProjectsList();

		void BuildDefaultLayout(unsigned int dockspaceId);
		void DrawMenuBar();
		void DrawViewport();
		// The Game tab: the primary camera at the game resolution, letterboxed, with the game UI. It is where the
		// game runs in Play (it owns the game UI's pointer and keyboard); in Edit and Simulate it previews the
		// primary camera, labelled with the mode.
		void DrawGameView();
		PlayStatistics GetPlayStatistics() const;
		// The session preference draws only in a visible, playing Game view.
		bool IsStatsOverlayActive() const { return m_GameView.ShowStats() && IsGameRunning() && m_GameViewVisible; }
		void DrawGameViewOptions();
		// Gives the window the cursor the game asks for (DecideGameCursor), before the scene updates: Locked captures the
		// mouse while the game runs with the Game view focused (ImGui ignores it meanwhile, as GLFW reports a virtual
		// cursor), Hidden hides it over the Game image. Escape hands a locked cursor back to the editor until the Game
		// view is clicked; the game sees neither that Escape nor that click.
		void UpdateGameCursor();
		// Gives the game its view before the scene updates and as Play starts: the Game image's geometry (the game
		// resolution at 0,0 before the tab is first drawn), the game resolution, and the routing (RouteGameView).
		void ApplyGameView();
		// The selected entity's material on a sphere, at the top of the inspector's Material section.
		void DrawMaterialPreview(Entity entity);
		// The inspector section of a prefab instance root: its actions and overrides.
		void DrawPrefabInstance(Entity entity);
		// Runs a GUI command and shows its result's warnings in the status bar.
		std::optional<nlohmann::json> ExecuteAndWarn(const std::string& command, const nlohmann::json& params = nlohmann::json::object());
		void DrawToolbar();
		void DrawViewportOptions();
		void DrawOverlayMenu();
		void DrawSceneSettingsPanel();
		void DrawStatusBar();
		void DrawModals();
		// Docks a window that has no saved layout yet next to an existing one (new windows in old imgui.ini files).
		// True when it docked the window (this frame's Begin creates it there).
		static bool DockNextToOnFirstUse(const char* window, const char* neighbour);
		void HandleShortcuts();

		void DrawEditorOverlays();
		// Draws the scene's game UI (UI components, plus the scripts' UI.* output in Play) over `renderer`'s last
		// image, rasterized at the capture size.
		void DrawGameUIOverCapture(Scene& scene, SceneRenderer& renderer, uint32_t width, uint32_t height);
		// The editor camera's view with editor helpers (the scene view, in every mode).
		void RenderSceneView(Scene& scene, const std::vector<DebugLineVertex>& scriptLines);
		// Renders the primary camera at the game resolution into m_GameRenderer (created on first use) and records
		// the outcome in m_GameViewStatus.
		void RenderGameView(Scene& scene, const std::vector<DebugLineVertex>& scriptLines);
		glm::uvec2 GetGameResolution() const { return m_GameView.Resolve(Project::GetActiveConfig()); }
		// "Play", "Simulate" or "Edit".
		const char* GetSessionModeName() const;
		nlohmann::json GetGameViewState();
		// Play mode proper: scripts run and the Game view hosts the game (not edit mode, not simulate).
		bool IsGameRunning() const { return m_Session.IsPlaying() && !m_Session.IsSimulating(); }
		// The cursor the game asks for, Normal while it does not run.
		CursorMode GetRequestedGameCursor() const { return IsGameRunning() ? Input::GetCursorMode() : CursorMode::Normal; }
		// Outlines every UI rect over the viewport (the selected entity's with its pivot and anchors).
		void DrawUIBounds(const glm::vec2& origin, const glm::vec2& size);
		bool HandleUIRectHandles(Entity selected, const glm::vec2& origin, const glm::vec2& size);
		UUID PickViewport(uint32_t x, uint32_t y);
		bool PrepareCaptureRenderer(uint32_t width, uint32_t height, float scale, std::string& error);
		void HandleVertexSnapDrag(Entity selected, bool enabled, const glm::vec2& origin);
		void FinishGizmo(bool cancel);
		void CancelVertexSnap() { m_VertexSnapDrag.reset(); }
		void HandleViewportClick(const glm::vec2& mouse);
		void HandleAssetDrop(const std::string& path);
		void OpenAsset(const std::string& path);

		void Play(bool simulate);
		void Stop();
		bool Undo();
		bool Redo();
		bool RestoreState(nlohmann::json& state, std::string& error);
		void SaveScene();
		void FocusSelection();
		void UpdateWindowTitle();

		// What a capture shows: the scene view (editor camera and helpers), the primary camera at any size
		// (render.screenshot; the editor camera without one, and the scene view's debug view), or the Game view's
		// image (primary camera at the game resolution, game UI, never a debug view; it fails without a camera).
		// Every capture resets exposure and renders at the session's render scale.
		enum class CaptureView
		{
			SceneView,
			GameCamera,
			GameView
		};
		bool RenderToFile(const std::filesystem::path& path, uint32_t width, uint32_t height, CaptureView view, std::string& outError);
		std::string HandleAutomationRequest(const std::shared_ptr<AutomationConnection>& connection, const std::string& request);

	private:
		EditorOptions m_Options;
		CommandRegistry m_Commands;
		AutomationSession m_Session;
		EditorHistory m_History;
		EditorContext m_Context;
		AutomationServer m_Server;

		SceneHierarchyPanel m_HierarchyPanel;
		InspectorPanel m_InspectorPanel;
		ContentBrowserPanel m_ContentBrowserPanel;
		MaterialEditorPanel m_MaterialEditorPanel;
		AnimationGraphPanel m_AnimationGraphPanel;
		PropertyAnimationPanel m_PropertyAnimationPanel;
		// The path typed into the inspector's "Save as Material Asset" prompt.
		std::string m_SaveMaterialPath;
		ConsolePanel m_ConsolePanel;
		ProjectSettingsPanel m_ProjectSettingsPanel;
		bool m_ProjectSettingsOpen = false;
		// Windows editor.panel asked to bring to the front; applied at the end of the next ImGui frame, once the
		// window exists.
		std::vector<std::string> m_PendingFocus;

		EditorCamera m_Camera;
		Scope<SceneRenderer> m_Renderer;
		Scope<SceneRenderer> m_CaptureRenderer;
		Scope<SceneRenderer> m_GameRenderer;
		Scope<ThumbnailManager> m_Thumbnails;
		nvrhi::CommandListHandle m_CommandList;
		// The viewport's game UI: built in OnRender, drawn over the viewport image in DrawViewport.
		UIFrame m_UIFrame;
		// The Game view's game UI, laid out for its letterboxed image.
		UIFrame m_GameUIFrame;
		GameViewSettings m_GameView;
		// The Game panel's content rectangle and the letterboxed image inside it, in ImGui (logical) screen units, and
		// how the tab was last drawn.
		glm::vec2 m_GamePanelMin = { 0.0f, 0.0f };
		glm::vec2 m_GamePanelSize = { 0.0f, 0.0f };
		glm::vec2 m_GameImageMin = { 0.0f, 0.0f };
		glm::vec2 m_GameImageSize = { 0.0f, 0.0f };
		bool m_GameViewVisible = false;
		bool m_GameViewHovered = false;
		bool m_GameViewFocused = false;
		// How the Game view's last render went (what the tab shows instead of an image when it did not render).
		enum class GameViewStatus
		{
			NotRendered,
			Rendered,
			NoCamera,
			RendererUnavailable,
			RenderFailed
		};
		GameViewStatus m_GameViewStatus = GameViewStatus::NotRendered;
		// The Game view renderer failed to initialize: not tried again this session.
		bool m_GameRendererFailed = false;
		static const char* GameViewStatusName(GameViewStatus status);
		// Play brings the Game tab to the front; Stop returns to the scene view if it was in front before.
		bool m_GameWasRunning = false;
		bool m_ReturnToViewport = false;
		// What UpdateGameCursor last decided, and whether the window holds the cursor for the game now.
		GameCursorDecision m_GameCursor;
		bool m_CursorLocked = false;
		bool m_ViewportVisible = true;

		glm::vec2 m_ViewportSize = { 1280.0f, 720.0f };
		glm::vec2 m_ViewportMin = { 0.0f, 0.0f };
		bool m_ViewportHovered = false;
		bool m_ViewportFocused = false;
		ViewportSettings m_ViewportSettings;
		struct VertexSnapDrag
		{
			UUID EntityId;
			glm::vec2 Source;
		};
		std::optional<VertexSnapDrag> m_VertexSnapDrag;
		struct UIRectDrag
		{
			UIRectComponent Initial;
			UIBox Box;
			glm::mat3 ParentTransform{ 1.0f };
			glm::vec2 Start;
			glm::vec2 Handle;
			GizmoOperation Operation = GizmoOperation::Translate;
			bool Resize = false;
		};
		std::optional<UIRectDrag> m_UIRectDrag;
		bool m_GizmoWasUsing = false;
		bool m_GizmoCancelled = false;
		glm::mat4 m_GizmoPivot{ 1 };
		std::vector<UUID> m_GizmoSelection;
		std::optional<glm::vec2> m_BoxStart;
		bool m_Paused = false;
		RendererDebugView m_DebugView = RendererDebugView::None;

		enum class PromptKind
		{
			None,
			OpenScene,
			SaveSceneAs,
			OpenPrefab,
			SavePrefabAs,
			ImportModel,
			OpenProject,
			NewProject,
			Export
		};
		void OpenPrompt(PromptKind kind, std::string path);
		void BrowseForPromptPath();
		PromptKind m_Prompt = PromptKind::None;
		bool m_PromptOpened = false;
		std::string m_PromptPath;
		std::string m_PromptName;
		// Export prompt: smoke-test the exported game (project.export smokeTestFrames).
		bool m_ExportSmokeTest = true;
		int m_ExportSmokeFrames = 120;
		bool m_ExportPackage = false;
		std::string m_WindowTitle;
		// prefab.overrides of the inspected instance, recomputed when the scene changes rather than every frame.
		struct PrefabOverridesCache
		{
			UUID Instance;
			uint64_t Revision = 0;
			nlohmann::json Overrides;
		};
		std::optional<PrefabOverridesCache> m_PrefabOverrides;
		// The waiting scene's undo history while a prefab is open, restored when it closes.
		std::optional<EditorHistory> m_WaitingHistory;

		EditorPreferences m_Preferences;
		// Empty when the user data directory is unavailable: preferences then last for this session only.
		std::filesystem::path m_PreferencesFile;
		// Where ImGui keeps the window layout (./imgui.ini until LoadPreferences points it next to the preferences).
		std::filesystem::path m_LayoutFile = EditorPreferences::LayoutFileName;
		bool m_PreferencesDirty = false;
		// Why the last save failed (empty after a success): retried PreferencesSaveDelay later and on exit.
		std::string m_PreferencesSaveError;
		// Since the last change.
		Timer m_PreferencesChanged;
		// The project last added to the recent list.
		std::weak_ptr<Project> m_RecentProject;
	};

}
