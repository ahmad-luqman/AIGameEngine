#include "Basalt/Project/Exporter.h"

#include "Basalt/Core/Base.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/Platform.h"
#include "Basalt/Project/Project.h"

#include <cctype>
#include <system_error>

namespace Basalt {

	namespace {

		std::string SanitizeFileName(const std::string& name)
		{
			std::string result;
			for (char c : name)
			{
				if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
					result += c;
				else if (c == ' ')
					result += '_';
			}
			return result.empty() ? std::string("Game") : result;
		}

		bool IsInside(const std::filesystem::path& path, const std::filesystem::path& directory)
		{
			const std::filesystem::path relative = path.lexically_normal().lexically_relative(directory.lexically_normal());
			return !relative.empty() && *relative.begin() != "..";
		}

		uint32_t CountFiles(const std::filesystem::path& directory)
		{
			uint32_t count = 0;
			std::error_code error;
			for (auto it = std::filesystem::recursive_directory_iterator(directory, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
			{
				if (it->is_regular_file(error))
					count++;
			}
			return count;
		}

#if defined(BS_PLATFORM_MACOS)
		// Copies the Vulkan loader and MoltenVK so the game runs on Macs without the Vulkan SDK.
		bool BundleMoltenVK(const std::filesystem::path& output, std::string& outError)
		{
			std::vector<std::filesystem::path> searchDirectories;
			if (const auto sdk = Platform::GetEnvVar("VULKAN_SDK"))
				searchDirectories.emplace_back(std::filesystem::path(*sdk) / "lib");
			searchDirectories.emplace_back("/opt/homebrew/lib");
			searchDirectories.emplace_back("/usr/local/lib");

			auto find = [&](const char* fileName) -> std::filesystem::path {
				for (const auto& directory : searchDirectories)
				{
					if (FileSystem::Exists(directory / fileName))
						return directory / fileName;
				}
				return {};
			};

			const std::filesystem::path loader = find("libvulkan.1.dylib");
			const std::filesystem::path moltenVK = find("libMoltenVK.dylib");
			if (loader.empty() || moltenVK.empty())
			{
				outError = "cannot find libvulkan.1.dylib and libMoltenVK.dylib (install the Vulkan SDK or Homebrew vulkan-loader + molten-vk)";
				return false;
			}

			std::error_code error;
			std::filesystem::copy_file(std::filesystem::canonical(loader, error), output / "libvulkan.1.dylib", std::filesystem::copy_options::overwrite_existing, error);
			if (!error)
				std::filesystem::copy_file(std::filesystem::canonical(moltenVK, error), output / "libMoltenVK.dylib", std::filesystem::copy_options::overwrite_existing, error);
			if (error)
			{
				outError = "cannot copy Vulkan libraries: " + error.message();
				return false;
			}
			// Package-manager files are often read-only; keep exports overwritable by later exports.
			for (const char* library : { "libvulkan.1.dylib", "libMoltenVK.dylib" })
				std::filesystem::permissions(output / library, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read | std::filesystem::perms::others_read, std::filesystem::perm_options::replace, error);

			// The ICD manifest path is relative to the manifest file itself.
			const std::string manifest = R"({ "file_format_version": "1.0.0", "ICD": { "library_path": "../../libMoltenVK.dylib", "api_version": "1.3.0", "is_portability_driver": true } })";
			if (!FileSystem::WriteTextFile(output / "vulkan" / "icd.d" / "MoltenVK_icd.json", manifest))
			{
				outError = "cannot write the MoltenVK ICD manifest";
				return false;
			}
			return true;
		}
#endif

	}

	std::filesystem::path Exporter::GetDefaultRuntimePath()
	{
#if defined(BS_PLATFORM_WINDOWS)
		return FileSystem::GetExecutableDirectory() / "BasaltRuntime.exe";
#else
		return FileSystem::GetExecutableDirectory() / "BasaltRuntime";
#endif
	}

	ExportResult Exporter::Export(const Project& project, const std::filesystem::path& outputDirectory, const std::filesystem::path& runtimeExecutable)
	{
		ExportResult result;
		std::error_code error;

		const ProjectConfig& config = project.GetConfig();
		if (config.StartScene.empty())
		{
			result.Error = "the project has no StartScene (set it with project.set)";
			return result;
		}
		if (!FileSystem::Exists(project.GetDirectory() / config.StartScene))
		{
			result.Error = "the start scene '" + config.StartScene + "' does not exist";
			return result;
		}

		const std::filesystem::path runtime = runtimeExecutable.empty() ? GetDefaultRuntimePath() : runtimeExecutable;
		if (!FileSystem::Exists(runtime))
		{
			result.Error = "runtime executable not found at '" + runtime.string() + "' (build the BasaltRuntime target)";
			return result;
		}

		const std::filesystem::path output = std::filesystem::absolute(outputDirectory, error);
		if (error || output.empty())
		{
			result.Error = "invalid output directory '" + outputDirectory.string() + "'";
			return result;
		}
		if (IsInside(output, project.GetDirectory()) || output.lexically_normal() == project.GetDirectory().lexically_normal())
		{
			result.Error = "the output directory must be outside the project directory";
			return result;
		}
		// Only ever replace a previous export, never an arbitrary non-empty directory.
		if (std::filesystem::exists(output, error) && !std::filesystem::is_empty(output, error) && !FileSystem::Exists(output / Project::FileName))
		{
			result.Error = "the output directory is not empty and is not a previous export";
			return result;
		}

		std::filesystem::remove_all(output / Project::AssetDirectoryName, error);
		std::filesystem::create_directories(output, error);
		if (error)
		{
			result.Error = "cannot create '" + output.string() + "': " + error.message();
			return result;
		}

		std::filesystem::copy(project.GetDirectory() / Project::AssetDirectoryName, output / Project::AssetDirectoryName, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
		if (!error)
			std::filesystem::copy_file(project.GetProjectFilePath(), output / Project::FileName, std::filesystem::copy_options::overwrite_existing, error);
		if (error)
		{
			result.Error = "cannot copy project files: " + error.message();
			return result;
		}

		std::filesystem::path executable = output / SanitizeFileName(config.Name);
#if defined(BS_PLATFORM_WINDOWS)
		executable += ".exe";
#endif
		std::filesystem::copy_file(runtime, executable, std::filesystem::copy_options::overwrite_existing, error);
		if (error)
		{
			result.Error = "cannot copy the runtime: " + error.message();
			return result;
		}
		std::filesystem::permissions(executable, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec, std::filesystem::perm_options::add, error);

#if defined(BS_PLATFORM_MACOS)
		if (!BundleMoltenVK(output, result.Error))
			return result;
#endif

		result.Success = true;
		result.ExecutablePath = executable;
		result.FileCount = CountFiles(output);
		BS_CORE_INFO("Exporter: exported '{}' to '{}' ({} files)", config.Name, output.string(), result.FileCount);
		return result;
	}

}
