#include "Basalt/Core/PackArchive.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Hash.h"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <system_error>

namespace Basalt {

	namespace {

		// Path length, asset ID, offset, stored size, size, method, hash.
		constexpr size_t EntryFixedSize = 4 + 8 + 8 + 8 + 8 + 1 + 8;

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

		// Little-endian reads from a buffer whose remaining length the caller has checked.
		uint32_t GetU32(const uint8_t* data)
		{
			uint32_t value = 0;
			for (int i = 0; i < 4; i++)
				value |= static_cast<uint32_t>(data[i]) << (8 * i);
			return value;
		}

		uint64_t GetU64(const uint8_t* data)
		{
			uint64_t value = 0;
			for (int i = 0; i < 8; i++)
				value |= static_cast<uint64_t>(data[i]) << (8 * i);
			return value;
		}

		std::vector<uint8_t> BuildHeader(uint32_t entryCount, uint64_t indexOffset, uint64_t indexSize, uint64_t indexHash)
		{
			std::vector<uint8_t> header(PackArchive::Magic, PackArchive::Magic + 4);
			PutU32(header, PackArchive::FormatVersion);
			PutU32(header, 0);
			PutU32(header, entryCount);
			PutU64(header, indexOffset);
			PutU64(header, indexSize);
			PutU64(header, indexHash);
			return header;
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
			outError = "cannot read the archive '" + name + "'" + (error ? ": " + error.message() : std::string(": not a file"));
			return nullptr;
		}
		auto archive = CreateRef<PackArchive>();
		archive->m_File = file;
		archive->m_Stream.open(file, std::ios::in | std::ios::binary);
		if (!archive->m_Stream)
		{
			outError = "cannot open the archive '" + name + "'";
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
		auto archive = CreateRef<PackArchive>();
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
		if (const uint32_t version = GetU32(header + 4); version != FormatVersion)
		{
			outError = "archive format version " + std::to_string(version) + " is not supported (this Basalt reads version " + std::to_string(FormatVersion) + ")";
			return false;
		}
		if (GetU32(header + 8) != 0)
		{
			outError = "the archive uses features this Basalt does not know (nonzero header flags)";
			return false;
		}
		const uint32_t count = GetU32(header + 12);
		const uint64_t indexOffset = GetU64(header + 16);
		const uint64_t indexSize = GetU64(header + 24);
		const uint64_t indexHash = GetU64(header + 32);
		if (count > MaxEntries)
		{
			outError = "the archive lists " + std::to_string(count) + " entries (at most " + std::to_string(MaxEntries) + ")";
			return false;
		}
		// The index ends the file, and each entry takes between EntryFixedSize and EntryFixedSize + MaxPathLength bytes.
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
			const uint32_t pathLength = GetU32(index.data() + cursor);
			cursor += 4;
			if (pathLength == 0 || pathLength > MaxPathLength || index.size() - cursor < pathLength + (EntryFixedSize - 4))
				return fail("invalid path length " + std::to_string(pathLength));
			Entry entry;
			entry.Path.assign(reinterpret_cast<const char*>(index.data() + cursor), pathLength);
			cursor += pathLength;
			entry.AssetID = GetU64(index.data() + cursor);
			entry.Offset = GetU64(index.data() + cursor + 8);
			entry.StoredSize = GetU64(index.data() + cursor + 16);
			entry.Size = GetU64(index.data() + cursor + 24);
			const uint8_t method = index[cursor + 32];
			entry.Hash = GetU64(index.data() + cursor + 33);
			cursor += EntryFixedSize - 4;

			if (!PackArchiveWriter::IsValidPath(entry.Path))
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
		return true;
	}

	const PackArchive::Entry* PackArchive::Find(std::string_view path) const
	{
		const auto it = std::lower_bound(m_Entries.begin(), m_Entries.end(), path, [](const Entry& entry, std::string_view value) { return entry.Path < value; });
		return it != m_Entries.end() && it->Path == path ? &*it : nullptr;
	}

	bool PackArchive::HasFolder(std::string_view path) const
	{
		const std::string prefix = std::string(path) + "/";
		const auto it = std::lower_bound(m_Entries.begin(), m_Entries.end(), prefix, [](const Entry& entry, const std::string& value) { return entry.Path < value; });
		return !path.empty() && it != m_Entries.end() && it->Path.starts_with(prefix);
	}

	const PackArchive::Entry* PackArchive::FindAsset(uint64_t assetID) const
	{
		if (assetID == 0)
			return nullptr;
		const auto it = std::lower_bound(m_ByAsset.begin(), m_ByAsset.end(), assetID, [&](uint32_t index, uint64_t value) { return m_Entries[index].AssetID < value; });
		return it != m_ByAsset.end() && m_Entries[*it].AssetID == assetID ? &m_Entries[*it] : nullptr;
	}

