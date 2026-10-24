#pragma once

#include "Basalt/Core/PackArchive.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	class Project;

	// How Exporter::Export builds the game.
	struct ExportOptions
	{
		// Defaults to BasaltRuntime next to the current executable. For distribution, export with a runtime built in
		// the Dist configuration.
		std::filesystem::path RuntimeExecutable;
		// Package the game for the host platform, with the project's icon, version and company: a macOS .app bundle,
		// a Windows folder whose executable carries icon and version resources, or a Linux AppImage (an AppDir, made
		// into a .AppImage file when appimagetool is found). Otherwise the game is a plain folder.
		bool Package = false;
		// appimagetool for Linux packages; empty: $APPIMAGETOOL, then PATH.
		std::filesystem::path AppImageTool;
		// Pack Assets into Assets.bpak (Project::ArchiveFileName) instead of copying the folder: one file to ship, with
		// the asset UUIDs in its index instead of .meta files. The project and input files stay beside it.
		bool Archive = false;
	};

	struct ExportResult
	{
		bool Success = false;
		std::string Error;
		std::filesystem::path ExecutablePath;
		// What to distribute: the .app bundle, the .AppImage file (the AppDir when appimagetool was not found), or the
		// output directory (folder exports and Windows packages).
		std::filesystem::path PackagePath;
		// Problems that did not stop the export (no project icon, no appimagetool).
		std::vector<std::string> Warnings;
		uint32_t FileCount = 0;
		// What ExportOptions::Archive packed.
		std::optional<PackArchiveWriter::Statistics> Archive;
	};

	// Produces a standalone game: the runtime executable (named after the project, sanitized), the project file, its input
	// actions and Assets (a folder, or Assets.bpak with ExportOptions::Archive), plus on macOS the Vulkan loader and MoltenVK
	// with a relative ICD manifest; with ExportOptions::Package, a platform package around them. The runtime started from
	// the export finds the project (GetExportedProjectDirectory), mounts its archive (MountExportedArchive) and plays its
	// start scene.
	class Exporter
	{
	public:
		static ExportResult Export(const Project& project, const std::filesystem::path& outputDirectory, const ExportOptions& options = {});

		static std::filesystem::path GetDefaultRuntimePath();
		// Where an exported runtime finds its project: next to the executable, or in Contents/Resources of a macOS
		// .app bundle. Returns executableDirectory when neither holds a project file.
		static std::filesystem::path GetExportedProjectDirectory(const std::filesystem::path& executableDirectory);
		// Mounts projectDirectory's Assets.bpak over it (FileSystem::Mount) when there is one. False, with the reason, for
		// an archive that cannot be opened; true (nothing mounted) without one.
		static bool MountExportedArchive(const std::filesystem::path& projectDirectory, std::string& outError);
	};

}
