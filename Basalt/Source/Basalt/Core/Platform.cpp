#include "Basalt/Core/Platform.h"

#include "Basalt/Core/Base.h"

#include <cstdlib>
#include <cstring>

namespace Basalt::Platform {

	std::optional<std::string> GetEnvVar(const char* name)
	{
#if defined(BS_PLATFORM_WINDOWS)
		char* buffer = nullptr;
		size_t length = 0;
		if (_dupenv_s(&buffer, &length, name) != 0 || !buffer)
			return std::nullopt;
		std::string value(buffer);
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
#if defined(BS_PLATFORM_WINDOWS)
		return _putenv_s(name, value.c_str()) == 0;
#else
		return setenv(name, value.c_str(), 0) == 0;
#endif
	}

	bool SetEnvVar(const char* name, const std::string& value)
	{
#if defined(BS_PLATFORM_WINDOWS)
		return _putenv_s(name, value.c_str()) == 0;
#else
		return setenv(name, value.c_str(), 1) == 0;
#endif
	}

	bool UnsetEnvVar(const char* name)
	{
#if defined(BS_PLATFORM_WINDOWS)
		// An empty value removes the variable on Windows.
		return _putenv_s(name, "") == 0;
#else
		return unsetenv(name) == 0;
#endif
	}

	namespace {

		// The variable as a path, read as wide text on Windows so user names outside the ANSI code page survive.
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
		std::optional<std::filesystem::path> AbsolutePathVar(const char* name, std::string& outError)
		{
			std::optional<std::filesystem::path> value = GetPathVar(name);
			if (!value)
			{
				outError = std::string(name) + " is not set";
				return std::nullopt;
			}
			// Relative values would resolve against the working directory, which changes between launches.
			if (!value->is_absolute())
			{
				outError = std::string(name) + " is set to '" + value->string() + "', which is not an absolute path";
				return std::nullopt;
			}
			return value;
		}

	}

	std::optional<std::filesystem::path> GetUserDataDirectory(std::string& outError)
	{
		std::string reason;
		if (GetPathVar(UserDataDirectoryVariable))
		{
			// A set but unusable override is a configuration mistake: falling back to the real user directory
			// would let tests write into it.
			auto overridden = AbsolutePathVar(UserDataDirectoryVariable, reason);
			if (!overridden)
				outError = "no user data directory: " + reason;
			return overridden;
		}
#if defined(BS_PLATFORM_WINDOWS)
		auto appData = AbsolutePathVar("APPDATA", reason);
		if (!appData)
			outError = "no user data directory: " + reason;
		return appData;
#elif defined(BS_PLATFORM_MACOS)
		if (auto home = AbsolutePathVar("HOME", reason))
			return *home / "Library" / "Application Support";
		outError = "no user data directory: " + reason;
		return std::nullopt;
#else
		// The XDG spec says to ignore an unset, empty or relative XDG_DATA_HOME.
		if (auto data = AbsolutePathVar("XDG_DATA_HOME", reason))
			return data;
		if (auto home = AbsolutePathVar("HOME", reason))
			return *home / ".local" / "share";
		outError = "no user data directory: " + reason;
		return std::nullopt;
#endif
	}

}
