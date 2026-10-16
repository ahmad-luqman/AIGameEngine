#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace Basalt::Platform {

	// Value of an environment variable, or nullopt if it is not set. Names and values are UTF-8 on every platform
	// (Windows reads and writes the wide environment, which child processes inherit).
	std::optional<std::string> GetEnvVar(const char* name);
	// Sets the variable only if it is not already set. Returns false on failure.
	bool SetEnvVarIfUnset(const char* name, const std::string& value);

	// Sets (or with UnsetEnvVar removes) a variable for this process. Returns false on failure (including text that
	// is not valid UTF-8 on Windows). On Windows an empty value removes the variable.
	bool SetEnvVar(const char* name, const std::string& value);
	bool UnsetEnvVar(const char* name);

	// Overrides GetUserDataDirectory (tests and tools point it at a scratch directory).
	inline constexpr const char* UserDataDirectoryVariable = "BASALT_USER_DATA_DIR";
	// Base directory for per-user application data: $BASALT_USER_DATA_DIR when set, otherwise %APPDATA%
	// (Windows), ~/Library/Application Support (macOS) or $XDG_DATA_HOME / ~/.local/share (Linux; HOME is used
	// when XDG_DATA_HOME is unset or relative). nullopt (with outError) when the variable it ends up using is
	// unset or not an absolute path. A set BASALT_USER_DATA_DIR always wins, even an unusable one: falling back
	// would let a misconfigured test write into the real directory.
	std::optional<std::filesystem::path> GetUserDataDirectory(std::string& outError);

}
