#include "Breccia/Core/PackArchive.h"

#include "Breccia/Core/Assert.h"
#include "Breccia/Core/FileSystem.h"
#include "Breccia/Core/Hash.h"
#include "Breccia/Core/JobSystem.h"
#include "Breccia/Core/Log.h"

#include <zstd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <functional>
#include <exception>
#include <new>
#include <numeric>
#include <span>
#include <system_error>

namespace Breccia {

	namespace {

		// Path length, asset ID, offset, stored size, size, method, hash.
		constexpr size_t EntryFixedSize = 4 + 8 + 8 + 8 + 8 + 1 + 8;
		// ZSTD_BLOCKSIZE_MAX, which zstd.h only defines with ZSTD_STATIC_LINKING_ONLY.
		constexpr uint64_t MaxZstdBlockSize = uint64_t{ 128 } * 1024;

		void PutU32(std::vector<uint8_t>& out, uint32_t value)
		{
			for (int i = 0; i < 4; i++)
				out.push_back(static_cast<uint8_t>(value >> (8 * i)));
		}

		void PutU64(std::vector<uint8_t>& out, uint64_t value)
		{
			for (int i = 0; i < 8; i++)
				out.push_back(static_cast<uint8_t>(value >> (8 * i)));
		}

		// Little-endian reads that advance through a buffer whose remaining length the caller has checked.
		class ByteReader
		{
		public:
			explicit ByteReader(const uint8_t* data)
				: m_Data(data)
			{
			}

			uint8_t U8() { return *m_Data++; }
			uint32_t U32() { return static_cast<uint32_t>(Read(4)); }
			uint64_t U64() { return Read(8); }
			std::string Text(size_t length)
			{
				std::string text(reinterpret_cast<const char*>(m_Data), length);
				m_Data += length;
				return text;
			}

		private:
			uint64_t Read(int bytes)
			{
				uint64_t value = 0;
				for (int i = 0; i < bytes; i++)
					value |= static_cast<uint64_t>(m_Data[i]) << (8 * i);
				m_Data += bytes;
				return value;
			}

			const uint8_t* m_Data;
		};

		std::vector<uint8_t> BuildHeader(uint32_t entryCount, uint64_t indexOffset, uint64_t indexSize, uint64_t indexHash)
		{
			std::vector<uint8_t> header(std::begin(PackArchive::Magic), std::end(PackArchive::Magic));
			PutU32(header, PackArchive::FormatVersion);
			PutU32(header, 0);
			PutU32(header, entryCount);
			PutU64(header, indexOffset);
			PutU64(header, indexSize);
			PutU64(header, indexHash);
			BR_CORE_ASSERT(header.size() == PackArchive::HeaderSize, "the header layout must match HeaderSize");
			return header;
		}

