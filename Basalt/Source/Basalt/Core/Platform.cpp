#include "Basalt/Core/Platform.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"

#include <climits>
#include <cstdlib>

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
	namespace {

		// Narrow text in codePage to UTF-16; flags MB_ERR_INVALID_CHARS makes invalid input fail instead of becoming U+FFFD.
		std::optional<std::wstring> Widen(std::string_view text, UINT codePage, DWORD flags)
		{
			if (text.empty())
				return std::wstring();
			if (text.size() > static_cast<size_t>(INT_MAX))
				return std::nullopt;
			const int size = MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
			if (size <= 0)
				return std::nullopt;
			std::wstring wide(static_cast<size_t>(size), L'\0');
			if (MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), wide.data(), size) != size)
				return std::nullopt;
			return wide;
		}

	}

	std::optional<std::wstring> Utf8ToWide(std::string_view text)
	{
		return Widen(text, CP_UTF8, MB_ERR_INVALID_CHARS);
	}

	std::wstring Utf8ToWideReplacing(std::string_view text)
	{
		return Widen(text, CP_UTF8, 0).value_or(std::wstring());
	}

	std::string WideToUtf8(std::wstring_view text)
	{
		if (text.empty() || text.size() > static_cast<size_t>(INT_MAX))
			return std::string();
		const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		if (size <= 0)
			return std::string();
		std::string narrow(static_cast<size_t>(size), '\0');
		if (WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), size, nullptr, nullptr) != size)
			return std::string();
		return narrow;
	}

	std::string GetErrorMessage(uint32_t code)
	{
		wchar_t* buffer = nullptr;
		const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
		std::string message = length != 0 && buffer ? WideToUtf8(std::wstring_view(buffer, length)) : std::string();
		if (buffer)
			LocalFree(buffer);
		while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' '))
			message.pop_back();
		return message;
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
		std::vector<std::string> arguments;
		arguments.reserve(argc > 0 ? static_cast<size_t>(argc) : 0);
#if defined(BS_PLATFORM_WINDOWS)
		int count = 0;
		if (wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count))
		{
			for (int i = 0; i < count; i++)
				arguments.push_back(WideToUtf8(wide[i]));
			LocalFree(wide);
			// Callers that look at argv first (a test child's marker argument) would then read the wrong entries.
			if (argv && count != argc)
				BS_CORE_WARN("The wide command line splits into {} arguments, the C runtime's argv into {}", count, argc);
			return arguments;
		}
		BS_CORE_WARN("Cannot read the wide command line (error {}): arguments outside the ANSI code page are lost", GetLastError());
		// argv is ANSI text: converted, so non-ASCII characters inside the code page still arrive as UTF-8.
		for (int i = 0; i < argc; i++)
		{
			const std::optional<std::wstring> wide = Widen(argv[i] ? argv[i] : "", CP_ACP, 0);
			arguments.push_back(wide ? WideToUtf8(*wide) : std::string());
		}
#else
		for (int i = 0; i < argc; i++)
			arguments.emplace_back(argv[i] ? argv[i] : "");
#endif
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

		// The variable as a path. GetEnvVar reads the wide environment on Windows, so user names outside the ANSI code page
		// survive.
		std::optional<std::filesystem::path> GetPathVar(const char* name)
		{
			if (auto value = GetEnvVar(name))
				return FileSystem::PathFromUtf8(*value);
			return std::nullopt;
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
