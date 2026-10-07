#include "Basalt/Core/FileSystem.h"

#include "Basalt/Core/Base.h"
#include "Basalt/Core/Log.h"

#include <fstream>
#include <system_error>

#if defined(BS_PLATFORM_WINDOWS)
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
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

	bool FileSystem::WriteBinaryFile(const std::filesystem::path& path, const void* data, size_t size)
	{
		std::error_code error;
		if (path.has_parent_path())
			std::filesystem::create_directories(path.parent_path(), error);

		std::filesystem::path tempPath = path;
		tempPath += ".tmp";
		{
			std::ofstream stream(tempPath, std::ios::out | std::ios::binary | std::ios::trunc);
			if (!stream)
			{
				BS_CORE_ERROR("FileSystem: cannot open '{}' for writing", tempPath.string());
				return false;
			}
			stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
			// Buffered data is only known to be on disk after a successful flush + close; never replace
			// the target with a temp file that may be truncated (disk full, quota).
			stream.flush();
			stream.close();
			if (stream.fail())
			{
				BS_CORE_ERROR("FileSystem: failed writing '{}'", tempPath.string());
				std::filesystem::remove(tempPath, error);
				return false;
			}
		}

		std::filesystem::rename(tempPath, path, error);
		if (error)
		{
			BS_CORE_ERROR("FileSystem: cannot replace '{}': {}", path.string(), error.message());
			std::filesystem::remove(tempPath, error);
			return false;
		}
		return true;
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