		// A source file for the writer, from disk: its size is checked against MaxEntrySize before anything is allocated, and
		// at most that many bytes are read (a file growing meanwhile cannot make the read larger).
		std::optional<std::vector<uint8_t>> ReadSource(const std::filesystem::path& source, std::string& outError)
		{
			std::error_code error;
			const bool regular = std::filesystem::is_regular_file(source, error);
			const uint64_t size = regular && !error ? std::filesystem::file_size(source, error) : 0;
			if (error || !regular)
			{
				outError = error ? FileSystem::DescribeError(error) : "not a file";
				return std::nullopt;
			}
			if (size > PackArchive::MaxEntrySize)
			{
				outError = "larger than an archive entry may be (" + std::to_string(PackArchive::MaxEntrySize) + " bytes)";
				return std::nullopt;
			}
			std::ifstream stream(source, std::ios::in | std::ios::binary);
			std::vector<uint8_t> data(static_cast<size_t>(size));
			if (!stream || !stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
			{
				outError = "reading it failed";
				return std::nullopt;
			}
			return data;
		}

		std::string DescribeErrno()
		{
			const int reason = errno;
			return reason != 0 ? ": " + std::generic_category().message(reason) : std::string();
		}

		// An entry ready to write, made on a job.
		struct PreparedEntry
		{
			// The bytes to store, unless they are the writer's own (AddData) as they stand.
			std::optional<std::vector<uint8_t>> Owned;
			uint64_t Size = 0;
			uint64_t Hash = 0;
			PackArchive::Method Compression = PackArchive::Method::Store;

			std::span<const uint8_t> GetBytes(const std::vector<uint8_t>& pendingData) const
			{
				return Owned ? std::span<const uint8_t>(*Owned) : std::span<const uint8_t>(pendingData);
			}
		};

		// What a job left: an entry or why not. Neither means the job never ran, or even reporting its failure failed.
		struct PrepareSlot
		{
			std::optional<PreparedEntry> Entry;
			std::string Error;
		};

		// Reads source, or takes pendingData when source is empty, and compresses it unless told to store it. A compressor
		// failure stops the export rather than storing the file: the bytes would then depend on memory at that moment.
		std::optional<PreparedEntry> PrepareEntry(const std::string& path, const std::filesystem::path& source, const std::vector<uint8_t>& pendingData, PackArchiveWriter::Storage storage, int level, std::string& outError)
		{
			std::optional<std::vector<uint8_t>> read;
			if (!source.empty())
			{
				std::string readError;
				read = ReadSource(source, readError);
				if (!read)
				{
					outError = "cannot read '" + FileSystem::PathToUtf8(source) + "' (" + readError + ")";
					return std::nullopt;
				}
			}
			const std::vector<uint8_t>& data = read ? *read : pendingData;
			PreparedEntry entry;
			entry.Size = data.size();
			entry.Hash = HashBytes64(data.data(), data.size());
			if (storage == PackArchiveWriter::Storage::Smallest)
			{
				std::vector<uint8_t> compressed(ZSTD_compressBound(data.size()));
				const size_t compressedSize = ZSTD_compress(compressed.data(), compressed.size(), data.data(), data.size(), level);
				if (ZSTD_isError(compressedSize))
				{
					outError = "cannot compress '" + path + "' (" + ZSTD_getErrorName(compressedSize) + ")";
					return std::nullopt;
				}
				if (compressedSize < data.size())
				{
					compressed.resize(compressedSize);
					entry.Owned = std::move(compressed);
					entry.Compression = PackArchive::Method::Zstd;
					return entry;
				}
			}
			entry.Owned = std::move(read);
			return entry;
		}

	}

	Ref<PackArchive> PackArchive::Open(const std::filesystem::path& file, std::string& outError)
	{
		const std::string name = FileSystem::PathToUtf8(file);
		std::error_code error;
		// Only regular files: a FIFO would block, and a directory opens as a stream on Linux.
		const bool regular = std::filesystem::is_regular_file(file, error);
		const uint64_t size = regular && !error ? std::filesystem::file_size(file, error) : 0;
		if (!regular || error)
		{
			outError = "cannot read the archive '" + name + "'" + (error ? ": " + FileSystem::DescribeError(error) : std::string(": not a file"));
			return nullptr;
		}
		auto archive = CreateRef<PackArchive>(Token{});
		archive->m_File = file;
		errno = 0;
		archive->m_Stream.open(file, std::ios::in | std::ios::binary);
		if (!archive->m_Stream)
		{
			outError = "cannot open the archive '" + name + "'" + DescribeErrno();
			return nullptr;
		}
		if (!archive->Parse(size, outError))
		{
			outError = "'" + name + "': " + outError;
			return nullptr;
		}
		return archive;
	}

	Ref<PackArchive> PackArchive::OpenMemory(std::vector<uint8_t> bytes, std::string& outError)
	{
		auto archive = CreateRef<PackArchive>(Token{});
		archive->m_Memory = std::move(bytes);
		if (!archive->Parse(archive->m_Memory.size(), outError))
			return nullptr;
		return archive;
	}

