#include <doctest/doctest.h>

#include <Breccia/Asset/AssetManager.h>
#include <Breccia/Asset/AssetRegistry.h>
#include <Breccia/Audio/AudioEngine.h>
#include <Breccia/Audio/AudioSystem.h>
#include <Breccia/Core/FileSystem.h>
#include <Breccia/Core/Hash.h>
#include <Breccia/Core/PackArchive.h>
#include <Breccia/Project/Exporter.h>
#include <Breccia/Project/Packaging.h>
#include <Breccia/Project/Project.h>
#include <Breccia/Scene/Components.h>
#include <Breccia/Scene/Entity.h>
#include <Breccia/Scene/Scene.h>
#include <Breccia/Scene/SceneSerializer.h>
#include <Breccia/UI/UIFonts.h>

#include "MeshTestUtils.h"
#include "TestUtils.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Breccia;

namespace {

	std::vector<uint8_t> Bytes(std::string_view text)
	{
		return { text.begin(), text.end() };
	}

	// A texture name outside ASCII, and outside every single-byte Windows code page.
	const std::string UnicodeTexture = "Assets/Textures/模様.png";

	// Bytes that zstd cannot shrink.
	std::vector<uint8_t> Noise(size_t size, uint32_t seed = 7)
	{
		std::mt19937 random(seed);
		std::vector<uint8_t> data(size);
		for (uint8_t& byte : data)
			byte = static_cast<uint8_t>(random());
		return data;
	}

	// Text that compresses well.
	std::vector<uint8_t> Repetitive(size_t size)
	{
		std::vector<uint8_t> data(size);
		constexpr std::string_view Pattern = "pack archive text ";
		for (size_t i = 0; i < size; i++)
			data[i] = static_cast<uint8_t>(Pattern[i % Pattern.size()]);
		return data;
	}

	// An entry the archive must hold (a missing one ends the test case instead of crashing the run).
	const PackArchive::Entry& RequireEntry(const PackArchive& archive, std::string_view path)
	{
		const PackArchive::Entry* entry = archive.Find(path);
		REQUIRE_MESSAGE(entry, path);
		return *entry;
	}

	// The bytes of an archive built from (path, data, asset ID) entries.
	std::vector<uint8_t> BuildArchive(const std::vector<std::tuple<std::string, std::vector<uint8_t>, uint64_t>>& entries, const std::filesystem::path& file)
	{
		PackArchiveWriter writer;
		std::string error;
		for (const auto& [path, data, id] : entries)
			REQUIRE_MESSAGE(writer.AddData(path, data, id, error), error);
		REQUIRE_MESSAGE(writer.Write(file, error).has_value(), error);
		auto bytes = FileSystem::ReadBinaryFile(file);
		REQUIRE(bytes);
		return *bytes;
	}

	uint64_t ReadU64(const std::vector<uint8_t>& bytes, size_t offset)
	{
		uint64_t value = 0;
		for (int i = 0; i < 8; i++)
			value |= static_cast<uint64_t>(bytes[offset + i]) << (8 * i);
		return value;
	}

	void WriteU64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value)
	{
		for (int i = 0; i < 8; i++)
			bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
	}

	uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset)
	{
		uint32_t value = 0;
		for (int i = 0; i < 4; i++)
			value |= static_cast<uint32_t>(bytes[offset + i]) << (8 * i);
		return value;
	}

	void WriteU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
	{
		for (int i = 0; i < 4; i++)
			bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
	}

	size_t IndexOffset(const std::vector<uint8_t>& bytes)
	{
		return static_cast<size_t>(ReadU64(bytes, 16));
	}

	// Recomputes the header's index hash after a test edited the index, so the edit reaches the entry checks.
	void RehashIndex(std::vector<uint8_t>& bytes)
	{
		const size_t offset = IndexOffset(bytes);
		WriteU64(bytes, 32, HashBytes64(bytes.data() + offset, bytes.size() - offset));
	}

	// The offset of the field `fieldOffset` bytes after the path of the first index entry.
	size_t FirstEntryField(const std::vector<uint8_t>& bytes, size_t fieldOffset)
	{
		const size_t index = IndexOffset(bytes);
		return index + 4 + ReadU32(bytes, index) + fieldOffset;
	}

	// Asset ID, offset, stored size, size, method, hash: offsets after an entry's path.
	constexpr size_t AssetIDField = 0;
	constexpr size_t OffsetField = 8;
	constexpr size_t StoredSizeField = 16;
	constexpr size_t SizeField = 24;
	constexpr size_t MethodField = 32;

	std::string OpenError(std::vector<uint8_t> bytes)
	{
		std::string error;
		CHECK_FALSE(PackArchive::OpenMemory(std::move(bytes), error));
		return error;
	}
}

