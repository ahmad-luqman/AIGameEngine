#pragma once

#include "Basalt/Core/Base.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Basalt {

	// An archive's totals: what PackArchiveWriter::Write reports for the file it wrote and PackArchive::GetStatistics for
	// an opened one.
	struct PackArchiveStatistics
	{
		uint32_t Entries = 0;
		// Entries kept zstd-compressed.
		uint32_t CompressedEntries = 0;
		// Total uncompressed bytes of all entries.
		uint64_t UncompressedSize = 0;
		// The archive file's size.
		uint64_t ArchiveSize = 0;
	};

	// A read-only .bpak archive: an exported game's Assets in one file (Docs/Shipping.md describes the format). Each
	// entry is a file under a '/'-separated relative path, stored as is or zstd-compressed, with the asset UUID of its
	// .meta sidecar, so an archive replaces both the files and their sidecars. Opening reads and checks the whole index;
	// Read checks an entry's bytes against its hash. Archives are untrusted input: every size and offset is checked
	// before anything is allocated for it. Read and ReadStored are thread-safe (asset workers load in parallel).
	class PackArchive
	{
		// Only Open and OpenMemory make archives (CreateRef needs a public constructor): an archive is always parsed.
		struct Token
		{
			explicit Token() = default;
		};

	public:
		enum class Method : uint8_t
		{
			Store = 0,
			Zstd = 1
		};

		// One file, as the index lists it. Pass Read and ReadStored the reference GetEntries, Find or FindAsset gave, never a
		// copy: they refuse anything else (another archive's entry, an edited copy).
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
		// Game assets stay far below this; the cap bounds what a forged size can make Read allocate.
		static constexpr uint64_t MaxEntrySize = uint64_t{ 4 } << 30;

		explicit PackArchive(Token) {}
		PackArchive(const PackArchive&) = delete;
		PackArchive& operator=(const PackArchive&) = delete;

		static Ref<PackArchive> Open(const std::filesystem::path& file, std::string& outError);
		// An archive held in memory (tests, fuzzing).
		static Ref<PackArchive> OpenMemory(std::vector<uint8_t> bytes, std::string& outError);

		const std::vector<Entry>& GetEntries() const { return m_Entries; }
		// The entry at a relative path, matched exactly (letter case included, as on Linux); nullptr when absent.
		const Entry* Find(std::string_view path) const;
		const Entry* FindAsset(uint64_t assetID) const;
		// Whether some entry lies under the folder path ("Assets/Models" for "Assets/Models/Ship.glb").
		bool HasFolder(std::string_view path) const;
		// The uncompressed bytes of one of this archive's entries (from GetEntries, Find or FindAsset), checked against its
		// size and hash.
		std::optional<std::vector<uint8_t>> Read(const Entry& entry, std::string& outError) const;
		// Copies up to size bytes of a stored entry of this archive (Method::Store, from GetEntries, Find or FindAsset),
		// starting at offset within it, without touching the rest: a streaming reader's Read. Returns how many bytes it
		// copied, fewer only at the entry's end. nullopt with outError for a compressed or foreign entry or a failed read.
		// A part of an entry cannot be checked against its hash: streamed bytes stay inside the entry but may be damaged.
		std::optional<size_t> ReadStored(const Entry& entry, uint64_t offset, void* destination, size_t size, std::string& outError) const;
		// The file Open read; empty for OpenMemory.
		const std::filesystem::path& GetFile() const { return m_File; }
		PackArchiveStatistics GetStatistics() const;

		// Whether a path may name an entry: relative, components separated by single '/', none empty, "." or "..", no NUL,
		// backslash or colon (which would name another file or a drive on some platform), at most MaxPathLength bytes.
		static bool IsValidPath(std::string_view path);

	private:
		bool ReadRange(uint64_t offset, uint64_t size, uint8_t* destination) const;
		// Whether entry is an element of m_Entries, the only entries Parse checked.
		bool Owns(const Entry& entry) const;
		// Read after the ownership check; may throw std::bad_alloc, which Read turns into an error.
		std::optional<std::vector<uint8_t>> ReadChecked(const Entry& entry, std::string& outError) const;
		bool Parse(uint64_t fileSize, std::string& outError);

		// Open's file; empty for an archive in memory (m_Memory), which needs no stream.
		std::filesystem::path m_File;
		mutable std::mutex m_StreamMutex;
		mutable std::ifstream m_Stream;
		std::vector<uint8_t> m_Memory;
		std::vector<Entry> m_Entries;
		// The archive's size in bytes, as Parse checked it.
		uint64_t m_ArchiveSize = 0;
		// Indices into m_Entries sorted by AssetID (nonzero IDs only).
		std::vector<uint32_t> m_ByAsset;
	};

	// Builds a .bpak file. Entries may be added in any order and are written sorted by path. Write compresses each
	// entry with zstd and keeps the compressed form only when it is smaller, so files zstd cannot shrink (PNG, JPEG) are
	// stored as they are, unless AddFile is told to store it anyway. The output is deterministic: the same entries, level
	// and zstd version always give the same bytes.
	class PackArchiveWriter
	{
	public:
		using Statistics = PackArchiveStatistics;

		// How many uncompressed bytes and entries Write takes on at once. Each job holds its compressed form too, so the
		// peak is about twice the bytes. Only a single larger entry passes them, and 0 bytes gives every entry its own batch.
		static constexpr uint64_t DefaultBatchBytes = uint64_t{ 256 } * 1024 * 1024;
		static constexpr size_t MaxBatchEntries = 256;

		// AddFile reads source when Write runs; AddData takes the bytes now. Both return false (outError set) for an
		// invalid or duplicate path, a duplicate nonzero asset UUID, more than MaxEntries files, (AddData) more than
		// MaxEntrySize bytes, or (AddFile) an empty source path (a missing file fails Write). With storeUncompressed the file
		// stays readable in part (PackArchive::ReadStored), however little zstd could save.
		bool AddFile(std::string path, std::filesystem::path source, uint64_t assetID, std::string& outError, bool storeUncompressed = false);
		bool AddData(std::string path, std::vector<uint8_t> data, uint64_t assetID, std::string& outError);
		// Writes the archive atomically (a temporary sibling renamed over the target), at a zstd level clamped to
		// 1..ZSTD_maxCLevel() (22). Entries are read and compressed on the job system a batch at a time (batchBytes) and
		// written in path order, so the bytes do not depend on the batches or the number of workers. The added entries
		// stay, so Write may run again (a retry after a full disk).
		std::optional<Statistics> Write(const std::filesystem::path& file, std::string& outError, int compressionLevel = 9, uint64_t batchBytes = DefaultBatchBytes) const;
		// Where the batch starting at begin ends, for entries of these sizes in write order.
		static size_t FindBatchEnd(std::span<const uint64_t> sizes, size_t begin, uint64_t batchBytes);

	private:
		struct Pending
		{
			std::string Path;
			// Read when Write runs; empty for AddData.
			std::filesystem::path Source;
			std::vector<uint8_t> Data;
			uint64_t AssetID = 0;
			bool StoreUncompressed = false;
		};
		bool AddPending(Pending pending, std::string& outError);

		std::vector<Pending> m_Pending;
		std::unordered_set<std::string> m_Paths;
		std::unordered_set<uint64_t> m_AssetIDs;
	};

}