	std::optional<std::vector<uint8_t>> PackArchive::Read(const Entry& entry, std::string& outError) const
	{
		std::vector<uint8_t> data;
		if (entry.Compression == Method::Store)
		{
			data.resize(static_cast<size_t>(entry.Size));
			if (!ReadRange(entry.Offset, entry.Size, data.data()))
			{
				outError = "cannot read '" + entry.Path + "' from the archive";
				return std::nullopt;
			}
		}
		else
		{
			std::vector<uint8_t> stored(static_cast<size_t>(entry.StoredSize));
			if (!ReadRange(entry.Offset, entry.StoredSize, stored.data()))
			{
				outError = "cannot read '" + entry.Path + "' from the archive";
				return std::nullopt;
			}
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

	bool PackArchiveWriter::IsValidPath(std::string_view path)
	{
		if (path.empty() || path.size() > PackArchive::MaxPathLength)
			return false;
		// Components separated by single '/', none empty, "." or "..": the normalized relative form of asset keys. No
		// NUL, backslash or colon, which would name another file (or a drive) on some platform.
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
		if (!IsValidPath(pending.Path))
		{
			outError = "invalid archive path '" + pending.Path + "'";
			return false;
		}
		const bool duplicate = std::any_of(m_Pending.begin(), m_Pending.end(), [&](const Pending& other) { return other.Path == pending.Path; });
		const bool duplicateID = pending.AssetID != 0 && std::any_of(m_Pending.begin(), m_Pending.end(), [&](const Pending& other) { return other.AssetID == pending.AssetID; });
		if (duplicate || duplicateID)
		{
			outError = duplicate ? "'" + pending.Path + "' is already in the archive" : "asset UUID " + std::to_string(pending.AssetID) + " is already in the archive";
			return false;
		}
		if (m_Pending.size() >= PackArchive::MaxEntries)
		{
			outError = "an archive holds at most " + std::to_string(PackArchive::MaxEntries) + " files";
			return false;
		}
		m_Pending.push_back(std::move(pending));
		return true;
	}

	bool PackArchiveWriter::AddFile(std::string path, std::filesystem::path source, uint64_t assetID, std::string& outError)
	{
		return AddPending({ std::move(path), std::move(source), {}, assetID }, outError);
	}

	bool PackArchiveWriter::AddData(std::string path, std::vector<uint8_t> data, uint64_t assetID, std::string& outError)
	{
		return AddPending({ std::move(path), {}, std::move(data), assetID }, outError);
	}

	bool PackArchiveWriter::Write(const std::filesystem::path& file, std::string& outError, int compressionLevel)
	{
		m_Statistics = {};
		std::sort(m_Pending.begin(), m_Pending.end(), [](const Pending& a, const Pending& b) { return a.Path < b.Path; });
		std::error_code error;
		if (file.has_parent_path())
		{
			std::filesystem::create_directories(file.parent_path(), error);
			if (error)
			{
				outError = "cannot create the folder '" + FileSystem::PathToUtf8(file.parent_path()) + "': " + error.message();
				return false;
			}
		}
		const std::filesystem::path temporary = FileSystem::MakeTemporaryPath(file);
		std::ofstream stream(temporary, std::ios::out | std::ios::binary | std::ios::trunc);
		if (!stream)
		{
			outError = "cannot open '" + FileSystem::PathToUtf8(temporary) + "' for writing";
			return false;
		}
		const auto fail = [&](const std::string& problem) {
			outError = problem;
			stream.close();
			std::filesystem::remove(temporary, error);
			return false;
		};

		const std::vector<uint8_t> placeholder(PackArchive::HeaderSize, 0);
		stream.write(reinterpret_cast<const char*>(placeholder.data()), static_cast<std::streamsize>(placeholder.size()));
		const int level = std::clamp(compressionLevel, 1, ZSTD_maxCLevel());
		uint64_t offset = PackArchive::HeaderSize;
		std::vector<uint8_t> index;
		std::vector<uint8_t> compressed;
		for (Pending& pending : m_Pending)
		{
			std::vector<uint8_t> data = std::move(pending.Data);
			if (!pending.Source.empty())
			{
				auto read = FileSystem::ReadBinaryFile(pending.Source);
				if (!read)
					return fail("cannot read '" + FileSystem::PathToUtf8(pending.Source) + "'");
				data = std::move(*read);
			}
			if (data.size() > PackArchive::MaxEntrySize)
				return fail("'" + pending.Path + "' is larger than an archive entry may be (" + std::to_string(PackArchive::MaxEntrySize) + " bytes)");
			compressed.resize(ZSTD_compressBound(data.size()));
			const size_t compressedSize = ZSTD_compress(compressed.data(), compressed.size(), data.data(), data.size(), level);
			const bool useZstd = !ZSTD_isError(compressedSize) && compressedSize < data.size();
			const uint8_t* stored = useZstd ? compressed.data() : data.data();
			const size_t storedSize = useZstd ? compressedSize : data.size();
			stream.write(reinterpret_cast<const char*>(stored), static_cast<std::streamsize>(storedSize));

			PutU32(index, static_cast<uint32_t>(pending.Path.size()));
			index.insert(index.end(), pending.Path.begin(), pending.Path.end());
			PutU64(index, pending.AssetID);
			PutU64(index, offset);
			PutU64(index, storedSize);
			PutU64(index, data.size());
			index.push_back(static_cast<uint8_t>(useZstd ? PackArchive::Method::Zstd : PackArchive::Method::Store));
			PutU64(index, HashBytes64(data.data(), data.size()));
			offset += storedSize;
			m_Statistics.Entries++;
			m_Statistics.Compressed += useZstd ? 1 : 0;
			m_Statistics.Size += data.size();
		}
		stream.write(reinterpret_cast<const char*>(index.data()), static_cast<std::streamsize>(index.size()));
		const std::vector<uint8_t> header = BuildHeader(static_cast<uint32_t>(m_Pending.size()), offset, index.size(), HashBytes64(index.data(), index.size()));
		stream.seekp(0);
		stream.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
		// Buffered data is only known to be on disk after a successful flush and close.
		stream.flush();
		stream.close();
		if (stream.fail())
			return fail("failed writing '" + FileSystem::PathToUtf8(temporary) + "' (disk full?)");
		if (!FileSystem::ReplaceFile(temporary, file, outError))
			return false;
		m_Statistics.ArchiveSize = offset + index.size();
		return true;
	}

}
