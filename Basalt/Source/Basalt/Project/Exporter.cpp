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

		// Where a package keeps the project: Contents/Resources of a macOS bundle, the AppDir itself on Linux.
		std::filesystem::path PackagedProjectDirectory(const std::filesystem::path& package)
		{
			return package.extension() == ".app" ? package / "Contents" / "Resources" : package;
		}

		// A folder export holds the project file itself; a package export holds this game's package, which does. Other
		// games' packages do not count: a folder of downloaded games is not a previous export of this one.
		bool IsPreviousExport(const std::filesystem::path& output, const std::string& executableName)
		{
			if (FileSystem::Exists(output / Project::FileName))
				return true;
			for (const char* extension : { ".app", ".AppDir" })
				if (FileSystem::Exists(PackagedProjectDirectory(output / (executableName + extension)) / Project::FileName))
					return true;
			return false;
		}

		// Copies the project file, its Assets and its input actions into directory (replacing a previous export's).
		bool CopyProjectFiles(const Project& project, const std::filesystem::path& directory, std::string& outError)
		{
			std::error_code error;
			// Assets the project no longer has must not survive from the previous export.
			std::filesystem::remove_all(directory / Project::AssetDirectoryName, error);
			if (error)
			{
				outError = "cannot remove the previous export's Assets in '" + FileSystem::PathToUtf8(directory) + "': " + error.message();
				return false;
			}
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

#if defined(BS_PLATFORM_MACOS)
		struct VulkanLibraries
		{
			std::filesystem::path Loader;
			std::filesystem::path MoltenVK;
		};

		// The Vulkan loader and MoltenVK to ship. scripts/package_release.py searches the same places for the tools archive.
		bool FindVulkanLibraries(VulkanLibraries& outLibraries, std::string& outError)
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
			outLibraries = { find("libvulkan.1.dylib"), find("libMoltenVK.dylib") };
			if (outLibraries.Loader.empty() || outLibraries.MoltenVK.empty())
			{
				outError = "cannot find libvulkan.1.dylib and libMoltenVK.dylib (install the Vulkan SDK or Homebrew vulkan-loader + molten-vk)";
				return false;
			}
			return true;
		}

		// Copies the Vulkan loader and MoltenVK into libraryDirectory, and writes the ICD manifest that names MoltenVK into
		// <manifestBase>/vulkan/icd.d, where VulkanLoader looks for it, so the game runs on Macs without the Vulkan SDK.
		bool BundleMoltenVK(const VulkanLibraries& libraries, const std::filesystem::path& libraryDirectory, const std::filesystem::path& manifestBase, std::string& outError)
		{
			std::error_code error;
			std::filesystem::create_directories(libraryDirectory, error);
			if (!error)
				std::filesystem::copy_file(std::filesystem::canonical(libraries.Loader, error), libraryDirectory / "libvulkan.1.dylib", std::filesystem::copy_options::overwrite_existing, error);
			if (!error)
				std::filesystem::copy_file(std::filesystem::canonical(libraries.MoltenVK, error), libraryDirectory / "libMoltenVK.dylib", std::filesystem::copy_options::overwrite_existing, error);
			// Package-manager files are often read-only; keep exports overwritable by later exports.
			for (const char* library : { "libvulkan.1.dylib", "libMoltenVK.dylib" })
				if (!error)
					std::filesystem::permissions(libraryDirectory / library, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read | std::filesystem::perms::others_read, std::filesystem::perm_options::replace, error);
			if (error)
			{
				outError = "cannot copy Vulkan libraries: " + error.message();
				return false;
			}

			// The manifest names the library relative to the manifest file itself.
			const std::filesystem::path manifestDirectory = manifestBase / "vulkan" / "icd.d";
			const std::string library = FileSystem::PathToUtf8((libraryDirectory / "libMoltenVK.dylib").lexically_relative(manifestDirectory).generic_u8string());
			const std::string manifest = R"({ "file_format_version": "1.0.0", "ICD": { "library_path": ")" + library + R"(", "api_version": "1.3.0", "is_portability_driver": true } })";
			if (!FileSystem::WriteTextFile(manifestDirectory / "MoltenVK_icd.json", manifest, outError))
			{
				outError = "cannot write the MoltenVK ICD manifest: " + outError;
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
				const auto icns = BuildIcns(*icon);
				if (!icns)
				{
					outError = "cannot encode the icon as .icns";
					return false;
				}
				info.IconFileName = info.ExecutableName + ".icns";
				if (!FileSystem::WriteBinaryFile(contents / "Resources" / info.IconFileName, icns->data(), icns->size(), outError))
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

		// AppImages an earlier export of this game wrote (<Name>-<version>-<arch>.AppImage, any version), so a new export
		// never leaves an outdated one beside its package.
		bool RemoveOldAppImages(const std::filesystem::path& output, const std::string& executableName, std::string& outError)
		{
			const std::string prefix = executableName + "-";
			std::error_code error;
			std::vector<std::filesystem::path> old;
			for (auto it = std::filesystem::directory_iterator(output, error); !error && it != std::filesystem::directory_iterator(); it.increment(error))
			{
				const std::string name = FileSystem::PathToUtf8(it->path().filename());
				if (!name.starts_with(prefix) || !name.ends_with(".AppImage"))
					continue;
				// What follows the name is "<version>-<arch>": another game's name ("<Name>-Demo") has letters first.
				const std::string rest = name.substr(prefix.size());
				const size_t dash = rest.find('-');
				if (dash != std::string::npos && dash > 0 && rest.find_first_not_of("0123456789.") == dash)
					old.push_back(it->path());
			}
			for (const auto& path : old)
				if (!error)
					std::filesystem::remove(path, error);
			if (error)
			{
				outError = "cannot remove the previous AppImage in '" + FileSystem::PathToUtf8(output) + "': " + error.message();
				return false;
			}
			return true;
		}

		// The AppDir's desktop entry, icon and AppRun around an executable and project already in place, then the
		// .AppImage file when appimagetool is available.
		bool FinishAppImage(const std::filesystem::path& appDir, const std::filesystem::path& output, const PackageInfo& info, const std::optional<IconImage>& icon, const std::filesystem::path& configuredTool, ExportResult& result)
		{
			const std::string iconFile = info.ExecutableName + ".png";
			if (!icon)
				result.Warnings.push_back("the project has no icon: the AppImage uses Basalt's default icon (set one with project.set icon)");
			const std::vector<uint8_t> png = EncodeIconPng(icon ? ResizeIcon(*icon, std::min<uint32_t>(512, std::max(icon->Width, icon->Height))) : MakeDefaultIcon(256));
			if (png.empty())
			{
				result.Error = "cannot encode the AppImage icon";
				return false;
			}
			if (!FileSystem::WriteBinaryFile(appDir / iconFile, png.data(), png.size(), result.Error) || !FileSystem::WriteTextFile(appDir / (info.ExecutableName + ".desktop"), BuildDesktopEntry(info), result.Error))
				return false;
			std::vector<std::pair<std::string, std::string>> links = { { ".DirIcon", iconFile } };
			// A game named AppRun already is the entry point; a link would replace it with a link to itself.
			if (info.ExecutableName != "AppRun")
				links.emplace_back("AppRun", info.ExecutableName);
			for (const auto& [link, target] : links)
			{
				std::error_code error;
				std::filesystem::create_symlink(target, appDir / link, error);
				if (error)
				{
					result.Error = "cannot create '" + FileSystem::PathToUtf8(appDir / link) + "': " + error.message();
					return false;
				}
			}

			if (!RemoveOldAppImages(output, info.ExecutableName, result.Error))
				return false;
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
		const std::string executableName = SanitizeFileName(config.Name);
		// Only ever replace a previous export, never an arbitrary non-empty directory.
		if (std::filesystem::exists(output, error) && !std::filesystem::is_empty(output, error) && !IsPreviousExport(output, executableName))
		{
			result.Error = "the output directory is not empty and is not a previous export of this game";
			return result;
		}

		// Everything that can be checked is read before anything is written, so a bad version, icon or missing library
		// leaves a previous export untouched.
		std::optional<PackageInfo> info;
		std::optional<IconImage> icon;
		if (options.Package)
		{
			info = MakePackageInfo(config, executableName, result.Error);
			if (!info || !LoadProjectIcon(project, icon, result.Error))
				return result;
		}
#if defined(BS_PLATFORM_MACOS)
		VulkanLibraries vulkan;
		if (!FindVulkanLibraries(vulkan, result.Error))
			return result;
#endif

		// Folder exports (and Windows packages) keep everything in the output directory; macOS and Linux packages are a
		// directory of their own inside it, replaced whole on every export (a Linux AppImage file sits beside its AppDir).
		std::filesystem::path executableDirectory = output;
		std::filesystem::path package;
#if defined(BS_PLATFORM_MACOS)
		if (options.Package)
		{
			package = output / (executableName + ".app");
			executableDirectory = package / "Contents" / "MacOS";
		}
#elif defined(BS_PLATFORM_LINUX)
		if (options.Package)
		{
			package = output / (executableName + ".AppDir");
			executableDirectory = package;
		}
#endif
		const std::filesystem::path projectDirectory = package.empty() ? output : PackagedProjectDirectory(package);
		if (!package.empty())
		{
			std::filesystem::remove_all(package, error);
			if (error)
			{
				result.Error = "cannot replace '" + FileSystem::PathToUtf8(package) + "': " + error.message();
				return result;
			}
		}

		// From here on a failure leaves a partial package (the previous one is gone): say so.
		const auto fail = [&]() {
			if (!package.empty())
				result.Error += " (the incomplete package '" + FileSystem::PathToUtf8(package) + "' remains; export again once this is fixed)";
			return result;
		};
		if (!CopyProjectFiles(project, projectDirectory, result.Error))
			return fail();

		std::filesystem::path executable = executableDirectory / executableName;
#if defined(BS_PLATFORM_WINDOWS)
		executable += ".exe";
#endif
		std::filesystem::create_directories(executableDirectory, error);
		if (!error)
			std::filesystem::copy_file(runtime, executable, std::filesystem::copy_options::overwrite_existing, error);
		if (!error)
			std::filesystem::permissions(executable, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec, std::filesystem::perm_options::add, error);
		if (error)
		{
			result.Error = "cannot copy the runtime: " + error.message();
			return fail();
		}

		std::filesystem::path packagePath = output;
#if defined(BS_PLATFORM_MACOS)
		if (options.Package)
		{
			if (!BundleMoltenVK(vulkan, package / "Contents" / "Frameworks", projectDirectory, result.Error) || !FinishAppBundle(package, *info, icon, result.Error))
				return fail();
			packagePath = package;
		}
		else if (!BundleMoltenVK(vulkan, output, output, result.Error))
			return fail();
#elif defined(BS_PLATFORM_WINDOWS)
		if (options.Package && !WriteWindowsResources(executable, *info, icon ? &*icon : nullptr, result.Error))
			return fail();
#elif defined(BS_PLATFORM_LINUX)
		if (options.Package)
		{
			if (!FinishAppImage(package, output, *info, icon, options.AppImageTool, result))
				return fail();
			packagePath = result.PackagePath;
		}
#endif

		result.Success = true;
		result.PackagePath = std::move(packagePath);
		result.ExecutablePath = std::move(executable);
		result.FileCount = CountFiles(output);
		BS_CORE_INFO("Exporter: exported '{}' to '{}' ({} files)", config.Name, FileSystem::PathToUtf8(result.PackagePath), result.FileCount);
		for (const std::string& warning : result.Warnings)
			BS_CORE_WARN("Exporter: {}", warning);
		return result;
	}

}
