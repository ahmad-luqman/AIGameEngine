#pragma once

#include "Basalt/Core/Layer.h"
#include "Basalt/ImGui/ImGuiRenderer.h"

#include <glm/glm.hpp>

#include <string>

struct ImDrawList;

namespace Basalt {

	// Owns the Dear ImGui context: GLFW platform backend + Basalt's nvrhi renderer backend.
	// The Application pushes it as an overlay when ApplicationSpecification::EnableImGui is set.
	class ImGuiLayer : public Layer
	{
	public:
		ImGuiLayer();
		~ImGuiLayer() override = default;

		void OnAttach() override;
		void OnDetach() override;
		void OnEvent(Event& event) override;

		void Begin();
		void End();

		// When true, mouse/keyboard events ImGui wants are not forwarded to lower layers.
		void SetBlockEvents(bool block) { m_BlockEvents = block; }
		// False if the platform or renderer backend failed to initialize; ImGui frames are then skipped.
		bool IsInitialized() const { return m_Initialized; }

		// Renders one draw list (built outside the ImGui frame, e.g. game UI for a screenshot) into `target`,
		// whose top-left `displaySize` pixels it covers. Uploads pending font atlas updates first.
		void RenderDrawList(ImDrawList& drawList, const glm::vec2& displaySize, nvrhi::IFramebuffer* target);

		static void SetDarkThemeColors();
		// The editor's light theme: ImGui's light colours with the same shape as the dark theme.
		static void SetLightThemeColors();

		// Where ImGui loads and saves window layouts (empty: nowhere). The layer owns the string: ImGui writes the file
		// through this pointer until the context is destroyed. Call before the first frame for the layout to load.
		void SetIniFilename(std::string path);

	private:
		ImGuiRenderer m_Renderer;
		std::string m_IniFilename = "imgui.ini";
		bool m_BlockEvents = true;
		bool m_Initialized = false;
		bool m_HasPlatformBackend = false;
	};

}
