#include "RuntimeLayer.h"

#include "Basalt/Core/Application.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Renderer/GraphicsDevice.h"
#include "Basalt/Renderer/RenderUtils.h"
#include "Basalt/Scene/SceneSerializer.h"

#include <nvrhi/utils.h>

namespace Basalt {

	RuntimeLayer::RuntimeLayer(RuntimeOptions options)
		: Layer("RuntimeLayer")
		, m_Options(std::move(options))
	{
	}

	void RuntimeLayer::OnAttach()
	{
		Application& application = Application::Get();

		if (m_Options.ScenePath.empty())
		{
			BS_CORE_CRITICAL("Runtime: no scene to play (set StartScene in the project or pass --scene)");
			application.Close();
			return;
		}

		std::string error;
		m_Scene = SceneSerializer::LoadScene(Project::ResolvePath(m_Options.ScenePath), error);
		if (!m_Scene)
		{
			BS_CORE_CRITICAL("Runtime: {}", error);
			application.Close();
			return;
		}

		if (GraphicsDevice* device = application.GetGraphicsDevice())
		{
			m_Renderer = CreateScope<SceneRenderer>(device->GetDevice());
			if (!m_Renderer->Initialize())
			{
				application.Close();
				return;
			}
			m_CommandList = device->GetDevice()->createCommandList();
			m_Scene->OnViewportResize(device->GetBackBufferWidth(), device->GetBackBufferHeight());
		}

		m_Scene->GetRendererSettings().DebugView = m_Options.DebugView;
		m_Scene->OnRuntimeStart();
		BS_CORE_INFO("Runtime: playing '{}'", m_Options.ScenePath);
	}

	void RuntimeLayer::OnDetach()
	{
		if (m_Scene)
			m_Scene->OnRuntimeStop();
		m_Scene.reset();
		m_CommandList = nullptr;
		m_Renderer.reset();
	}

	void RuntimeLayer::OnUpdate(Timestep ts)
	{
		if (!m_Scene)
			return;

		m_Scene->OnUpdate(ts);
		m_Frame++;
		if (m_Scene->IsQuitRequested())
			Application::Get().Close();
	}

	void RuntimeLayer::OnRender()
	{
		Application& application = Application::Get();
		GraphicsDevice* device = application.GetGraphicsDevice();
		if (!m_Scene || !m_Renderer || !device)
			return;

		const uint32_t width = device->GetBackBufferWidth();
		const uint32_t height = device->GetBackBufferHeight();
		m_Scene->OnViewportResize(width, height);
		m_Renderer->SetViewportSize(width, height);

		const auto camera = GetPrimaryRenderCamera(*m_Scene, width, height);
		if (camera)
			m_Renderer->Render(*m_Scene, *camera);
		else if (!m_WarnedNoCamera)
		{
			BS_CORE_WARN("Runtime: the scene has no primary camera");
			m_WarnedNoCamera = true;
		}

		m_CommandList->open();
		if (camera)
			m_Renderer->Blit(m_CommandList, device->GetCurrentFramebuffer());
		else
			nvrhi::utils::ClearColorAttachment(m_CommandList, device->GetCurrentFramebuffer(), 0, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
		m_CommandList->close();
		device->GetDevice()->executeCommandList(m_CommandList);

		if (m_Options.MaxFrames > 0 && m_Frame >= m_Options.MaxFrames)
		{
			if (!m_Options.ScreenshotPath.empty() && camera)
			{
				std::string error;
				if (SaveScreenshot(*m_Renderer, m_Options.ScreenshotPath, error))
					BS_CORE_INFO("Runtime: screenshot saved to '{}'", m_Options.ScreenshotPath);
				else
					BS_CORE_ERROR("Runtime: {}", error);
			}
			application.Close();
		}
	}

}
