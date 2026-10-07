#include "Basalt/Core/Application.h"

#include "Basalt/Audio/AudioEngine.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Input.h"
#include "Basalt/Core/Timer.h"
#include "Basalt/ImGui/ImGuiLayer.h"
#include "Basalt/Renderer/GraphicsDevice.h"
#include "Basalt/Renderer/VulkanLoader.h"

#include <system_error>

namespace Basalt {

	Application* Application::s_Instance = nullptr;

	Application::Application(const ApplicationSpecification& specification)
		: m_Specification(specification)
	{
		BS_CORE_ASSERT(!s_Instance, "Only one Application may exist at a time");
		s_Instance = this;

		Log::Init();

		if (!m_Specification.WorkingDirectory.empty())
		{
			std::error_code error;
			std::filesystem::current_path(m_Specification.WorkingDirectory, error);
			if (error)
			{
				BS_CORE_ERROR("Cannot change working directory to '{}': {}", m_Specification.WorkingDirectory.string(), error.message());
				return;
			}
		}

		AudioEngine::Init(m_Specification.Headless);

		if (m_Specification.Headless)
		{
			m_Initialized = true;
			return;
		}

		if (!VulkanLoader::Load())
			return;

		WindowSpecification windowSpecification;
		windowSpecification.Title = m_Specification.Name;
		windowSpecification.Width = m_Specification.WindowWidth;
		windowSpecification.Height = m_Specification.WindowHeight;
		windowSpecification.StartMaximized = m_Specification.StartMaximized;
		m_Window = CreateScope<Window>(windowSpecification);
		if (!m_Window->IsValid())
			return;
		m_Window->SetEventCallback(BS_BIND_EVENT_FN(Application::OnEvent));

		GraphicsDeviceSpecification deviceSpecification;
		deviceSpecification.ApplicationName = m_Specification.Name;
		deviceSpecification.EnableValidation = m_Specification.EnableValidation;
		deviceSpecification.VSync = m_Specification.VSync;
		m_GraphicsDevice = GraphicsDevice::Create(*m_Window, deviceSpecification);
		if (!m_GraphicsDevice)
			return;

		if (m_Specification.EnableImGui)
		{
			m_ImGuiLayer = new ImGuiLayer();
			PushOverlay(m_ImGuiLayer);
		}

		m_Initialized = true;
	}

	Application::~Application()
	{
		if (m_GraphicsDevice)
			m_GraphicsDevice->WaitIdle();

		// Run deferred work (e.g. queued layer pushes) so ownership of everything queued is resolved.
		ExecuteMainThreadQueue();
		// Layers may own GPU resources, so they go before the device.
		m_LayerStack.Clear();
		m_ImGuiLayer = nullptr;
		m_GraphicsDevice.reset();
		m_Window.reset();
		VulkanLoader::Unload();
		AudioEngine::Shutdown();

		s_Instance = nullptr;
	}

	void Application::PushLayer(Layer* layer)
	{
		// Layers may push layers from their callbacks; mutating the stack mid-iteration is not allowed.
		if (m_IteratingLayers)
			SubmitToMainThread([this, layer]() { m_LayerStack.PushLayer(layer); });
		else
			m_LayerStack.PushLayer(layer);
	}

	void Application::PushOverlay(Layer* overlay)
	{
		if (m_IteratingLayers)
			SubmitToMainThread([this, overlay]() { m_LayerStack.PushOverlay(overlay); });
		else
			m_LayerStack.PushOverlay(overlay);
	}

	void Application::Close()
	{
		m_Running = false;
	}

	void Application::SubmitToMainThread(std::function<void()> function)
	{
		std::scoped_lock lock(m_MainThreadQueueMutex);
		m_MainThreadQueue.emplace_back(std::move(function));
	}

	void Application::OnEvent(Event& event)
	{
		Input::OnEvent(event);

		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<WindowCloseEvent>(BS_BIND_EVENT_FN(Application::OnWindowClose));

		const bool wasIterating = m_IteratingLayers;
		m_IteratingLayers = true;
		for (auto it = m_LayerStack.rbegin(); it != m_LayerStack.rend(); ++it)
		{
			if (event.Handled)
				break;
			(*it)->OnEvent(event);
		}
		m_IteratingLayers = wasIterating;
	}

	int Application::Run()
	{
		if (!m_Initialized)
		{
			BS_CORE_CRITICAL("Application '{}' failed to initialize", m_Specification.Name);
			return 1;
		}

		Timer frameTimer;
		while (m_Running)
		{
			float deltaTime = frameTimer.Elapsed();
			frameTimer.Reset();
			if (m_Specification.FixedTimestep > 0.0f)
				deltaTime = m_Specification.FixedTimestep;
			// Clamp long stalls (debugger breaks, window drags) so simulations stay stable.
			deltaTime = std::min(deltaTime, 0.1f);
			const Timestep timestep(deltaTime);
			m_Time += deltaTime;

			if (m_Window)
				m_Window->PollEvents();

			ExecuteMainThreadQueue();

			const bool minimized = m_Window && m_Window->IsMinimized();
			m_IteratingLayers = true;
			for (Layer* layer : m_LayerStack)
				layer->OnUpdate(timestep);

			if (m_GraphicsDevice && !minimized && m_GraphicsDevice->BeginFrame())
			{
				for (Layer* layer : m_LayerStack)
					layer->OnRender();

				if (m_ImGuiLayer && m_ImGuiLayer->IsInitialized())
				{
					m_ImGuiLayer->Begin();
					for (Layer* layer : m_LayerStack)
						layer->OnImGuiRender();
					m_ImGuiLayer->End();
				}

				m_GraphicsDevice->Present();
			}
			m_IteratingLayers = false;

			Input::EndFrame();
			m_FrameCount++;

			if (m_Specification.MaxFrames > 0 && m_FrameCount >= m_Specification.MaxFrames)
				m_Running = false;
		}

		if (m_GraphicsDevice)
			m_GraphicsDevice->WaitIdle();
		return m_ExitCode;
	}

	bool Application::OnWindowClose(WindowCloseEvent& event)
	{
		m_Running = false;
		return true;
	}

	void Application::ExecuteMainThreadQueue()
	{
		std::vector<std::function<void()>> queue;
		{
			std::scoped_lock lock(m_MainThreadQueueMutex);
			queue.swap(m_MainThreadQueue);
		}
		for (auto& function : queue)
			function();
	}

	Application& Application::Get()
	{
		BS_CORE_ASSERT(s_Instance, "No Application instance");
		return *s_Instance;
	}

	bool Application::HasInstance()
	{
		return s_Instance != nullptr;
	}

}
