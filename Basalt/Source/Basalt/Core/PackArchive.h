#pragma once

#include "Basalt/Core/Base.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Basalt {

	// A read-only .bpak archive: an exported game's Assets in one file (Docs/Shipping.md describes the format). Each
	// entry is a file under a '/'-separated relative path, stored as is or zstd-compressed, with the asset UUID of its
	// .meta sidecar, so an archive replaces both the files and their sidecars. Opening reads and checks the whole index;
	// Read checks an entry's bytes against its hash. Archives are untrusted input: every size and offset is checked
	// before anything is allocated for it. Read is thread-safe (asset workers load in parallel).
	class PackArchive
	{
	public:
		enum class Method : uint8_t
		{
			Store = 0,
			Zstd = 1
		};

		struct Entry
		{
			// Relative, normalized and '/'-separated ("Assets/Models/Ship.glb"); entries are sorted by it (byte order).
			std::string Path;
			// The .meta UUID; 0 for files without one.
			uint64_t AssetID = 0;
			uint64_t Offset = 0;
			uint64_t StoredSize = 0;
			uint64_t Size = 0;
			Method Compression = Method::Store;
			// HashBytes64 of the uncompressed bytes.
			uint64_t Hash = 0;
		};

		static constexpr uint8_t Magic[4] = { 'B', 'P', 'A', 'K' };
		static constexpr uint32_t FormatVersion = 1;
		static constexpr size_t HeaderSize = 40;
		static constexpr uint32_t MaxEntries = 1u << 20;
		static constexpr size_t MaxPathLength = 1024;
		// Larger files would not load anyway (glTF buffers, textures and audio clips stay far below), and the cap bounds
		// what a forged size can make Read allocate.
		static constexpr uint64_t MaxEntrySize = uint64_t{ 4 } << 30;

		static Ref<PackArchive> Open(const std::filesystem::path& file, std::string& outError);
		// An archive held in memory (tests, fuzzing).
		static Ref<PackArchive> OpenMemory(std::vector<uint8_t> bytes, std::string& outError);

		const std::vector<Entry>& GetEntries() const { return m_Entries; }
		// The entry at a relative path, matched exactly (letter case included, as on Linux); nullptr when absent.
		const Entry* Find(std::string_view path) const;
		const Entry* FindAsset(uint64_t assetID) const;
		// Whether some entry lies under the folder path ("Assets/Models" for "Assets/Models/Ship.glb").
		bool HasFolder(std::string_view path) const;
		// The entry's uncompressed bytes, checked against its size and hash.
		std::optional<std::vector<uint8_t>> Read(const Entry& entry, std::string& outError) const;
		// The file Open read; empty for OpenMemory.
		const std::filesystem::path& GetFile() const { return m_File; }

	private:
		bool ReadRange(uint64_t offset, uint64_t size, uint8_t* destination) const;
		bool Parse(uint64_t fileSize, std::string& outError);

		std::filesystem::path m_File;
		mutable std::mutex m_StreamMutex;
		mutable std::ifstream m_Stream;
		std::vector<uint8_t> m_Memory;
		std::vector<Entry> m_Entries;
		// Indices into m_Entries sorted by AssetID (nonzero IDs only).
		std::vector<uint32_t> m_ByAsset;
	};

	// Builds a .bpak file. Entries may be added in any order and are written sorted by path. Write compresses each
	// entry with zstd and keeps the compressed form only when it is smaller, so already compressed files (KTX2, PNG,
	// Ogg) are stored as they are. The output is deterministic: the same entries always give the same bytes.
	class PackArchiveWriter
	{
	public:
		struct Statistics
		{
			uint32_t Entries = 0;
			uint32_t Compressed = 0;
			uint64_t Size = 0;
			// The archive file's size.
			uint64_t ArchiveSize = 0;
		};

		// Adds a file read from disk when Write runs. False (outError set) for an invalid or duplicate path.
		bool AddFile(std::string path, std::filesystem::path source, uint64_t assetID, std::string& outError);
		bool AddData(std::string path, std::vector<uint8_t> data, uint64_t assetID, std::string& outError);
		// Writes the archive atomically (a temporary sibling renamed over the target). zstd level 1-19.
		bool Write(const std::filesystem::path& file, std::string& outError, int compressionLevel = 9);
		const Statistics& GetStatistics() const { return m_Statistics; }

		// The rules PackArchive applies to entry paths, for callers that want to check a path before adding it.
		static bool IsValidPath(std::string_view path);

	private:
		struct Pending
		{
			std::string Path;
			std::filesystem::path Source;
			std::vector<uint8_t> Data;
			uint64_t AssetID = 0;
		};
		bool AddPending(Pending pending, std::string& outError);

		std::vector<Pending> m_Pending;
		Statistics m_Statistics;
	};

}
