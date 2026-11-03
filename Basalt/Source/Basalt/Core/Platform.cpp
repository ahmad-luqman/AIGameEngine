#include "Basalt/Core/Platform.h"

#include "Basalt/Core/Base.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"

#include <climits>
#include <cstdlib>
#include <cstring>

#if defined(BS_PLATFORM_WINDOWS)
	#include <process.h>
	#include <windows.h>
	// After windows.h, which it depends on.
	#include <shellapi.h>
#else
	#include <unistd.h>
#endif

namespace Basalt::Platform {

#if defined(BS_PLATFORM_WINDOWS)
	std::optional<std::wstring> Utf8ToWide(std::string_view text)
	{
		if (text.empty())
			return std::wstring();
		if (text.size() > static_cast<size_t>(INT_MAX))
			return std::nullopt;
		const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
		if (size <= 0)
			return std::nullopt;
		std::wstring wide(static_cast<size_t>(size), L'\0');
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), size) != size)
			return std::nullopt;
		return wide;
	}

	std::string WideToUtf8(std::wstring_view text)
	{
		if (text.empty() || text.size() > static_cast<size_t>(INT_MAX))
			return std::string();
		const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		if (size <= 0)
			return std::string();
		std::string narrow(static_cast<size_t>(size), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), size, nullptr, nullptr);
		return narrow;
	}
#endif

	int GetProcessId()
	{
#if defined(BS_PLATFORM_WINDOWS)
		return _getpid();
#else
		return static_cast<int>(getpid());
#endif
	}

	std::vector<std::string> GetProcessArguments(int argc, const char* const* argv)
	{
#if defined(BS_PLATFORM_WINDOWS)
		int count = 0;
		if (wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count))
		{
			std::vector<std::string> arguments;
			arguments.reserve(static_cast<size_t>(count));
			for (int i = 0; i < count; i++)
				arguments.push_back(WideToUtf8(wide[i]));
			LocalFree(wide);
			return arguments;
		}
		BS_CORE_WARN("Cannot read the wide command line (error {}): arguments outside the ANSI code page are lost", GetLastError());
#endif
		std::vector<std::string> arguments;
		arguments.reserve(argc > 0 ? static_cast<size_t>(argc) : 0);
		for (int i = 0; i < argc; i++)
			arguments.emplace_back(argv[i] ? argv[i] : "");
		return arguments;
	}

	std::optional<std::string> GetEnvVar(const char* name)
	{
#if defined(BS_PLATFORM_WINDOWS)
		const auto wideName = Utf8ToWide(name);
		wchar_t* buffer = nullptr;
		size_t length = 0;
		if (!wideName || _wdupenv_s(&buffer, &length, wideName->c_str()) != 0 || !buffer)
			return std::nullopt;
		std::string value = WideToUtf8(buffer);
		std::free(buffer);
		return value;
#else
		const char* value = std::getenv(name);
		if (!value)
			return std::nullopt;
		return std::string(value);
#endif
	}

	bool SetEnvVarIfUnset(const char* name, const std::string& value)
	{
		if (GetEnvVar(name))
			return true;
		return SetEnvVar(name, value);
	}

	bool SetEnvVar(const char* name, const std::string& value)
	{
#if defined(BS_PLATFORM_WINDOWS)
		const auto wideName = Utf8ToWide(name);
		const auto wideValue = Utf8ToWide(value);
		return wideName && wideValue && _wputenv_s(wideName->c_str(), wideValue->c_str()) == 0;
#else
		return setenv(name, value.c_str(), 1) == 0;
#endif
	}

	bool UnsetEnvVar(const char* name)
	{
#if defined(BS_PLATFORM_WINDOWS)
		// An empty value removes the variable on Windows.
		const auto wideName = Utf8ToWide(name);
		return wideName && _wputenv_s(wideName->c_str(), L"") == 0;
#else
		return unsetenv(name) == 0;
#endif
	}

	namespace {

		// The variable as a path, read as wide text on Windows so user names outside the ANSI code page survive.
		// Names are ASCII constants, so widening them byte by byte is exact.
		std::optional<std::filesystem::path> GetPathVar(const char* name)
		{
#if defined(BS_PLATFORM_WINDOWS)
			const std::wstring wideName(name, name + std::strlen(name));
			wchar_t* buffer = nullptr;
			size_t length = 0;
			if (_wdupenv_s(&buffer, &length, wideName.c_str()) != 0 || !buffer)
				return std::nullopt;
			std::filesystem::path value(buffer);
			std::free(buffer);
			return value;
#else
			if (auto value = GetEnvVar(name))
				return std::filesystem::path(*value);
			return std::nullopt;
#endif
		}

		// The variable's path when it is set to an absolute one; otherwise nullopt and why.
		std::optional<std::filesystem::path> AbsolutePathVar(const char* name, std::string& outReason)
		{
			std::optional<std::filesystem::path> value = GetPathVar(name);
			if (!value)
			{
				outReason = std::string(name) + " is not set";
				return std::nullopt;
			}
			// Relative values would resolve against the working directory, which changes between launches.
			if (!value->is_absolute())
			{
				outReason = std::string(name) + " is set to '" + FileSystem::PathToUtf8(*value) + "', which is not an absolute path";
				return std::nullopt;
			}
			return value;
		}

		std::optional<std::filesystem::path> FindUserDataDirectory(std::string& outReason)
		{
			// A set but unusable override is a configuration mistake: falling back to the real user directory
			// would let tests write into it.
			if (GetPathVar(UserDataDirectoryVariable))
				return AbsolutePathVar(UserDataDirectoryVariable, outReason);
#if defined(BS_PLATFORM_WINDOWS)
			return AbsolutePathVar("APPDATA", outReason);
#elif defined(BS_PLATFORM_MACOS)
			if (auto home = AbsolutePathVar("HOME", outReason))
				return *home / "Library" / "Application Support";
			return std::nullopt;
#else
			// The XDG spec says to ignore an unset, empty or relative XDG_DATA_HOME.
			std::string dataReason;
			if (auto data = AbsolutePathVar("XDG_DATA_HOME", dataReason))
				return data;
			if (auto home = AbsolutePathVar("HOME", outReason))
				return *home / ".local" / "share";
			outReason = dataReason + ", and " + outReason;
			return std::nullopt;
#endif
		}

	}

	std::optional<std::filesystem::path> GetUserDataDirectory(std::string& outError)
	{
		std::string reason;
		std::optional<std::filesystem::path> directory = FindUserDataDirectory(reason);
		if (!directory)
			outError = "no user data directory: " + reason;
		return directory;
	}

}
