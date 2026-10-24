#include "Basalt/Core/FileSystem.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Base.h"
#include "Basalt/Core/Log.h"

#include <spdlog/fmt/fmt.h>

#if defined(_WIN32)
	#include <process.h>
#else
	#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <system_error>
#include <thread>

#if defined(BS_PLATFORM_WINDOWS)
	#include <windows.h>
#elif defined(BS_PLATFORM_MACOS)
	#include <mach-o/dyld.h>
#elif defined(BS_PLATFORM_LINUX)
	#include <unistd.h>
#endif

namespace Basalt {

	namespace {

		struct Mounted
		{
			// The root as given and with links resolved (/var vs /private/var on macOS): callers hold either spelling (the
			// asset registry resolves its root), and comparing both keeps lookups free of link resolution.
			std::filesystem::path Root;
			std::filesystem::path CanonicalRoot;
			Ref<PackArchive> Archive;
		};
		std::shared_mutex s_MountMutex;
		Mounted s_Mounted;

		std::filesystem::path NormalizeAbsolute(const std::filesystem::path& path)
		{
			std::error_code error;
			const std::filesystem::path absolute = std::filesystem::absolute(path, error);
			std::filesystem::path normal = (error ? path : absolute).lexically_normal();
			// "Game/." normalizes to "Game/": the same folder as "Game".
			if (!normal.has_filename() && normal.has_relative_path())
				normal = normal.parent_path();
			return normal;
		}

		// The mounted archive and the '/'-separated key of path inside it; a null archive when path is not under the root.
		std::pair<Ref<PackArchive>, std::string> FindMountKey(const std::filesystem::path& path)
		{
			// Readers share the lock, and Mount/Unmount run only at startup (and in tests), so holding it while computing the
			// key never makes a reader wait in practice.
			std::shared_lock lock(s_MountMutex);
			if (!s_Mounted.Archive)
				return {};
			const std::filesystem::path normal = NormalizeAbsolute(path);
			for (const std::filesystem::path* root : { &s_Mounted.Root, &s_Mounted.CanonicalRoot })
			{
				const std::filesystem::path relative = normal.lexically_relative(*root);
				if (!relative.empty() && *relative.begin() != ".." && relative != ".")
					return { s_Mounted.Archive, FileSystem::PathToGenericUtf8(relative) };
			}
			return {};
		}

		std::filesystem::path Canonical(const std::filesystem::path& root)
		{
			std::error_code error;
			const std::filesystem::path canonical = std::filesystem::weakly_canonical(root, error);
			return error ? root : NormalizeAbsolute(canonical);
		}

	}

	void FileSystem::Mount(const std::filesystem::path& root, Ref<PackArchive> archive)
	{
		BS_CORE_ASSERT(archive, "Unmount removes the mount");
		if (!archive)
		{
			Unmount();
			return;
		}
		const std::filesystem::path normal = NormalizeAbsolute(root);
		Mounted mounted{ normal, Canonical(normal), std::move(archive) };
		std::unique_lock lock(s_MountMutex);
		s_Mounted = std::move(mounted);
	}

	void FileSystem::Unmount()
	{
		std::unique_lock lock(s_MountMutex);
		s_Mounted = {};
	}

	FileSystem::MountedFile FileSystem::FindMounted(const std::filesystem::path& path)
	{
		auto [archive, key] = FindMountKey(path);
		if (!archive)
			return {};
		const PackArchive::Entry* entry = archive->Find(key);
		return entry ? MountedFile{ std::move(archive), entry } : MountedFile{};
	}

	Ref<PackArchive> FileSystem::GetMountedArchive(const std::filesystem::path& root)
	{
		const std::filesystem::path normal = NormalizeAbsolute(root);
		std::shared_lock lock(s_MountMutex);
		return s_Mounted.Archive && (s_Mounted.Root == normal || s_Mounted.CanonicalRoot == normal) ? s_Mounted.Archive : nullptr;
	}

	std::optional<std::vector<uint8_t>> FileSystem::ReadMounted(const MountedFile& file, std::string* outError)
	{
		std::string error;
		std::optional<std::vector<uint8_t>> data;
		if (!file)
			error = "no such file in the mounted archive";
		else
			data = file.GetArchive()->Read(file.GetEntry(), error);
		if (!data)
		{
			// A caller that takes the reason reports it in its own error; otherwise the log is the only place it shows.
			if (outError)
				*outError = std::move(error);
			else
				BS_CORE_ERROR("Archive{}: {}", file && !file.GetArchive()->GetFile().empty() ? " '" + PathToUtf8(file.GetArchive()->GetFile()) + "'" : std::string(), error);
		}
		return data;
	}

