#pragma once

#include "Breccia/Core/Base.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Breccia::Platform {

	// Value of an environment variable, or nullopt if it is not set. Names and values are UTF-8 on every platform
	// (Windows reads and writes the wide environment, which child processes inherit).
	std::optional<std::string> GetEnvVar(const char* name);
	// Sets the variable only if it is not already set. Returns false on failure.
	bool SetEnvVarIfUnset(const char* name, const std::string& value);

	// Sets (or with UnsetEnvVar removes) a variable for this process. Returns false on failure (including text that
	// is not valid UTF-8 on Windows). On Windows an empty value removes the variable.
	bool SetEnvVar(const char* name, const std::string& value);
	bool UnsetEnvVar(const char* name);

	// This process's ID, for names that must differ between processes running at the same time.
	int GetProcessId();

	// The process's arguments as UTF-8 text, program name first. Elsewhere main's argv, passed through unchanged (UTF-8 in
	// practice). On Windows argv is in the ANSI code page and loses every character outside it (a Japanese "--project"
	// path on a Western-locale system, say), so the wide command line is read instead and argc/argv are only the fallback
	// should it be unavailable (converted from the ANSI code page, with a warning). CommandLineToArgvW splits it like the
	// UCRT except for "" inside a quoted argument and quotes within the program name, which neither Breccia's own child
	// processes nor Python ever write.
	std::vector<std::string> GetProcessArguments(int argc, const char* const* argv);

#if defined(BR_PLATFORM_WINDOWS)
	// UTF-8 to and from the UTF-16 of Windows' wide APIs. Utf8ToWide is nullopt for text that is not valid UTF-8 (or longer
	// than the API takes); Utf8ToWideReplacing turns invalid sequences into U+FFFD instead, for paths, which must never
	// throw (and is empty for overlong text). WideToUtf8 turns unpaired surrogates into U+FFFD and is empty only for empty
	// or overlong text.
	std::optional<std::wstring> Utf8ToWide(std::string_view text);
	std::wstring Utf8ToWideReplacing(std::string_view text);
	std::string WideToUtf8(std::wstring_view text);
	// The system's message for a Win32 error code (GetLastError) as UTF-8, without trailing whitespace; empty when the
	// system has none. FormatMessageA (and MSVC's std::error_code::message) give it in the ANSI code page.
	std::string GetErrorMessage(uint32_t code);
#endif

	// Overrides GetUserDataDirectory (tests and tools point it at a scratch directory).
	inline constexpr const char* UserDataDirectoryVariable = "BRECCIA_USER_DATA_DIR";
	// Base directory for per-user application data: $BRECCIA_USER_DATA_DIR when set, otherwise %APPDATA%
	// (Windows), ~/Library/Application Support (macOS) or $XDG_DATA_HOME / ~/.local/share (Linux; HOME is used
	// when XDG_DATA_HOME is unset or relative). nullopt (with outError) when the variable it ends up using is
	// unset or not an absolute path. A set BRECCIA_USER_DATA_DIR always wins, even an unusable one: falling back
	// would let a misconfigured test write into the real directory.
	std::optional<std::filesystem::path> GetUserDataDirectory(std::string& outError);

}
