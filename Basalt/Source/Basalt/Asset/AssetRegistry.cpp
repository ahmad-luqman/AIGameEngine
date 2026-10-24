#include "Basalt/Asset/AssetRegistry.h"

#include "Basalt/Asset/AssetKey.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"

#include <charconv>
#include <system_error>
#include <utility>

namespace Basalt {

	// Opaque unresolved keys keep the UUID and the exact UTF-8 hint in existing string component fields.
	// Hex encoding prevents a hint from being interpreted as a filesystem path or embedded-image selector.
	namespace {
		std::string EncodeHint(const std::string& hint)
		{
			constexpr char Digits[] = "0123456789abcdef";
			std::string result;
			result.reserve(hint.size() * 2);
			for (unsigned char byte : hint)
			{
				result += Digits[byte >> 4];
				result += Digits[byte & 15];
			}
			return result;
		}

		std::optional<std::string> DecodeHint(std::string_view encoded)
		{
			if (encoded.size() % 2 != 0)
				return std::nullopt;
			auto digit = [](char value) -> int {
				if (value >= '0' && value <= '9')
					return value - '0';
				if (value >= 'a' && value <= 'f')
					return value - 'a' + 10;
				return -1;
			};
			std::string result;
			result.reserve(encoded.size() / 2);
			for (size_t i = 0; i < encoded.size(); i += 2)
			{
				const int high = digit(encoded[i]), low = digit(encoded[i + 1]);
				if (high < 0 || low < 0)
					return std::nullopt;
				result += static_cast<char>(high * 16 + low);
			}
			try
			{
				// Opaque keys are user-editable strings; reject hex that decodes to invalid JSON UTF-8.
				(void)nlohmann::json(result).dump();
			}
			catch (const nlohmann::json::exception&)
			{
				return std::nullopt;
			}
			return result;
		}
	}

	AssetRegistry::AssetRegistry(std::filesystem::path root)
		: m_Root(std::move(root))
	{
		if (!m_Root.empty())
		{
			std::error_code error;
			const auto canonical = std::filesystem::weakly_canonical(m_Root, error);
			if (!error)
				m_Root = canonical;
		}
	}

