#include "Basalt/Renderer/ValidationLayerManifest.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace Basalt::ValidationLayerManifest {

	namespace {

		CopyResult Failed(std::string message)
		{
			return { CopyOutcome::Failed, {}, std::move(message) };
		}

	}

	CopyResult WriteAbsoluteCopy(const std::filesystem::path& prefix, const std::filesystem::path& outputDirectory)
	{
		const std::filesystem::path manifestPath = prefix / "share" / "vulkan" / "explicit_layer.d" / FileName;
		const std::string manifestName = FileSystem::PathToUtf8(manifestPath);
		if (!FileSystem::Exists(manifestPath))
			return {};
		const std::optional<std::string> text = FileSystem::ReadTextFile(manifestPath);
		if (!text)
			return Failed("cannot read '" + manifestName + "'");
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
			return { CopyOutcome::NotNeeded, {}, "'" + manifestName + "' names " + libraryPath->get<std::string>() + ", but '" + FileSystem::PathToUtf8(absoluteLibrary) + "' does not exist" };

		*libraryPath = FileSystem::PathToUtf8(absoluteLibrary);
		const std::string copy = manifest.dump(4);
		const std::filesystem::path copyPath = outputDirectory / FileName;
		if (FileSystem::Exists(copyPath) && FileSystem::ReadTextFile(copyPath) == copy)
			return { CopyOutcome::Written, outputDirectory, {} };
		std::string error;
		if (!FileSystem::WriteTextFile(copyPath, copy, error))
			return Failed(error);
		return { CopyOutcome::Written, outputDirectory, {} };
	}

	std::vector<std::filesystem::path> HomebrewPrefixes(const std::optional<std::string>& homebrewPrefix)
	{
		std::vector<std::filesystem::path> prefixes;
		if (homebrewPrefix && !homebrewPrefix->empty())
			prefixes.push_back(FileSystem::PathFromUtf8(*homebrewPrefix));
		for (const char* standard : { "/opt/homebrew", "/usr/local" })
		{
			if (std::ranges::find(prefixes, std::filesystem::path(standard)) == prefixes.end())
				prefixes.emplace_back(standard);
		}
		return prefixes;
	}

	std::string AppendSearchPath(const std::string& existing, const std::string& directory, char separator)
	{
		size_t start = 0;
		while (start <= existing.size())
		{
			const size_t end = std::min(existing.find(separator, start), existing.size());
			if (existing.compare(start, end - start, directory) == 0 && end - start == directory.size())
				return existing;
			start = end + 1;
		}
		return existing.empty() ? directory : existing + separator + directory;
	}

}