	std::optional<uint64_t> FileSystem::GetRegularFileSize(const std::filesystem::path& path, std::error_code& outError)
	{
		outError.clear();
		if (const MountedFile mounted = FindMounted(path))
			return mounted.GetEntry().Size;
		const bool regular = std::filesystem::is_regular_file(path, outError);
		if (outError == std::errc::no_such_file_or_directory)
			outError.clear();
		if (!regular || outError)
			return std::nullopt;
		const uintmax_t size = std::filesystem::file_size(path, outError);
		return outError ? std::nullopt : std::optional<uint64_t>(size);
	}

	namespace {

		// A whole file from disk; outError (optional) says why it cannot be read.
		template<typename Container>
		std::optional<Container> ReadDiskFile(const std::filesystem::path& path, std::string* outError)
		{
			const auto fail = [&](std::string reason) -> std::optional<Container> {
				if (outError)
					*outError = std::move(reason);
				return std::nullopt;
			};
			// A directory opens as a stream on Linux, with a meaningless size (reading it would allocate that much).
			std::error_code error;
			const bool regular = std::filesystem::is_regular_file(path, error);
			if (error)
				return fail(error == std::errc::no_such_file_or_directory ? "no such file" : error.message());
			if (!regular)
				return fail(std::filesystem::exists(path, error) ? "not a file" : "no such file");
			errno = 0;
			std::ifstream stream(path, std::ios::in | std::ios::binary);
			if (!stream)
				return fail(errno != 0 ? std::generic_category().message(errno) : "cannot open it");

			stream.seekg(0, std::ios::end);
			const std::streamoff size = stream.tellg();
			if (size < 0)
				return fail("cannot determine its size");
			Container result(static_cast<size_t>(size), {});
			stream.seekg(0, std::ios::beg);
			stream.read(reinterpret_cast<char*>(result.data()), size);
			if (!stream && !stream.eof())
				return fail("reading it failed");
			return result;
		}

	}

	std::optional<std::string> FileSystem::ReadTextFile(const std::filesystem::path& path, std::string* outError)
	{
		if (const MountedFile mounted = FindMounted(path))
		{
			auto data = ReadMounted(mounted, outError);
			return data ? std::optional<std::string>(std::in_place, data->begin(), data->end()) : std::nullopt;
		}
		return ReadDiskFile<std::string>(path, outError);
	}

	std::optional<std::vector<uint8_t>> FileSystem::ReadBinaryFile(const std::filesystem::path& path, std::string* outError)
	{
		if (const MountedFile mounted = FindMounted(path))
			return ReadMounted(mounted, outError);
		return ReadDiskFile<std::vector<uint8_t>>(path, outError);
	}

	std::optional<std::vector<uint8_t>> FileSystem::ReadBinaryFilePrefix(const std::filesystem::path& path, uint64_t size, std::string* outError)
	{
		if (size == 0)
			return ReadBinaryFile(path, outError);
		const auto fail = [&](std::string reason) -> std::optional<std::vector<uint8_t>> {
			if (outError)
				*outError = std::move(reason);
			return std::nullopt;
		};
		const auto shorter = [&]() { return fail("shorter than the " + std::to_string(size) + " bytes to read"); };
		if (const MountedFile mounted = FindMounted(path))
		{
			auto data = ReadMounted(mounted, outError);
			if (!data)
				return std::nullopt;
			if (data->size() < size)
				return shorter();
			data->resize(static_cast<size_t>(size));
			return data;
		}
		std::error_code error;
		const std::optional<uint64_t> available = GetRegularFileSize(path, error);
		if (error)
			return fail(error.message());
		if (!available)
			return fail("no such file");
		if (*available < size)
			return shorter();
		std::ifstream stream(path, std::ios::in | std::ios::binary);
		std::vector<uint8_t> bytes(static_cast<size_t>(size));
		if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
			return fail("reading it failed");
		return bytes;
	}

	bool FileSystem::WriteTextFile(const std::filesystem::path& path, std::string_view text)
	{
		return WriteBinaryFile(path, text.data(), text.size());
	}

