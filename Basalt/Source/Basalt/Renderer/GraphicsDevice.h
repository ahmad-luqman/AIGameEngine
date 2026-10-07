#pragma once

#include "Basalt/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <functional>
#include <string>
#include <vector>

namespace Basalt {

	class Window;

	struct GraphicsDeviceSpecification
	{
		std::string ApplicationName = "Basalt";
		bool EnableValidation = false;
		bool VSync = true;
		uint32_t MaxFramesInFlight = 2;
		uint32_t SwapchainImageCount = 3;
	};

	// Owns the Vulkan instance, device and swapchain and exposes them through nvrhi.
	// Frame protocol: BeginFrame() -> record/submit nvrhi command lists -> Present().
	class GraphicsDevice
	{
	public:
		// Returns nullptr (after logging the reason) if no suitable Vulkan device is available.
		// VulkanLoader::Load() must have succeeded before the window was created.
		static Scope<GraphicsDevice> Create(Window& window, const GraphicsDeviceSpecification& specification);
		~GraphicsDevice();

		GraphicsDevice(const GraphicsDevice&) = delete;
		GraphicsDevice& operator=(const GraphicsDevice&) = delete;

		// Acquires the next swapchain image. Returns false when nothing should be rendered this frame
		// (window minimized or swapchain unavailable); Present() must not be called in that case.
		bool BeginFrame();
		void Present();
		void WaitIdle();

		void SetVSync(bool enabled);

		// Reads the back buffer back right before the next Present (after all UI is drawn) and calls the
		// callback with RGBA8 pixels, top row first. Waits for the GPU; meant for tools and automation.
		using CaptureCallback = std::function<void(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height)>;
		void RequestBackBufferCapture(CaptureCallback callback) { m_PendingCapture = std::move(callback); }
		bool IsVSync() const { return m_Specification.VSync; }

		nvrhi::IDevice* GetDevice() const { return m_NvrhiDevice; }
		nvrhi::ITexture* GetCurrentBackBuffer() const;
		nvrhi::IFramebuffer* GetCurrentFramebuffer() const;
		nvrhi::Format GetBackBufferFormat() const { return m_BackBufferFormat; }
		uint32_t GetBackBufferWidth() const { return m_BackBufferWidth; }
		uint32_t GetBackBufferHeight() const { return m_BackBufferHeight; }
		uint32_t GetBackBufferCount() const;
		uint32_t GetCurrentBackBufferIndex() const { return m_SwapchainIndex; }

		const std::string& GetAdapterName() const { return m_AdapterName; }
		uint64_t GetFrameIndex() const { return m_FrameIndex; }
		// Number of errors reported by the Vulkan validation layers or nvrhi since creation.
		uint32_t GetValidationErrorCount() const;
		// True when the Khronos validation layer is active (it may be requested but unavailable).
		bool IsValidationActive() const { return m_ValidationActive; }

	private:
		struct Impl;

		explicit GraphicsDevice(const GraphicsDeviceSpecification& specification);

		bool Initialize(Window& window);
		bool CreateSwapchain();
		void DestroySwapchain();
		bool RecreateSwapchainIfNeeded();

	private:
		GraphicsDeviceSpecification m_Specification;
		Scope<Impl> m_Impl;
		Window* m_Window = nullptr;

		nvrhi::DeviceHandle m_NvrhiDevice;
		nvrhi::Format m_BackBufferFormat = nvrhi::Format::UNKNOWN;
		uint32_t m_BackBufferWidth = 0;
		uint32_t m_BackBufferHeight = 0;
		uint32_t m_RequestedWidth = 0;
		uint32_t m_RequestedHeight = 0;
		uint32_t m_SwapchainIndex = 0;
		uint64_t m_FrameIndex = 0;
		std::string m_AdapterName;
		bool m_SwapchainDirty = false;
		bool m_ValidationActive = false;
		CaptureCallback m_PendingCapture;
	};

}
