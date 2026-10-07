#pragma once

#include "EditorCamera.h"
#include "EditorContext.h"
#include "EditorHistory.h"
#include "Panels/ConsolePanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/InspectorPanel.h"
#include "Panels/SceneHierarchyPanel.h"

#include "Basalt/Automation/AutomationServer.h"
#include "Basalt/Automation/AutomationSession.h"
#include "Basalt/Automation/CommandRegistry.h"
#include "Basalt/Core/Layer.h"
#include "Basalt/Renderer/SceneRenderer.h"

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
		void RegisterEditorCommands();
		void OnSceneChanged();

		void BuildDefaultLayout(unsigned int dockspaceId);
		void DrawMenuBar();
		void DrawViewport();
		void DrawToolbar();
		void DrawSceneSettingsPanel();
		void DrawStatusBar();
		void DrawModals();
		void HandleShortcuts();

		void DrawEditorOverlays();
		void HandleViewportClick(const glm::vec2& mouse);
		void HandleAssetDrop(const std::string& path);
		void OpenAsset(const std::string& path);

		void Play(bool simulate);
		void Stop();
		void Undo();
		void Redo();
		void RestoreState(const nlohmann::json& state);
		void SaveScene();
		void FocusSelection();
		void UpdateWindowTitle();

		bool RenderToFile(const std::filesystem::path& path, uint32_t width, uint32_t height, bool useEditorCamera, std::string& outError);
		std::string HandleAutomationRequest(const std::string& request);

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

		EditorCamera m_Camera;
		Scope<SceneRenderer> m_Renderer;
		Scope<SceneRenderer> m_CaptureRenderer;
		nvrhi::CommandListHandle m_CommandList;

		glm::vec2 m_ViewportSize = { 1280.0f, 720.0f };
		glm::vec2 m_ViewportMin = { 0.0f, 0.0f };
		bool m_ViewportHovered = false;
		bool m_ViewportFocused = false;
		int m_GizmoOperation = 7; // ImGuizmo::TRANSLATE
		bool m_GizmoWasUsing = false;
		bool m_Paused = false;
		bool m_ShowGrid = true;
		bool m_ShowColliders = true;
		bool m_ShowLightGizmos = true;
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
		std::string m_WindowTitle;
	};

}
