#pragma once

#include <filesystem>
#include <string>

namespace Basalt {

	class Project;

	struct ExportResult
	{
		bool Success = false;
		std::string Error;
		std::filesystem::path ExecutablePath;
		uint32_t FileCount = 0;
	};

	// Produces a standalone game folder: the runtime executable (renamed after the project), the project
	// file and its Assets, plus on macOS the Vulkan loader and MoltenVK with a relative ICD manifest.
	// The runtime started from that folder loads the project next to it and plays its start scene.
	class Exporter
	{
	public:
		// runtimeExecutable: defaults to BasaltRuntime next to the current executable. For distribution,
		// export with a runtime built in the Dist configuration.
		static ExportResult Export(const Project& project, const std::filesystem::path& outputDirectory, const std::filesystem::path& runtimeExecutable = {});

		static std::filesystem::path GetDefaultRuntimePath();
	};

}
