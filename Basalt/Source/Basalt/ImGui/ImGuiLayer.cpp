#include "Basalt/ImGui/ImGuiLayer.h"

#include "Basalt/Core/Application.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Renderer/GraphicsDevice.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
// ImGuizmo.h relies on imgui.h being included first.
#include <ImGuizmo.h>

namespace Basalt {

	ImGuiLayer::ImGuiLayer()
		: Layer("ImGuiLayer")
	{
	}

	void ImGuiLayer::OnAttach()
	{
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		io.IniFilename = "imgui.ini";

		ImGui::StyleColorsDark();
		SetDarkThemeColors();
		ImGuiStyle& style = ImGui::GetStyle();
		style.WindowRounding = 2.0f;
		style.FrameRounding = 2.0f;
		style.FontSizeBase = 15.0f;

		Application& application = Application::Get();
		// Offscreen applications have no window: there is no platform backend, and Begin() feeds the
		// display size itself. Drawing (the game UI) works the same.
		m_HasPlatformBackend = application.GetWindow() != nullptr;
		if (m_HasPlatformBackend && !ImGui_ImplGlfw_InitForOther(application.GetWindow()->GetNativeWindow(), true))
		{
			BS_CORE_ERROR("ImGuiLayer: GLFW backend initialization failed");
			return;
		}

		if (!m_Renderer.Init(application.GetGraphicsDevice()->GetDevice()))
		{
			BS_CORE_ERROR("ImGuiLayer: renderer backend initialization failed");
			if (m_HasPlatformBackend)
				ImGui_ImplGlfw_Shutdown();
			return;
		}
		m_Initialized = true;
	}

	void ImGuiLayer::OnDetach()
	{
		if (m_Initialized)
		{
			m_Renderer.Shutdown();
			if (m_HasPlatformBackend)
				ImGui_ImplGlfw_Shutdown();
			m_Initialized = false;
		}
		ImGui::DestroyContext();
	}

	void ImGuiLayer::OnEvent(Event& event)
	{
		if (!m_BlockEvents || !m_Initialized)
			return;

		const ImGuiIO& io = ImGui::GetIO();
		if (event.IsInCategory(EventCategoryMouse) && io.WantCaptureMouse)
			event.Handled = true;
		if (event.IsInCategory(EventCategoryKeyboard) && io.WantCaptureKeyboard)
			event.Handled = true;
	}

	void ImGuiLayer::Begin()
	{
		if (!m_Initialized)
			return;
		if (m_HasPlatformBackend)
		{
			ImGui_ImplGlfw_NewFrame();
		}
		else
		{
			const GraphicsDevice* device = Application::Get().GetGraphicsDevice();
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(static_cast<float>(device->GetBackBufferWidth()), static_cast<float>(device->GetBackBufferHeight()));
			io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
			// Offscreen runs are fixed-step captures; ImGui only needs a positive frame time.
			io.DeltaTime = 1.0f / 60.0f;
		}
		ImGui::NewFrame();
		ImGuizmo::BeginFrame();
	}

	void ImGuiLayer::End()
	{
		if (!m_Initialized)
			return;
		ImGui::Render();

		GraphicsDevice* device = Application::Get().GetGraphicsDevice();
		m_Renderer.Render(ImGui::GetDrawData(), device->GetCurrentFramebuffer());
	}

	void ImGuiLayer::SetDarkThemeColors()
	{
		auto& colors = ImGui::GetStyle().Colors;
		colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.105f, 0.11f, 1.0f);

		colors[ImGuiCol_Header] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.3f, 0.305f, 0.31f, 1.0f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);

		colors[ImGuiCol_Button] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);
		colors[ImGuiCol_ButtonHovered] = ImVec4(0.3f, 0.305f, 0.31f, 1.0f);
		colors[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);

		colors[ImGuiCol_FrameBg] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.3f, 0.305f, 0.31f, 1.0f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);

		colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
		colors[ImGuiCol_TabHovered] = ImVec4(0.38f, 0.3805f, 0.381f, 1.0f);
		colors[ImGuiCol_TabSelected] = ImVec4(0.28f, 0.2805f, 0.281f, 1.0f);
		colors[ImGuiCol_TabDimmed] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
		colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);

		colors[ImGuiCol_TitleBg] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
		colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
	}

}