	bool PackArchive::ReadRange(uint64_t offset, uint64_t size, uint8_t* destination) const
	{
		// An empty entry's buffer may be null, which memcpy and read must not see.
		if (size == 0)
			return true;
		if (m_File.empty())
		{
			if (offset > m_Memory.size() || size > m_Memory.size() - offset)
				return false;
			std::memcpy(destination, m_Memory.data() + offset, static_cast<size_t>(size));
			return true;
		}
		std::scoped_lock lock(m_StreamMutex);
		m_Stream.clear();
		m_Stream.seekg(static_cast<std::streamoff>(offset));
		m_Stream.read(reinterpret_cast<char*>(destination), static_cast<std::streamsize>(size));
		return m_Stream.good() || (m_Stream.eof() && static_cast<uint64_t>(m_Stream.gcount()) == size);
	}

	bool PackArchive::Parse(uint64_t fileSize, std::string& outError)
	{
		if (fileSize < HeaderSize)
		{
			outError = "not a .bpak archive (shorter than its header)";
			return false;
		}
		uint8_t header[HeaderSize];
		if (!ReadRange(0, HeaderSize, header))
		{
			outError = "cannot read the archive header";
			return false;
		}
		if (std::memcmp(header, Magic, sizeof(Magic)) != 0)
		{
			outError = "not a .bpak archive";
			return false;
		}
		ByteReader fields(header + sizeof(Magic));
		const uint32_t version = fields.U32();
		const uint32_t flags = fields.U32();
		const uint32_t count = fields.U32();
		const uint64_t indexOffset = fields.U64();
		const uint64_t indexSize = fields.U64();
		const uint64_t indexHash = fields.U64();
		if (version != FormatVersion)
		{
			outError = "archive format version " + std::to_string(version) + " is not supported (this Breccia reads version " + std::to_string(FormatVersion) + ")";
			return false;
		}
		if (flags != 0)
		{
			outError = "the archive uses features this Breccia does not know (nonzero header flags)";
			return false;
		}
		if (count > MaxEntries)
		{
			outError = "the archive lists " + std::to_string(count) + " entries (at most " + std::to_string(MaxEntries) + ")";
			return false;
		}
		// The index ends the file, and each entry takes between EntryFixedSize + 1 (a one-byte path) and
		// EntryFixedSize + MaxPathLength bytes.
		if (indexOffset < HeaderSize || indexOffset > fileSize || indexSize != fileSize - indexOffset)
		{
			outError = "the archive's index does not end the file (truncated or damaged)";
			return false;
		}
		if (indexSize < uint64_t{ count } * (EntryFixedSize + 1) || indexSize > uint64_t{ count } * (EntryFixedSize + MaxPathLength))
		{
			outError = "the archive's index size does not match its " + std::to_string(count) + " entries";
			return false;
		}
		std::vector<uint8_t> index(static_cast<size_t>(indexSize));
		if (!ReadRange(indexOffset, indexSize, index.data()))
		{
			outError = "cannot read the archive's index";
			return false;
		}
		if (HashBytes64(index.data(), index.size()) != indexHash)
		{
			outError = "the archive's index is damaged (hash mismatch)";
			return false;
		}

		std::vector<Entry> entries;
		entries.reserve(count);
		size_t cursor = 0;
		for (uint32_t i = 0; i < count; i++)
		{
			const auto fail = [&](const std::string& problem) {
				outError = "archive entry " + std::to_string(i) + ": " + problem;
				return false;
			};
			if (index.size() - cursor < EntryFixedSize)
				return fail("the index ends inside it");
			ByteReader reader(index.data() + cursor);
			const uint32_t pathLength = reader.U32();
			if (pathLength == 0 || pathLength > MaxPathLength || index.size() - cursor < EntryFixedSize + pathLength)
				return fail("invalid path length " + std::to_string(pathLength));
			cursor += EntryFixedSize + pathLength;
			Entry entry;
			entry.Path = reader.Text(pathLength);
			entry.AssetID = reader.U64();
			entry.Offset = reader.U64();
			entry.StoredSize = reader.U64();
			entry.Size = reader.U64();
			const uint8_t method = reader.U8();
			entry.Hash = reader.U64();

			if (!IsValidPath(entry.Path))
				return fail("invalid path '" + entry.Path + "'");
			// Sorted and unique, so lookups can binary search and a path never names two files.
			if (!entries.empty() && !(entries.back().Path < entry.Path))
				return fail("'" + entry.Path + "' is out of order or listed twice");
			if (method > static_cast<uint8_t>(Method::Zstd))
				return fail("'" + entry.Path + "' uses unknown compression method " + std::to_string(method));
			entry.Compression = static_cast<Method>(method);
			if (entry.Size > MaxEntrySize)
				return fail("'" + entry.Path + "' is larger than " + std::to_string(MaxEntrySize) + " bytes");
			if (entry.Offset < HeaderSize || entry.Offset > indexOffset || entry.StoredSize > indexOffset - entry.Offset)
				return fail("'" + entry.Path + "' lies outside the archive's data");
			const bool sizeMatches = entry.Compression == Method::Store ? entry.StoredSize == entry.Size : entry.StoredSize > 0 && entry.StoredSize <= ZSTD_compressBound(static_cast<size_t>(entry.Size));
			if (!sizeMatches)
				return fail("'" + entry.Path + "' has inconsistent sizes");
			// Every zstd block takes at least a 3-byte header and yields at most 128 KiB, so a few stored bytes cannot claim
			// gigabytes: Read allocates Size bytes before decompressing.
			if (entry.Compression == Method::Zstd && entry.Size > (entry.StoredSize / 3 + 1) * MaxZstdBlockSize)
				return fail("'" + entry.Path + "' claims more data than its compressed bytes can hold");
			entries.push_back(std::move(entry));
		}
		if (cursor != index.size())
		{
			outError = "the archive's index has " + std::to_string(index.size() - cursor) + " unexpected trailing bytes";
			return false;
		}

		std::vector<uint32_t> byAsset;
		for (uint32_t i = 0; i < entries.size(); i++)
			if (entries[i].AssetID != 0)
				byAsset.push_back(i);
		std::sort(byAsset.begin(), byAsset.end(), [&](uint32_t a, uint32_t b) { return entries[a].AssetID < entries[b].AssetID; });
		for (size_t i = 1; i < byAsset.size(); i++)
		{
			if (entries[byAsset[i]].AssetID == entries[byAsset[i - 1]].AssetID)
			{
				outError = "asset UUID " + std::to_string(entries[byAsset[i]].AssetID) + " belongs to both '" + entries[byAsset[i - 1]].Path + "' and '" + entries[byAsset[i]].Path + "'";
				return false;
			}
		}
		m_Entries = std::move(entries);
		m_ByAsset = std::move(byAsset);
		m_ArchiveSize = fileSize;
		return true;
	}

