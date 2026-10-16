#include "Basalt/Renderer/ValidationLayerManifest.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"

#include <nlohmann/json.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <functional>

namespace Basalt::ValidationLayerManifest {

	namespace {

		CopyResult Written()
		{
			return { CopyOutcome::Written, {} };
		}

		CopyResult NotNeeded(std::string message)
		{
			return { CopyOutcome::NotNeeded, std::move(message) };
		}

		CopyResult Failed(std::string message)
		{
			return { CopyOutcome::Failed, std::move(message) };
		}

		// "/opt/homebrew/" and "/opt/homebrew" name the same prefix.
		std::filesystem::path Normalized(const std::filesystem::path& path)
		{
			std::filesystem::path normal = path.lexically_normal();
			if (!normal.has_filename() && normal.has_parent_path() && normal != normal.root_path())
				normal = normal.parent_path();
			return normal;
		}

	}

	CopyResult WriteAbsoluteCopy(const std::filesystem::path& prefix, const std::filesystem::path& outputDirectory)
	{
		const std::filesystem::path manifestPath = prefix / "share" / "vulkan" / "explicit_layer.d" / FileName;
		if (!FileSystem::Exists(manifestPath))
			return {};
		const std::string manifestName = FileSystem::PathToUtf8(manifestPath);
		const std::optional<std::string> text = FileSystem::ReadTextFile(manifestPath);
		if (!text)
			return Failed("cannot read '" + manifestName + "' (check its permissions)");
		nlohmann::json manifest = ParseJson(*text);
		if (manifest.is_discarded())
			return Failed("'" + manifestName + "' is not valid JSON");
		nlohmann::json* libraryPath = nullptr;
		if (auto layer = manifest.is_object() ? manifest.find("layer") : manifest.end(); layer != manifest.end() && layer->is_object())
		{
			if (auto path = layer->find("library_path"); path != layer->end() && path->is_string())
				libraryPath = &*path;
		}
		if (!libraryPath)
			return Failed("'" + manifestName + "' has no layer.library_path");

		const std::filesystem::path library = FileSystem::PathFromUtf8(libraryPath->get<std::string>());
		if (library.is_absolute() || library.has_parent_path())
			return {};
		const std::filesystem::path absoluteLibrary = prefix / "lib" / library;
		if (!FileSystem::Exists(absoluteLibrary))
			return NotNeeded("'" + manifestName + "' names " + libraryPath->get<std::string>() + ", but '" + FileSystem::PathToUtf8(absoluteLibrary) + "' does not exist");

		*libraryPath = FileSystem::PathToUtf8(absoluteLibrary);
		const std::string copy = manifest.dump(4);
		const std::filesystem::path copyPath = outputDirectory / FileName;
		std::string error;
		if (FileSystem::ReadTextFile(copyPath) != copy && !FileSystem::WriteTextFile(copyPath, copy, error))
			return Failed(error);
		return Written();
	}

	std::vector<std::filesystem::path> HomebrewPrefixes(const std::optional<std::string>& homebrewPrefix)
	{
		std::vector<std::filesystem::path> prefixes;
		if (homebrewPrefix && !homebrewPrefix->empty())
			prefixes.push_back(Normalized(FileSystem::PathFromUtf8(*homebrewPrefix)));
		for (const char* standard : { "/opt/homebrew", "/usr/local" })
		{
			if (std::ranges::find(prefixes, std::filesystem::path(standard)) == prefixes.end())
				prefixes.emplace_back(standard);
		}
		return prefixes;
	}

	std::filesystem::path CopyDirectory(const std::filesystem::path& root, const std::filesystem::path& prefix)
	{
		return root / fmt::format("{:016x}", std::hash<std::string>{}(FileSystem::PathToUtf8(Normalized(prefix))));
	}

	std::optional<std::filesystem::path> FirstAbsoluteCopy(const std::vector<std::filesystem::path>& prefixes, const std::filesystem::path& root, std::vector<std::string>& outMessages)
	{
		for (const std::filesystem::path& prefix : prefixes)
		{
			std::filesystem::path directory = CopyDirectory(root, prefix);
			const CopyResult copy = WriteAbsoluteCopy(prefix, directory);
			if (copy.Outcome == CopyOutcome::Written)
				return directory;
			if (!copy.Message.empty())
				outMessages.push_back(FileSystem::PathToUtf8(prefix) + ": " + copy.Message);
		}
		return std::nullopt;
	}

	std::string AppendSearchPath(const std::string& existing, const std::string& directory, char separator)
	{
		std::string result;
		bool present = false;
		size_t start = 0;
		while (start <= existing.size())
		{
			const size_t end = std::min(existing.find(separator, start), existing.size());
			const std::string entry = existing.substr(start, end - start);
			if (!entry.empty())
			{
				present = present || entry == directory;
				result += (result.empty() ? "" : std::string(1, separator)) + entry;
			}
			start = end + 1;
		}
		if (!present)
			result += (result.empty() ? "" : std::string(1, separator)) + directory;
		return result;
	}

}
