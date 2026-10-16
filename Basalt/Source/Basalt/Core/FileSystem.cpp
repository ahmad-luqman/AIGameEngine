#include "Basalt/Core/FileSystem.h"

#include "Basalt/Core/Base.h"
#include "Basalt/Core/Log.h"

#include <spdlog/fmt/fmt.h>

#if defined(_WIN32)
	#include <process.h>
#else
	#include <unistd.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <fstream>
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

	std::optional<std::string> FileSystem::ReadTextFile(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::in | std::ios::binary);
		if (!stream)
			return std::nullopt;

		std::string result;
		stream.seekg(0, std::ios::end);
		const std::streamoff size = stream.tellg();
		if (size < 0)
			return std::nullopt;
		result.resize(static_cast<size_t>(size));
		stream.seekg(0, std::ios::beg);
		stream.read(result.data(), size);
		if (!stream && !stream.eof())
			return std::nullopt;
		return result;
	}

	std::optional<std::vector<uint8_t>> FileSystem::ReadBinaryFile(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::in | std::ios::binary);
		if (!stream)
			return std::nullopt;

		stream.seekg(0, std::ios::end);
		const std::streamoff size = stream.tellg();
		if (size < 0)
			return std::nullopt;
		std::vector<uint8_t> result(static_cast<size_t>(size));
		stream.seekg(0, std::ios::beg);
		stream.read(reinterpret_cast<char*>(result.data()), size);
		if (!stream && !stream.eof())
			return std::nullopt;
		return result;
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

		std::filesystem::rename(tempPath, path, error);
#if defined(_WIN32)
		// Windows refuses to replace a file that another writer is replacing (or a scanner has open) at that moment;
		// such a conflict passes quickly. A directory in the way never does, so only retry while the target is a file.
		std::error_code ignored;
		for (int attempt = 0; error && attempt < 50 && !std::filesystem::is_directory(path, ignored); attempt++)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			error.clear();
			std::filesystem::rename(tempPath, path, error);
		}
#endif
		if (error)
		{
			outError = "cannot replace '" + path.string() + "': " + error.message();
			std::filesystem::remove(tempPath, error);
			return false;
		}
		return true;
	}

	std::string FileSystem::PathToUtf8(const std::filesystem::path& path)
	{
		const std::u8string text = path.u8string();
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

	bool FileSystem::Exists(const std::filesystem::path& path)
	{
		std::error_code error;
		return std::filesystem::exists(path, error);
	}

	std::filesystem::path FileSystem::GetExecutableDirectory()
	{
#if defined(BS_PLATFORM_WINDOWS)
		wchar_t buffer[MAX_PATH] = {};
		const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
		if (length == 0 || length == MAX_PATH)
			return std::filesystem::current_path();
		return std::filesystem::path(buffer).parent_path();
#elif defined(BS_PLATFORM_MACOS)
		uint32_t size = 0;
		_NSGetExecutablePath(nullptr, &size);
		std::string buffer(size, '\0');
		if (_NSGetExecutablePath(buffer.data(), &size) != 0)
			return std::filesystem::current_path();
		std::error_code error;
		auto canonical = std::filesystem::canonical(buffer.c_str(), error);
		return error ? std::filesystem::path(buffer.c_str()).parent_path() : canonical.parent_path();
#else
		std::error_code error;
		auto path = std::filesystem::read_symlink("/proc/self/exe", error);
		return error ? std::filesystem::current_path() : path.parent_path();
#endif
	}

}