	PackArchiveStatistics PackArchive::GetStatistics() const
	{
		PackArchiveStatistics statistics;
		statistics.Entries = static_cast<uint32_t>(m_Entries.size());
		for (const Entry& entry : m_Entries)
		{
			statistics.CompressedEntries += entry.Compression == Method::Zstd ? 1 : 0;
			statistics.UncompressedSize += entry.Size;
		}
		statistics.ArchiveSize = m_ArchiveSize;
		return statistics;
	}

	const PackArchive::Entry* PackArchive::Find(std::string_view path) const
	{
		const auto it = std::lower_bound(m_Entries.begin(), m_Entries.end(), path, [](const Entry& entry, std::string_view value) { return entry.Path < value; });
		return it != m_Entries.end() && it->Path == path ? &*it : nullptr;
	}

	bool PackArchive::HasFolder(std::string_view path) const
	{
		if (path.empty())
			return false;
		const std::string prefix = std::string(path) + "/";
		const auto it = std::lower_bound(m_Entries.begin(), m_Entries.end(), prefix, [](const Entry& entry, const std::string& value) { return entry.Path < value; });
		return it != m_Entries.end() && it->Path.starts_with(prefix);
	}

	const PackArchive::Entry* PackArchive::FindAsset(uint64_t assetID) const
	{
		if (assetID == 0)
			return nullptr;
		const auto it = std::lower_bound(m_ByAsset.begin(), m_ByAsset.end(), assetID, [&](uint32_t index, uint64_t value) { return m_Entries[index].AssetID < value; });
		return it != m_ByAsset.end() && m_Entries[*it].AssetID == assetID ? &m_Entries[*it] : nullptr;
	}

