#include "Basalt/Renderer/VulkanLoader.h"

#include "Basalt/Core/Base.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/Platform.h"
#include "Basalt/Renderer/VulkanHeaders.h"

#include <GLFW/glfw3.h>
#include <spdlog/fmt/fmt.h>

#include <cstdlib>
#include <string>
#include <vector>

#if defined(BS_PLATFORM_WINDOWS)
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

		// A path, not text: LoadLibraryA would read it in the ANSI code page, and a game installed in a folder named outside
		// it would not start.
		void* OpenLibrary(const std::filesystem::path& path)
		{
#if defined(BS_PLATFORM_WINDOWS)
			return reinterpret_cast<void*>(LoadLibraryW(path.c_str()));
#else
			return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
		}

		// Why OpenLibrary just failed, or nothing when the library is simply not there: a file named by path that does not
		// exist, or on Windows a name the search finds nowhere (ERROR_MOD_NOT_FOUND, which a library missing a dependency
		// also reports; then it was named by path, and the file check keeps it). POSIX's dlerror does not tell a bare name
		// that is not installed from one that failed, so bare names report nothing there.
		std::string WhyNotLoaded(const std::filesystem::path& candidate)
		{
#if defined(BS_PLATFORM_WINDOWS)
			// Read first: checking the file would replace the error code.
			const DWORD code = GetLastError();
			if (candidate.has_parent_path() ? !FileSystem::Exists(candidate) : code == ERROR_MOD_NOT_FOUND)
				return {};
			const std::string message = Platform::GetErrorMessage(code);
			return message.empty() ? "error " + std::to_string(code) : message;
#else
			const char* message = dlerror();
			std::string reason = message ? message : "unknown error";
			if (!candidate.has_parent_path() || !FileSystem::Exists(candidate))
				return {};
			return reason;
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

		std::vector<std::filesystem::path> GetCandidatePaths()
		{
			const std::filesystem::path exeDir = FileSystem::GetExecutableDirectory();
			std::vector<std::filesystem::path> candidates;

#if defined(BS_PLATFORM_WINDOWS)
			candidates.push_back(exeDir / "vulkan-1.dll");
			candidates.emplace_back("vulkan-1.dll");
#elif defined(BS_PLATFORM_MACOS)
			candidates.push_back(exeDir / "libvulkan.1.dylib");
			candidates.push_back(exeDir / "../Frameworks/libvulkan.1.dylib");
			candidates.emplace_back("libvulkan.1.dylib");
			if (const auto sdk = Platform::GetEnvVar("VULKAN_SDK"))
				candidates.push_back(FileSystem::PathFromUtf8(*sdk) / "lib/libvulkan.1.dylib");
			candidates.emplace_back("/opt/homebrew/lib/libvulkan.1.dylib");
			candidates.emplace_back("/usr/local/lib/libvulkan.1.dylib");
#else
			candidates.push_back(exeDir / "libvulkan.so.1");
			candidates.emplace_back("libvulkan.so.1");
			candidates.emplace_back("libvulkan.so");
#endif
			return candidates;
		}

		// Shipped macOS builds carry a MoltenVK ICD manifest (vulkan/icd.d/MoltenVK_icd.json): next to the executable in
		// folder exports and the release's tools archive, in Contents/Resources of .app bundles (naming the library in
		// Contents/Frameworks). Point the loader at it.
		void RegisterBundledDrivers()
		{
			const std::filesystem::path executableDirectory = FileSystem::GetExecutableDirectory();
			for (const std::filesystem::path& base : { executableDirectory, executableDirectory.parent_path() / "Resources" })
			{
				const std::filesystem::path manifest = base / "vulkan" / "icd.d" / "MoltenVK_icd.json";
				if (FileSystem::Exists(manifest))
				{
					Platform::SetEnvVarIfUnset("VK_DRIVER_FILES", FileSystem::PathToUtf8(manifest));
					return;
				}
			}
		}

	}

	bool VulkanLoader::Load()
	{
		if (s_Library)
			return true;

		RegisterBundledDrivers();

		// Libraries that exist but would not load or lack the entry point: a damaged or mismatched loader is not a missing one.
		std::string problems;
		for (const std::filesystem::path& candidate : GetCandidatePaths())
		{
			void* library = OpenLibrary(candidate);
			if (!library)
			{
				if (const std::string reason = WhyNotLoaded(candidate); !reason.empty())
					problems += fmt::format("\n  '{}': {}", FileSystem::PathToUtf8(candidate), reason);
				continue;
			}

			auto getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetSymbol(library, "vkGetInstanceProcAddr"));
			if (!getInstanceProcAddr)
			{
				problems += fmt::format("\n  '{}': no vkGetInstanceProcAddr", FileSystem::PathToUtf8(candidate));
				CloseLibrary(library);
				continue;
			}

			s_Library = library;
			s_GetInstanceProcAddr = getInstanceProcAddr;
			VULKAN_HPP_DEFAULT_DISPATCHER.init(s_GetInstanceProcAddr);
			glfwInitVulkanLoader(s_GetInstanceProcAddr);
			BS_CORE_INFO("Vulkan loader: {}", FileSystem::PathToUtf8(candidate));
			// A bundled loader that failed while another works would break the game where only it is installed.
			if (!problems.empty())
				BS_CORE_WARN("Vulkan loader: earlier candidates failed:{}", problems);
			return true;
		}

		if (!problems.empty())
			BS_CORE_ERROR("Vulkan loader library found but not usable:{}", problems);
		else
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
