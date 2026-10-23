#include "Basalt/Project/Exporter.h"

#include "Basalt/Core/Base.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/Platform.h"
#include "Basalt/Core/Process.h"
#include "Basalt/Project/Packaging.h"
#include "Basalt/Project/Project.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <optional>
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

		// Where a package keeps the project (relative to its own directory), by the package directory's extension.
		std::optional<std::filesystem::path> PackagedProjectDirectory(const std::filesystem::path& package)
		{
			if (package.extension() == ".app")
				return std::filesystem::path("Contents") / "Resources";
			if (package.extension() == ".AppDir")
				return std::filesystem::path(".");
			return std::nullopt;
		}

		// A folder export holds the project file itself; a package export holds a package that does.
		bool IsPreviousExport(const std::filesystem::path& output)
		{
			if (FileSystem::Exists(output / Project::FileName))
				return true;
			std::error_code error;
			for (auto it = std::filesystem::directory_iterator(output, error); !error && it != std::filesystem::directory_iterator(); it.increment(error))
			{
				const auto projectDirectory = PackagedProjectDirectory(it->path());
				if (projectDirectory && FileSystem::Exists(it->path() / *projectDirectory / Project::FileName))
					return true;
			}
			return false;
		}

		// Copies the project file, its Assets and its input actions into directory (replacing a previous export's).
		bool CopyProjectFiles(const Project& project, const std::filesystem::path& directory, std::string& outError)
		{
			std::error_code error;
			std::filesystem::remove_all(directory / Project::AssetDirectoryName, error);
			std::filesystem::create_directories(directory, error);
			if (error)
			{
				outError = "cannot create '" + FileSystem::PathToUtf8(directory) + "': " + error.message();
				return false;
			}
			std::filesystem::copy(project.GetDirectory() / Project::AssetDirectoryName, directory / Project::AssetDirectoryName, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
			if (!error)
				std::filesystem::copy_file(project.GetProjectFilePath(), directory / Project::FileName, std::filesystem::copy_options::overwrite_existing, error);
			if (error)
			{
				outError = "cannot copy project files: " + error.message();
				return false;
			}
			// The input actions are optional; a previous export's copy must not outlive the project's file. A file that
			// cannot even be checked fails the export rather than shipping the game without its controls.
			const std::filesystem::file_status input = std::filesystem::status(project.GetInputFilePath(), error);
			if (input.type() == std::filesystem::file_type::not_found)
			{
				error.clear();
				std::filesystem::remove(directory / Project::InputFileName, error);
			}
			else if (!error)
				std::filesystem::copy_file(project.GetInputFilePath(), directory / Project::InputFileName, std::filesystem::copy_options::overwrite_existing, error);
			if (error)
			{
				outError = "cannot copy '" + FileSystem::PathToUtf8(project.GetInputFilePath()) + "': " + error.message();
				return false;
			}
			return true;
		}

#if !defined(BS_PLATFORM_WINDOWS)
		bool WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, std::string& outError)
		{
			return FileSystem::WriteBinaryFile(path, bytes.data(), bytes.size(), outError);
		}
#endif

		// The project icon, or nullopt (and no error) when the project has none.
		bool LoadProjectIcon(const Project& project, std::optional<IconImage>& outIcon, std::string& outError)
		{
			const std::string& icon = project.GetConfig().Icon;
			if (icon.empty())
				return true;
			if (AssetRegistry::IsUnresolvedKey(icon))
			{
				outError = "the project icon no longer exists (set it again with project.set icon)";
				return false;
			}
			outIcon = LoadIconImage(project.GetDirectory() / FileSystem::PathFromUtf8(icon), outError);
			return outIcon.has_value();
		}

#if defined(BS_PLATFORM_MACOS)
		// Copies the Vulkan loader and MoltenVK so the game runs on Macs without the Vulkan SDK. The ICD manifest names
		// MoltenVK relative to the manifest file itself (libraryFromManifest).
		bool BundleMoltenVK(const std::filesystem::path& libraryDirectory, const std::filesystem::path& manifestDirectory, const std::string& libraryFromManifest, std::string& outError)
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
			std::filesystem::create_directories(libraryDirectory, error);
			if (!error)
				std::filesystem::copy_file(std::filesystem::canonical(loader, error), libraryDirectory / "libvulkan.1.dylib", std::filesystem::copy_options::overwrite_existing, error);
			if (!error)
				std::filesystem::copy_file(std::filesystem::canonical(moltenVK, error), libraryDirectory / "libMoltenVK.dylib", std::filesystem::copy_options::overwrite_existing, error);
			if (error)
			{
				outError = "cannot copy Vulkan libraries: " + error.message();
				return false;
			}
			// Package-manager files are often read-only; keep exports overwritable by later exports.
			for (const char* library : { "libvulkan.1.dylib", "libMoltenVK.dylib" })
				std::filesystem::permissions(libraryDirectory / library, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read | std::filesystem::perms::others_read, std::filesystem::perm_options::replace, error);

			const std::string manifest = R"({ "file_format_version": "1.0.0", "ICD": { "library_path": ")" + libraryFromManifest + R"(", "api_version": "1.3.0", "is_portability_driver": true } })";
			if (!FileSystem::WriteTextFile(manifestDirectory / "MoltenVK_icd.json", manifest))
			{
				outError = "cannot write the MoltenVK ICD manifest";
				return false;
			}
			return true;
		}

		// Info.plist, PkgInfo and the icon of a bundle whose executable and resources are already in place.
		bool FinishAppBundle(const std::filesystem::path& bundle, PackageInfo info, const std::optional<IconImage>& icon, std::string& outError)
		{
			const std::filesystem::path contents = bundle / "Contents";
			if (icon)
			{
				info.IconFileName = info.ExecutableName + ".icns";
				if (!WriteBytes(contents / "Resources" / info.IconFileName, BuildIcns(*icon), outError))
					return false;
			}
			return FileSystem::WriteTextFile(contents / "Info.plist", BuildInfoPlist(info), outError) && FileSystem::WriteTextFile(contents / "PkgInfo", "APPL????", outError);
		}