	bool PackArchive::Owns(const Entry& entry) const
	{
		const Entry* first = m_Entries.data();
		return !std::less<const Entry*>()(&entry, first) && std::less<const Entry*>()(&entry, first + m_Entries.size());
	}

	std::optional<size_t> PackArchive::ReadStored(const Entry& entry, uint64_t offset, void* destination, size_t size, std::string& outError) const
	{
		// Only entries Parse checked: the offset and size of any other Entry were never bounded.
		if (!Owns(entry))
		{
			outError = "'" + entry.Path + "' is not an entry of this archive";
			return std::nullopt;
		}
		if (entry.Compression != Method::Store)
		{
			outError = "'" + entry.Path + "' is compressed, so it cannot be read in part";
			return std::nullopt;
		}
		const size_t count = offset < entry.StoredSize ? static_cast<size_t>(std::min<uint64_t>(size, entry.StoredSize - offset)) : 0;
		if (!ReadRange(entry.Offset + offset, count, static_cast<uint8_t*>(destination)))
		{
			outError = "cannot read '" + entry.Path + "' from the archive";
			return std::nullopt;
		}
		return count;
	}

	std::optional<std::vector<uint8_t>> PackArchive::Read(const Entry& entry, std::string& outError) const
	{
		// Only entries Parse checked: the sizes of any other Entry were never bounded, so allocating for them is unsafe.
		if (!Owns(entry))
		{
			outError = "'" + entry.Path + "' is not an entry of this archive";
			return std::nullopt;
		}
		try
		{
			return ReadChecked(entry, outError);
		}
		catch (const std::bad_alloc&)
		{
			outError = "'" + entry.Path + "' needs " + std::to_string(entry.Size) + " bytes, which cannot be allocated";
			return std::nullopt;
		}
	}

	std::optional<std::vector<uint8_t>> PackArchive::ReadChecked(const Entry& entry, std::string& outError) const
	{
		// A stored entry's StoredSize equals its Size (Parse checks it), so this reads the whole file either way.
		std::vector<uint8_t> stored(static_cast<size_t>(entry.StoredSize));
		if (!ReadRange(entry.Offset, entry.StoredSize, stored.data()))
		{
			outError = "cannot read '" + entry.Path + "' from the archive";
			return std::nullopt;
		}
		std::vector<uint8_t> data;
		if (entry.Compression == Method::Store)
			data = std::move(stored);
		else
		{
			// One complete frame that declares exactly the indexed size, checked before allocating for it.
			const unsigned long long declared = ZSTD_getFrameContentSize(stored.data(), stored.size());
			if (declared != entry.Size || ZSTD_findFrameCompressedSize(stored.data(), stored.size()) != stored.size())
			{
				outError = "'" + entry.Path + "' is damaged in the archive (its compressed data does not match the index)";
				return std::nullopt;
			}
			data.resize(static_cast<size_t>(entry.Size));
			const size_t written = ZSTD_decompress(data.data(), data.size(), stored.data(), stored.size());
			if (ZSTD_isError(written) || written != data.size())
			{
				outError = "'" + entry.Path + "' is damaged in the archive" + (ZSTD_isError(written) ? std::string(" (") + ZSTD_getErrorName(written) + ")" : std::string());
				return std::nullopt;
			}
		}
		if (HashBytes64(data.data(), data.size()) != entry.Hash)
		{
			outError = "'" + entry.Path + "' is damaged in the archive (hash mismatch)";
			return std::nullopt;
		}
		return data;
	}