TEST_SUITE("Archive")
{
	TEST_CASE("Archives round-trip entries, compressing only what shrinks")
	{
		BrecciaTest::TempDirectory directory("ArchiveRoundTrip");
		const std::vector<uint8_t> noise = Noise(4096);
		const std::vector<uint8_t> text = Repetitive(size_t{ 64 } * 1024);
		const std::vector<std::tuple<std::string, std::vector<uint8_t>, uint64_t>> entries = {
			{ "Assets/Textures/Noise.ktx2", noise, 11 },
			{ "Assets/Scripts/Main.lua", text, 22 },
			{ "Assets/Empty.txt", {}, 0 },
			{ "Assets/Scenes/Main.bscene", Bytes(R"({"Entities":[]})"), 33 },
			// Compresses about 10000:1, inside the size bound Parse sets for compressed entries.
			{ "Assets/Audio/Silence.raw", std::vector<uint8_t>(size_t{ 8 } << 20, 0), 0 },
		};
		const std::filesystem::path file = directory.GetPath() / "Out" / "Assets.bpak";
		const std::vector<uint8_t> bytes = BuildArchive(entries, file);
		// Deterministic: the same entries, added in another order, give the same bytes.
		auto reversed = entries;
		std::reverse(reversed.begin(), reversed.end());
		CHECK(BuildArchive(reversed, directory.GetPath() / "Again.bpak") == bytes);
		CHECK_FALSE(BrecciaTest::HasTempFiles(file.parent_path()));

		std::string error;
		const Ref<PackArchive> archive = PackArchive::Open(file, error);
		REQUIRE_MESSAGE(archive, error);
		CHECK(archive->GetFile() == file);
		const auto& listed = archive->GetEntries();
		REQUIRE(listed.size() == 5);
		// Sorted by path.
		CHECK(listed[0].Path == "Assets/Audio/Silence.raw");
		CHECK(RequireEntry(*archive, "Assets/Audio/Silence.raw").StoredSize < 4096);
		CHECK(listed.back().Path == "Assets/Textures/Noise.ktx2");
		for (const auto& [path, data, id] : entries)
		{
			const PackArchive::Entry* entry = archive->Find(path);
			REQUIRE_MESSAGE(entry, path);
			CHECK(entry->AssetID == id);
			CHECK(entry->Size == data.size());
			const auto read = archive->Read(*entry, error);
			REQUIRE_MESSAGE(read, error);
			CHECK(*read == data);
			if (id != 0)
				CHECK(archive->FindAsset(id) == entry);
		}
		CHECK(RequireEntry(*archive, "Assets/Scripts/Main.lua").Compression == PackArchive::Method::Zstd);
		CHECK(RequireEntry(*archive, "Assets/Scripts/Main.lua").StoredSize < text.size() / 10);
		CHECK(RequireEntry(*archive, "Assets/Textures/Noise.ktx2").Compression == PackArchive::Method::Store);
		CHECK(RequireEntry(*archive, "Assets/Empty.txt").Compression == PackArchive::Method::Store);
		// Exact matches only: letter case counts, as on Linux.
		CHECK_FALSE(archive->Find("assets/scripts/main.lua"));
		CHECK_FALSE(archive->Find("Assets/Scripts"));
		CHECK_FALSE(archive->FindAsset(0));
		CHECK_FALSE(archive->FindAsset(44));
		CHECK(archive->HasFolder("Assets"));
		CHECK(archive->HasFolder("Assets/Scripts"));
		CHECK_FALSE(archive->HasFolder("Assets/Script"));
		CHECK_FALSE(archive->HasFolder("Assets/Scripts/Main.lua"));
		CHECK_FALSE(archive->HasFolder(""));

		// The same archive from memory.
		const Ref<PackArchive> memory = PackArchive::OpenMemory(bytes, error);
		REQUIRE_MESSAGE(memory, error);
		CHECK(memory->GetFile().empty());
		CHECK(memory->Read(RequireEntry(*memory, "Assets/Scripts/Main.lua"), error) == text);
		const auto empty = memory->Read(RequireEntry(*memory, "Assets/Empty.txt"), error);
		REQUIRE_MESSAGE(empty, error);
		CHECK(empty->empty());
	}

	TEST_CASE("The writer reports its statistics and rejects bad entries")
	{
		BrecciaTest::TempDirectory directory("ArchiveWriter");
		PackArchiveWriter writer;
		std::string error;
		CHECK(writer.AddData("Assets/A.txt", Repetitive(10000), 1, error));
		CHECK(writer.AddData("Assets/B.bin", Noise(1000), 2, error));
		for (const char* path : { "", "/Assets/A", "Assets//A", "Assets/A/", "Assets/./A", "Assets/../A", "..", ".", "Assets\\A", "C:/A", "Assets/A:B" })
		{
			CHECK_FALSE_MESSAGE(writer.AddData(path, {}, 0, error), path);
			CHECK(error.find("invalid archive path") != std::string::npos);
			CHECK_FALSE(PackArchive::IsValidPath(path));
		}
		CHECK_FALSE(writer.AddData(std::string(PackArchive::MaxPathLength + 1, 'a'), {}, 0, error));
		CHECK(PackArchive::IsValidPath(std::string(PackArchive::MaxPathLength, 'a')));
		CHECK_FALSE(writer.AddData("Assets/A.txt", {}, 0, error));
		CHECK(error.find("already in the archive") != std::string::npos);
		CHECK_FALSE(writer.AddData("Assets/C.txt", {}, 2, error));
		CHECK(error.find("asset UUID 2 is already") != std::string::npos);
		// A source that is gone by the time Write runs fails the write and leaves no file behind.
		PackArchiveWriter missing;
		CHECK(missing.AddFile("Assets/Gone.txt", directory.GetPath() / "Gone.txt", 0, error));
		CHECK_FALSE(missing.Write(directory.GetPath() / "Missing.bpak", error).has_value());
		CHECK(error.find("cannot read") != std::string::npos);
		CHECK_FALSE(FileSystem::Exists(directory.GetPath() / "Missing.bpak"));
		CHECK_FALSE(BrecciaTest::HasTempFiles(directory.GetPath()));

		const std::filesystem::path file = directory.GetPath() / "Stats.bpak";
		const auto statistics = writer.Write(file, error);
		REQUIRE_MESSAGE(statistics, error);
		CHECK(statistics->Entries == 2);
		CHECK(statistics->CompressedEntries == 1);
		CHECK(statistics->UncompressedSize == 11000);
		CHECK(statistics->ArchiveSize == std::filesystem::file_size(file));
		// The writer keeps its entries: writing again (a retry) gives the same bytes, in-memory data included.
		const std::filesystem::path again = directory.GetPath() / "Again.bpak";
		REQUIRE(writer.Write(again, error).has_value());
		CHECK(FileSystem::ReadBinaryFile(again) == FileSystem::ReadBinaryFile(file));
		CHECK_FALSE(writer.AddFile("Assets/NoSource.txt", {}, 0, error));
		CHECK(error.find("no source file") != std::string::npos);

		// An empty archive is valid.
		PackArchiveWriter empty;
		REQUIRE(empty.Write(directory.GetPath() / "Empty.bpak", error).has_value());
		const Ref<PackArchive> archive = PackArchive::Open(directory.GetPath() / "Empty.bpak", error);
		REQUIRE_MESSAGE(archive, error);
		CHECK(archive->GetEntries().empty());
	}

	TEST_CASE("Damaged and forged archives are rejected before anything is allocated for them")
	{
		BrecciaTest::TempDirectory directory("ArchiveDamage");
		const std::vector<uint8_t> valid = BuildArchive({ { "Assets/A.txt", Repetitive(5000), 5 }, { "Assets/B.bin", Noise(300), 6 } }, directory.GetPath() / "Valid.bpak");
		std::string error;
		REQUIRE(PackArchive::OpenMemory(valid, error));

		CHECK(OpenError({}).find("shorter than its header") != std::string::npos);
		CHECK(OpenError(std::vector<uint8_t>(valid.begin(), valid.begin() + 39)).find("shorter than its header") != std::string::npos);
		auto bytes = valid;
		bytes[0] = 'X';
		CHECK(OpenError(bytes) == "not a .bpak archive");
		bytes = valid;
		WriteU32(bytes, 4, 2);
		CHECK(OpenError(bytes).find("format version 2 is not supported") != std::string::npos);
		bytes = valid;
		WriteU32(bytes, 8, 1);
		CHECK(OpenError(bytes).find("nonzero header flags") != std::string::npos);
		bytes = valid;
		WriteU32(bytes, 12, PackArchive::MaxEntries + 1);
		CHECK(OpenError(bytes).find("at most") != std::string::npos);
		// Truncated, or with bytes appended: the index no longer ends the file.
		CHECK(OpenError(std::vector<uint8_t>(valid.begin(), valid.end() - 1)).find("does not end the file") != std::string::npos);
		bytes = valid;
		bytes.push_back(0);
		CHECK(OpenError(bytes).find("does not end the file") != std::string::npos);
		bytes = valid;
		WriteU64(bytes, 16, 8);
		CHECK(OpenError(bytes).find("does not end the file") != std::string::npos);
		// The entry count disagrees with the index size.
		bytes = valid;
		WriteU32(bytes, 12, 200);
		CHECK(OpenError(bytes).find("does not match its 200 entries") != std::string::npos);
		bytes = valid;
		bytes[IndexOffset(bytes) + 10] ^= 1;
		CHECK(OpenError(bytes).find("index is damaged") != std::string::npos);

		// Entry checks, reached with a rehashed index.
		const auto entryError = [&](size_t field, uint64_t value, int width = 8) {
			auto edited = valid;
			const size_t offset = FirstEntryField(edited, field);
			if (width == 1)
				edited[offset] = static_cast<uint8_t>(value);
			else
				WriteU64(edited, offset, value);
			RehashIndex(edited);
			return OpenError(edited);
		};
		CHECK(entryError(MethodField, 7, 1).find("unknown compression method 7") != std::string::npos);
		CHECK(entryError(SizeField, PackArchive::MaxEntrySize + 1).find("larger than") != std::string::npos);
		CHECK(entryError(OffsetField, 4).find("outside the archive's data") != std::string::npos);
		CHECK(entryError(OffsetField, ~uint64_t{ 0 }).find("outside the archive's data") != std::string::npos);
		CHECK(entryError(StoredSizeField, ~uint64_t{ 0 } - 10).find("outside the archive's data") != std::string::npos);
		// A stored entry's sizes must agree.
		CHECK(entryError(MethodField, 0, 1).find("inconsistent sizes") != std::string::npos);
		CHECK(entryError(AssetIDField, 6).find("asset UUID 6 belongs to both") != std::string::npos);

		// Paths: invalid, out of order, duplicated.
		bytes = valid;
		const size_t pathStart = IndexOffset(bytes) + 4;
		std::memcpy(bytes.data() + pathStart, "../A", 4);
		RehashIndex(bytes);
		CHECK(OpenError(bytes).find("invalid path '../Ats/A.txt'") != std::string::npos);
		bytes = valid;
		bytes[pathStart + 7] = 'Z';
		RehashIndex(bytes);
		CHECK(OpenError(bytes).find("out of order or listed twice") != std::string::npos);
		bytes = valid;
		WriteU32(bytes, IndexOffset(bytes), 0);
		RehashIndex(bytes);
		CHECK(OpenError(bytes).find("invalid path length 0") != std::string::npos);
		bytes = valid;
		WriteU32(bytes, IndexOffset(bytes), 60000);
		RehashIndex(bytes);
		CHECK(OpenError(bytes).find("invalid path length 60000") != std::string::npos);

		// Damaged data opens (the index is intact) but does not read.
		const size_t compressed = static_cast<size_t>(ReadU64(valid, FirstEntryField(valid, OffsetField)));
		bytes = valid;
		bytes[compressed] ^= 0xFF;
		Ref<PackArchive> archive = PackArchive::OpenMemory(bytes, error);
		REQUIRE_MESSAGE(archive, error);
		CHECK_FALSE(archive->Read(RequireEntry(*archive, "Assets/A.txt"), error));
		CHECK(error.find("'Assets/A.txt' is damaged in the archive") != std::string::npos);
		CHECK(archive->Read(RequireEntry(*archive, "Assets/B.bin"), error));
		const size_t stored = static_cast<size_t>(ReadU64(valid, FirstEntryField(valid, OffsetField)) + ReadU64(valid, FirstEntryField(valid, StoredSizeField)));
		bytes = valid;
		bytes[stored + 100] ^= 1;
		archive = PackArchive::OpenMemory(bytes, error);
		REQUIRE(archive);
		CHECK_FALSE(archive->Read(RequireEntry(*archive, "Assets/B.bin"), error));
		CHECK(error == "'Assets/B.bin' is damaged in the archive (hash mismatch)");
		// A compressed entry whose frame declares another size than the index.
		bytes = valid;
		WriteU64(bytes, FirstEntryField(bytes, SizeField), 4999);
		RehashIndex(bytes);
		archive = PackArchive::OpenMemory(bytes, error);
		REQUIRE_MESSAGE(archive, error);
		CHECK_FALSE(archive->Read(RequireEntry(*archive, "Assets/A.txt"), error));
		CHECK(error.find("does not match the index") != std::string::npos);
		// A compressed entry whose stored bytes run on into a second complete frame (the next entry's): the first frame
		// alone declares the right size, so only the whole-frame check refuses it.
		{
			const std::vector<uint8_t> twoFrames = BuildArchive({ { "Assets/A.txt", Repetitive(5000), 0 }, { "Assets/C.txt", Repetitive(3000), 0 } }, directory.GetPath() / "TwoFrames.bpak");
			const Ref<PackArchive> original = PackArchive::OpenMemory(twoFrames, error);
			REQUIRE_MESSAGE(original, error);
			const PackArchive::Entry& first = RequireEntry(*original, "Assets/A.txt");
			const PackArchive::Entry& second = RequireEntry(*original, "Assets/C.txt");
			REQUIRE((first.Compression == PackArchive::Method::Zstd && second.Compression == PackArchive::Method::Zstd));
			REQUIRE(second.Offset == first.Offset + first.StoredSize);
			bytes = twoFrames;
			WriteU64(bytes, FirstEntryField(bytes, StoredSizeField), first.StoredSize + second.StoredSize);
			RehashIndex(bytes);
			archive = PackArchive::OpenMemory(bytes, error);
			REQUIRE_MESSAGE(archive, error);
			CHECK_FALSE(archive->Read(RequireEntry(*archive, "Assets/A.txt"), error));
			CHECK(error.find("does not match the index") != std::string::npos);
			CHECK(archive->Read(RequireEntry(*archive, "Assets/C.txt"), error));
		}

		// Read only accepts this archive's own entries, whose sizes Parse bounded.
		const Ref<PackArchive> other = PackArchive::OpenMemory(valid, error);
		archive = PackArchive::OpenMemory(valid, error);
		REQUIRE((other && archive));
		CHECK_FALSE(archive->Read(RequireEntry(*other, "Assets/A.txt"), error));
		CHECK(error.find("is not an entry of this archive") != std::string::npos);
		PackArchive::Entry forged = RequireEntry(*archive, "Assets/A.txt");
		forged.Size = uint64_t{ 1 } << 62;
		CHECK_FALSE(archive->Read(forged, error));
		CHECK(error.find("is not an entry of this archive") != std::string::npos);

		// One entry where the index holds two: the second becomes trailing bytes.
		bytes = valid;
		WriteU32(bytes, 12, 1);
		RehashIndex(bytes);
		CHECK(OpenError(bytes).find("unexpected trailing bytes") != std::string::npos);
		// A compressed entry with no stored bytes.
		CHECK(entryError(StoredSizeField, 0).find("inconsistent sizes") != std::string::npos);
		// A few compressed bytes claiming gigabytes (a forged zstd frame would make Read allocate them all).
		CHECK(entryError(SizeField, PackArchive::MaxEntrySize).find("claims more data than its compressed bytes can hold") != std::string::npos);

		CHECK_FALSE(PackArchive::Open(directory.GetPath() / "Missing.bpak", error));
		CHECK(error.find("cannot read the archive") != std::string::npos);
		CHECK_FALSE(PackArchive::Open(directory.GetPath(), error));
		CHECK(error.find("not a file") != std::string::npos);
	}

	TEST_CASE("A mounted archive answers reads under its root before the disk")
	{
		BrecciaTest::TempDirectory directory("ArchiveMount");
		const std::filesystem::path root = directory.GetPath() / "Game";
		REQUIRE(FileSystem::WriteTextFile(root / "Project.bproject", "loose project"));
		REQUIRE(FileSystem::WriteTextFile(root / "Assets" / "Loose.txt", "on disk"));
		REQUIRE(FileSystem::WriteTextFile(root / "Assets" / "Shadowed.txt", "disk copy"));
		REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Outside.txt", "outside"));
		std::string error;
		BuildArchive({ { "Assets/Packed.txt", Bytes("from the archive"), 9 }, { "Assets/Shadowed.txt", Bytes("archive copy"), 0 }, { "Assets/Deep/Nested/File.bin", Noise(64), 0 } }, directory.GetPath() / "Assets.bpak");
		const Ref<PackArchive> archive = PackArchive::Open(directory.GetPath() / "Assets.bpak", error);
		REQUIRE_MESSAGE(archive, error);

		CHECK_FALSE(FileSystem::ReadTextFile(root / "Assets" / "Packed.txt"));
		{
			// "Game/." is the same folder as "Game".
			BrecciaTest::ScopedMount mount(root / ".", archive);
			CHECK(FileSystem::GetMountedArchive(root) == archive);
			CHECK(FileSystem::GetMountedArchive(root / "") == archive);
			CHECK_FALSE(FileSystem::GetMountedArchive(root / "Assets"));
			CHECK(FileSystem::ReadTextFile(root / "Assets" / "Packed.txt") == "from the archive");
			CHECK(FileSystem::ReadTextFile(root / "Assets" / "Sub" / ".." / "Packed.txt") == "from the archive");
			CHECK(FileSystem::ReadBinaryFile(root / "Assets" / "Deep" / "Nested" / "File.bin") == Noise(64));
			CHECK(FileSystem::ReadTextFile(root / "Assets" / "Shadowed.txt") == "archive copy");
			// Files the archive does not hold come from disk.
			CHECK(FileSystem::ReadTextFile(root / "Assets" / "Loose.txt") == "on disk");
			CHECK(FileSystem::ReadTextFile(root / "Project.bproject") == "loose project");
			CHECK(FileSystem::ReadTextFile(directory.GetPath() / "Outside.txt") == "outside");
			CHECK(FileSystem::Exists(root / "Assets" / "Packed.txt"));
			CHECK(FileSystem::Exists(root / "Assets" / "Deep"));
			CHECK(FileSystem::Exists(root / "Assets" / "Deep" / "Nested"));
			CHECK_FALSE(FileSystem::Exists(root / "Assets" / "Missing.txt"));
			CHECK_FALSE(FileSystem::Exists(root / "Assets" / "Dee"));
			std::error_code sizeError;
			CHECK(FileSystem::GetRegularFileSize(root / "Assets" / "Packed.txt", sizeError) == 16);
			CHECK(FileSystem::GetRegularFileSize(root / "Assets" / "Loose.txt", sizeError) == 7);
			CHECK_FALSE(FileSystem::GetRegularFileSize(root / "Assets" / "Deep", sizeError));
			CHECK_FALSE(sizeError);
			CHECK_FALSE(FileSystem::GetRegularFileSize(root / "Assets" / "Missing.txt", sizeError));
			CHECK_FALSE(sizeError);
			const FileSystem::MountedFile mounted = FileSystem::FindMounted(root / "Assets" / "Packed.txt");
			REQUIRE(mounted);
			CHECK(mounted.GetEntry().AssetID == 9);
			CHECK(mounted.GetArchive() == archive);
			CHECK_FALSE(FileSystem::FindMounted(root / "Assets" / "Loose.txt"));
			CHECK_FALSE(FileSystem::FindMounted(root));
			CHECK_FALSE(FileSystem::FindMounted(directory.GetPath() / "Assets" / "Packed.txt"));
			// A relative path resolves against the working directory, as on disk.
			const std::filesystem::path previous = std::filesystem::current_path();
			std::filesystem::current_path(root);
			const auto relative = FileSystem::ReadTextFile("Assets/Packed.txt");
			std::filesystem::current_path(previous);
			CHECK(relative == "from the archive");
		}
		CHECK_FALSE(FileSystem::GetMountedArchive(root));
		std::string notFound;
		CHECK_FALSE(FileSystem::ReadMounted({}, &notFound));
		CHECK(notFound == "no such file in the mounted archive");
		CHECK_FALSE(FileSystem::ReadTextFile(root / "Assets" / "Packed.txt"));
		CHECK(FileSystem::ReadTextFile(root / "Assets" / "Shadowed.txt") == "disk copy");

		// A damaged entry fails its read, says why in the log, and does not fall back to the disk copy.
		std::vector<uint8_t> bytes = *FileSystem::ReadBinaryFile(directory.GetPath() / "Assets.bpak");
		const PackArchive::Entry* shadowed = archive->Find("Assets/Shadowed.txt");
		bytes[static_cast<size_t>(shadowed->Offset)] ^= 1;
		const Ref<PackArchive> damaged = PackArchive::OpenMemory(bytes, error);
		REQUIRE(damaged);
		BrecciaTest::ScopedMount mount(root, damaged);
		// Without a place for the reason, the log gets it; with one, the caller does (and logs it with its own context).
		uint64_t since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(FileSystem::ReadTextFile(root / "Assets" / "Shadowed.txt"));
		CHECK(BrecciaTest::CountLogMessages(since, "'Assets/Shadowed.txt' is damaged in the archive") == 1);
		since = Log::GetHistory().GetTotalCount();
		std::string readError;
		CHECK_FALSE(FileSystem::ReadTextFile(root / "Assets" / "Shadowed.txt", &readError));
		CHECK(BrecciaTest::CountLogMessages(since, "'Assets/Shadowed.txt' is damaged in the archive") == 0);
		CHECK(readError == "'Assets/Shadowed.txt' is damaged in the archive (hash mismatch)");
		// Disk failures say why too.
		CHECK_FALSE(FileSystem::ReadTextFile(root / "Missing.txt", &readError));
		CHECK(readError == "no such file");
		CHECK_FALSE(FileSystem::ReadBinaryFile(root, &readError));
		CHECK(readError == "not a file");
		// Loaders pass the archive's reason on rather than calling the file missing.
		CHECK_FALSE(SceneSerializer::ReadSceneFile(root / "Assets" / "Shadowed.txt", "Shadowed.txt", error));
		CHECK_MESSAGE(error.find("damaged in the archive") != std::string::npos, error);
		CHECK(error.find("no such file") == std::string::npos);
	}

	TEST_CASE("Prefix reads return exactly the asked bytes, from disk and from a mounted archive")
	{
		BrecciaTest::TempDirectory directory("ArchivePrefix");
		const std::filesystem::path root = directory.GetPath() / "Game";
		REQUIRE(FileSystem::WriteTextFile(root / "Assets" / "Disk.txt", "0123456789"));
		BuildArchive({ { "Assets/Packed.txt", Repetitive(5000), 0 } }, directory.GetPath() / "Assets.bpak");
		std::string error;
		const Ref<PackArchive> archive = PackArchive::Open(directory.GetPath() / "Assets.bpak", error);
		REQUIRE_MESSAGE(archive, error);
		BrecciaTest::ScopedMount mount(root, archive);

		CHECK(FileSystem::ReadBinaryFilePrefix(root / "Assets" / "Disk.txt", 4) == Bytes("0123"));
		CHECK(FileSystem::ReadBinaryFilePrefix(root / "Assets" / "Disk.txt", 10) == Bytes("0123456789"));
		CHECK(FileSystem::ReadBinaryFilePrefix(root / "Assets" / "Disk.txt", 0) == Bytes("0123456789"));
		CHECK_FALSE(FileSystem::ReadBinaryFilePrefix(root / "Assets" / "Disk.txt", 11, &error));
		CHECK(error == "shorter than the 11 bytes to read");
		CHECK_FALSE(FileSystem::ReadBinaryFilePrefix(root / "Assets" / "Missing.txt", 1, &error));
		CHECK(error == "no such file");

		const std::vector<uint8_t> packed = Repetitive(5000);
		CHECK(FileSystem::ReadBinaryFilePrefix(root / "Assets" / "Packed.txt", 100) == std::vector<uint8_t>(packed.begin(), packed.begin() + 100));
		CHECK(FileSystem::ReadBinaryFilePrefix(root / "Assets" / "Packed.txt", 0) == packed);
		CHECK_FALSE(FileSystem::ReadBinaryFilePrefix(root / "Assets" / "Packed.txt", 5001, &error));
		CHECK(error == "shorter than the 5001 bytes to read");
	}

	TEST_CASE("An archived export loads its scenes, models, textures, fonts, scripts and audio from the archive")
	{
		BrecciaTest::TempProject temp("ArchiveExport");
		REQUIRE(temp.IsValid());
		const Ref<Project>& project = Project::GetActive();
		const std::filesystem::path assets = temp.GetDirectory() / "Assets";
		// The model's base color texture is an external PNG, which the importer loads by its own asset key.
		const std::filesystem::path gltf = BrecciaTest::WriteSphereGltf(assets / "Models");
		{
			nlohmann::json model = nlohmann::json::parse(*FileSystem::ReadTextFile(gltf));
			model["images"] = { { { "uri", "Sphere Albedo.png" } } };
			model["textures"] = { { { "source", 0 } } };
			model["materials"][0]["pbrMetallicRoughness"] = { { "baseColorTexture", { { "index", 0 } } } };
			REQUIRE(FileSystem::WriteTextFile(gltf, model.dump()));
			const std::vector<uint8_t> albedo = EncodeIconPng(MakeDefaultIcon(16));
			REQUIRE(FileSystem::WriteBinaryFile(assets / "Models" / "Sphere Albedo.png", albedo.data(), albedo.size()));
			// Two buffers sharing Sphere.bin, the first declaring only its vertex prefix: each read keeps exactly the declared
			// bytes, from disk and from the archive.
			const uint64_t vertexBytes = model["bufferViews"][0]["byteLength"].get<uint64_t>();
			model["buffers"] = { { { "uri", "Sphere.bin" }, { "byteLength", vertexBytes } }, model["buffers"][0] };
			model["bufferViews"][1]["buffer"] = 1;
			REQUIRE(FileSystem::WriteTextFile(assets / "Models" / "Shared.gltf", model.dump()));
			CHECK_MESSAGE(AssetManager::GetMesh("Assets/Models/Shared.gltf"), AssetManager::GetError("Assets/Models/Shared.gltf"));
			AssetManager::Clear();
		}
		const std::vector<uint8_t> unicodePng = EncodeIconPng(MakeDefaultIcon(8));
		REQUIRE(FileSystem::WriteBinaryFile(temp.GetDirectory() / FileSystem::PathFromUtf8(UnicodeTexture),
											unicodePng.data(), unicodePng.size()));
		const std::vector<uint8_t> png = EncodeIconPng(MakeDefaultIcon(64));
		REQUIRE(FileSystem::WriteBinaryFile(assets / "Textures" / "Icon.png", png.data(), png.size()));
		std::error_code copyError;
		std::filesystem::create_directories(assets / "Fonts", copyError);
		std::filesystem::copy_file(std::filesystem::path(BRECCIA_TEST_DATA_DIR) / "Textures" / "UASTC.ktx2", assets / "Textures" / "UASTC.ktx2", copyError);
		std::filesystem::copy_file(std::filesystem::path(BRECCIA_TEST_DATA_DIR) / "FeatureTest" / "Assets" / "Fonts" / "Karla-Regular.ttf", assets / "Fonts" / "Karla.ttf", copyError);
		REQUIRE_FALSE(copyError);
		temp.WriteFile("Assets/Audio/Beep.wav", BrecciaTest::MakeWav(0.5f));
		temp.WriteFile("Assets/Scripts/Lib/Util.lua", "return { Triple = function(x) return 3 * x end }");
		temp.WriteFile("Assets/Scripts/Pkg/init.lua", "return { Name = 'package' }");
		temp.WriteFile("Assets/Scripts/Layered/init.lua", "return 'archive init'");
		temp.WriteFile("Assets/Scripts/Lib/Bad.lua", "return {");
		temp.WriteFile("Assets/Scripts/Lib/Priority.lua", "return 'archive'");
		// A byte order mark and a shebang line, which Lua's file loader skips (line numbers stay right).
		temp.WriteFile("Assets/Scripts/Lib/Shebang.lua", "\xEF\xBB\xBF#!/usr/bin/env lua\nlocal value = 7\nreturn { Value = value, Fail = function() error('boom') end }");
		temp.WriteFile("Assets/Notes.txt", std::string(20000, 'n'));
		temp.WriteFile("FakeRuntime", "binary");
		const BrecciaTest::ScopedFakeVulkanSdk sdk(temp);

		std::string error;
		{
			Scene scene;
			Entity speaker = scene.CreateEntity("Speaker");
			auto& source = speaker.AddComponent<AudioSourceComponent>();
			source.Clip = "Assets/Audio/Beep.wav";
			source.PlayOnStart = true;
			// Streamed clips read the archive copy page by page, seeking back to the start as they loop.
			auto& music = scene.CreateEntity("Music").AddComponent<AudioSourceComponent>();
			music.Clip = "Assets/Audio/Beep.wav";
			music.Stream = true;
			music.Loop = true;
			music.PlayOnStart = true;
			scene.CreateEntity("Listener").AddComponent<AudioListenerComponent>();
			// Saving gives the clip and the scene sidecars: the scene refers to the clip by UUID.
			REQUIRE_MESSAGE(SceneSerializer::SaveScene(scene, temp.GetDirectory() / "Assets" / "Scenes" / "Main.bscene", error), error);
		}
		const auto sceneID = project->GetAssets().Ensure("Assets/Scenes/Main.bscene", error);
		REQUIRE_MESSAGE(sceneID, error);
		const auto clipID = project->GetAssets().Ensure("Assets/Audio/Beep.wav", error);
		REQUIRE_MESSAGE(clipID, error);
		project->GetConfig().StartScene = "Assets/Scenes/Main.bscene";
		REQUIRE_MESSAGE(project->Save(error), error);
		CHECK(FileSystem::ReadTextFile(project->GetProjectFilePath())->find(std::to_string(static_cast<uint64_t>(*sceneID))) != std::string::npos);

		const std::filesystem::path output = temp.GetDirectory().parent_path() / "ArchiveExportOut";
		std::filesystem::remove_all(output, copyError);
		ExportOptions options;
		options.RuntimeExecutable = temp.GetDirectory() / "FakeRuntime";
		options.Archive = true;
		ExportResult result = Exporter::Export(*project, output, options);
		REQUIRE_MESSAGE(result.Success, result.Error);
		REQUIRE(result.ArchiveStatistics);
		CHECK(FileSystem::Exists(output / Project::ArchiveFileName));
		CHECK_FALSE(FileSystem::Exists(output / Project::AssetDirectoryName));
		CHECK(FileSystem::Exists(output / Project::FileName));
		CHECK(result.ArchiveStatistics->ArchiveSize == std::filesystem::file_size(output / Project::ArchiveFileName));

		{
			const Ref<PackArchive> archive = PackArchive::Open(output / Project::ArchiveFileName, error);
			REQUIRE_MESSAGE(archive, error);
			CHECK(result.ArchiveStatistics->Entries == archive->GetEntries().size());
			// The opened archive's totals match what the writer reported.
			const PackArchiveStatistics opened = archive->GetStatistics();
			CHECK(opened.Entries == result.ArchiveStatistics->Entries);
			CHECK(opened.CompressedEntries == result.ArchiveStatistics->CompressedEntries);
			CHECK(opened.UncompressedSize == result.ArchiveStatistics->UncompressedSize);
			CHECK(opened.ArchiveSize == result.ArchiveStatistics->ArchiveSize);
			for (const PackArchive::Entry& entry : archive->GetEntries())
			{
				CHECK_MESSAGE(entry.Path.starts_with("Assets/"), entry.Path);
				CHECK_MESSAGE(!entry.Path.ends_with(".meta"), entry.Path);
			}
			CHECK(RequireEntry(*archive, "Assets/Scenes/Main.bscene").AssetID == static_cast<uint64_t>(*sceneID));
			CHECK(archive->FindAsset(static_cast<uint64_t>(*clipID)) == archive->Find("Assets/Audio/Beep.wav"));
			CHECK(archive->Find("Assets/Models/Sphere.bin"));
			CHECK(RequireEntry(*archive, "Assets/Notes.txt").Compression == PackArchive::Method::Zstd);
			CHECK(RequireEntry(*archive, "Assets/Textures/Icon.png").AssetID == 0);
		}

		// Switching forms removes the other one, so the runtime never reads a stale copy.
		options.Archive = false;
		result = Exporter::Export(*project, output, options);
		REQUIRE_MESSAGE(result.Success, result.Error);
		CHECK_FALSE(result.ArchiveStatistics);
		CHECK_FALSE(FileSystem::Exists(output / Project::ArchiveFileName));
		CHECK(FileSystem::Exists(output / "Assets" / "Audio" / "Beep.wav.meta"));
		options.Archive = true;
		result = Exporter::Export(*project, output, options);
		REQUIRE_MESSAGE(result.Success, result.Error);
		CHECK_FALSE(FileSystem::Exists(output / Project::AssetDirectoryName));

		// Play the export as the runtime does: mount, then load the project from the export folder.
		Project::SetActive(nullptr);
		AssetManager::Clear();
		UIFonts::Clear();
		struct Cleanup
		{
			~Cleanup()
			{
				FileSystem::Unmount();
				Project::SetActive(nullptr);
				AssetManager::Clear();
				UIFonts::Clear();
			}
		} cleanup;
		// Loose files beside the archive: the archive's copy wins, other files still come from disk.
		REQUIRE(FileSystem::WriteTextFile(output / "Assets" / "Scripts" / "Lib" / "Priority.lua", "return 'disk'"));
		REQUIRE(FileSystem::WriteTextFile(output / "Assets" / "Scripts" / "Lib" / "DiskOnly.lua", "return 'disk only'"));
		// Templates go in package.path order, each asking the archive and then the disk (as FileSystem reads one path):
		// "?.lua" on disk comes before "?/init.lua" in the archive.
		REQUIRE(FileSystem::WriteTextFile(output / "Assets" / "Scripts" / "Layered.lua", "return 'loose'"));
		const uint64_t beforeMount = Log::GetHistory().GetTotalCount();
		REQUIRE_MESSAGE(Exporter::MountExportedArchive(output, error), error);
		REQUIRE(FileSystem::GetMountedArchive(output));
		CHECK(BrecciaTest::CountLogMessages(beforeMount, "both Assets.bpak and an Assets folder") == 1);
		const Ref<Project> exported = Project::Load(output, error);
		REQUIRE_MESSAGE(exported, error);
		Project::SetActive(exported);
		// The start scene was stored by UUID; only the archive's index can resolve it (there are no .meta files).
		CHECK(exported->GetConfig().StartScene == "Assets/Scenes/Main.bscene");
		CHECK(exported->GetAssets().Find(*clipID) == "Assets/Audio/Beep.wav");
		// Ensure and Encode answer from the index too, and never write a sidecar beside the archive.
		const auto ensured = exported->GetAssets().Ensure("Assets/Audio/Beep.wav", error);
		REQUIRE_MESSAGE(ensured, error);
		CHECK(static_cast<uint64_t>(*ensured) == static_cast<uint64_t>(*clipID));
		CHECK_FALSE(exported->GetAssets().Ensure("Assets/Textures/Icon.png", error));
		CHECK(error.find("no UUID in the mounted archive") != std::string::npos);
		CHECK_FALSE(exported->GetAssets().Ensure("Assets/Textures/Missing.png", error));
		CHECK(error.find("asset does not exist") != std::string::npos);
		CHECK_FALSE(FileSystem::Exists(output / "Assets" / "Textures" / "Icon.png.meta"));
		nlohmann::json encoded;
		REQUIRE_MESSAGE(exported->GetAssets().Encode("Assets/Audio/Beep.wav", encoded, error), error);
		CHECK(encoded == nlohmann::json{ { "UUID", std::to_string(static_cast<uint64_t>(*clipID)) }, { "Path", "Assets/Audio/Beep.wav" } });
		REQUIRE_MESSAGE(exported->GetAssets().Encode("Assets/Textures/Icon.png", encoded, error), error);
		CHECK(encoded == "Assets/Textures/Icon.png");

		const Ref<Scene> scene = SceneSerializer::LoadScene(Project::ResolvePath(exported->GetConfig().StartScene), error);
		REQUIRE_MESSAGE(scene, error);
		Entity speaker = scene->FindEntityByName("Speaker");
		REQUIRE(speaker);
		CHECK(speaker.GetComponent<AudioSourceComponent>().Clip == "Assets/Audio/Beep.wav");

		const Ref<MeshSource> mesh = AssetManager::GetMesh("Assets/Models/Sphere.gltf");
		REQUIRE_MESSAGE(mesh, AssetManager::GetError("Assets/Models/Sphere.gltf"));
		REQUIRE_FALSE(mesh->Materials.empty());
		CHECK(mesh->Materials[0].AlbedoMap == "Assets/Models/Sphere Albedo.png");
		CHECK_MESSAGE(AssetManager::GetTexture(mesh->Materials[0].AlbedoMap), AssetManager::GetError(mesh->Materials[0].AlbedoMap));
		CHECK_MESSAGE(AssetManager::GetTexture("Assets/Textures/Icon.png"), AssetManager::GetError("Assets/Textures/Icon.png"));
		CHECK_MESSAGE(AssetManager::GetTexture("Assets/Textures/UASTC.ktx2"), AssetManager::GetError("Assets/Textures/UASTC.ktx2"));
		CHECK(UIFonts::CheckFile("Assets/Fonts/Karla.ttf").empty());
		CHECK_MESSAGE(AssetManager::GetMesh("Assets/Models/Shared.gltf"), AssetManager::GetError("Assets/Models/Shared.gltf"));
		CHECK_MESSAGE(AssetManager::GetTexture(UnicodeTexture), AssetManager::GetError(UnicodeTexture));
		// Archive paths match letter case exactly on every platform, as on Linux.
		CHECK_FALSE(FileSystem::ReadBinaryFile(output / "Assets" / "textures" / "Icon.png"));

		{
			const BrecciaTest::HeadlessAudio audio;
			scene->OnRuntimeStart();
			REQUIRE(scene->GetAudioSystem());
			CHECK(scene->GetAudioSystem()->IsPlaying(speaker));
			Entity music = scene->FindEntityByName("Music");
			REQUIRE(music);
			CHECK(scene->GetAudioSystem()->IsPlaying(music));
			std::vector<float> mix;
			// Twice the clip's length: the speaker's clip ends half way, so sound in the last quarter comes from the stream
			// wrapping around to the start of the archive copy.
			REQUIRE(AudioEngine::ReadDeviceless(1.0f, mix));
			CHECK(scene->GetAudioSystem()->IsPlaying(music));
			REQUIRE_FALSE(mix.empty());
			float lateLevel = 0.0f;
			for (size_t i = mix.size() * 3 / 4; i < mix.size(); i++)
				lateLevel = std::max(lateLevel, std::abs(mix[i]));
			CHECK(lateLevel > 0.01f);
			// Modules come from the archive too, found through both package.path templates.
			CHECK(BrecciaTest::Eval(*scene, "return require('Assets.Scripts.Lib.Util').Triple(3)") == "9");
			CHECK(BrecciaTest::Eval(*scene, "return require('Assets.Scripts.Pkg').Name") == "package");
			CHECK(BrecciaTest::Eval(*scene, "return require('Assets.Scripts.Lib.Priority')") == "archive");
			CHECK(BrecciaTest::Eval(*scene, "return require('Assets.Scripts.Lib.DiskOnly')") == "disk only");
			CHECK(BrecciaTest::Eval(*scene, "return require('Assets.Scripts.Layered')") == "loose");
			CHECK(BrecciaTest::Eval(*scene, "return require('Assets.Scripts.Lib.Shebang').Value") == "7");
			const std::string failure = BrecciaTest::Eval(*scene, "local ok, message = pcall(require('Assets.Scripts.Lib.Shebang').Fail) return message");
			CHECK_MESSAGE(failure.find("Shebang.lua:3: boom") != std::string::npos, failure);
			const std::string bad = BrecciaTest::Eval(*scene, "return require('Assets.Scripts.Lib.Bad')");
			CHECK_MESSAGE(bad.find("error loading module 'Assets.Scripts.Lib.Bad' from file") != std::string::npos, bad);
			CHECK(bad.find("no file '") == std::string::npos);
			const std::string missing = BrecciaTest::Eval(*scene, "return require('Assets.Scripts.Missing')");
			CHECK_MESSAGE(missing.find("no file '") != std::string::npos, missing);
			// require puts "\n\t" before each searcher's lines; the engine's file searcher must not add another (no empty line).
			CHECK_MESSAGE(missing.find("\n\t\n") == std::string::npos, missing);
			scene->OnRuntimeStop();
		}
	}

	TEST_CASE("Texture and material loads report a damaged archive entry")
	{
		BrecciaTest::TempProject temp("ArchiveDamagedLoads");
		REQUIRE(temp.IsValid());
		const std::vector<uint8_t> png = EncodeIconPng(MakeDefaultIcon(8));
		const std::filesystem::path file = temp.GetDirectory() / "Damaged.bpak";
		std::vector<uint8_t> bytes = BuildArchive({ { "Assets/Textures/T.png", png, 0 }, { "Assets/Materials/M.bmaterial", Bytes(R"({"Format":"BrecciaMaterial","Version":1})"), 0 } }, file);
		std::string error;
		Ref<PackArchive> archive = PackArchive::OpenMemory(bytes, error);
		REQUIRE_MESSAGE(archive, error);
		for (const char* path : { "Assets/Textures/T.png", "Assets/Materials/M.bmaterial" })
			bytes[static_cast<size_t>(RequireEntry(*archive, path).Offset)] ^= 0x5A;
		archive = PackArchive::OpenMemory(bytes, error);
		REQUIRE_MESSAGE(archive, error);
		BrecciaTest::ScopedMount mount(temp.GetDirectory(), archive);
		AssetManager::Clear();
		CHECK_FALSE(AssetManager::GetTexture("Assets/Textures/T.png"));
		CHECK_MESSAGE(AssetManager::GetError("Assets/Textures/T.png").find("damaged in the archive") != std::string::npos, AssetManager::GetError("Assets/Textures/T.png"));
		CHECK_FALSE(AssetManager::GetMaterial("Assets/Materials/M.bmaterial"));
		CHECK_MESSAGE(AssetManager::GetError("Assets/Materials/M.bmaterial").find("damaged in the archive") != std::string::npos, AssetManager::GetError("Assets/Materials/M.bmaterial"));
		AssetManager::Clear();
	}

	TEST_CASE("Archived exports refuse what an archive cannot hold and report assets without UUIDs")
	{
		BrecciaTest::TempProject temp("ArchiveExportProblems");
		REQUIRE(temp.IsValid());
		const Ref<Project>& project = Project::GetActive();
		temp.WriteFile("Assets/Scenes/Main.bscene", R"({"Entities":[]})");
		temp.WriteFile("FakeRuntime", "binary");
		const BrecciaTest::ScopedFakeVulkanSdk sdk(temp);
		project->GetConfig().StartScene = "Assets/Scenes/Main.bscene";
		const std::filesystem::path output = temp.GetDirectory().parent_path() / "ArchiveExportProblemsOut";
		std::error_code error;
		std::filesystem::remove_all(output, error);
		ExportOptions options;
		options.RuntimeExecutable = temp.GetDirectory() / "FakeRuntime";
		options.Archive = true;

		// Two sidecars with one UUID (a copied .meta): both assets go in without it, and the export says so.
		std::string registryError;
		REQUIRE(project->GetAssets().Ensure("Assets/Scenes/Main.bscene", registryError));
		temp.WriteFile("Assets/Scenes/Copy.bscene", R"({"Entities":[]})");
		std::filesystem::copy_file(temp.GetDirectory() / "Assets/Scenes/Main.bscene.meta", temp.GetDirectory() / "Assets/Scenes/Copy.bscene.meta", error);
		REQUIRE_FALSE(error);
		ExportResult result = Exporter::Export(*project, output, options);
		REQUIRE_MESSAGE(result.Success, result.Error);
		const auto warning = std::find_if(result.Warnings.begin(), result.Warnings.end(), [](const std::string& text) { return text.find("'Assets/Scenes/Copy.bscene' is archived without its UUID") != std::string::npos; });
		CHECK_MESSAGE(warning != result.Warnings.end(), "warnings: ", result.Warnings.size());
		std::filesystem::remove(temp.GetDirectory() / "Assets/Scenes/Copy.bscene.meta", error);

		// A failure after the archive is written (the runtime cannot be copied over a folder in the way) reports no
		// archive statistics: they describe a finished export only.
#if defined(_WIN32)
		const std::filesystem::path executable = output / "ArchiveExportProblems.exe";
#else
		const std::filesystem::path executable = output / "ArchiveExportProblems";
#endif
		std::filesystem::remove(executable, error);
		std::filesystem::create_directories(executable / "InTheWay", error);
		result = Exporter::Export(*project, output, options);
		CHECK_FALSE(result.Success);
		CHECK_MESSAGE(result.Error.find("cannot copy the runtime") != std::string::npos, result.Error);
		CHECK(FileSystem::Exists(output / Project::ArchiveFileName));
		CHECK_FALSE(result.ArchiveStatistics);
		// Back to a previous export of this game (its executable in place), which the next exports may replace.
		std::filesystem::remove_all(executable, error);
		REQUIRE(FileSystem::WriteTextFile(executable, "binary"));

#if !defined(_WIN32)
		// A linked folder: the folder export copies it, so the archive must not silently leave it out.
		BrecciaTest::TempDirectory linked("ArchiveExportLinkedFolder");
		temp.WriteFile("../ArchiveExportLinkedFolder/Inside.txt", "linked");
		std::filesystem::create_directory_symlink(linked.GetPath(), temp.GetDirectory() / "Assets" / "Linked");
		result = Exporter::Export(*project, output, options);
		CHECK_FALSE(result.Success);
		CHECK_MESSAGE(result.Error.find("'Assets/Linked' is a linked folder") != std::string::npos, result.Error);
		std::filesystem::remove(temp.GetDirectory() / "Assets" / "Linked", error);

		// A link to nothing is refused rather than left out; a link to a file is read through, as a folder export copies it.
		std::filesystem::create_symlink(temp.GetDirectory() / "Nowhere.txt", temp.GetDirectory() / "Assets" / "Dangling.txt");
		result = Exporter::Export(*project, output, options);
		CHECK_FALSE(result.Success);
		CHECK_MESSAGE(result.Error.find("'Assets/Dangling.txt' is a broken link") != std::string::npos, result.Error);
		std::filesystem::remove(temp.GetDirectory() / "Assets" / "Dangling.txt", error);
		temp.WriteFile("Target.txt", "linked file");
		std::filesystem::create_symlink(temp.GetDirectory() / "Target.txt", temp.GetDirectory() / "Assets" / "LinkedFile.txt");
		result = Exporter::Export(*project, output, options);
		REQUIRE_MESSAGE(result.Success, result.Error);
		{
			std::string archiveError;
			const Ref<PackArchive> archive = PackArchive::Open(output / Project::ArchiveFileName, archiveError);
			REQUIRE_MESSAGE(archive, archiveError);
			CHECK(archive->Read(RequireEntry(*archive, "Assets/LinkedFile.txt"), archiveError) == Bytes("linked file"));
		}
		std::filesystem::remove(temp.GetDirectory() / "Assets" / "LinkedFile.txt", error);

		// A name with a colon is a valid file name here but not an archive path.
		temp.WriteFile("Assets/Bad:Name.txt", "colon");
		result = Exporter::Export(*project, output, options);
		CHECK_FALSE(result.Success);
		CHECK_MESSAGE(result.Error.find("'Assets/Bad:Name.txt' cannot be stored in an archive") != std::string::npos, result.Error);
		CHECK_FALSE(result.ArchiveStatistics);
#endif
		std::filesystem::remove_all(output, error);
	}

	TEST_CASE("The runtime refuses an archive that does not open")
	{
		BrecciaTest::TempDirectory directory("ArchiveBroken");
		std::string error;
		// Without an archive nothing is mounted and nothing fails.
		CHECK(Exporter::MountExportedArchive(directory.GetPath(), error));
		CHECK_FALSE(FileSystem::GetMountedArchive(directory.GetPath()));
		REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / Project::ArchiveFileName, "not an archive at all, but long enough for a header"));
		CHECK_FALSE(Exporter::MountExportedArchive(directory.GetPath(), error));
		CHECK(error.find("not a .bpak archive") != std::string::npos);
		CHECK_FALSE(FileSystem::GetMountedArchive(directory.GetPath()));
	}

	TEST_CASE("Stored entries read in part, from a file and from memory")
	{
		BrecciaTest::TempDirectory directory("ArchiveStoredReads");
		const std::vector<uint8_t> noise = Noise(5000);
		const std::vector<uint8_t> bytes = BuildArchive({ { "Assets/Noise.bin", noise, 0 }, { "Assets/Text.txt", Repetitive(5000), 0 } }, directory.GetPath() / "Stored.bpak");
		std::string error;
		const Ref<PackArchive> file = PackArchive::Open(directory.GetPath() / "Stored.bpak", error);
		REQUIRE_MESSAGE(file, error);
		const Ref<PackArchive> memory = PackArchive::OpenMemory(bytes, error);
		REQUIRE_MESSAGE(memory, error);
		for (const Ref<PackArchive>& archive : { file, memory })
		{
			const PackArchive::Entry& stored = RequireEntry(*archive, "Assets/Noise.bin");
			REQUIRE(stored.Compression == PackArchive::Method::Store);
			std::vector<uint8_t> part(100);
			CHECK(archive->ReadStored(stored, 200, part.data(), part.size(), error) == size_t{ 100 });
			CHECK(std::equal(part.begin(), part.end(), noise.begin() + 200));
			// The end of the entry, not the next entry's bytes.
			CHECK(archive->ReadStored(stored, 4950, part.data(), part.size(), error) == size_t{ 50 });
			CHECK(std::equal(part.begin(), part.begin() + 50, noise.begin() + 4950));
			CHECK(archive->ReadStored(stored, 5000, part.data(), part.size(), error) == size_t{ 0 });
			CHECK(archive->ReadStored(stored, UINT64_MAX, part.data(), part.size(), error) == size_t{ 0 });

			error.clear();
			CHECK_FALSE(archive->ReadStored(RequireEntry(*archive, "Assets/Text.txt"), 0, part.data(), part.size(), error));
			CHECK(error.find("compressed") != std::string::npos);
			// A forged copy's offset and size were never checked.
			PackArchive::Entry forged = stored;
			forged.StoredSize = UINT64_MAX;
			error.clear();
			CHECK_FALSE(archive->ReadStored(forged, 0, part.data(), part.size(), error));
			CHECK(error.find("not an entry of this archive") != std::string::npos);
		}
	}

