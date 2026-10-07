#include "Basalt/Renderer/VulkanLoader.h"

#include "Basalt/Core/Base.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Renderer/VulkanHeaders.h"

#include <GLFW/glfw3.h>

#include <cstdlib>
#include <string>
#include <vector>

#if defined(BS_PLATFORM_WINDOWS)
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include <windows.h>
#else
	#include <dlfcn.h>
#endif

// The vulkan.hpp dynamic dispatcher storage. Exactly one definition exists in the program, here.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace Basalt {

	namespace {

		void* s_Library = nullptr;
		PFN_vkGetInstanceProcAddr s_GetInstanceProcAddr = nullptr;

		void* OpenLibrary(const std::string& path)
		{
#if defined(BS_PLATFORM_WINDOWS)
			return reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
#else
			return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
		}

		void CloseLibrary(void* library)
		{
#if defined(BS_PLATFORM_WINDOWS)
			FreeLibrary(reinterpret_cast<HMODULE>(library));
#else
			dlclose(library);
#endif
		}

		void* GetSymbol(void* library, const char* name)
		{
#if defined(BS_PLATFORM_WINDOWS)
			return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
			return dlsym(library, name);
#endif
		}

		std::vector<std::string> GetCandidatePaths()
		{
			const std::filesystem::path exeDir = FileSystem::GetExecutableDirectory();
			std::vector<std::string> candidates;

#if defined(BS_PLATFORM_WINDOWS)
			candidates.push_back((exeDir / "vulkan-1.dll").string());
			candidates.push_back("vulkan-1.dll");
#elif defined(BS_PLATFORM_MACOS)
			candidates.push_back((exeDir / "libvulkan.1.dylib").string());
			candidates.push_back((exeDir / "../Frameworks/libvulkan.1.dylib").string());
			candidates.push_back("libvulkan.1.dylib");
			if (const char* sdk = std::getenv("VULKAN_SDK"))
				candidates.push_back((std::filesystem::path(sdk) / "lib/libvulkan.1.dylib").string());
			candidates.push_back("/opt/homebrew/lib/libvulkan.1.dylib");
			candidates.push_back("/usr/local/lib/libvulkan.1.dylib");
#else
			candidates.push_back((exeDir / "libvulkan.so.1").string());
			candidates.push_back("libvulkan.so.1");
			candidates.push_back("libvulkan.so");
#endif
			return candidates;
		}

		void SetEnvironmentVariableIfUnset(const char* name, const std::string& value)
		{
			if (std::getenv(name))
				return;
#if defined(BS_PLATFORM_WINDOWS)
			_putenv_s(name, value.c_str());
#else
			setenv(name, value.c_str(), 0);
#endif
		}

		// Shipped macOS builds carry MoltenVK next to the executable; point the loader at it.
		void RegisterBundledDrivers()
		{
			const std::filesystem::path icdDirectory = FileSystem::GetExecutableDirectory() / "vulkan" / "icd.d";
			if (FileSystem::Exists(icdDirectory / "MoltenVK_icd.json"))
				SetEnvironmentVariableIfUnset("VK_DRIVER_FILES", (icdDirectory / "MoltenVK_icd.json").string());
		}

	}

	bool VulkanLoader::Load()
	{
		if (s_Library)
			return true;

		RegisterBundledDrivers();

		for (const std::string& candidate : GetCandidatePaths())
		{
			void* library = OpenLibrary(candidate);
			if (!library)
				continue;

			auto getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetSymbol(library, "vkGetInstanceProcAddr"));
			if (!getInstanceProcAddr)
			{
				CloseLibrary(library);
				continue;
			}

			s_Library = library;
			s_GetInstanceProcAddr = getInstanceProcAddr;
			VULKAN_HPP_DEFAULT_DISPATCHER.init(s_GetInstanceProcAddr);
			glfwInitVulkanLoader(s_GetInstanceProcAddr);
			BS_CORE_INFO("Vulkan loader: {}", candidate);
			return true;
		}

		BS_CORE_ERROR("Vulkan loader library not found. Install a Vulkan runtime (or the Vulkan SDK / MoltenVK on macOS).");
		return false;
	}

	void VulkanLoader::Unload()
	{
		if (!s_Library)
			return;
		CloseLibrary(s_Library);
		s_Library = nullptr;
		s_GetInstanceProcAddr = nullptr;
	}

	bool VulkanLoader::IsLoaded()
	{
		return s_Library != nullptr;
	}

	void* VulkanLoader::GetInstanceProcAddr()
	{
		return reinterpret_cast<void*>(s_GetInstanceProcAddr);
	}

}
