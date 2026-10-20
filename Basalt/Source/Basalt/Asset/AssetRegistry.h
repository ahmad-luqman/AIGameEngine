#pragma once

#include "Basalt/Core/UUID.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Basalt {

	// Project-local identity index. Sidecars travel with assets; paths are hints, never replacement identities.
	// Main-thread only. Refresh is read-only; Ensure, Encode and Move may create metadata for existing assets.
	class AssetRegistry
	{
	public:
		explicit AssetRegistry(std::filesystem::path root = {});
		static std::optional<UUID> ParseID(std::string_view text, std::string& outError);
		static std::optional<UUID> ParseMeta(const nlohmann::json& data, std::string& outError);
		// Returns false on an incomplete scan, preserving the previous index. Invalid sidecars are
		// excluded and recorded per asset; unrelated valid assets remain available.
		bool Refresh(std::string& outError);
		const std::map<std::string, std::string>& GetProblems() const { return m_Problems; }
		const std::vector<std::string>& GetOrphans() const { return m_Orphans; }
		std::optional<UUID> Ensure(const std::string& path, std::string& outError);
		std::string Find(UUID id) const;
		// Newly created project-relative sidecar paths, drained by hosts to acknowledge their own writes.
		std::vector<std::string> TakeWrittenPaths();
		// Matches canonical source paths, including legacy ./ paths and sub-asset (embedded image, imported material) references.
		bool ReferencesSource(const std::string& key, const std::string& source) const;
		// Converts a legacy path or UUID/path object to a path. Unknown IDs fail even if the hint exists.
		// preserveUnresolved retains a valid missing identity as an opaque asset-uuid:// key, never its hint.
		bool Decode(const nlohmann::json& value, std::string& path, std::string& outError, bool preserveUnresolved = false) const;
		static constexpr std::string_view UnresolvedPrefix = "asset-uuid://";
		static bool IsUnresolvedKey(std::string_view key) { return key.starts_with(UnresolvedPrefix); }
		// "unresolved asset UUID <id> (was '<path hint>')" for an unresolved key, for messages.
		static std::string DescribeUnresolved(std::string_view key);
		// Uses the last scan; batch writers refresh once before encoding their references. May create sidecars.
		bool Encode(const std::string& path, nlohmann::json& value, std::string& outError);
		// Deletes one source and its identity together; never leaves a reusable orphan on success.
		bool Remove(const std::string& path, std::string& outError);
		bool ValidateMove(const std::string& from, const std::string& to, std::string& outError) const;
		bool Move(const std::string& from, const std::string& to, std::string& outError);

	private:
		std::optional<UUID> EnsureIndexed(const std::string& path, std::string& outError);
		bool CheckPath(const std::string& path, std::filesystem::path& absolute, std::string& outError, bool allowHash = false) const;
		std::filesystem::path m_Root;
		std::map<uint64_t, std::string> m_Paths;
		std::map<std::string, uint64_t> m_IDs;
		std::vector<std::string> m_WrittenPaths;
		std::map<std::string, std::string> m_Problems;
		std::vector<std::string> m_Orphans;
		bool m_Indexed = false;
	};

}
