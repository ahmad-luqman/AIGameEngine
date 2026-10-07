#include "Basalt/Renderer/GraphicsDevice.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Window.h"
#include "Basalt/Renderer/DeviceSelection.h"
#include "Basalt/Renderer/VulkanHeaders.h"
#include "Basalt/Renderer/VulkanLoader.h"

#include <GLFW/glfw3.h>
#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <queue>
#include <unordered_set>
#include <vector>

namespace Basalt {

	namespace {

		constexpr uint32_t RequiredApiVersion = VK_API_VERSION_1_3;

		std::atomic<uint32_t> s_ValidationErrorCount = 0;

		VKAPI_ATTR vk::Bool32 VKAPI_CALL VulkanDebugCallback(
			vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
			vk::DebugUtilsMessageTypeFlagsEXT type,
			const vk::DebugUtilsMessengerCallbackDataEXT* callbackData,
			void* userData)
		{
			const char* message = callbackData && callbackData->pMessage ? callbackData->pMessage : "(null)";
			if (severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eError)
			{
				s_ValidationErrorCount++;
				BS_CORE_ERROR("[Vulkan] {}", message);
			}
			else if (severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning)
			{
				BS_CORE_WARN("[Vulkan] {}", message);
			}
			return VK_FALSE;
		}

		class NvrhiMessageCallback final : public nvrhi::IMessageCallback
		{
		public:
			void message(nvrhi::MessageSeverity severity, const char* messageText) override
			{
				switch (severity)
				{
					case nvrhi::MessageSeverity::Info:
						BS_CORE_TRACE("[nvrhi] {}", messageText);
						break;
					case nvrhi::MessageSeverity::Warning:
						BS_CORE_WARN("[nvrhi] {}", messageText);
						break;
					case nvrhi::MessageSeverity::Error:
					case nvrhi::MessageSeverity::Fatal:
						s_ValidationErrorCount++;
						BS_CORE_ERROR("[nvrhi] {}", messageText);
						break;
				}
			}
		};

		NvrhiMessageCallback s_NvrhiMessageCallback;

		bool HasExtension(const std::vector<vk::ExtensionProperties>& extensions, const char* name)
		{
			return std::any_of(extensions.begin(), extensions.end(),
							   [name](const vk::ExtensionProperties& ext) { return std::strcmp(ext.extensionName, name) == 0; });
		}

		bool HasLayer(const std::vector<vk::LayerProperties>& layers, const char* name)
		{
			return std::any_of(layers.begin(), layers.end(),
							   [name](const vk::LayerProperties& layer) { return std::strcmp(layer.layerName, name) == 0; });
		}

		nvrhi::Format ToNvrhiFormat(vk::Format format)
		{
			switch (format)
			{
				case vk::Format::eB8G8R8A8Unorm:
					return nvrhi::Format::BGRA8_UNORM;
				case vk::Format::eR8G8B8A8Unorm:
					return nvrhi::Format::RGBA8_UNORM;
				case vk::Format::eB8G8R8A8Srgb:
					return nvrhi::Format::SBGRA8_UNORM;
				case vk::Format::eR8G8B8A8Srgb:
					return nvrhi::Format::SRGBA8_UNORM;
				case vk::Format::eA2B10G10R10UnormPack32:
					return nvrhi::Format::R10G10B10A2_UNORM;
				default:
					return nvrhi::Format::UNKNOWN;
			}
		}

	}

	namespace {

		PhysicalDeviceKind ToDeviceKind(vk::PhysicalDeviceType type)
		{
			switch (type)
			{
				case vk::PhysicalDeviceType::eDiscreteGpu:
					return PhysicalDeviceKind::Discrete;
				case vk::PhysicalDeviceType::eIntegratedGpu:
					return PhysicalDeviceKind::Integrated;
				case vk::PhysicalDeviceType::eVirtualGpu:
					return PhysicalDeviceKind::Virtual;
				case vk::PhysicalDeviceType::eCpu:
					return PhysicalDeviceKind::Cpu;
				default:
					return PhysicalDeviceKind::Other;
			}
		}

