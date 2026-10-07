#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Core/LayerStack.h"
#include "Basalt/Core/Timestep.h"
#include "Basalt/Core/Window.h"
#include "Basalt/Events/ApplicationEvent.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace Basalt {

	class GraphicsDevice;
	class ImGuiLayer;

	struct ApplicationCommandLineArgs
	{
		int Count = 0;
		char** Args = nullptr;

		const char* operator[](int index) const { return index >= 0 && index < Count ? Args[index] : nullptr; }
	};

	struct ApplicationSpecification
	{
		std::string Name = "Basalt Application";
		std::filesystem::path WorkingDirectory;
		ApplicationCommandLineArgs CommandLineArgs;

		uint32_t WindowWidth = 1600;
		uint32_t WindowHeight = 900;
		bool StartMaximized = false;
		bool VSync = true;
		bool EnableImGui = false;
		// GPU to render with (--gpu): empty chooses automatically. See SelectPhysicalDevice.
		std::string GpuSelector;

		// No window, no GPU: layers only receive OnUpdate. Used by the CLI and automated tests.
		bool Headless = false;
		// GPU validation (Vulkan layers + nvrhi validation). Defaults on outside Dist builds.
#if defined(BS_DIST)
		bool EnableValidation = false;
#else
		bool EnableValidation = true;
#endif
		// When > 0 every frame advances by exactly this many seconds (deterministic runs).
		float FixedTimestep = 0.0f;
		// When > 0 the application closes itself after this many frames.
		uint64_t MaxFrames = 0;
	};

	// Owns the window, graphics device and layer stack, and runs the main loop. One instance per process.
	class Application
	{
	public:
		explicit Application(const ApplicationSpecification& specification);
		virtual ~Application();

		Application(const Application&) = delete;
		Application& operator=(const Application&) = delete;

		// Returns the process exit code.
		int Run();
		void Close();
		// Process exit code returned by Run() (default 0). Lets layers report failures (e.g. validation errors).
		void SetExitCode(int exitCode) { m_ExitCode = exitCode; }

		// Takes ownership. Pushes made from layer callbacks take effect at the start of the next frame.
		void PushLayer(Layer* layer);
		void PushOverlay(Layer* overlay);

		void OnEvent(Event& event);

		// Queues work to run on the main thread at the start of the next frame (thread-safe).
		void SubmitToMainThread(std::function<void()> function);

		bool IsHeadless() const { return m_Specification.Headless; }
		// True once construction succeeded; Run() returns an error immediately otherwise.
		bool IsInitialized() const { return m_Initialized; }
		Window* GetWindow() const { return m_Window.get(); }
		GraphicsDevice* GetGraphicsDevice() const { return m_GraphicsDevice.get(); }
		ImGuiLayer* GetImGuiLayer() const { return m_ImGuiLayer; }
		const ApplicationSpecification& GetSpecification() const { return m_Specification; }
		uint64_t GetFrameCount() const { return m_FrameCount; }
		float GetTime() const { return m_Time; }

		static Application& Get();
		static bool HasInstance();

	private:
		bool OnWindowClose(WindowCloseEvent& event);
		void ExecuteMainThreadQueue();

	private:
		ApplicationSpecification m_Specification;
		Scope<Window> m_Window;
		Scope<GraphicsDevice> m_GraphicsDevice;
		ImGuiLayer* m_ImGuiLayer = nullptr;
		LayerStack m_LayerStack;

		bool m_Initialized = false;
		bool m_IteratingLayers = false;
		bool m_Running = true;
		int m_ExitCode = 0;
		uint64_t m_FrameCount = 0;
		float m_Time = 0.0f;

		std::mutex m_MainThreadQueueMutex;
		std::vector<std::function<void()>> m_MainThreadQueue;

		static Application* s_Instance;
	};

	// Implemented by each executable (editor, runtime, CLI).
	Application* CreateApplication(ApplicationCommandLineArgs args);

}