#if !defined(_WIN32)
	TEST_CASE("A stored read of a file cut short reports it")
	{
		BrecciaTest::TempDirectory directory("ArchiveStoredCut");
		const std::filesystem::path file = directory.GetPath() / "Cut.bpak";
		const std::vector<uint8_t> noise = Noise(1 << 20);
		BuildArchive({ { "Assets/Noise.bin", noise, 0 } }, file);
		std::string error;
		const Ref<PackArchive> archive = PackArchive::Open(file, error);
		REQUIRE_MESSAGE(archive, error);
		std::error_code cutError;
		std::filesystem::resize_file(file, 1000, cutError);
		REQUIRE_FALSE(cutError);
		// Past what the stream has buffered.
		std::vector<uint8_t> part(100);
		error.clear();
		CHECK_FALSE(archive->ReadStored(RequireEntry(*archive, "Assets/Noise.bin"), 900000, part.data(), part.size(), error));
		CHECK(error.find("cannot read") != std::string::npos);
	}
#endif

	TEST_CASE("Stored clips stream from the archive, unlike whole reads that check their hash")
	{
		BrecciaTest::TempProject temp("ArchiveStreamedAudio");
		REQUIRE(temp.IsValid());
		// Noise for samples, which zstd cannot shrink, behind the 44 bytes of a WAV header. One clip is long enough to stream.
		const auto noiseWav = [](float seconds) {
			std::string wav = BrecciaTest::MakeWav(seconds);
			const std::vector<uint8_t> noise = Noise(wav.size() - 44);
			std::copy(noise.begin(), noise.end(), wav.begin() + 44);
			return wav;
		};
		const std::filesystem::path file = temp.GetDirectory() / Project::ArchiveFileName;
		std::vector<uint8_t> bytes = BuildArchive({ { "Assets/Audio/Noise.wav", Bytes(noiseWav(60.0f)), 0 }, { "Assets/Audio/Short.wav", Bytes(noiseWav(0.5f)), 0 }, { "Assets/Audio/Whole.wav", Bytes(noiseWav(0.5f)), 0 } }, file);
		std::string error;
		Ref<PackArchive> archive = PackArchive::Open(file, error);
		REQUIRE_MESSAGE(archive, error);
		const PackArchive::Entry stored = RequireEntry(*archive, "Assets/Audio/Noise.wav");
		const PackArchive::Entry shortClip = RequireEntry(*archive, "Assets/Audio/Short.wav");
		REQUIRE(stored.Compression == PackArchive::Method::Store);
		REQUIRE(shortClip.Compression == PackArchive::Method::Store);

		// A flipped sample fails every whole read: the long clip playing anyway shows it was never read whole, and the short
		// one, which is, must not play.
		bytes[static_cast<size_t>(stored.Offset + stored.Size - 1)] ^= 0xFF;
		bytes[static_cast<size_t>(shortClip.Offset + shortClip.Size - 1)] ^= 0xFF;
		archive = nullptr;
		REQUIRE(FileSystem::WriteBinaryFile(file, bytes.data(), bytes.size()));
		archive = PackArchive::Open(file, error);
		REQUIRE_MESSAGE(archive, error);
		CHECK_FALSE(archive->Read(RequireEntry(*archive, "Assets/Audio/Noise.wav"), error));
		CHECK(error.find("hash mismatch") != std::string::npos);

		const BrecciaTest::ScopedMount mount(temp.GetDirectory(), archive);
		const Ref<Scene> scene = CreateRef<Scene>();
		auto& decoded = scene->CreateEntity("Decoded").AddComponent<AudioSourceComponent>();
		decoded.Clip = "Assets/Audio/Noise.wav";
		decoded.PlayOnStart = true;
		auto& streamed = scene->CreateEntity("Streamed").AddComponent<AudioSourceComponent>();
		streamed.Clip = "Assets/Audio/Noise.wav";
		streamed.PlayOnStart = true;
		streamed.Stream = true;
		// Seeks back to the entry's start, not the archive's.
		streamed.Loop = true;
		auto& damaged = scene->CreateEntity("Damaged").AddComponent<AudioSourceComponent>();
		damaged.Clip = "Assets/Audio/Short.wav";
		damaged.PlayOnStart = true;
		// As short and undamaged: what keeps the damaged clip's check from passing for some other reason.
		auto& whole = scene->CreateEntity("Whole").AddComponent<AudioSourceComponent>();
		whole.Clip = "Assets/Audio/Whole.wav";
		whole.PlayOnStart = true;
		scene->CreateEntity("Listener").AddComponent<AudioListenerComponent>();
		const BrecciaTest::HeadlessAudio audio;
		const uint64_t logged = Log::GetHistory().GetTotalCount();
		scene->OnRuntimeStart();
		uint64_t next = 0;
		const std::vector<LogMessage> messages = Log::GetHistory().GetMessagesSince(logged, next);
		CHECK(std::any_of(messages.begin(), messages.end(), [](const LogMessage& message) { return message.Text.find("'Assets/Audio/Short.wav' is damaged in the archive (hash mismatch)") != std::string::npos; }));
		REQUIRE(scene->GetAudioSystem());
		CHECK(scene->GetAudioSystem()->IsPlaying(scene->FindEntityByName("Whole")));
		CHECK(scene->GetAudioSystem()->IsPlaying(scene->FindEntityByName("Decoded")));
		CHECK(scene->GetAudioSystem()->IsPlaying(scene->FindEntityByName("Streamed")));
		CHECK(scene->GetAudioSystem()->IsStreaming(scene->FindEntityByName("Streamed")));
		CHECK_FALSE(scene->GetAudioSystem()->IsPlaying(scene->FindEntityByName("Damaged")));
		std::vector<float> mix;
		REQUIRE(AudioEngine::ReadDeviceless(1.0f, mix));
		REQUIRE_FALSE(mix.empty());
		float lateLevel = 0.0f;
		for (size_t i = mix.size() * 3 / 4; i < mix.size(); i++)
			lateLevel = std::max(lateLevel, std::abs(mix[i]));
		CHECK(lateLevel > 0.01f);
		scene->OnRuntimeStop();
	}

	TEST_CASE("An archive's bytes do not depend on how Write batches its entries")
	{
		BrecciaTest::TempDirectory directory("ArchiveBatches");
		// More entries than one batch holds, in no particular order.
		std::vector<std::tuple<std::string, std::vector<uint8_t>, uint64_t>> entries;
		entries.reserve(PackArchiveWriter::MaxBatchEntries + 44);
		for (uint32_t i = 0; i < PackArchiveWriter::MaxBatchEntries + 44; i++)
			entries.emplace_back("Assets/File" + std::to_string((i * 37) % 1000) + ".bin", i % 3 == 0 ? Noise(200 + i, i) : Repetitive(300 + i), i + 1);
		const std::vector<uint8_t> expected = BuildArchive(entries, directory.GetPath() / "Default.bpak");
		PackArchiveWriter writer;
		std::string error;
		for (const auto& [path, data, id] : entries)
			REQUIRE_MESSAGE(writer.AddData(path, data, id, error), error);
		std::optional<PackArchiveStatistics> first;
		for (const uint64_t batchBytes : { uint64_t{ 1 }, uint64_t{ 2000 }, PackArchiveWriter::DefaultBatchBytes })
		{
			const std::filesystem::path file = directory.GetPath() / ("Batch" + std::to_string(batchBytes) + ".bpak");
			const auto statistics = writer.Write(file, error, 9, batchBytes);
			REQUIRE_MESSAGE(statistics, error);
			CHECK(FileSystem::ReadBinaryFile(file) == expected);
			if (first)
			{
				CHECK(statistics->Entries == first->Entries);
				CHECK(statistics->CompressedEntries == first->CompressedEntries);
				CHECK(statistics->ArchiveSize == first->ArchiveSize);
			}
			first = statistics;
		}
		// Files read on jobs go in the same place as bytes added directly.
		const Ref<PackArchive> archive = PackArchive::OpenMemory(expected, error);
		REQUIRE_MESSAGE(archive, error);
		PackArchiveWriter fromFiles;
		for (const auto& [path, data, id] : entries)
		{
			const std::filesystem::path source = directory.GetPath() / "Sources" / FileSystem::PathFromUtf8(path);
			REQUIRE(FileSystem::WriteBinaryFile(source, data.data(), data.size()));
			REQUIRE_MESSAGE(fromFiles.AddFile(path, source, id, error), error);
		}
		REQUIRE_MESSAGE(fromFiles.Write(directory.GetPath() / "Files.bpak", error, 9, 2000), error);
		CHECK(FileSystem::ReadBinaryFile(directory.GetPath() / "Files.bpak") == expected);
	}

	TEST_CASE("Batches hold what fits, never fewer than one entry")
	{
		const auto ends = [](const std::vector<uint64_t>& sizes, uint64_t batchBytes) {
			std::vector<size_t> found;
			for (size_t begin = 0; begin < sizes.size(); begin = found.back())
				found.push_back(PackArchiveWriter::FindBatchEnd(sizes, begin, batchBytes));
			return found;
		};
		CHECK(ends({ 100, 100, 100 }, 200) == std::vector<size_t>{ 2, 3 });
		CHECK(ends({ 100, 100, 100 }, 300) == std::vector<size_t>{ 3 });
		// A larger entry goes alone, and a batch past its limit takes nothing more.
		CHECK(ends({ 1, 300, 1 }, 250) == std::vector<size_t>{ 1, 2, 3 });
		CHECK(ends({ 300, 1, 1 }, 250) == std::vector<size_t>{ 1, 3 });
		CHECK(ends({ UINT64_MAX, 1 }, 250) == std::vector<size_t>{ 1, 2 });
		CHECK(ends({ 1, 1 }, 0) == std::vector<size_t>{ 1, 2 });
		CHECK(ends(std::vector<uint64_t>(PackArchiveWriter::MaxBatchEntries + 1, 0), 10) == std::vector<size_t>{ PackArchiveWriter::MaxBatchEntries, PackArchiveWriter::MaxBatchEntries + 1 });
	}

	TEST_CASE("Write reports the first entry in path order that fails, in any batch, and can run again")
	{
		BrecciaTest::TempDirectory directory("ArchiveBatchFailure");
		const std::filesystem::path sources = directory.GetPath() / "Sources";
		const std::filesystem::path output = directory.GetPath() / "Output";
		REQUIRE(FileSystem::WriteTextFile(sources / "A", std::string(100, 'a')));
		const std::filesystem::path file = output / "Out.bpak";
		PackArchiveWriter writer;
		std::string error;
		REQUIRE(writer.AddFile("Assets/A.bin", sources / "A", 0, error));
		REQUIRE(writer.AddFile("Assets/C.bin", sources / "C", 0, error));
		REQUIRE(writer.AddFile("Assets/B.bin", sources / "B", 0, error));
		// 0 bytes puts each in a batch of its own, so A has been written when B fails.
		for (const uint64_t batchBytes : { uint64_t{ 0 }, PackArchiveWriter::DefaultBatchBytes })
		{
			error.clear();
			CHECK_FALSE(writer.Write(file, error, 9, batchBytes));
			CHECK_MESSAGE(error.find(FileSystem::PathToUtf8(sources / "B")) != std::string::npos, error);
			// Neither the archive nor its temporary file stays behind.
			CHECK(std::filesystem::is_empty(output));
		}
		REQUIRE(FileSystem::WriteTextFile(sources / "B", "b"));
		REQUIRE(FileSystem::WriteTextFile(sources / "C", "c"));
		REQUIRE_MESSAGE(writer.Write(file, error), error);
		const std::vector<uint8_t> expected = BuildArchive({ { "Assets/A.bin", Bytes(std::string(100, 'a')), 0 }, { "Assets/B.bin", Bytes("b"), 0 }, { "Assets/C.bin", Bytes("c"), 0 } }, directory.GetPath() / "Clean.bpak");
		CHECK(FileSystem::ReadBinaryFile(file) == expected);
	}

	TEST_CASE("A streamable file is stored, however well it compresses")
	{
		BrecciaTest::TempDirectory directory("ArchiveStoreUncompressed");
		const std::vector<uint8_t> text = Repetitive(5000);
		REQUIRE(FileSystem::WriteBinaryFile(directory.GetPath() / "Source", text.data(), text.size()));
		PackArchiveWriter writer;
		std::string error;
		REQUIRE(writer.AddFile("Assets/Stored.bin", directory.GetPath() / "Source", 0, error, PackArchiveWriter::Storage::Streamable));
		REQUIRE(writer.AddFile("Assets/Zipped.bin", directory.GetPath() / "Source", 0, error));
		REQUIRE_MESSAGE(writer.Write(directory.GetPath() / "Out.bpak", error), error);
		const Ref<PackArchive> archive = PackArchive::Open(directory.GetPath() / "Out.bpak", error);
		REQUIRE_MESSAGE(archive, error);
		CHECK(RequireEntry(*archive, "Assets/Stored.bin").Compression == PackArchive::Method::Store);
		CHECK(RequireEntry(*archive, "Assets/Zipped.bin").Compression == PackArchive::Method::Zstd);
		CHECK(archive->Read(RequireEntry(*archive, "Assets/Stored.bin"), error) == text);
	}

	TEST_CASE("Archived exports leave out originals only their compressed replacement stands for")
	{
		BrecciaTest::TempProject temp("ArchiveOmittedOriginals");
		REQUIRE(temp.IsValid());
		const Ref<Project>& project = Project::GetActive();
		std::string error;
		temp.WriteFile("Assets/Scenes/Main.bscene", R"({"Entities":[]})");
		for (const char* name : { "Wall", "Named", "Spaced Out", "ByUuid", "Icon" })
		{
			temp.WriteFile(std::string("Assets/Textures/") + name + ".png", "original");
			temp.WriteFile(std::string("Assets/Textures/") + name + ".ktx2", "replacement");
		}
		// A replacement elsewhere, or in another case (archive paths match exactly), is not recognized, and an image without
		// one is never touched.
		temp.WriteFile("Assets/Textures/Moved.png", "original");
		temp.WriteFile("Assets/Compressed/Moved.ktx2", "replacement");
		temp.WriteFile("Assets/Textures/Lonely.png", "original");
		temp.WriteFile("Assets/Textures/Upper.png", "original");
		temp.WriteFile("Assets/Textures/Upper.KTX2", "replacement");
		// Every image type counts, and what is left out comes back sorted, whatever order the folder lists.
		for (const char* name : { "Edge.bmp", "Flat.PNG", "Brick.jpg", "Dirt.tga", "Clay.jpeg", "Not\xC3\xA9.png" })
		{
			const std::string path = std::string("Assets/Textures/") + name;
			temp.WriteFile(path, "original");
			temp.WriteFile(path.substr(0, path.rfind('.')) + ".ktx2", "replacement");
		}
		// Mentions a walk over declared asset fields would miss, and escapes of every kind the readers undo.
		const std::pair<const char*, const char*> mentioned[] = {
			{ "Spaced Out.png", "Assets/Models/Ship.gltf" },
			{ "Caf\xC3\xA9.png", "Assets/Models/Lower.gltf" },
			{ "Wall (1).png", "Assets/Models/Partly.gltf" },
			{ "Json\xC3\xA9.png", "Assets/Materials/Json.bmaterial" },
			{ "Emoji\xF0\x9F\x98\x80.png", "Assets/Materials/Emoji.bmaterial" },
			{ "Lua\xC3\xA9.png", "Assets/Scripts/Decimal.lua" },
			{ "Hex\xC3\xA9.png", "Assets/Scripts/Hex.lua" },
			{ "Brace\xC3\xA9.png", "Assets/Scripts/Brace.lua" },
			// A name that looks escaped itself, written as it is, also beside a real escape.
			{ "Sale%41.png", "Assets/Scripts/Literal.lua" },
			// A backslash that escapes nothing: a Windows path in a note, where \t is no tab.
			{ "test.png", "Assets/Notes.txt" },
			{ "50%20off \xC3\xA9.png", "Assets/Scripts/Literal.lua" },
			{ "Bob's Tile.png", "Assets/Scripts/Quoted.lua" },
			{ "Say Hi.png", "Assets/Scripts/Quoted.lua" },
			{ "Split.png", "Assets/Scripts/Quoted.lua" },
			{ "Joined.png", "Assets/Scripts/Quoted.lua" },
			{ "Tabbed Name.png", "Assets/Scripts/Quoted.lua" },
			{ "Zeros.png", "Assets/Scripts/Quoted.lua" },
			{ "Twice Over.png", "Assets/Materials/Twice.bmaterial" },
		};
		for (const auto& [image, mentionedBy] : mentioned)
		{
			const std::string path = std::string("Assets/Textures/") + image;
			temp.WriteFile(path, "original");
			temp.WriteFile(path.substr(0, path.rfind('.')) + ".ktx2", "replacement");
		}
		temp.WriteFile("Assets/Scripts/Sky.lua", "local texture = 'Textures/Named.png'");
		temp.WriteFile("Assets/Models/Ship.gltf", R"({"images":[{"uri":"../Textures/Spaced%20Out.png"}]})");
		temp.WriteFile("Assets/Models/Lower.gltf", R"({"images":[{"uri":"../Textures/Caf%c3%a9.png"}]})");
		temp.WriteFile("Assets/Models/Partly.gltf", R"({"images":[{"uri":"../Textures/Wall%20(1).png"}]})");
		temp.WriteFile("Assets/Materials/Json.bmaterial", R"({"Albedo":"Assets/Textures/Json\u00e9.png"})");
		temp.WriteFile("Assets/Materials/Emoji.bmaterial", R"({"Albedo":"Assets/Textures/Emoji\ud83d\ude00.png"})");
		temp.WriteFile("Assets/Scripts/Decimal.lua", R"(return "Lua\195\169.png")");
		temp.WriteFile("Assets/Scripts/Hex.lua", R"(return "Hex\xC3\xA9.png")");
		temp.WriteFile("Assets/Scripts/Brace.lua", R"(return "Brace\u{E9}.png")");
		// An escaped backslash ends the escape: this names no file of the project, so Not\xC3\xA9.png stays unmentioned.
		temp.WriteFile("Assets/Scripts/Literal.lua", R"(return "Not\\u00e9.png", "Sale%41.png", "50%20off \u{E9}.png")");
		temp.WriteFile("Assets/Notes.txt", R"(See Assets\Textures\test.png)");
		temp.WriteFile("Assets/Scripts/Quoted.lua", R"(return 'Bob\'s Tile.png', "Say \"Hi\".png", "Spl\z
			it.png", "Joined" .. ".png", "Tabbed\tName.png", "\u{00000000005A}eros.png")");
		// A percent escape that is JSON-escaped in turn.
		temp.WriteFile("Assets/Materials/Twice.bmaterial", R"({"Albedo":"Assets/Textures/Twice\u002520Over.png"})");
		// Audio that is compressed already stays streamable, whatever zstd could still save; other audio is compressed.
		temp.WriteFile("Assets/Audio/Quiet.mp3", std::string(5000, 'm'));
		temp.WriteFile("Assets/Audio/Lossless.FLAC", std::string(5000, 'f'));
		temp.WriteFile("Assets/Audio/Raw.wav", std::string(5000, 'w'));
		const auto id = project->GetAssets().Ensure("Assets/Textures/ByUuid.png", error);
		REQUIRE_MESSAGE(id, error);
		temp.WriteFile("Assets/Materials/Old.bmaterial", "{\"Albedo\":{\"UUID\":\"" + std::to_string(static_cast<uint64_t>(*id)) + "\",\"Path\":\"Assets/Textures/Renamed.png\"}}");
		project->GetConfig().StartScene = "Assets/Scenes/Main.bscene";
		project->GetConfig().Icon = "Assets/Textures/Icon.png";
		REQUIRE_MESSAGE(project->Save(error), error);
		temp.WriteFile("FakeRuntime", "binary");
		const BrecciaTest::ScopedFakeVulkanSdk sdk(temp);

		ExportOptions options;
		options.RuntimeExecutable = temp.GetDirectory() / "FakeRuntime";
		options.Archive = true;
		const std::filesystem::path output = temp.GetDirectory().parent_path() / "ArchiveOmittedOriginalsOut";
		std::error_code removeError;
		std::filesystem::remove_all(output, removeError);
		ExportResult result;
		Ref<PackArchive> archive;
		const auto exportedPaths = [&]() {
			// Windows will not replace a file that is open.
			archive = nullptr;
			result = Exporter::Export(*project, output, options);
			REQUIRE_MESSAGE(result.Success, result.Error);
			std::string openError;
			archive = PackArchive::Open(output / Project::ArchiveFileName, openError);
			REQUIRE_MESSAGE(archive, openError);
			std::vector<std::string> paths;
			paths.reserve(archive->GetEntries().size());
			for (const PackArchive::Entry& entry : archive->GetEntries())
				paths.push_back(entry.Path);
			return paths;
		};
		const auto holds = [](const std::vector<std::string>& paths, const std::string& path) { return std::find(paths.begin(), paths.end(), path) != paths.end(); };

		const std::vector<std::string> paths = exportedPaths();
		const std::vector<std::string> omitted = { "Assets/Textures/Brick.jpg", "Assets/Textures/Clay.jpeg", "Assets/Textures/Dirt.tga", "Assets/Textures/Edge.bmp", "Assets/Textures/Flat.PNG", "Assets/Textures/Not\xC3\xA9.png", "Assets/Textures/Wall.png" };
		CHECK(result.ArchiveOmitted == omitted);
		CHECK(result.ArchiveStatistics->Entries == paths.size());
		for (const std::string& path : omitted)
			CHECK_MESSAGE(!holds(paths, path), path);
		CHECK(holds(paths, "Assets/Textures/Wall.ktx2"));
		for (const char* kept : { "Named.png", "ByUuid.png", "Icon.png", "Moved.png", "Lonely.png", "Upper.png" })
			CHECK_MESSAGE(holds(paths, std::string("Assets/Textures/") + kept), kept);
		for (const auto& [image, mentionedBy] : mentioned)
		{
			const std::string path = std::string("Assets/Textures/") + image + ", which " + mentionedBy + " mentions";
			CHECK_MESSAGE(holds(paths, std::string("Assets/Textures/") + image), path);
		}
		// Whoever exports hears what is gone, and where a long list ends.
		REQUIRE(result.Warnings.size() == 1);
		CHECK(result.Warnings[0].find("keepOriginals") != std::string::npos);
		CHECK(result.Warnings[0].find("Assets/Textures/Brick.jpg") != std::string::npos);
		CHECK(result.Warnings[0].find("and 2 more") != std::string::npos);
		for (const char* path : { "Assets/Audio/Quiet.mp3", "Assets/Audio/Lossless.FLAC" })
			CHECK_MESSAGE(RequireEntry(*archive, path).Compression == PackArchive::Method::Store, path);
		CHECK(RequireEntry(*archive, "Assets/Audio/Raw.wav").Compression == PackArchive::Method::Zstd);

		options.KeepOriginals = true;
		const std::vector<std::string> everything = exportedPaths();
		CHECK(result.ArchiveOmitted.empty());
		CHECK(result.Warnings.empty());
		CHECK(holds(everything, "Assets/Textures/Wall.png"));
		CHECK(everything.size() == paths.size() + omitted.size());

		// Only an archive leaves files out, so asking to keep them without one is a mistake.
		options.Archive = false;
		result = Exporter::Export(*project, output, options);
		CHECK_FALSE(result.Success);
		CHECK(result.Error.find("keepOriginals needs archive") != std::string::npos);
		archive = nullptr;
		std::filesystem::remove_all(output, removeError);
	}

