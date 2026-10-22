#include "FileDialogs.h"

#include "Basalt/Core/FileSystem.h"

#ifdef BS_NATIVE_DIALOGS
	#include <nfd.h>
#endif

namespace Basalt {

	bool FileDialogs::IsAvailable()
	{
#ifdef BS_NATIVE_DIALOGS
		return true;
#else
		return false;
#endif
	}

	std::optional<std::filesystem::path> FileDialogs::Browse(Kind kind, const std::filesystem::path& initial, const std::vector<Filter>& filters, std::string& outError)
	{
		outError.clear();
#ifdef BS_NATIVE_DIALOGS
		// Per call: on Linux the portal connection, on Windows COM, belong to the calling (main) thread only while
		// a dialog is open.
		if (NFD_Init() != NFD_OKAY)
		{
			outError = std::string("cannot open a file dialog: ") + (NFD_GetError() ? NFD_GetError() : "initialization failed");
			NFD_ClearError();
			return std::nullopt;
		}
		std::error_code error;
		const bool isDirectory = std::filesystem::is_directory(initial, error);
		std::filesystem::path directory = kind == Kind::Folder || isDirectory ? initial : initial.parent_path();
		// Dialogs reject (Windows) or ignore start directories that do not exist; start from the nearest one.
		while (!directory.empty() && !std::filesystem::is_directory(directory, error) && directory != directory.parent_path())
			directory = directory.parent_path();
		const std::string start = FileSystem::PathToUtf8(directory);
		const std::string name = isDirectory || kind == Kind::Folder ? std::string() : FileSystem::PathToUtf8(initial.filename());
		std::vector<nfdu8filteritem_t> items;
		items.reserve(filters.size());
		for (const Filter& filter : filters)
			items.push_back({ filter.Name.c_str(), filter.Extensions.c_str() });
		const auto count = static_cast<nfdfiltersize_t>(items.size());
		const nfdu8char_t* startPath = start.empty() ? nullptr : start.c_str();

		nfdu8char_t* chosen = nullptr;
		nfdresult_t result = NFD_CANCEL;
		switch (kind)
		{
			case Kind::Open:
				result = NFD_OpenDialogU8(&chosen, items.data(), count, startPath);
				break;
			case Kind::Save:
				result = NFD_SaveDialogU8(&chosen, items.data(), count, startPath, name.empty() ? nullptr : name.c_str());
				break;
			case Kind::Folder:
				result = NFD_PickFolderU8(&chosen, startPath);
				break;
		}
		std::optional<std::filesystem::path> path;
		if (result == NFD_OKAY)
		{
			path = FileSystem::PathFromUtf8(chosen);
			NFD_FreePathU8(chosen);
		}
		else if (result == NFD_ERROR)
		{
			outError = std::string("file dialog failed: ") + (NFD_GetError() ? NFD_GetError() : "unknown error");
			NFD_ClearError();
		}
		NFD_Quit();
		return path;
#else
		(void)kind;
		(void)initial;
		(void)filters;
		outError = "native file dialogs are not available in this build";
		return std::nullopt;
#endif
	}

}
