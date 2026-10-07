#pragma once

namespace Basalt {

	// Locates and opens the Vulkan loader library at runtime and initializes the vulkan.hpp dynamic
	// dispatcher. Must run before the first GLFW window is created so GLFW uses the same loader.
	//
	// Search order: next to the executable (shipped builds), the default library search path,
	// $VULKAN_SDK, then common package-manager locations. On macOS a bundled MoltenVK ICD
	// (<exe dir>/vulkan/icd.d) is registered automatically when present.
	class VulkanLoader
	{
	public:
		static bool Load();
		static void Unload();
		static bool IsLoaded();

		// PFN_vkGetInstanceProcAddr, type-erased so this header does not pull in Vulkan.
		static void* GetInstanceProcAddr();
	};

}