#if !defined(_WIN32)
	TEST_CASE("A file that cannot be searched keeps every original")
	{
		BrecciaTest::TempProject temp("ArchiveUnsearchable");
		REQUIRE(temp.IsValid());
		const Ref<Project>& project = Project::GetActive();
		temp.WriteFile("Assets/Scenes/Main.bscene", R"({"Entities":[]})");
		temp.WriteFile("Assets/Textures/Wall.png", "original");
		temp.WriteFile("Assets/Textures/Wall.ktx2", "replacement");
		temp.WriteFile("Assets/Scripts/Secret.lua", "return 1");
		temp.WriteFile("FakeRuntime", "binary");
		const BrecciaTest::ScopedFakeVulkanSdk sdk(temp);
		project->GetConfig().StartScene = "Assets/Scenes/Main.bscene";
		const std::filesystem::path secret = temp.GetDirectory() / "Assets" / "Scripts" / "Secret.lua";
		std::filesystem::permissions(secret, std::filesystem::perms::none);
		// Root reads anything, so there is nothing to see.
		if (FileSystem::ReadBinaryFile(secret))
			return;
		ExportOptions options;
		options.RuntimeExecutable = temp.GetDirectory() / "FakeRuntime";
		options.Archive = true;
		const std::filesystem::path output = temp.GetDirectory().parent_path() / "ArchiveUnsearchableOut";
		const ExportResult result = Exporter::Export(*project, output, options);
		std::filesystem::permissions(secret, std::filesystem::perms::owner_all);
		std::error_code removeError;
		std::filesystem::remove_all(output, removeError);
		// The archive needs that file too, so the export fails; what matters is that nothing was judged unused without it.
		CHECK_FALSE(result.Success);
		REQUIRE_FALSE(result.Warnings.empty());
		CHECK(result.Warnings[0].find("every original texture stays") != std::string::npos);
		CHECK(result.ArchiveOmitted.empty());
	}