	std::optional<UUID> AssetRegistry::ParseID(std::string_view text, std::string& outError)
	{
		if (text.empty())
		{
			outError = "asset UUID must be a nonzero canonical decimal uint64 string";
			return std::nullopt;
		}
		uint64_t id = 0;
		const auto result = std::from_chars(text.data(), text.data() + text.size(), id);
		if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || id == 0 || std::to_string(id) != text)
		{
			outError = "asset UUID must be a nonzero canonical decimal uint64 string";
			return std::nullopt;
		}
		return UUID(id);
	}

	std::optional<UUID> AssetRegistry::ParseMeta(const nlohmann::json& data, std::string& outError)
	{
		if (!data.is_object() || data.size() != 2 || !data.contains("Version") || !data["Version"].is_number_integer() || data["Version"] != 1 || !data.contains("UUID") || !data["UUID"].is_string())
		{
			outError = "metadata must contain Version: 1 and UUID: a nonzero decimal string";
			return std::nullopt;
		}
		return ParseID(data["UUID"].get_ref<const std::string&>(), outError);
	}

	bool AssetRegistry::CheckPath(const std::string& path, std::filesystem::path& absolute, std::string& outError, bool allowHash) const
	{
		const auto relative = FileSystem::PathFromUtf8(path).lexically_normal();
		if (m_Root.empty() || relative.is_absolute() || relative.empty() || *relative.begin() != "Assets" || relative == "Assets" || path.find('\0') != std::string::npos || (!allowHash && path.find('#') != std::string::npos) || FileSystem::GetLowercaseExtension(relative) == ".meta")
		{
			outError = "asset path must name a file under Assets/ (not metadata or a # sub-asset key)";
			return false;
		}
		std::error_code error;
		// Never turn a requested directory entry into a different asset during move/delete.
		// Reject linked ancestors too, since a directory alias would have the same effect.
		std::filesystem::path entry;
		if (FileSystem::FindSymlink(m_Root, relative, entry, error))
		{
			outError = error ? "cannot inspect asset path '" + path + "': " + error.message() : "asset path must not contain symlinks: " + path;
			return false;
		}
		absolute = std::filesystem::weakly_canonical(m_Root / relative, error);
		if (!error)
		{
			const auto inside = absolute.lexically_relative(m_Root / "Assets");
			if (!inside.empty() && *inside.begin() != ".." && inside != ".")
				return true;
		}
		outError = "asset path escapes Assets/ or cannot be resolved: " + path;
		return false;
	}

	bool AssetRegistry::Refresh(std::string& outError)
	{
		if (m_Root.empty())
		{
			outError = "asset registry has no project directory";
			return false;
		}
		// An exported game's archive replaces the sidecars: its index holds each asset's UUID (unique, which Open checks).
		if (const Ref<PackArchive> archive = FileSystem::GetMountedArchive(m_Root))
		{
			std::map<uint64_t, std::string> paths;
			for (const PackArchive::Entry& entry : archive->GetEntries())
				if (entry.AssetID != 0 && entry.Path.starts_with("Assets/"))
					paths.emplace(entry.AssetID, entry.Path);
			SetIndex(std::move(paths), {}, {});
			return true;
		}
		std::map<uint64_t, std::string> paths, owners;
		std::map<std::string, std::string> problems;
		std::vector<std::string> orphans;
		std::error_code error;
		const auto assets = m_Root / "Assets";
		if (!std::filesystem::exists(assets, error) && !error)
		{
			SetIndex({}, {}, {});
			return true;
		}
		auto scanPath = assets;
		for (auto it = std::filesystem::recursive_directory_iterator(assets, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
		{
			scanPath = it->path();
			if (it->is_symlink(error))
			{
				if (FileSystem::GetLowercaseExtension(it->path()) == ".meta")
				{
					auto source = it->path();
					source.replace_extension();
					problems[FileSystem::PathToGenericUtf8(source.lexically_relative(m_Root))] = "metadata must be a regular file, not a symlink";
				}
				continue;
			}
			if (error)
				break;
			const bool regular = it->is_regular_file(error);
			if (error)
				break;
			if (!regular || FileSystem::GetLowercaseExtension(it->path()) != ".meta")
				continue;
			auto source = it->path();
			source.replace_extension();
			if (!std::filesystem::is_regular_file(source, error))
			{
				// Orphans cannot resolve to an asset; permission and traversal failures must still abort the scan.
				if (error == std::errc::no_such_file_or_directory)
					error.clear();
				if (error)
					break;
				orphans.push_back(FileSystem::PathToGenericUtf8(source.lexically_relative(m_Root)));
				continue;
			}
			const auto path = FileSystem::PathToGenericUtf8(source.lexically_relative(m_Root));
			std::filesystem::path checked;
			std::string problem;
			const auto text = FileSystem::ReadTextFile(it->path());
			const auto id = text ? ParseMeta(ParseJson(*text), problem) : std::nullopt;
			if (!id || !CheckPath(path, checked, problem))
			{
				problems[path] = text ? problem : "cannot read metadata";
				continue;
			}
			const auto [owner, first] = owners.emplace(*id, path);
			if (!first)
			{
				const auto message = "duplicate asset UUID " + std::to_string(*id) + " in '" + path + "' and '" + owner->second + "'";
				problems[path] = message;
				problems[owner->second] = message;
				paths.erase(*id);
			}
			else
				paths.emplace(*id, path);
		}
		if (error)
		{
			outError = "cannot scan asset metadata at '" + FileSystem::PathToUtf8(scanPath) + "': " + error.message();
			return false;
		}
		SetIndex(std::move(paths), std::move(problems), std::move(orphans));
		return true;
	}

	void AssetRegistry::SetIndex(std::map<uint64_t, std::string> paths, std::map<std::string, std::string> problems, std::vector<std::string> orphans)
	{
		m_Paths = std::move(paths);
		m_IDs.clear();
		for (const auto& [id, path] : m_Paths)
			m_IDs.emplace(path, id);
		m_Problems = std::move(problems);
		m_Orphans = std::move(orphans);
		m_Indexed = true;
	}

	std::optional<UUID> AssetRegistry::Ensure(const std::string& path, std::string& outError)
	{
		if (!Refresh(outError))
			return std::nullopt;
		return EnsureIndexed(path, outError);
	}

	std::optional<UUID> AssetRegistry::EnsureIndexed(const std::string& path, std::string& outError)
	{
		std::filesystem::path absolute;
		if (!CheckPath(path, absolute, outError))
			return std::nullopt;
		const auto normalized = FileSystem::PathToGenericUtf8(absolute.lexically_relative(m_Root));
		if (const auto problem = m_Problems.find(normalized); problem != m_Problems.end())
		{
			outError = normalized + ".meta: " + problem->second;
			return std::nullopt;
		}
		std::error_code error;
		if (!std::filesystem::is_regular_file(absolute, error))
		{
			outError = "asset does not exist: " + path;
			return std::nullopt;
		}
		if (const auto it = m_IDs.find(normalized); it != m_IDs.end())
			return UUID(it->second);
		auto meta = absolute;
		meta += ".meta";
		// Cooperating importers serialize on this lock; the existence check also detects other writers.
		auto lock = meta;
		lock += ".importing";
		if (!std::filesystem::create_directory(lock, error))
		{
			outError = "cannot acquire metadata import lock '" + FileSystem::PathToUtf8(lock) + "': " + (error ? error.message() : "another import is active (remove the lock only if that importer stopped)");
			return std::nullopt;
		}
		// Owns only this import's lock; the atomic file writer owns its temporary file.
		struct ImportLock
		{
			std::filesystem::path Path;
			~ImportLock()
			{
				std::error_code removeError;
				std::filesystem::remove(Path, removeError);
				if (removeError)
					BS_CORE_WARN("AssetRegistry: cannot release import lock '{}': {}", FileSystem::PathToUtf8(Path), removeError.message());
			}
		} guard{ lock };
		const auto status = std::filesystem::symlink_status(meta, error);
		if (status.type() != std::filesystem::file_type::not_found)
		{
			outError = "metadata appeared during import; refresh and retry: " + path;
			return std::nullopt;
		}
		if (error && error != std::errc::no_such_file_or_directory)
		{
			outError = "cannot inspect metadata: " + error.message();
			return std::nullopt;
		}
		UUID id;
		while (!id.IsValid() || m_Paths.contains(id))
			id = UUID();
		if (!FileSystem::WriteTextFile(meta, nlohmann::json{ { "Version", 1 }, { "UUID", std::to_string(id) } }.dump(1, '\t'), outError))
			return std::nullopt;
		m_WrittenPaths.push_back(normalized + ".meta");
		m_Paths.emplace(id, normalized);
		m_IDs.emplace(normalized, id);
		return id;
	}

	std::vector<std::string> AssetRegistry::TakeWrittenPaths()
	{
		return std::exchange(m_WrittenPaths, {});
	}

	std::optional<UUID> AssetRegistry::FindID(const std::string& path) const
	{
		const auto it = m_IDs.find(FileSystem::NormalizeGeneric(path));
		return it != m_IDs.end() ? std::optional<UUID>(UUID(it->second)) : std::nullopt;
	}

	std::string AssetRegistry::Find(UUID id) const
	{
		const auto it = m_Paths.find(id);
		return it == m_Paths.end() ? std::string{} : it->second;
	}

	bool AssetRegistry::ReferencesSource(const std::string& key, const std::string& source) const
	{
		std::filesystem::path asset, target;
		std::string ignored;
		return CheckPath(std::string(GetAssetSource(key)), asset, ignored) && CheckPath(source, target, ignored) && asset == target;
	}

	std::optional<std::pair<UUID, std::string>> AssetRegistry::ParseUnresolvedKey(std::string_view key)
	{
		if (!IsUnresolvedKey(key))
			return std::nullopt;
		const std::string_view encoded = key.substr(UnresolvedPrefix.size());
		const size_t split = encoded.find('/');
		std::string ignored;
		const auto id = split == std::string_view::npos ? std::nullopt : ParseID(encoded.substr(0, split), ignored);
		auto hint = split == std::string_view::npos ? std::nullopt : DecodeHint(encoded.substr(split + 1));
		if (!id || !hint)
			return std::nullopt;
		return std::pair{ *id, std::move(*hint) };
	}

	std::string AssetRegistry::DescribeUnresolved(UUID id, const std::string& hint) const
	{
		std::string description = "unresolved asset UUID " + std::to_string(static_cast<uint64_t>(id)) + " (hint: " + hint + ")";
		if (const auto problem = m_Problems.find(std::string(GetAssetSource(hint))); problem != m_Problems.end())
			description += ": " + problem->second;
		return description;
	}

	std::string AssetRegistry::DescribeUnresolved(std::string_view key) const
	{
		BS_CORE_ASSERT(IsUnresolvedKey(key), "DescribeUnresolved takes asset-uuid:// keys");
		const auto parsed = ParseUnresolvedKey(key);
		return parsed ? DescribeUnresolved(parsed->first, parsed->second) : "invalid opaque asset UUID reference '" + std::string(key) + "'";
	}

	bool AssetRegistry::Decode(const nlohmann::json& value, std::string& path, std::string& outError, bool preserveUnresolved) const
	{
		outError.clear();
		if (value.is_string())
		{
			path = value.get<std::string>();
			return true;
		}
		if (!value.is_object() || value.size() != 2 || !value.contains("UUID") || !value["UUID"].is_string() || !value.contains("Path") || !value["Path"].is_string())
		{
			outError = "asset reference must be a path string or {UUID, Path} strings";
			return false;
		}
		const auto id = ParseID(value["UUID"].get_ref<const std::string&>(), outError);
		if (!id)
			return false;
		path = Find(*id);
		if (path.empty())
		{
			const auto& hint = value["Path"].get_ref<const std::string&>();
			outError = DescribeUnresolved(*id, hint);
			if (!preserveUnresolved)
				return false;
			path = "asset-uuid://" + std::to_string(*id) + "/" + EncodeHint(hint);
			return true;
		}
		const auto& hint = value["Path"].get_ref<const std::string&>();
		path += GetSubAssetFragment(hint);
		return true;
	}

	bool AssetRegistry::Encode(const std::string& path, nlohmann::json& value, std::string& outError)
	{
		value = path;
		if (IsUnresolvedKey(path))
		{
			const auto parsed = ParseUnresolvedKey(path);
			if (!parsed)
			{
				outError = "invalid opaque asset UUID reference";
				return false;
			}
			value = { { "UUID", std::to_string(static_cast<uint64_t>(parsed->first)) }, { "Path", parsed->second } };
			return true;
		}
		if (path.empty() || path.starts_with("builtin://"))
			return true;
		const std::string source(GetAssetSource(path));
		std::filesystem::path absolute;
		std::string ignored;
		// Legacy references outside the registry's domain remain editable strings, including literal '#' names.
		if (!CheckPath(source, absolute, ignored))
			return true;
		std::error_code error;
		if (!std::filesystem::is_regular_file(absolute, error))
		{
			if (!error || error == std::errc::no_such_file_or_directory)
				return true;
			outError = "cannot inspect asset '" + path + "': " + error.message();
			return false;
		}
		if (!m_Indexed && !Refresh(outError))
			return false;
		const auto id = EnsureIndexed(source, outError);
		if (!id)
			return false;
		value = { { "UUID", std::to_string(*id) }, { "Path", path } };
		return true;
	}

	bool AssetRegistry::Remove(const std::string& path, std::string& outError)
	{
		std::filesystem::path source;
		if (!CheckPath(path, source, outError, true))
			return false;
		std::error_code error;
		if (!std::filesystem::is_regular_file(source, error))
		{
			outError = "asset source is not a regular file: " + path;
			return false;
		}
		auto meta = source;
		meta += ".meta";
		const auto status = std::filesystem::symlink_status(meta, error);
		const bool hasMeta = status.type() != std::filesystem::file_type::not_found;
		if (error && error != std::errc::no_such_file_or_directory)
		{
			outError = "cannot inspect metadata: " + error.message();
			return false;
		}
		const auto before = hasMeta ? FileSystem::ReadTextFile(meta) : std::optional<std::string>{};
		if (hasMeta && (!before || !std::filesystem::remove(meta, error)))
		{
			outError = "cannot remove metadata for '" + path + "': " + (error ? error.message() : "cannot read metadata");
			return false;
		}
		if (!std::filesystem::remove(source, error))
		{
			outError = "cannot remove asset '" + path + "': " + error.message();
			std::string restoreError;
			if (before && !FileSystem::WriteTextFile(meta, *before, restoreError))
				outError += "; cannot restore metadata: " + restoreError;
			return false;
		}
		const auto normalized = FileSystem::PathToGenericUtf8(source.lexically_relative(m_Root));
		if (auto it = m_IDs.find(normalized); it != m_IDs.end())
		{
			m_Paths.erase(it->second);
			m_IDs.erase(it);
		}
		m_Problems.erase(normalized);
		return true;
	}

	bool AssetRegistry::ValidateMove(const std::string& from, const std::string& to, std::string& outError) const
	{
		std::filesystem::path source, destination;
		if (!CheckPath(from, source, outError) || !CheckPath(to, destination, outError))
			return false;
		std::error_code error;
		if (!std::filesystem::is_regular_file(source, error))
		{
			outError = "asset source is not a regular file: " + from;
			return false;
		}
		auto targetMeta = destination;
		targetMeta += ".meta";
		for (const auto& target : { destination, targetMeta })
		{
			const auto status = std::filesystem::symlink_status(target, error);
			if (status.type() != std::filesystem::file_type::not_found || (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory))
			{
				outError = "asset destination already exists or cannot be inspected: " + to;
				return false;
			}
		}
		return true;
	}

	bool AssetRegistry::Move(const std::string& from, const std::string& to, std::string& outError)
	{
		if (!ValidateMove(from, to, outError))
			return false;
		const auto id = Ensure(from, outError);
		if (!id)
			return false;
		std::filesystem::path source, destination;
		if (!CheckPath(from, source, outError) || !CheckPath(to, destination, outError))
			return false;
		std::error_code error;
		std::filesystem::create_directories(destination.parent_path(), error);
		if (!error)
			std::filesystem::rename(source, destination, error);
		if (error)
		{
			outError = "cannot move asset: " + error.message();
			return false;
		}
		auto sourceMeta = source, targetMeta = destination;
		sourceMeta += ".meta";
		targetMeta += ".meta";
		std::filesystem::rename(sourceMeta, targetMeta, error);
		if (error)
		{
			outError = "cannot move asset metadata: " + error.message();
			std::filesystem::rename(destination, source, error);
			if (error)
				outError += "; cannot restore source: " + error.message();
			return false;
		}
		// Both renames committed. Publish this known change without a fallible scan after the commit point.
		const auto oldPath = m_Paths.at(*id);
		const auto newPath = FileSystem::PathToGenericUtf8(destination.lexically_relative(m_Root));
		m_IDs.erase(oldPath);
		m_IDs.emplace(newPath, *id);
		m_Paths[*id] = newPath;
		return true;
	}

}
