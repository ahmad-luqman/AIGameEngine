#include "Basalt/Renderer/ValidationLayerManifest.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <random>

namespace Basalt::ValidationLayerManifest {

	namespace {

		// Several processes (parallel ctest runs) may write the copy at once: each writes its own temporary file
		// and renames it into place, which replaces the target atomically.
		bool WriteShared(const std::filesystem::path& path, const std::string& text, std::string& outError)
		{
			std::filesystem::path temporary = path;
			temporary += "." + std::to_string(std::random_device{}()) + ".tmp";
			{
				std::ofstream stream(temporary, std::ios::out | std::ios::binary | std::ios::trunc);
				stream << text;
				stream.close();
				if (!stream)
				{
					std::error_code ignored;
					std::filesystem::remove(temporary, ignored);
					outError = "cannot write '" + temporary.string() + "'";
					return false;
				}
			}
			std::error_code error;
			std::filesystem::rename(temporary, path, error);
			if (error)
			{
				std::error_code ignored;
				std::filesystem::remove(temporary, ignored);
				outError = "cannot replace '" + path.string() + "': " + error.message();
				return false;
			}
			return true;
		}

	}

	std::optional<std::filesystem::path> WriteAbsoluteCopy(const std::filesystem::path& prefix, const std::filesystem::path& outputDirectory, std::string& outError)
	{
		outError.clear();
		const std::filesystem::path manifestPath = prefix / "share" / "vulkan" / "explicit_layer.d" / FileName;
		if (!FileSystem::Exists(manifestPath))
			return std::nullopt;
		const std::optional<std::string> text = FileSystem::ReadTextFile(manifestPath);
		if (!text)
		{
			outError = "cannot read '" + manifestPath.string() + "'";
			return std::nullopt;
		}
		nlohmann::json manifest = ParseJson(*text);
		const auto layer = manifest.is_object() ? manifest.find("layer") : manifest.end();
		if (layer == manifest.end() || !layer->is_object() || !layer->contains("library_path") || !(*layer)["library_path"].is_string())
		{
			outError = "'" + manifestPath.string() + "' has no layer.library_path";
			return std::nullopt;
		}
		const std::filesystem::path library = FileSystem::PathFromUtf8((*layer)["library_path"].get<std::string>());
		const std::filesystem::path absoluteLibrary = prefix / "lib" / library;
		if (library.is_absolute() || library.has_parent_path() || !FileSystem::Exists(absoluteLibrary))
			return std::nullopt;

		(*layer)["library_path"] = FileSystem::PathToUtf8(absoluteLibrary);
		const std::string copy = manifest.dump(4);
		const std::filesystem::path copyPath = outputDirectory / FileName;
		if (const std::optional<std::string> existing = FileSystem::ReadTextFile(copyPath); existing && *existing == copy)
			return outputDirectory;
		std::error_code error;
		std::filesystem::create_directories(outputDirectory, error);
		if (error)
		{
			outError = "cannot create '" + outputDirectory.string() + "': " + error.message();
			return std::nullopt;
		}
		if (!WriteShared(copyPath, copy, outError))
			return std::nullopt;
		return outputDirectory;
	}

}
