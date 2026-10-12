#include "Basalt/Core/Platform.h"

#include "Basalt/Core/Base.h"

#include <cstdlib>

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

	std::optional<std::filesystem::path> GetUserDataDirectory()
	{
		// Relative values would resolve against the working directory, which changes between launches.
		auto absolute = [](const std::optional<std::string>& value) -> std::optional<std::filesystem::path> {
			if (!value || value->empty())
				return std::nullopt;
			std::filesystem::path path(*value);
			if (!path.is_absolute())
				return std::nullopt;
			return path;
		};

		if (auto overridden = GetEnvVar(UserDataDirectoryVariable))
		{
			// A set but unusable override is a configuration mistake: falling back to the real user directory
			// would let tests write into it.
			return absolute(overridden);
		}
#if defined(BS_PLATFORM_WINDOWS)
		return absolute(GetEnvVar("APPDATA"));
#elif defined(BS_PLATFORM_MACOS)
		if (auto home = absolute(GetEnvVar("HOME")))
			return *home / "Library" / "Application Support";
		return std::nullopt;
#else
		if (auto data = absolute(GetEnvVar("XDG_DATA_HOME")))
			return data;
		if (auto home = absolute(GetEnvVar("HOME")))
			return *home / ".local" / "share";
		return std::nullopt;
#endif
	}

}