	bool FileSystem::WriteTextFile(const std::filesystem::path& path, std::string_view text, std::string& outError)
	{
		return WriteAtomically(path, text.data(), text.size(), outError);
	}

	bool FileSystem::WriteBinaryFile(const std::filesystem::path& path, const void* data, size_t size)
	{
		std::string error;
		if (WriteAtomically(path, data, size, error))
			return true;
		BS_CORE_ERROR("FileSystem: {}", error);
		return false;
	}

	bool FileSystem::WriteBinaryFile(const std::filesystem::path& path, const void* data, size_t size, std::string& outError)
	{
		return WriteAtomically(path, data, size, outError);
	}

	bool FileSystem::WriteAtomically(const std::filesystem::path& path, const void* data, size_t size, std::string& outError)
	{
		std::error_code error;
		if (path.has_parent_path())
		{
			std::filesystem::create_directories(path.parent_path(), error);
			if (error)
			{
				outError = "cannot create the folder '" + PathToUtf8(path.parent_path()) + "': " + error.message();
				return false;
			}
		}

		const std::filesystem::path tempPath = MakeTemporaryPath(path);
		{
			errno = 0;
			std::ofstream stream(tempPath, std::ios::out | std::ios::binary | std::ios::trunc);
			if (!stream)
			{
				const int reason = errno;
				outError = "cannot open '" + tempPath.string() + "' for writing" + (reason != 0 ? ": " + std::generic_category().message(reason) : std::string());
				return false;
			}
			stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
			// Buffered data is only known to be on disk after a successful flush + close; never replace
			// the target with a temp file that may be truncated (disk full, quota).
			stream.flush();
			stream.close();
			if (stream.fail())
			{
				outError = "failed writing '" + tempPath.string() + "' (disk full?)";
				std::filesystem::remove(tempPath, error);
				return false;
			}
		}

		return RenameReplacing(tempPath, path, outError);
	}

	std::filesystem::path FileSystem::MakeTemporaryPath(const std::filesystem::path& path)
	{
		// A name of its own per write (process, then a counter): several processes (parallel test runs) may replace the
		// same file at once.
		static std::atomic<uint64_t> s_TempCounter = 0;
#if defined(_WIN32)
		const int processId = _getpid();
#else
		const int processId = static_cast<int>(getpid());
#endif
		std::filesystem::path tempPath = path;
		tempPath += fmt::format(".{}-{}.tmp", processId, s_TempCounter.fetch_add(1));
		return tempPath;
	}