#endif

	TEST_CASE("File archives read in parallel")
	{
		BrecciaTest::TempDirectory directory("ArchiveParallel");
		std::vector<std::tuple<std::string, std::vector<uint8_t>, uint64_t>> entries;
		entries.reserve(32);
		for (uint32_t i = 0; i < 32; i++)
			entries.emplace_back("Assets/File" + std::to_string(i) + ".bin", i % 2 ? Noise(3000 + i * 97, i) : Repetitive(5000 + i * 131), 0);
		BuildArchive(entries, directory.GetPath() / "Parallel.bpak");
		std::string error;
		// From the file: the reads share one stream, so they must not interleave.
		const Ref<PackArchive> archive = PackArchive::Open(directory.GetPath() / "Parallel.bpak", error);
		REQUIRE_MESSAGE(archive, error);
		std::atomic<int> mismatches = 0;
		std::vector<std::thread> threads;
		threads.reserve(8);
		for (uint32_t t = 0; t < 8; t++)
		{
			threads.emplace_back([&, t]() {
				std::string threadError;
				for (uint32_t i = 0; i < 50; i++)
				{
					const auto& [path, data, id] = entries[(t * 7 + i * 5) % entries.size()];
					const PackArchive::Entry* entry = archive->Find(path);
					if (!entry || archive->Read(*entry, threadError) != data)
						mismatches++;
					// Streaming shares the stream with whole reads.
					if (entry && entry->Compression == PackArchive::Method::Store)
					{
						std::vector<uint8_t> part(1000);
						if (archive->ReadStored(*entry, 500, part.data(), part.size(), threadError) != part.size() || !std::equal(part.begin(), part.end(), data.begin() + 500))
							mismatches++;
					}
				}
			});
		}
		for (std::thread& thread : threads)
			thread.join();
		CHECK(mismatches == 0);
	}