	bool PackArchive::IsValidPath(std::string_view path)
	{
		if (path.empty() || path.size() > MaxPathLength)
			return false;
		size_t start = 0;
		while (start <= path.size())
		{
			const size_t end = std::min(path.find('/', start), path.size());
			const std::string_view component = path.substr(start, end - start);
			if (component.empty() || component == "." || component == "..")
				return false;
			start = end + 1;
		}
		return path.find_first_of(std::string_view("\0\\:", 3)) == std::string_view::npos;
	}

	bool PackArchiveWriter::AddPending(Pending pending, std::string& outError)
	{
		if (!PackArchive::IsValidPath(pending.Path))
		{
			outError = "invalid archive path '" + pending.Path + "'";
			return false;
		}
		if (m_Paths.contains(pending.Path))
		{
			outError = "'" + pending.Path + "' is already in the archive";
			return false;
		}
		if (pending.AssetID != 0 && m_AssetIDs.contains(pending.AssetID))
		{
			outError = "asset UUID " + std::to_string(pending.AssetID) + " is already in the archive";
			return false;
		}
		if (m_Pending.size() >= PackArchive::MaxEntries)
		{
			outError = "an archive holds at most " + std::to_string(PackArchive::MaxEntries) + " files";
			return false;
		}
		m_Paths.insert(pending.Path);
		if (pending.AssetID != 0)
			m_AssetIDs.insert(pending.AssetID);
		m_Pending.push_back(std::move(pending));
		return true;
	}

	bool PackArchiveWriter::AddFile(std::string path, std::filesystem::path source, uint64_t assetID, std::string& outError, Storage storage)
	{
		if (source.empty())
		{
			outError = "no source file for '" + path + "'";
			return false;
		}
		return AddPending({ std::move(path), std::move(source), {}, assetID, storage }, outError);
	}

	bool PackArchiveWriter::AddData(std::string path, std::vector<uint8_t> data, uint64_t assetID, std::string& outError)
	{
		if (data.size() > PackArchive::MaxEntrySize)
		{
			outError = "'" + path + "' is larger than an archive entry may be (" + std::to_string(PackArchive::MaxEntrySize) + " bytes)";
			return false;
		}
		return AddPending({ std::move(path), {}, std::move(data), assetID, Storage::Smallest }, outError);
	}

	size_t PackArchiveWriter::FindBatchEnd(std::span<const uint64_t> sizes, size_t begin, uint64_t batchBytes)
	{
		size_t end = begin;
		uint64_t held = 0;
		while (end < sizes.size() && end - begin < MaxBatchEntries)
		{
			// Compared this way round so that a large size cannot wrap.
			if (end > begin && sizes[end] > batchBytes - std::min(held, batchBytes))
				break;
			held += sizes[end++];
		}
		return end;
	}

	std::optional<PackArchiveWriter::Statistics> PackArchiveWriter::Write(const std::filesystem::path& file, std::string& outError, int compressionLevel, uint64_t batchBytes) const
	{
		// Sorted indices rather than sorting m_Pending: Write leaves the writer as it was.
		std::vector<size_t> order(m_Pending.size());
		std::iota(order.begin(), order.end(), size_t{ 0 });
		std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return m_Pending[a].Path < m_Pending[b].Path; });

		std::error_code error;
		if (file.has_parent_path())
		{
			std::filesystem::create_directories(file.parent_path(), error);
			if (error)
			{
				outError = "cannot create the folder '" + FileSystem::PathToUtf8(file.parent_path()) + "': " + FileSystem::DescribeError(error);
				return std::nullopt;
			}
		}
		const std::filesystem::path temporary = FileSystem::MakeTemporaryPath(file);
		errno = 0;
		std::ofstream stream(temporary, std::ios::out | std::ios::binary | std::ios::trunc);
		if (!stream)
		{
			outError = "cannot open '" + FileSystem::PathToUtf8(temporary) + "' for writing" + DescribeErrno();
			return std::nullopt;
		}
		const auto fail = [&](const std::string& problem) -> std::optional<Statistics> {
			outError = problem;
			stream.close();
			std::filesystem::remove(temporary, error);
			return std::nullopt;
		};

