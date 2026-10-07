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

}
