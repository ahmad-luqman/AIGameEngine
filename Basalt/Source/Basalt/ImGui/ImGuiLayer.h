#pragma once

#include "Basalt/Core/Layer.h"
#include "Basalt/ImGui/ImGuiRenderer.h"

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

		static void SetDarkThemeColors();

	private:
		ImGuiRenderer m_Renderer;
		bool m_BlockEvents = true;
		bool m_Initialized = false;
		bool m_HasPlatformBackend = false;
	};

}