		const char placeholder[PackArchive::HeaderSize] = {};
		stream.write(placeholder, sizeof(placeholder));
		const int level = std::clamp(compressionLevel, 1, ZSTD_maxCLevel());
		Statistics statistics;
		uint64_t offset = PackArchive::HeaderSize;
		std::vector<uint8_t> index;
		// Sizes only plan the batches: a file that cannot be sized here fails with its reason on its job.
		std::vector<uint64_t> sizes(order.size());
		for (size_t i = 0; i < order.size(); i++)
		{
			const Pending& pending = m_Pending[order[i]];
			std::error_code sizeError;
			const uint64_t size = pending.Source.empty() ? pending.Data.size() : std::filesystem::file_size(pending.Source, sizeError);
			sizes[i] = sizeError ? 0 : size;
		}
		const auto writeFailed = [&]() { return fail("failed writing '" + FileSystem::PathToUtf8(temporary) + "' (disk full?)"); };
		for (size_t begin = 0; begin < order.size();)
		{
			const size_t end = FindBatchEnd(sizes, begin, batchBytes);
			const auto count = static_cast<uint32_t>(end - begin);
			std::vector<PrepareSlot> slots(count);
			// Failures are in the slots, which also show a job that never ran.
			(void)JobSystem::Get().ParallelFor(count, 1, [&](uint32_t first, uint32_t last) {
				for (uint32_t i = first; i < last; i++)
				{
					const Pending& pending = m_Pending[order[begin + i]];
					try
					{
						slots[i].Entry = PrepareEntry(pending.Path, pending.Source, pending.Data, pending.EntryStorage, level, slots[i].Error);
					}
					catch (const std::exception& exception)
					{
						// Most likely out of memory for this entry, which the job system would only log without its name.
						slots[i].Error = "cannot prepare '" + pending.Path + "': " + exception.what();
					}
				}
			});
			// The first failure in path order, whichever job met one first.
			for (uint32_t i = 0; i < count; i++)
			{
				const Pending& pending = m_Pending[order[begin + i]];
				const PrepareSlot& slot = slots[i];
				if (!slot.Entry)
					return fail(!slot.Error.empty() ? slot.Error : "'" + pending.Path + "' could not be prepared (the log says why)");
				const PreparedEntry& entry = *slot.Entry;
				const std::span<const uint8_t> stored = entry.GetBytes(pending.Data);
				stream.write(reinterpret_cast<const char*>(stored.data()), static_cast<std::streamsize>(stored.size()));

				PutU32(index, static_cast<uint32_t>(pending.Path.size()));
				index.insert(index.end(), pending.Path.begin(), pending.Path.end());
				PutU64(index, pending.AssetID);
				PutU64(index, offset);
				PutU64(index, stored.size());
				PutU64(index, entry.Size);
				index.push_back(static_cast<uint8_t>(entry.Compression));
				PutU64(index, entry.Hash);
				offset += stored.size();
				statistics.Entries++;
				statistics.CompressedEntries += entry.Compression == PackArchive::Method::Zstd ? 1 : 0;
				statistics.UncompressedSize += entry.Size;
			}
			// A full disk stops here rather than after compressing every batch that is left.
			if (!stream)
				return writeFailed();
			begin = end;
		}
		stream.write(reinterpret_cast<const char*>(index.data()), static_cast<std::streamsize>(index.size()));
		const std::vector<uint8_t> header = BuildHeader(static_cast<uint32_t>(order.size()), offset, index.size(), HashBytes64(index.data(), index.size()));
		stream.seekp(0);
		stream.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
		// Buffered data is only known to be written (handed to the OS) after a successful flush and close.
		stream.flush();
		stream.close();
		if (stream.fail())
			return writeFailed();
		if (!FileSystem::RenameReplacing(temporary, file, outError))
			return std::nullopt;
		statistics.ArchiveSize = offset + index.size();
		return statistics;
	}

}