#endif

#if defined(BS_PLATFORM_LINUX)
		std::filesystem::path FindAppImageTool(const std::filesystem::path& configured)
		{
			if (!configured.empty())
				return configured;
			if (const auto variable = Platform::GetEnvVar("APPIMAGETOOL"); variable && !variable->empty())
				return *variable;
			const std::string path = Platform::GetEnvVar("PATH").value_or("");
			for (size_t start = 0; start <= path.size();)
			{
				const size_t end = std::min(path.find(':', start), path.size());
				std::error_code error;
				const std::filesystem::path candidate = std::filesystem::path(path.substr(start, end - start)) / "appimagetool";
				if (end > start && std::filesystem::is_regular_file(candidate, error))
					return candidate;
				start = end + 1;
			}
			return {};
		}

		// The AppDir's desktop entry, icon and AppRun around an executable and project already in place, then the
		// .AppImage file when appimagetool is available.
		bool FinishAppImage(const std::filesystem::path& appDir, const std::filesystem::path& output, const PackageInfo& info, const std::optional<IconImage>& icon, const std::filesystem::path& configuredTool, ExportResult& result)
		{
			const std::string iconFile = info.ExecutableName + ".png";
			const IconImage image = icon ? ResizeIcon(*icon, std::min<uint32_t>(512, std::max(icon->Width, icon->Height))) : MakeDefaultIcon(256);
			if (!WriteBytes(appDir / iconFile, EncodeIconPng(image), result.Error) || !FileSystem::WriteTextFile(appDir / (info.ExecutableName + ".desktop"), BuildDesktopEntry(info), result.Error))
				return false;
			for (const auto& [link, target] : { std::pair{ std::string("AppRun"), info.ExecutableName }, std::pair{ std::string(".DirIcon"), iconFile } })
			{
				std::error_code error;
				std::filesystem::remove(appDir / link, error);
				std::filesystem::create_symlink(target, appDir / link, error);
				if (error)
				{
					result.Error = "cannot create '" + FileSystem::PathToUtf8(appDir / link) + "': " + error.message();
					return false;
				}
			}

			const std::filesystem::path tool = FindAppImageTool(configuredTool);
			if (tool.empty())
			{
				result.Warnings.push_back("appimagetool was not found (set APPIMAGETOOL or add it to PATH): the package is the AppDir '" + FileSystem::PathToUtf8(appDir) + "'");
				result.PackagePath = appDir;
				return true;
			}
	#if defined(__aarch64__)
			const char* architecture = "aarch64";
	#else
			const char* architecture = "x86_64";
	#endif
			const std::filesystem::path appImage = output / (info.ExecutableName + "-" + info.Version + "-" + architecture + ".AppImage");
			ProcessSpec spec;
			spec.Executable = tool;
			spec.Arguments = { "--no-appstream", FileSystem::PathToUtf8(appDir), FileSystem::PathToUtf8(appImage) };
			// Without FUSE (containers, CI) an AppImage, including appimagetool itself, can still run extracted.
			spec.Environment = { { "ARCH", architecture }, { "APPIMAGE_EXTRACT_AND_RUN", "1" } };
			spec.Timeout = std::chrono::minutes(10);
			const ProcessResult run = RunProcess(spec);
			if (!run.Succeeded())
			{
				result.Error = "appimagetool failed (" + (run.Error.empty() ? "exit code " + std::to_string(run.ExitCode) : run.Error) + "): " + run.Output;
				return false;
			}
			result.PackagePath = appImage;
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

	std::filesystem::path Exporter::GetExportedProjectDirectory(const std::filesystem::path& executableDirectory)
	{
		std::filesystem::path resources = executableDirectory.parent_path() / "Resources";
		if (!FileSystem::Exists(executableDirectory / Project::FileName) && executableDirectory.filename() == "MacOS" && FileSystem::Exists(resources / Project::FileName))
			return resources;
		return executableDirectory;
	}

	ExportResult Exporter::Export(const Project& project, const std::filesystem::path& outputDirectory, const ExportOptions& options)
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

		// The runtime refuses a broken input actions file: say so now (the file may have been edited since the project
		// loaded) rather than ship a game that does not start.
		if (std::string inputError; !Project::ReadInputActions(project.GetInputFilePath(), inputError))
		{
			result.Error = inputError;
			return result;
		}

		const std::filesystem::path runtime = options.RuntimeExecutable.empty() ? GetDefaultRuntimePath() : options.RuntimeExecutable;
		if (!FileSystem::Exists(runtime))
		{
			result.Error = "runtime executable not found at '" + FileSystem::PathToUtf8(runtime) + "' (build the BasaltRuntime target)";
			return result;
		}

		const std::filesystem::path output = std::filesystem::absolute(outputDirectory, error);
		if (error || output.empty())
		{
			result.Error = "invalid output directory '" + FileSystem::PathToUtf8(outputDirectory) + "'";
			return result;
		}
		if (IsInside(output, project.GetDirectory()) || output.lexically_normal() == project.GetDirectory().lexically_normal())
		{
			result.Error = "the output directory must be outside the project directory";
			return result;
		}
		// Only ever replace a previous export, never an arbitrary non-empty directory.
		if (std::filesystem::exists(output, error) && !std::filesystem::is_empty(output, error) && !IsPreviousExport(output))
		{
			result.Error = "the output directory is not empty and is not a previous export";
			return result;
		}

		const std::string executableName = SanitizeFileName(config.Name);
		// Read before anything is written, so a bad version or icon leaves a previous export untouched.
		std::optional<PackageInfo> info;
		std::optional<IconImage> icon;
		if (options.Package)
		{
			info = MakePackageInfo(config, executableName, result.Error);
			if (!info || !LoadProjectIcon(project, icon, result.Error))
				return result;
		}

		// Folder exports (and Windows packages) keep everything in the output directory; macOS and Linux packages are a
		// directory of their own inside it, replaced whole on every export.
		std::filesystem::path executableDirectory = output;
		std::filesystem::path projectDirectory = output;
		std::filesystem::path package;
#if defined(BS_PLATFORM_MACOS)
		if (options.Package)
		{
			package = output / (executableName + ".app");
			executableDirectory = package / "Contents" / "MacOS";
			projectDirectory = package / "Contents" / "Resources";
		}
#elif defined(BS_PLATFORM_LINUX)
		if (options.Package)
		{
			package = output / (executableName + ".AppDir");
			executableDirectory = package;
			projectDirectory = package;
		}
#endif
		if (!package.empty())
		{
			std::filesystem::remove_all(package, error);
			if (error)
			{
				result.Error = "cannot replace '" + FileSystem::PathToUtf8(package) + "': " + error.message();
				return result;
			}
		}

		if (!CopyProjectFiles(project, projectDirectory, result.Error))
			return result;

		std::filesystem::path executable = executableDirectory / executableName;
#if defined(BS_PLATFORM_WINDOWS)
		executable += ".exe";
#endif
		std::filesystem::create_directories(executableDirectory, error);
		if (!error)
			std::filesystem::copy_file(runtime, executable, std::filesystem::copy_options::overwrite_existing, error);
		if (error)
		{
			result.Error = "cannot copy the runtime: " + error.message();
			return result;
		}
		std::filesystem::permissions(executable, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec, std::filesystem::perm_options::add, error);

		result.PackagePath = output;
#if defined(BS_PLATFORM_MACOS)
		if (options.Package)
		{
			if (!BundleMoltenVK(package / "Contents" / "Frameworks", projectDirectory / "vulkan" / "icd.d", "../../../Frameworks/libMoltenVK.dylib", result.Error) || !FinishAppBundle(package, *info, icon, result.Error))
				return result;
			result.PackagePath = package;
		}
		else if (!BundleMoltenVK(output, output / "vulkan" / "icd.d", "../../libMoltenVK.dylib", result.Error))
			return result;
#elif defined(BS_PLATFORM_WINDOWS)
		if (options.Package && !WriteWindowsResources(executable, *info, icon ? &*icon : nullptr, result.Error))
			return result;
#elif defined(BS_PLATFORM_LINUX)
		if (options.Package && !FinishAppImage(package, output, *info, icon, options.AppImageTool, result))
			return result;
#endif

		result.Success = true;
		result.ExecutablePath = std::move(executable);
		result.FileCount = CountFiles(output);
		BS_CORE_INFO("Exporter: exported '{}' to '{}' ({} files)", config.Name, FileSystem::PathToUtf8(result.PackagePath), result.FileCount);
		for (const std::string& warning : result.Warnings)
			BS_CORE_WARN("Exporter: {}", warning);
		return result;
	}

}
