#include "Basalt/Project/Exporter.h"

#include "Basalt/Asset/AssetRegistry.h"
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

		// The executable's file name on this platform.
		std::string ExecutableFileName(const std::string& executableName)
		{
#if defined(BS_PLATFORM_WINDOWS)
			return executableName + ".exe";
#else
			return executableName;
#endif
		}

		// A folder export of this game holds the project file and its executable; a package export holds this game's
		// package. Other games' exports do not count: a folder of downloaded games is not a previous export of this one.
		bool IsPreviousExport(const std::filesystem::path& output, const std::string& executableName)
		{
			if (FileSystem::Exists(output / Project::FileName) && FileSystem::Exists(output / ExecutableFileName(executableName)))
				return true;
			for (const char* extension : { ".app", ".AppDir" })
				if (FileSystem::Exists(PackagedProjectDirectory(output / (executableName + extension)) / Project::FileName))
					return true;
			return false;
		}

		// Packs the project's Assets into archive: every file except the .meta sidecars, whose UUIDs go into the index.
		// Assets whose sidecar is broken go in without a UUID, so references to them will not resolve: outWarnings says so
		// (project.validate reports them as errors, so only an export with validation off gets here).
		std::optional<PackArchiveWriter::Statistics> WriteAssetArchive(const Project& project, const std::filesystem::path& archive, std::vector<std::string>& outWarnings, std::string& outError)
		{
			// A fresh scan: the project's own registry may predate files written since it loaded.
			AssetRegistry registry(project.GetDirectory());
			if (!registry.Refresh(outError))
				return std::nullopt;
			for (const auto& [asset, problem] : registry.GetProblems())
			{
				std::string warning = "'" + asset + "' is archived without its UUID (";
				warning.append(asset).append(".meta: ").append(problem).append("), so references to it will not resolve");
				outWarnings.push_back(std::move(warning));
			}
			PackArchiveWriter writer;
			const std::filesystem::path assets = project.GetDirectory() / Project::AssetDirectoryName;
			std::error_code error;
			for (auto it = std::filesystem::recursive_directory_iterator(assets, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
			{
				const std::string key = FileSystem::PathToGenericUtf8(it->path().lexically_relative(project.GetDirectory()));
				const auto inspectFailed = [&]() {
					outError = "cannot inspect '" + key + "': " + error.message();
					return std::nullopt;
				};
				// The iterator does not enter linked folders (a folder export's copy does): refuse rather than ship a game
				// that silently lacks their files. Linked files are read through, as the folder export copies them, and a
				// link to nothing is refused rather than left out.
				const bool link = it->is_symlink(error);
				if (error)
					return inspectFailed();
				if (link)
				{
					std::error_code targetError;
					const std::filesystem::file_status target = std::filesystem::status(it->path(), targetError);
					if (target.type() == std::filesystem::file_type::directory)
					{
						outError = "'" + key + "' is a linked folder, which an archive cannot hold (replace the link with the folder)";
						return std::nullopt;
					}
					if (target.type() != std::filesystem::file_type::regular)
					{
						outError = "'" + key + "' is a broken link (it names no file)";
						return std::nullopt;
					}
				}
				const bool regular = it->is_regular_file(error);
				if (error)
					return inspectFailed();
				if (!regular || FileSystem::GetLowercaseExtension(it->path()) == ".meta")
					continue;
				if (!PackArchive::IsValidPath(key))
				{
					outError = "'" + key + "' cannot be stored in an archive (a name with ':' or '\\', or a path over " + std::to_string(PackArchive::MaxPathLength) + " bytes)";
					return std::nullopt;
				}
				const std::optional<UUID> id = registry.FindID(key);
				if (!writer.AddFile(key, it->path(), id ? static_cast<uint64_t>(*id) : 0, outError))
					return std::nullopt;
			}
			if (error)
			{
				outError = "cannot list '" + FileSystem::PathToUtf8(assets) + "': " + error.message();
				return std::nullopt;
			}
			return writer.Write(archive, outError);
		}

		// Copies the project file and its input actions into directory with its Assets (a folder, or with archive an
		// Assets.bpak whose statistics go to outArchive), replacing a previous export's.
		bool CopyProjectFiles(const Project& project, const std::filesystem::path& directory, bool archive, std::optional<PackArchiveWriter::Statistics>& outArchive, std::vector<std::string>& outWarnings, std::string& outError)
		{
			std::error_code error;
			// Assets the project no longer has must not survive from the previous export, in either form: the runtime would
			// read an archive left beside a new Assets folder.
			for (const char* previous : { Project::AssetDirectoryName, Project::ArchiveFileName })
			{
				std::filesystem::remove_all(directory / previous, error);
				if (error)
				{
					outError = "cannot remove the previous export's '" + FileSystem::PathToUtf8(directory / previous) + "': " + error.message();
					return false;
				}
			}
			std::filesystem::create_directories(directory, error);
			if (error)
			{
				outError = "cannot create '" + FileSystem::PathToUtf8(directory) + "': " + error.message();
				return false;
			}
			if (archive)
			{
				outArchive = WriteAssetArchive(project, directory / Project::ArchiveFileName, outWarnings, outError);
				if (!outArchive)
				{
					outError = "cannot write " + std::string(Project::ArchiveFileName) + ": " + outError;
					return false;
				}
			}
			else
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
		constexpr const char* LoaderFileName = "libvulkan.1.dylib";
		constexpr const char* MoltenVKFileName = "libMoltenVK.dylib";

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
			outLibraries = { find(LoaderFileName), find(MoltenVKFileName) };
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
			if (error)
			{
				outError = "cannot create '" + FileSystem::PathToUtf8(libraryDirectory) + "': " + error.message();
				return false;
			}
			for (const auto& [source, name] : { std::pair{ &libraries.Loader, LoaderFileName }, std::pair{ &libraries.MoltenVK, MoltenVKFileName } })
			{
				// Package managers install versioned files behind symlinks; ship the file itself.
				const std::filesystem::path file = std::filesystem::canonical(*source, error);
				if (!error)
					std::filesystem::copy_file(file, libraryDirectory / name, std::filesystem::copy_options::overwrite_existing, error);
				// Package-manager files are often read-only; keep exports overwritable by later exports.
				if (!error)
					std::filesystem::permissions(libraryDirectory / name, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read | std::filesystem::perms::others_read, std::filesystem::perm_options::replace, error);
				if (error)
				{
					outError = "cannot copy '" + FileSystem::PathToUtf8(*source) + "' to '" + FileSystem::PathToUtf8(libraryDirectory / name) + "': " + error.message();
					return false;
				}
			}

			// The manifest names the library relative to the manifest file itself.
			const std::filesystem::path manifestDirectory = manifestBase / "vulkan" / "icd.d";
			const std::string library = FileSystem::PathToUtf8((libraryDirectory / MoltenVKFileName).lexically_relative(manifestDirectory).generic_u8string());
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

		// Removes the AppImages an earlier export of this game wrote (<Name>-<version>-<arch>.AppImage, any version), so a
		// new AppImage never sits beside an outdated one.
		bool RemoveOldAppImages(const std::filesystem::path& output, const std::string& executableName, std::string& outError)
		{
			const std::string prefix = executableName + "-";
			std::error_code error;
			std::vector<std::filesystem::path> old;
			for (auto it = std::filesystem::directory_iterator(output, error); !error && it != std::filesystem::directory_iterator(); it.increment(error))
			{
				const std::string name = FileSystem::PathToUtf8(it->path().filename());
				if (!name.starts_with(prefix))
					continue;
				// What follows the name is "<version>-<arch>.AppImage". Sanitized names have no dots and a version has two or
				// three, so another game's name ("<Name>-Demo", "<Name>-2") never passes for one.
				const std::string rest = name.substr(prefix.size());
				const size_t dash = rest.find('-');
				if (dash != std::string::npos && ParseProjectVersion(std::string_view(rest).substr(0, dash)) && (rest.substr(dash + 1) == "x86_64.AppImage" || rest.substr(dash + 1) == "aarch64.AppImage"))
					old.push_back(it->path());
			}
			if (error)
			{
				outError = "cannot list '" + FileSystem::PathToUtf8(output) + "': " + error.message();
				return false;
			}
			for (const auto& path : old)
			{
				if (!std::filesystem::remove(path, error) && error)
				{
					outError = "cannot remove the earlier AppImage '" + FileSystem::PathToUtf8(path) + "': " + error.message();
					return false;
				}
				BS_CORE_INFO("Exporter: removed the earlier AppImage '{}'", FileSystem::PathToUtf8(path));
			}
			return true;
		}

		// The AppDir's desktop entry, icon and AppRun around an executable and project already in place, then the
		// .AppImage file when appimagetool is available. outPackage is the AppImage, or the AppDir without appimagetool.
		bool FinishAppImage(const std::filesystem::path& appDir, const std::filesystem::path& output, const PackageInfo& info, const std::optional<IconImage>& icon, const std::filesystem::path& configuredTool, std::filesystem::path& outPackage, ExportResult& result)
		{
			const std::string iconFile = info.ExecutableName + ".png";
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

			const std::filesystem::path tool = FindAppImageTool(configuredTool);
			if (tool.empty())
			{
				// Earlier AppImages stay: without a new one, the newest of them is still the last working package.
				result.Warnings.push_back("appimagetool was not found (set APPIMAGETOOL or add it to PATH): the package is the AppDir '" + FileSystem::PathToUtf8(appDir) + "'");
				outPackage = appDir;
				return true;
			}
	#if defined(__aarch64__)
			const char* architecture = "aarch64";
	#else
			const char* architecture = "x86_64";
	#endif
			if (!RemoveOldAppImages(output, info.ExecutableName, result.Error))
				return false;
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
			outPackage = appImage;
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

	bool Exporter::MountExportedArchive(const std::filesystem::path& projectDirectory, std::string& outError)
	{
		const std::filesystem::path file = projectDirectory / Project::ArchiveFileName;
		std::error_code error;
		const bool exists = std::filesystem::exists(file, error);
		if (!exists && !error)
			return true;
		const Ref<PackArchive> archive = PackArchive::Open(file, outError);
		if (!archive)
			return false;
		// Exports never leave both forms. A folder copied in later is shadowed wherever the archive has the same file, and its
		// sidecars are ignored (the asset registry takes UUIDs from the index only), so UUID references to its files fail.
		if (FileSystem::Exists(projectDirectory / Project::AssetDirectoryName))
			BS_CORE_WARN("Exporter: both {} and an Assets folder are in '{}'; files in the archive take precedence", Project::ArchiveFileName, FileSystem::PathToUtf8(projectDirectory));
		FileSystem::Mount(projectDirectory, archive);
		BS_CORE_INFO("Exporter: reading assets from '{}' ({} files)", FileSystem::PathToUtf8(file), archive->GetEntries().size());
		return true;
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
		const bool outputExists = std::filesystem::exists(output, error);
		const bool outputEmpty = !error && (!outputExists || std::filesystem::is_empty(output, error));
		if (error)
		{
			result.Error = "cannot inspect the output directory '" + FileSystem::PathToUtf8(output) + "': " + error.message();
			return result;
		}
		if (!outputEmpty && !IsPreviousExport(output, executableName))
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
#if defined(BS_PLATFORM_LINUX)
			// An AppImage must have an icon.
			const char* fallback = "the AppImage uses Basalt's default icon";
#else
			const char* fallback = "the package shows the platform's generic application icon";
#endif
			if (!icon)
				result.Warnings.push_back(std::string("the project has no icon: ") + fallback + " (set one with project.set icon)");
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
			else
				result.Error += " (the export in '" + FileSystem::PathToUtf8(output) + "' is incomplete; export again once this is fixed)";
			return result;
		};
		std::optional<PackArchiveWriter::Statistics> archive;
		if (!CopyProjectFiles(project, projectDirectory, options.Archive, archive, result.Warnings, result.Error))
			return fail();

		std::filesystem::path executable = executableDirectory / ExecutableFileName(executableName);
		std::filesystem::create_directories(executableDirectory, error);
		// The previous export's executable may still be locked by a smoke test that just ran it.
		if (!error)
			FileSystem::CopyFileReplacing(runtime, executable, error);
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
		if (options.Package && !FinishAppImage(package, output, *info, icon, options.AppImageTool, packagePath, result))
			return fail();
#endif

		result.Success = true;
		result.ArchiveStatistics = archive;
		result.PackagePath = std::move(packagePath);
		result.ExecutablePath = std::move(executable);
		result.FileCount = CountFiles(output);
		BS_CORE_INFO("Exporter: exported '{}' to '{}' ({} files)", config.Name, FileSystem::PathToUtf8(result.PackagePath), result.FileCount);
		for (const std::string& warning : result.Warnings)
			BS_CORE_WARN("Exporter: {}", warning);
		return result;
	}

}
