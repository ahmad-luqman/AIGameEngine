#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace Basalt::Platform {

	// Value of an environment variable, or nullopt if it is not set.
	std::optional<std::string> GetEnvVar(const char* name);
	// Sets the variable only if it is not already set. Returns false on failure.
	bool SetEnvVarIfUnset(const char* name, const std::string& value);

	// Overrides GetUserDataDirectory (tests and tools point it at a scratch directory).
	inline constexpr const char* UserDataDirectoryVariable = "BASALT_USER_DATA_DIR";
	// Base directory for per-user application data: $BASALT_USER_DATA_DIR when set, otherwise %APPDATA%
	// (Windows), ~/Library/Application Support (macOS) or $XDG_DATA_HOME / ~/.local/share (Linux).
	// nullopt when none of the variables it is derived from is set to an absolute path.
	std::optional<std::filesystem::path> GetUserDataDirectory();

}