		// Returns a queue family with graphics + compute (and presentation to `surface`, when one is
		// given), or UINT32_MAX when the device lacks a feature Basalt requires.
		uint32_t FindQueueFamily(vk::PhysicalDevice device, const vk::PhysicalDeviceProperties& properties, vk::SurfaceKHR surface)
		{
			if (properties.apiVersion < RequiredApiVersion)
				return UINT32_MAX;

			if (surface && !HasExtension(device.enumerateDeviceExtensionProperties(), VK_KHR_SWAPCHAIN_EXTENSION_NAME))
				return UINT32_MAX;

			const auto features = device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features>();
			const auto& features12 = features.get<vk::PhysicalDeviceVulkan12Features>();
			const auto& features13 = features.get<vk::PhysicalDeviceVulkan13Features>();
			if (!features12.timelineSemaphore || !features13.dynamicRendering || !features13.synchronization2)
				return UINT32_MAX;

			const auto families = device.getQueueFamilyProperties();
			for (uint32_t i = 0; i < families.size(); i++)
			{
				const bool graphics = static_cast<bool>(families[i].queueFlags & vk::QueueFlagBits::eGraphics);
				const bool compute = static_cast<bool>(families[i].queueFlags & vk::QueueFlagBits::eCompute);
				if (graphics && compute && (!surface || device.getSurfaceSupportKHR(i, surface)))
					return i;
			}
			return UINT32_MAX;
		}

	}

	struct GraphicsDevice::Impl
	{
		vk::Instance Instance;
		vk::DebugUtilsMessengerEXT DebugMessenger;
		vk::SurfaceKHR Surface;
		vk::PhysicalDevice PhysicalDevice;
		vk::Device Device;
		vk::Queue GraphicsQueue;
		uint32_t GraphicsQueueFamily = UINT32_MAX;

		// The raw nvrhi Vulkan device (semaphore interop); GraphicsDevice::m_NvrhiDevice may wrap it in
		// nvrhi's validation layer.
		nvrhi::vulkan::DeviceHandle VulkanDevice;

		vk::SwapchainKHR Swapchain;
		vk::Format SwapchainFormat = vk::Format::eUndefined;
		std::vector<nvrhi::TextureHandle> BackBuffers;
		std::vector<nvrhi::FramebufferHandle> Framebuffers;

		// Acquire semaphores rotate per frame; present semaphores belong to a swapchain image, because the
		// presentation engine may still be waiting on one after the frame that signalled it has retired.
		std::vector<vk::Semaphore> AcquireSemaphores;
		std::vector<vk::Semaphore> PresentSemaphores;
		uint32_t AcquireSemaphoreIndex = 0;

		std::queue<nvrhi::EventQueryHandle> FramesInFlight;
		std::vector<nvrhi::EventQueryHandle> QueryPool;
		nvrhi::CommandListHandle BarrierCommandList;

		std::vector<const char*> InstanceExtensions;
		std::vector<const char*> DeviceExtensions;
	};

	Scope<GraphicsDevice> GraphicsDevice::Create(Window& window, const GraphicsDeviceSpecification& specification)
	{
		Scope<GraphicsDevice> device(new GraphicsDevice(specification));
		try
		{
			if (!device->Initialize(&window, 0, 0))
				return nullptr;
		}
		catch (const std::exception& e)
		{
			BS_CORE_ERROR("GraphicsDevice: initialization failed: {}", e.what());
			return nullptr;
		}
		return device;
	}

	Scope<GraphicsDevice> GraphicsDevice::CreateOffscreen(uint32_t width, uint32_t height, const GraphicsDeviceSpecification& specification)
	{
		if (width == 0 || height == 0)
		{
			BS_CORE_ERROR("GraphicsDevice: offscreen size must be non-zero (got {}x{})", width, height);
			return nullptr;
		}
		Scope<GraphicsDevice> device(new GraphicsDevice(specification));
		try
		{
			if (!device->Initialize(nullptr, width, height))
				return nullptr;
		}
		catch (const std::exception& e)
		{
			BS_CORE_ERROR("GraphicsDevice: initialization failed: {}", e.what());
			return nullptr;
		}
		return device;
	}

	GraphicsDevice::GraphicsDevice(const GraphicsDeviceSpecification& specification)
		: m_Specification(specification)
		, m_Impl(CreateScope<Impl>())
	{
	}

	GraphicsDevice::~GraphicsDevice()
	{
		Impl& impl = *m_Impl;
		if (impl.Device)
			VULKAN_HPP_DEFAULT_DISPATCHER.vkDeviceWaitIdle(impl.Device);

		while (!impl.FramesInFlight.empty())
			impl.FramesInFlight.pop();
		impl.QueryPool.clear();
		impl.BarrierCommandList = nullptr;

		DestroySwapchain();

		for (vk::Semaphore semaphore : impl.AcquireSemaphores)
			impl.Device.destroySemaphore(semaphore);
		for (vk::Semaphore semaphore : impl.PresentSemaphores)
			impl.Device.destroySemaphore(semaphore);
		impl.AcquireSemaphores.clear();
		impl.PresentSemaphores.clear();

		m_NvrhiDevice = nullptr;
		impl.VulkanDevice = nullptr;

		if (impl.Device)
			impl.Device.destroy();
		if (impl.Surface)
			impl.Instance.destroySurfaceKHR(impl.Surface);
		if (impl.DebugMessenger)
			impl.Instance.destroyDebugUtilsMessengerEXT(impl.DebugMessenger);
		if (impl.Instance)
			impl.Instance.destroy();
	}

