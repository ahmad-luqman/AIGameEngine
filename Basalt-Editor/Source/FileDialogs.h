#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	// Native open/save/folder pickers (nativefiledialog-extended). Builds without them (no D-Bus development files
	// on Linux) report IsAvailable() false, and the editor's typed-path prompts remain the way to enter paths.
	class FileDialogs
	{
	public:
		enum class Kind
		{
			Open,
			Save,
			Folder
		};

		struct Filter
		{
			std::string Name;
			// Comma-separated extensions without dots, e.g. "gltf,glb".
			std::string Extensions;
		};

		static bool IsAvailable();
		// Blocks until the user chooses. initial is a file or directory to start from. Returns the chosen path, or
		// nothing when cancelled (outError empty) or when the dialog failed (outError set).
		static std::optional<std::filesystem::path> Browse(Kind kind, const std::filesystem::path& initial, const std::vector<Filter>& filters, std::string& outError);
	};

}