	bool FileSystem::RenameReplacing(const std::filesystem::path& source, const std::filesystem::path& target, std::string& outError)
	{
		std::error_code error;
		std::filesystem::rename(source, target, error);
#if defined(_WIN32)
		// Windows refuses to replace a file that another writer is replacing (or a scanner has open) at that moment;
		// such a conflict passes quickly. A directory in the way never does, so only retry while the target is a file.
		std::error_code ignored;
		for (int attempt = 0; error && attempt < 50 && !std::filesystem::is_directory(target, ignored); attempt++)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			error.clear();
			std::filesystem::rename(source, target, error);
		}
#endif
		if (error)
		{
			outError = "cannot replace '" + PathToUtf8(target) + "': " + error.message();
			std::filesystem::remove(source, error);
			return false;
		}
		return true;
	}

	std::string FileSystem::PathToUtf8(const std::filesystem::path& path)
	{
		const std::u8string text = path.u8string();
		return std::string(text.begin(), text.end());
	}

	std::string FileSystem::PathToGenericUtf8(const std::filesystem::path& path)
	{
		const std::u8string text = path.generic_u8string();
		return std::string(text.begin(), text.end());
	}

	std::filesystem::path FileSystem::PathFromUtf8(const std::string& text)
	{
		return std::filesystem::path(std::u8string(text.begin(), text.end()));
	}

	std::string FileSystem::GetLowercaseExtension(const std::filesystem::path& path)
	{
		return ToLowerAscii(PathToUtf8(path.extension()));
	}

	std::string FileSystem::ToLowerAscii(std::string text)
	{
		for (char& c : text)
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
		return text;
	}

	std::string FileSystem::NormalizeGeneric(const std::string& path)
	{
		return PathToGenericUtf8(PathFromUtf8(path).lexically_normal());
	}

	const std::vector<std::string>* FileSystem::DirectoryListings::Get(const std::filesystem::path& directory, std::string& outError)
	{
		auto [it, inserted] = m_Listings.try_emplace(PathToUtf8(directory));
		if (inserted)
		{
			std::error_code error;
			for (auto entry = std::filesystem::directory_iterator(directory, error); !error && entry != std::filesystem::directory_iterator(); entry.increment(error))
				it->second.Names.push_back(PathToUtf8(entry->path().filename()));
			// A missing directory means a missing file, not an unreadable one.
			if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory)
				it->second.Error = error.message();
		}
		outError = it->second.Error;
		return outError.empty() ? &it->second.Names : nullptr;
	}

	FileSystem::PathMatch FileSystem::FindExactCase(const std::filesystem::path& root, const std::filesystem::path& relative, std::string& outActual, std::string& outProblem, DirectoryListings* listings)
	{
		DirectoryListings local;
		DirectoryListings& cache = listings ? *listings : local;
		std::filesystem::path current = root;
		std::filesystem::path actual;
		bool caseDiffers = false;
		for (const auto& component : relative)
		{
			std::string error;
			const std::vector<std::string>* names = cache.Get(current, error);
			if (!names)
			{
				outProblem = "cannot list '" + PathToUtf8(current) + "': " + error;
				return PathMatch::Unreadable;
			}
			const std::string name = PathToUtf8(component);
			auto found = std::find(names->begin(), names->end(), name);
			if (found == names->end())
			{
				const std::string folded = ToLowerAscii(name);
				found = std::find_if(names->begin(), names->end(), [&](const std::string& entry) { return ToLowerAscii(entry) == folded; });
				if (found == names->end())
					return PathMatch::Missing;
				caseDiffers = true;
			}
			actual /= PathFromUtf8(*found);
			current /= PathFromUtf8(*found);
		}
		std::error_code error;
		const bool regular = std::filesystem::is_regular_file(current, error);
		if (error || !regular)
		{
			outProblem = error ? error.message() : "it is not a file";
			return PathMatch::Unreadable;
		}
		outActual = PathToGenericUtf8(actual);
		return caseDiffers ? PathMatch::DifferentCase : PathMatch::Exact;
	}

	bool FileSystem::Exists(const std::filesystem::path& path)
	{
		if (const auto [archive, key] = FindMountKey(path); archive && (archive->Find(key) || archive->HasFolder(key)))
			return true;
		std::error_code error;
		return std::filesystem::exists(path, error);
	}

	bool FileSystem::FindSymlink(const std::filesystem::path& root, const std::filesystem::path& relative, std::filesystem::path& outEntry, std::error_code& outError)
	{
		outError.clear();
		auto entry = root;
		for (const auto& component : relative)
		{
			entry /= component;
			std::error_code error;
			const auto status = std::filesystem::symlink_status(entry, error);
			if (std::filesystem::is_symlink(status) || (error && error != std::errc::no_such_file_or_directory))
			{
				if (!std::filesystem::is_symlink(status))
					outError = error;
				outEntry = std::move(entry);
				return true;
			}
		}
		return false;
	}

	std::filesystem::path FileSystem::GetExecutablePath()
	{
		// Callers name things after it (crash report folders); say so rather than silently naming them "Unknown".
		const auto unknown = []() {
			// Once: several callers ask (crash reports, exports, the runtime's project lookup).
			static std::once_flag s_Warned;
			std::call_once(s_Warned, [] { BS_CORE_WARN("FileSystem: cannot find the running executable's path"); });
			std::error_code error;
			return std::filesystem::current_path(error) / "Unknown";
		};
#if defined(BS_PLATFORM_WINDOWS)
		wchar_t buffer[MAX_PATH] = {};
		const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
		if (length == 0 || length == MAX_PATH)
			return unknown();
		return std::filesystem::path(buffer);
#elif defined(BS_PLATFORM_MACOS)
		uint32_t size = 0;
		_NSGetExecutablePath(nullptr, &size);
		std::string buffer(size, '\0');
		if (_NSGetExecutablePath(buffer.data(), &size) != 0)
			return unknown();
		std::error_code error;
		auto canonical = std::filesystem::canonical(buffer.c_str(), error);
		return error ? std::filesystem::path(buffer.c_str()) : canonical;
#else
		std::error_code error;
		auto path = std::filesystem::read_symlink("/proc/self/exe", error);
		return error ? unknown() : path;
#endif
	}

	std::filesystem::path FileSystem::GetExecutableDirectory()
	{
		return GetExecutablePath().parent_path();
	}

}