	bool GraphicsDevice::Initialize(Window* window, uint32_t width, uint32_t height)
	{
		Impl& impl = *m_Impl;
		m_Window = window;

		if (!VulkanLoader::IsLoaded())
		{
			BS_CORE_ERROR("GraphicsDevice: Vulkan loader is not loaded");
			return false;
		}

		const uint32_t loaderVersion = vk::enumerateInstanceVersion();
		if (loaderVersion < RequiredApiVersion)
		{
			BS_CORE_ERROR("GraphicsDevice: Vulkan 1.3 is required, loader reports {}.{}", VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));
			return false;
		}

		// --- Instance ---------------------------------------------------------------------------
		const auto availableExtensions = vk::enumerateInstanceExtensionProperties();
		const auto availableLayers = vk::enumerateInstanceLayerProperties();

		// Surface extensions are only needed to present; an offscreen device must also work where no
		// window system exists at all (headless CI machines).
		if (window)
		{
			uint32_t glfwExtensionCount = 0;
			const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
			if (!glfwExtensions)
			{
				BS_CORE_ERROR("GraphicsDevice: GLFW reports no Vulkan surface support");
				return false;
			}
			impl.InstanceExtensions.assign(glfwExtensions, glfwExtensions + glfwExtensionCount);
		}

		vk::InstanceCreateFlags instanceFlags = {};
		if (HasExtension(availableExtensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
		{
			impl.InstanceExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
			instanceFlags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
		}

		const bool hasDebugUtils = HasExtension(availableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		if (hasDebugUtils)
			impl.InstanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

		std::vector<const char*> layers;
		bool validationEnabled = false;
		if (m_Specification.EnableValidation)
		{
			if (HasLayer(availableLayers, "VK_LAYER_KHRONOS_validation"))
			{
				layers.push_back("VK_LAYER_KHRONOS_validation");
				validationEnabled = true;
			}
			else
			{
				BS_CORE_WARN("GraphicsDevice: validation requested but VK_LAYER_KHRONOS_validation is not installed");
			}
		}

		vk::ApplicationInfo applicationInfo;
		applicationInfo.pApplicationName = m_Specification.ApplicationName.c_str();
		applicationInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
		applicationInfo.pEngineName = "Basalt";
		applicationInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
		applicationInfo.apiVersion = RequiredApiVersion;

		vk::InstanceCreateInfo instanceInfo;
		instanceInfo.flags = instanceFlags;
		instanceInfo.pApplicationInfo = &applicationInfo;
		instanceInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
		instanceInfo.ppEnabledLayerNames = layers.data();
		instanceInfo.enabledExtensionCount = static_cast<uint32_t>(impl.InstanceExtensions.size());
		instanceInfo.ppEnabledExtensionNames = impl.InstanceExtensions.data();

		vk::Result instanceResult = vk::createInstance(&instanceInfo, nullptr, &impl.Instance);
		if (instanceResult == vk::Result::eErrorLayerNotPresent && validationEnabled)
		{
			// The layer manifest exists but its library failed to load (on macOS/Homebrew this happens when
			// /opt/homebrew/lib is not on the dyld search path). Continue without validation.
			BS_CORE_WARN("GraphicsDevice: VK_LAYER_KHRONOS_validation failed to load; continuing without validation. "
						 "On macOS with Homebrew, run with DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib");
			layers.clear();
			validationEnabled = false;
			instanceInfo.enabledLayerCount = 0;
			instanceInfo.ppEnabledLayerNames = nullptr;
			instanceResult = vk::createInstance(&instanceInfo, nullptr, &impl.Instance);
		}
		if (instanceResult != vk::Result::eSuccess)
		{
			BS_CORE_ERROR("GraphicsDevice: vkCreateInstance failed ({})", vk::to_string(instanceResult));
			return false;
		}
		VULKAN_HPP_DEFAULT_DISPATCHER.init(impl.Instance);

		if (validationEnabled && hasDebugUtils)
		{
			vk::DebugUtilsMessengerCreateInfoEXT messengerInfo;
			messengerInfo.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
			messengerInfo.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
			messengerInfo.pfnUserCallback = &VulkanDebugCallback;
			impl.DebugMessenger = impl.Instance.createDebugUtilsMessengerEXT(messengerInfo);
		}

		// --- Surface ----------------------------------------------------------------------------
		if (window)
		{
			VkSurfaceKHR surface = VK_NULL_HANDLE;
			if (glfwCreateWindowSurface(impl.Instance, window->GetNativeWindow(), nullptr, &surface) != VK_SUCCESS)
			{
				BS_CORE_ERROR("GraphicsDevice: failed to create window surface");
				return false;
			}
			impl.Surface = surface;
		}

		// --- Physical device --------------------------------------------------------------------
		// Every device is listed (with its index for --gpu) so a log shows what could have been chosen.
		const std::vector<vk::PhysicalDevice> physicalDevices = impl.Instance.enumeratePhysicalDevices();
		std::vector<PhysicalDeviceInfo> deviceInfos;
		std::vector<uint32_t> queueFamilies;
		for (size_t index = 0; index < physicalDevices.size(); index++)
		{
			const vk::PhysicalDevice candidate = physicalDevices[index];
			const vk::PhysicalDeviceProperties properties = candidate.getProperties();
			PhysicalDeviceInfo& info = deviceInfos.emplace_back();
			info.Name = properties.deviceName.data();
			info.Kind = ToDeviceKind(properties.deviceType);
			const uint32_t queueFamily = FindQueueFamily(candidate, properties, impl.Surface);
			queueFamilies.push_back(queueFamily);
			info.Suitable = queueFamily != UINT32_MAX;
			BS_CORE_INFO("GraphicsDevice: GPU {}: {} ({}, Vulkan {}.{}, driver 0x{:x}){}", index, info.Name, ToString(info.Kind),
						 VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion), properties.driverVersion,
						 info.Suitable ? "" : " - unsuitable");
		}

		std::string selectionError;
		const std::optional<size_t> selected = SelectPhysicalDevice(deviceInfos, m_Specification.DeviceSelector, selectionError);
		if (!selected)
		{
			BS_CORE_ERROR("GraphicsDevice: {}", selectionError);
			return false;
		}
		impl.PhysicalDevice = physicalDevices[*selected];
		impl.GraphicsQueueFamily = queueFamilies[*selected];

		const vk::PhysicalDeviceProperties deviceProperties = impl.PhysicalDevice.getProperties();
		m_AdapterName = deviceProperties.deviceName.data();

		// --- Logical device ---------------------------------------------------------------------
		const auto deviceExtensions = impl.PhysicalDevice.enumerateDeviceExtensionProperties();
		if (window)
			impl.DeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		if (HasExtension(deviceExtensions, "VK_KHR_portability_subset"))
			impl.DeviceExtensions.push_back("VK_KHR_portability_subset");

		const auto supported = impl.PhysicalDevice.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features, vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features>();
		const vk::PhysicalDeviceFeatures& supportedCore = supported.get<vk::PhysicalDeviceFeatures2>().features;
		const auto& supported12 = supported.get<vk::PhysicalDeviceVulkan12Features>();

		vk::StructureChain<vk::DeviceCreateInfo, vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features, vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features> deviceChain;

		vk::PhysicalDeviceFeatures& enabledCore = deviceChain.get<vk::PhysicalDeviceFeatures2>().features;
		enabledCore.samplerAnisotropy = supportedCore.samplerAnisotropy;
		enabledCore.fillModeNonSolid = supportedCore.fillModeNonSolid;
		enabledCore.depthClamp = supportedCore.depthClamp;
		enabledCore.independentBlend = supportedCore.independentBlend;
		enabledCore.imageCubeArray = supportedCore.imageCubeArray;
		enabledCore.shaderImageGatherExtended = supportedCore.shaderImageGatherExtended;
		enabledCore.textureCompressionBC = supportedCore.textureCompressionBC;
		enabledCore.shaderInt16 = supportedCore.shaderInt16;

		auto& enabled12 = deviceChain.get<vk::PhysicalDeviceVulkan12Features>();
		enabled12.timelineSemaphore = VK_TRUE;
		enabled12.scalarBlockLayout = supported12.scalarBlockLayout;
		enabled12.bufferDeviceAddress = supported12.bufferDeviceAddress;
		enabled12.shaderSampledImageArrayNonUniformIndexing = supported12.shaderSampledImageArrayNonUniformIndexing;

		auto& enabled13 = deviceChain.get<vk::PhysicalDeviceVulkan13Features>();
		enabled13.dynamicRendering = VK_TRUE;
		enabled13.synchronization2 = VK_TRUE;

		const float queuePriority = 1.0f;
		vk::DeviceQueueCreateInfo queueInfo;
		queueInfo.queueFamilyIndex = impl.GraphicsQueueFamily;
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &queuePriority;

		vk::DeviceCreateInfo& deviceInfo = deviceChain.get<vk::DeviceCreateInfo>();
		deviceInfo.queueCreateInfoCount = 1;
		deviceInfo.pQueueCreateInfos = &queueInfo;
		deviceInfo.enabledExtensionCount = static_cast<uint32_t>(impl.DeviceExtensions.size());
		deviceInfo.ppEnabledExtensionNames = impl.DeviceExtensions.data();

		impl.Device = impl.PhysicalDevice.createDevice(deviceInfo);
		VULKAN_HPP_DEFAULT_DISPATCHER.init(impl.Device);
		impl.GraphicsQueue = impl.Device.getQueue(impl.GraphicsQueueFamily, 0);

		// --- nvrhi ------------------------------------------------------------------------------
		nvrhi::vulkan::DeviceDesc nvrhiDesc;
		nvrhiDesc.errorCB = &s_NvrhiMessageCallback;
		nvrhiDesc.instance = impl.Instance;
		nvrhiDesc.physicalDevice = impl.PhysicalDevice;
		nvrhiDesc.device = impl.Device;
		nvrhiDesc.graphicsQueue = impl.GraphicsQueue;
		nvrhiDesc.graphicsQueueIndex = static_cast<int>(impl.GraphicsQueueFamily);
		nvrhiDesc.instanceExtensions = impl.InstanceExtensions.data();
		nvrhiDesc.numInstanceExtensions = impl.InstanceExtensions.size();
		nvrhiDesc.deviceExtensions = impl.DeviceExtensions.data();
		nvrhiDesc.numDeviceExtensions = impl.DeviceExtensions.size();
		nvrhiDesc.bufferDeviceAddressSupported = enabled12.bufferDeviceAddress;

		impl.VulkanDevice = nvrhi::vulkan::createDevice(nvrhiDesc);
		if (!impl.VulkanDevice)
		{
			BS_CORE_ERROR("GraphicsDevice: nvrhi device creation failed");
			return false;
		}

		m_NvrhiDevice = impl.VulkanDevice;
		if (m_Specification.EnableValidation)
			m_NvrhiDevice = nvrhi::validation::createValidationLayer(m_NvrhiDevice);

		impl.BarrierCommandList = m_NvrhiDevice->createCommandList();

		if (window && !CreateSwapchain())
		{
			BS_CORE_ERROR("GraphicsDevice: could not create the swapchain (window framebuffer size is zero or the surface is unusable)");
			return false;
		}
		if (!window && !CreateOffscreenTargets(width, height))
		{
			BS_CORE_ERROR("GraphicsDevice: could not create {}x{} offscreen render targets", width, height);
			return false;
		}

		m_ValidationActive = validationEnabled;
		BS_CORE_INFO("GraphicsDevice: {} (Vulkan {}.{}.{}){}{}", m_AdapterName,
					 VK_API_VERSION_MAJOR(deviceProperties.apiVersion), VK_API_VERSION_MINOR(deviceProperties.apiVersion), VK_API_VERSION_PATCH(deviceProperties.apiVersion),
					 validationEnabled ? ", validation enabled" : "", window ? "" : ", offscreen");
		return true;
	}

	bool GraphicsDevice::CreateSwapchain()
	{
		Impl& impl = *m_Impl;

		uint32_t framebufferWidth = 0;
		uint32_t framebufferHeight = 0;
		m_Window->GetFramebufferSize(framebufferWidth, framebufferHeight);
		if (framebufferWidth == 0 || framebufferHeight == 0)
			return false;
		// The surface extent may differ from the GLFW size (clamping, live resize); resize detection
		// compares against what was requested so it does not rebuild every frame.
		m_RequestedWidth = framebufferWidth;
		m_RequestedHeight = framebufferHeight;

		const vk::SurfaceCapabilitiesKHR capabilities = impl.PhysicalDevice.getSurfaceCapabilitiesKHR(impl.Surface);
		const auto formats = impl.PhysicalDevice.getSurfaceFormatsKHR(impl.Surface);
		const auto presentModes = impl.PhysicalDevice.getSurfacePresentModesKHR(impl.Surface);

		// Basalt writes display-referred (already gamma encoded) values, so the swapchain is UNORM.
		vk::SurfaceFormatKHR chosenFormat = formats.front();
		for (const vk::SurfaceFormatKHR& format : formats)
		{
			if ((format.format == vk::Format::eB8G8R8A8Unorm || format.format == vk::Format::eR8G8B8A8Unorm) && format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear)
			{
				chosenFormat = format;
				break;
			}
		}

		vk::PresentModeKHR presentMode = vk::PresentModeKHR::eFifo;
		if (!m_Specification.VSync)
		{
			auto supports = [&](vk::PresentModeKHR mode) { return std::find(presentModes.begin(), presentModes.end(), mode) != presentModes.end(); };
			if (supports(vk::PresentModeKHR::eMailbox))
				presentMode = vk::PresentModeKHR::eMailbox;
			else if (supports(vk::PresentModeKHR::eImmediate))
				presentMode = vk::PresentModeKHR::eImmediate;
		}

		vk::Extent2D extent = capabilities.currentExtent;
		if (extent.width == UINT32_MAX)
		{
			extent.width = std::clamp(framebufferWidth, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
			extent.height = std::clamp(framebufferHeight, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
		}
		if (extent.width == 0 || extent.height == 0)
			return false;

		uint32_t imageCount = std::max(m_Specification.SwapchainImageCount, capabilities.minImageCount);
		if (capabilities.maxImageCount > 0)
			imageCount = std::min(imageCount, capabilities.maxImageCount);

		const vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;

		vk::SwapchainCreateInfoKHR swapchainInfo;
		swapchainInfo.surface = impl.Surface;
		swapchainInfo.minImageCount = imageCount;
		swapchainInfo.imageFormat = chosenFormat.format;
		swapchainInfo.imageColorSpace = chosenFormat.colorSpace;
		swapchainInfo.imageExtent = extent;
		swapchainInfo.imageArrayLayers = 1;
		swapchainInfo.imageUsage = usage & capabilities.supportedUsageFlags;
		swapchainInfo.imageSharingMode = vk::SharingMode::eExclusive;
		swapchainInfo.preTransform = capabilities.currentTransform;
		swapchainInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
		swapchainInfo.presentMode = presentMode;
		swapchainInfo.clipped = VK_TRUE;
		swapchainInfo.oldSwapchain = impl.Swapchain;

		vk::SwapchainKHR newSwapchain = impl.Device.createSwapchainKHR(swapchainInfo);
		DestroySwapchain();
		impl.Swapchain = newSwapchain;
		impl.SwapchainFormat = chosenFormat.format;

		m_BackBufferFormat = ToNvrhiFormat(chosenFormat.format);
		m_BackBufferWidth = extent.width;
		m_BackBufferHeight = extent.height;

		const auto images = impl.Device.getSwapchainImagesKHR(impl.Swapchain);
		for (vk::Image image : images)
		{
			nvrhi::TextureDesc textureDesc;
			textureDesc.width = extent.width;
			textureDesc.height = extent.height;
			textureDesc.format = m_BackBufferFormat;
			textureDesc.debugName = "SwapchainImage";
			textureDesc.initialState = nvrhi::ResourceStates::Present;
			textureDesc.keepInitialState = true;
			textureDesc.isRenderTarget = true;

			nvrhi::TextureHandle texture = m_NvrhiDevice->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image, nvrhi::Object(static_cast<VkImage>(image)), textureDesc);
			impl.BackBuffers.push_back(texture);
			impl.Framebuffers.push_back(m_NvrhiDevice->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture)));
		}

		// Semaphore counts depend on the image count, so rebuild them with the swapchain.
		for (vk::Semaphore semaphore : impl.AcquireSemaphores)
			impl.Device.destroySemaphore(semaphore);
		for (vk::Semaphore semaphore : impl.PresentSemaphores)
			impl.Device.destroySemaphore(semaphore);
		impl.AcquireSemaphores.clear();
		impl.PresentSemaphores.clear();

		const size_t acquireCount = std::max<size_t>(images.size(), m_Specification.MaxFramesInFlight) + 1;
		for (size_t i = 0; i < acquireCount; i++)
			impl.AcquireSemaphores.push_back(impl.Device.createSemaphore({}));
		for (size_t i = 0; i < images.size(); i++)
			impl.PresentSemaphores.push_back(impl.Device.createSemaphore({}));
		impl.AcquireSemaphoreIndex = 0;

		m_SwapchainDirty = false;
		return true;
	}

