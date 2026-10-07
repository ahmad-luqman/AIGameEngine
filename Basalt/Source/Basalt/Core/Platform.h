#pragma once

#include <optional>
#include <string>

namespace Basalt::Platform {

	// Value of an environment variable, or nullopt if it is not set.
	std::optional<std::string> GetEnvVar(const char* name);
	// Sets the variable only if it is not already set. Returns false on failure.
	bool SetEnvVarIfUnset(const char* name, const std::string& value);

}
