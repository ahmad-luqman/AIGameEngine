#include "FileDialogs.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/FileSystem.h"

#ifdef BS_NATIVE_DIALOGS
	#include <nfd.h>
#endif

namespace Basalt {

#ifdef BS_NATIVE_DIALOGS
	namespace {
		// nfd's last error (or fallback), cleared so the next dialog starts clean.
		std::string TakeError(const char* fallback)
		{
			std::string error = NFD_GetError() ? NFD_GetError() : fallback;
			NFD_ClearError();
			return error;
		}
	}
#endif

	bool FileDialogs::IsAvailable()
	{
#ifdef BS_NATIVE_DIALOGS
		return true;
#else
		return false;
#endif
	}

	std::optional<std::filesystem::path> FileDialogs::Browse([[maybe_unused]] Kind kind, [[maybe_unused]] const std::filesystem::path& initial, [[maybe_unused]] const std::vector<Filter>& filters, std::string& outError)
	{
		outError.clear();
#ifdef BS_NATIVE_DIALOGS
		// Initialized per call, so the portal's D-Bus connection (Linux) and the COM apartment (Windows) are held only
		// while a dialog is open, never across frames.
		if (NFD_Init() != NFD_OKAY)
		{
			outError = "cannot open a file dialog: " + TakeError("initialization failed");
			return std::nullopt;
		}
		// Windows parses the start directory as a shell path: native separators and no trailing one.
		std::filesystem::path start = initial.lexically_normal().make_preferred();
		if (!start.empty() && !start.has_filename())
			start = start.parent_path();
		std::error_code error;
		const bool startsInDirectory = kind == Kind::Folder || std::filesystem::is_directory(start, error);
		std::filesystem::path directory = startsInDirectory ? start : start.parent_path();
		// Dialogs reject (Windows) or ignore start directories that do not exist; start from the nearest one.
		while (!directory.empty() && !std::filesystem::is_directory(directory, error) && directory != directory.parent_path())
			directory = directory.parent_path();
		const std::string startDirectory = FileSystem::PathToUtf8(directory);
		const std::string name = startsInDirectory ? std::string() : FileSystem::PathToUtf8(start.filename());
		std::vector<nfdu8filteritem_t> items;
		items.reserve(filters.size());
		for (const Filter& filter : filters)
		{
			BS_CORE_ASSERT(filter.Extensions.find('.') == std::string::npos, "filter extensions are written without dots");
			items.push_back({ filter.Name.c_str(), filter.Extensions.c_str() });
		}
		const auto count = static_cast<nfdfiltersize_t>(items.size());
		const nfdu8char_t* startPath = startDirectory.empty() ? nullptr : startDirectory.c_str();

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
			outError = "file dialog failed: " + TakeError("unknown error");
		}
		NFD_Quit();
		return path;
#else
		outError = "native file dialogs are not available in this build";
		return std::nullopt;
#endif
	}

}
