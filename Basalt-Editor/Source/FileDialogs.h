#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	// Native open/save/folder pickers (nativefiledialog-extended). Builds without them (Linux without D-Bus development
	// files) report IsAvailable() false and keep only the editor's typed-path prompts; a dialog that fails at runtime
	// (no portal) reports an error and the typed path stays usable.
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
		// Blocks until the user chooses. initial is a file or directory to start from; folder pickers ignore filters.
		// Returns the chosen path, or nothing when cancelled (outError empty) or when the dialog failed (outError set).
		static std::optional<std::filesystem::path> Browse(Kind kind, const std::filesystem::path& initial, const std::vector<Filter>& filters, std::string& outError);
	};

}