	bool GraphicsDevice::CreateOffscreenTargets(uint32_t width, uint32_t height)
	{
		Impl& impl = *m_Impl;
		m_BackBufferFormat = nvrhi::Format::RGBA8_UNORM;
		m_BackBufferWidth = width;
		m_BackBufferHeight = height;

		// One target per frame in flight, like swapchain images: the CPU can record frame N+1 while the
		// GPU still renders frame N. Between frames they rest in CopySource, ready for a capture.
		for (uint32_t i = 0; i < std::max(m_Specification.MaxFramesInFlight, 1u); i++)
		{
			nvrhi::TextureDesc textureDesc;
			textureDesc.width = width;
			textureDesc.height = height;
			textureDesc.format = m_BackBufferFormat;
			textureDesc.debugName = "OffscreenBackBuffer";
			textureDesc.initialState = nvrhi::ResourceStates::CopySource;
			textureDesc.keepInitialState = true;
			textureDesc.isRenderTarget = true;

			nvrhi::TextureHandle texture = m_NvrhiDevice->createTexture(textureDesc);
			if (!texture)
				return false;
			impl.BackBuffers.push_back(texture);
			impl.Framebuffers.push_back(m_NvrhiDevice->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture)));
		}
		m_SwapchainIndex = 0;
		return true;
	}

	void GraphicsDevice::DestroySwapchain()
	{
		Impl& impl = *m_Impl;
		// Non-throwing on purpose: also runs from the destructor, possibly after device loss.
		if (impl.Device)
			VULKAN_HPP_DEFAULT_DISPATCHER.vkDeviceWaitIdle(impl.Device);

		impl.Framebuffers.clear();
		impl.BackBuffers.clear();

		if (impl.Swapchain)
		{
			impl.Device.destroySwapchainKHR(impl.Swapchain);
			impl.Swapchain = nullptr;
		}
	}

	bool GraphicsDevice::RecreateSwapchainIfNeeded()
	{
		uint32_t width = 0;
		uint32_t height = 0;
		m_Window->GetFramebufferSize(width, height);
		if (width == 0 || height == 0)
			return false;

		if (!m_SwapchainDirty && m_Impl->Swapchain && width == m_RequestedWidth && height == m_RequestedHeight)
			return true;

		// Release every in-flight reference to the old back buffers before destroying them.
		WaitIdle();
		try
		{
			return CreateSwapchain();
		}
		catch (const vk::SystemError& e)
		{
			// Surface or device lost (monitor unplugged, driver reset). Skip rendering; retried next frame.
			BS_CORE_ERROR("GraphicsDevice: swapchain recreation failed: {}", e.what());
			m_SwapchainDirty = true;
			return false;
		}
	}

	bool GraphicsDevice::BeginFrame()
	{
		Impl& impl = *m_Impl;
		// Offscreen targets rotate in Present(); there is nothing to acquire.
		if (IsOffscreen())
			return !impl.BackBuffers.empty();

		if (!RecreateSwapchainIfNeeded())
			return false;

		constexpr int MaxAttempts = 3;
		for (int attempt = 0; attempt < MaxAttempts; attempt++)
		{
			const vk::Semaphore semaphore = impl.AcquireSemaphores[impl.AcquireSemaphoreIndex];
			const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkAcquireNextImageKHR(impl.Device, impl.Swapchain, UINT64_MAX, semaphore, VK_NULL_HANDLE, &m_SwapchainIndex);

			if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
			{
				impl.AcquireSemaphoreIndex = (impl.AcquireSemaphoreIndex + 1) % static_cast<uint32_t>(impl.AcquireSemaphores.size());
				impl.VulkanDevice->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, semaphore, 0);
				if (result == VK_SUBOPTIMAL_KHR)
					m_SwapchainDirty = true;
				return true;
			}

			if (result != VK_ERROR_OUT_OF_DATE_KHR)
			{
				BS_CORE_ERROR("GraphicsDevice: vkAcquireNextImageKHR failed ({})", static_cast<int>(result));
				return false;
			}

			m_SwapchainDirty = true;
			if (!RecreateSwapchainIfNeeded())
				return false;
		}
		return false;
	}

	void GraphicsDevice::Present()
	{
		Impl& impl = *m_Impl;

		if (m_PendingCapture)
		{
			CaptureCallback callback = std::move(m_PendingCapture);
			m_PendingCapture = nullptr;
			nvrhi::ITexture* backBuffer = GetCurrentBackBuffer();
			nvrhi::TextureDesc desc = backBuffer->getDesc();
			desc.isRenderTarget = false;
			desc.initialState = nvrhi::ResourceStates::CopyDest;
			desc.keepInitialState = true;
			desc.debugName = "BackBufferCapture";
			nvrhi::StagingTextureHandle staging = m_NvrhiDevice->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
			if (staging)
			{
				impl.BarrierCommandList->open();
				impl.BarrierCommandList->copyTexture(staging, nvrhi::TextureSlice(), backBuffer, nvrhi::TextureSlice());
				impl.BarrierCommandList->close();
				m_NvrhiDevice->executeCommandList(impl.BarrierCommandList);
				m_NvrhiDevice->waitForIdle();
				size_t rowPitch = 0;
				const auto* data = static_cast<const uint8_t*>(m_NvrhiDevice->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
				if (data)
				{
					const bool bgra = desc.format == nvrhi::Format::BGRA8_UNORM || desc.format == nvrhi::Format::SBGRA8_UNORM;
					std::vector<uint8_t> pixels(static_cast<size_t>(desc.width) * desc.height * 4);
					for (uint32_t y = 0; y < desc.height; y++)
					{
						const uint8_t* row = data + y * rowPitch;
						for (uint32_t x = 0; x < desc.width; x++)
						{
							uint8_t* out = pixels.data() + (static_cast<size_t>(y) * desc.width + x) * 4;
							out[0] = row[x * 4 + (bgra ? 2 : 0)];
							out[1] = row[x * 4 + 1];
							out[2] = row[x * 4 + (bgra ? 0 : 2)];
							out[3] = 255;
						}
					}
					m_NvrhiDevice->unmapStagingTexture(staging);
					callback(pixels, desc.width, desc.height);
				}
			}
		}

		if (IsOffscreen())
		{
			m_SwapchainIndex = (m_SwapchainIndex + 1) % static_cast<uint32_t>(impl.BackBuffers.size());
		}
		else
		{
			// The signal is attached to the next submission; an empty command list guarantees there is one.
			const vk::Semaphore presentSemaphore = impl.PresentSemaphores[m_SwapchainIndex];
			impl.VulkanDevice->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, presentSemaphore, 0);
			impl.BarrierCommandList->open();
			impl.BarrierCommandList->close();
			m_NvrhiDevice->executeCommandList(impl.BarrierCommandList);

			VkSemaphore waitSemaphore = presentSemaphore;
			VkSwapchainKHR swapchain = impl.Swapchain;
			VkPresentInfoKHR presentInfo = {};
			presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
			presentInfo.waitSemaphoreCount = 1;
			presentInfo.pWaitSemaphores = &waitSemaphore;
			presentInfo.swapchainCount = 1;
			presentInfo.pSwapchains = &swapchain;
			presentInfo.pImageIndices = &m_SwapchainIndex;

			const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkQueuePresentKHR(impl.GraphicsQueue, &presentInfo);
			if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
				m_SwapchainDirty = true;
			else if (result != VK_SUCCESS)
				BS_CORE_ERROR("GraphicsDevice: vkQueuePresentKHR failed ({})", static_cast<int>(result));
		}

		// Bound the CPU to MaxFramesInFlight frames ahead of the GPU.
		while (impl.FramesInFlight.size() >= m_Specification.MaxFramesInFlight)
		{
			nvrhi::EventQueryHandle query = impl.FramesInFlight.front();
			impl.FramesInFlight.pop();
			m_NvrhiDevice->waitEventQuery(query);
			impl.QueryPool.push_back(query);
		}

		nvrhi::EventQueryHandle query;
		if (!impl.QueryPool.empty())
		{
			query = impl.QueryPool.back();
			impl.QueryPool.pop_back();
		}
		else
		{
			query = m_NvrhiDevice->createEventQuery();
		}
		m_NvrhiDevice->resetEventQuery(query);
		m_NvrhiDevice->setEventQuery(query, nvrhi::CommandQueue::Graphics);
		impl.FramesInFlight.push(query);

		m_NvrhiDevice->runGarbageCollection();
		m_FrameIndex++;
	}

	void GraphicsDevice::WaitIdle()
	{
		if (m_NvrhiDevice)
			m_NvrhiDevice->waitForIdle();
		if (m_Impl->Device)
			VULKAN_HPP_DEFAULT_DISPATCHER.vkDeviceWaitIdle(m_Impl->Device);
	}

	void GraphicsDevice::SetVSync(bool enabled)
	{
		if (m_Specification.VSync == enabled)
			return;
		m_Specification.VSync = enabled;
		m_SwapchainDirty = true;
	}

	nvrhi::ITexture* GraphicsDevice::GetCurrentBackBuffer() const
	{
		return m_Impl->BackBuffers.empty() ? nullptr : m_Impl->BackBuffers[m_SwapchainIndex].Get();
	}

	nvrhi::IFramebuffer* GraphicsDevice::GetCurrentFramebuffer() const
	{
		return m_Impl->Framebuffers.empty() ? nullptr : m_Impl->Framebuffers[m_SwapchainIndex].Get();
	}

	uint32_t GraphicsDevice::GetBackBufferCount() const
	{
		return static_cast<uint32_t>(m_Impl->BackBuffers.size());
	}

	uint32_t GraphicsDevice::GetValidationErrorCount() const
	{
		return s_ValidationErrorCount.load();
	}

}