#if !defined(_WIN32)
	TEST_CASE("A mount answers through the root's linked and resolved spellings")
	{
		BrecciaTest::TempDirectory directory("ArchiveLinkedRoot");
		const std::filesystem::path game = directory.GetPath() / "Game";
		const std::filesystem::path link = directory.GetPath() / "Link";
		std::filesystem::create_directories(game);
		std::filesystem::create_directory_symlink(game, link);
		BuildArchive({ { "Assets/Packed.txt", Bytes("packed"), 3 } }, directory.GetPath() / "Assets.bpak");
		std::string error;
		const Ref<PackArchive> archive = PackArchive::Open(directory.GetPath() / "Assets.bpak", error);
		REQUIRE_MESSAGE(archive, error);
		// Mounted through the link: the asset registry resolves its root, so the resolved spelling must answer too.
		BrecciaTest::ScopedMount mount(link, archive);
		CHECK(FileSystem::ReadTextFile(link / "Assets" / "Packed.txt") == "packed");
		CHECK(FileSystem::ReadTextFile(std::filesystem::canonical(game) / "Assets" / "Packed.txt") == "packed");
		CHECK(FileSystem::GetMountedArchive(link) == archive);
		CHECK(FileSystem::GetMountedArchive(std::filesystem::canonical(game)) == archive);
		AssetRegistry registry(link);
		REQUIRE(registry.Refresh(error));
		CHECK(registry.Find(UUID(3)) == "Assets/Packed.txt");
	}
#endif

	TEST_CASE("HashBytes64 is stable and sees every byte")
	{
		CHECK(HashBytes64(nullptr, 0) == HashBytes64("", 0));
		const std::vector<uint8_t> data = Noise(37);
		const uint64_t hash = HashBytes64(data.data(), data.size());
		// Pinned: archives written on one platform are read on every other. The inputs cover no words, words and a
		// tail, and words alone; they avoid the engine's name, so a rename never touches a format pin.
		CHECK(HashBytes64("bpak", 4) == 8908455541177533817ull);
		CHECK(HashBytes64("pack archive hash", 17) == 11655933400515416070ull);
		CHECK(HashBytes64("pack archive has", 16) == 12721861551262622531ull);
		CHECK(HashBytes64("bpak", 4) != HashBytes64("BPAK", 4));
		for (size_t i = 0; i < data.size(); i++)
		{
			auto changed = data;
			changed[i] ^= 0x80;
			CHECK_MESSAGE(HashBytes64(changed.data(), changed.size()) != hash, i);
		}
		CHECK(HashBytes64(data.data(), data.size() - 1) != hash);
	}
}
