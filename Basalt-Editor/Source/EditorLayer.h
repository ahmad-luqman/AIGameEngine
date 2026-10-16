#pragma once

#include "EditorCamera.h"
#include "EditorContext.h"
#include "Basalt/Automation/EditorHistory.h"
#include "Panels/ConsolePanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/InspectorPanel.h"
#include "Panels/ProjectSettingsPanel.h"
#include "Panels/SceneHierarchyPanel.h"

#include "Basalt/Automation/AutomationServer.h"
#include "Basalt/Automation/ViewportSettings.h"
#include "Basalt/Automation/AutomationSession.h"
#include "Basalt/Automation/CommandRegistry.h"
#include "Basalt/Automation/AutomationConnection.h"
#include "Basalt/Core/Layer.h"
#include "Basalt/Renderer/SceneRenderer.h"
#include "Basalt/UI/UIRenderer.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <optional>
#include <string>

namespace Basalt {

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

		void BuildDefaultLayout(unsigned int dockspaceId);
		void DrawMenuBar();
		void DrawViewport();
		void DrawToolbar();
		void DrawViewportOptions();
		void DrawOverlayMenu();
		void DrawSceneSettingsPanel();
		void DrawStatusBar();
		void DrawModals();
		// Docks a window that has no saved layout yet next to an existing one (new windows in old imgui.ini files).
		static void DockNextToOnFirstUse(const char* window, const char* neighbour);
		void HandleShortcuts();

		void DrawEditorOverlays();
		// Draws the scene's game UI over the capture renderer's last image (screenshots show what the viewport
		// shows).
		void DrawGameUIOverCapture(Scene& scene, uint32_t width, uint32_t height);
		// Play mode proper: the game's camera and its immediate-mode UI (not edit mode, not simulate).
		bool IsGameRunning() const { return m_Session.IsPlaying() && !m_Session.IsSimulating(); }
		// Outlines every UI rect over the viewport (the selected entity's with its pivot and anchors).
		void DrawUIBounds(const glm::vec2& origin, const glm::vec2& size);
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

		bool RenderToFile(const std::filesystem::path& path, uint32_t width, uint32_t height, bool useEditorCamera, std::string& outError);
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
		ConsolePanel m_ConsolePanel;
		ProjectSettingsPanel m_ProjectSettingsPanel;
		bool m_ProjectSettingsOpen = false;
		// Windows editor.panel asked to bring to the front; applied at the end of the next ImGui frame, once the
		// window exists.
		std::vector<std::string> m_PendingFocus;

		EditorCamera m_Camera;
		Scope<SceneRenderer> m_Renderer;
		Scope<SceneRenderer> m_CaptureRenderer;
		nvrhi::CommandListHandle m_CommandList;
		// The viewport's game UI: built in OnRender, drawn over the viewport image in DrawViewport.
		UIFrame m_UIFrame;

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
			OpenProject,
			NewProject,
			Export
		};
		PromptKind m_Prompt = PromptKind::None;
		bool m_PromptOpened = false;
		std::string m_PromptPath;
		std::string m_PromptName;
		// Export prompt: smoke-test the exported game (project.export smokeTestFrames).
		bool m_ExportSmokeTest = true;
		int m_ExportSmokeFrames = 120;
		std::string m_WindowTitle;
	};

}
