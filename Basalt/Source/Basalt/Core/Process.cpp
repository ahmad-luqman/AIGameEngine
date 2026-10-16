#include "Basalt/Core/Process.h"

#include "Basalt/Core/Base.h"

#include <thread>

#if defined(BS_PLATFORM_WINDOWS)
	#include <windows.h>

	#include <algorithm>
	#include <atomic>
	#include <cwchar>
	#include <string_view>
#else
	#include <algorithm>
	#include <cerrno>
	#include <csignal>
	#include <cstring>
	#include <string_view>

	#include <fcntl.h>
	#include <sys/wait.h>
	#include <unistd.h>

extern char** environ;
#endif

namespace Basalt {

	namespace {

		constexpr std::chrono::milliseconds PollInterval{ 5 };

	}

#if defined(BS_PLATFORM_WINDOWS)

	namespace {

		std::string LastErrorMessage(DWORD code)
		{
			char* buffer = nullptr;
			const DWORD length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
			std::string message = length && buffer ? std::string(buffer, length) : "error " + std::to_string(code);
			if (buffer)
				LocalFree(buffer);
			while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' '))
				message.pop_back();
			return message;
		}

		// Quotes one argument so CommandLineToArgvW / the MSVC runtime reads it back unchanged: backslashes are
		// literal except before a quote, where they (and the quote) are escaped.
		std::wstring QuoteArgument(const std::wstring& argument)
		{
			if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
				return argument;
			std::wstring quoted = L"\"";
			for (size_t i = 0;; i++)
			{
				size_t backslashes = 0;
				while (i < argument.size() && argument[i] == L'\\')
				{
					i++;
					backslashes++;
				}
				if (i == argument.size())
				{
					quoted.append(backslashes * 2, L'\\');
					break;
				}
				if (argument[i] == L'"')
				{
					quoted.append(backslashes * 2 + 1, L'\\');
					quoted.push_back(L'"');
				}
				else
				{
					quoted.append(backslashes, L'\\');
					quoted.push_back(argument[i]);
				}
			}
			quoted.push_back(L'"');
			return quoted;
		}

		std::wstring Widen(const std::string& text)
		{
			return std::filesystem::path(std::u8string(text.begin(), text.end())).wstring();
		}

		bool SameVariable(std::wstring_view entry, const std::wstring& name)
		{
			// Variable names are case-insensitive on Windows.
			return entry.size() > name.size() && entry[name.size()] == L'=' && CompareStringOrdinal(entry.data(), static_cast<int>(name.size()), name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
		}

		// The caller's environment block with the overrides applied (empty when there are none, which makes the
		// child inherit the block unchanged).
		std::wstring BuildEnvironment(const std::vector<std::pair<std::string, std::string>>& overrides)
		{
			std::wstring block;
			if (overrides.empty())
				return block;
			std::vector<std::wstring> names;
			for (const auto& [name, value] : overrides)
				names.push_back(Widen(name));
			if (wchar_t* current = GetEnvironmentStringsW())
			{
				for (const wchar_t* entry = current; *entry; entry += std::wcslen(entry) + 1)
				{
					const std::wstring_view text(entry);
					if (std::none_of(names.begin(), names.end(), [&](const std::wstring& name) { return SameVariable(text, name); }))
						block.append(text).push_back(L'\0');
				}
				FreeEnvironmentStringsW(current);
			}
			for (const auto& [name, value] : overrides)
				block.append(Widen(name) + L"=" + Widen(value)).push_back(L'\0');
			block.push_back(L'\0');
			return block;
		}

		// Closes a handle when the scope ends.
		struct HandleGuard
		{
			HANDLE Handle = nullptr;
			~HandleGuard()
			{
				if (Handle && Handle != INVALID_HANDLE_VALUE)
					CloseHandle(Handle);
			}
		};

		std::string ReadTail(HANDLE file, size_t maxBytes)
		{
			LARGE_INTEGER size{};
			if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0)
				return {};
			const auto total = static_cast<uint64_t>(size.QuadPart);
			const uint64_t start = total > maxBytes ? total - maxBytes : 0;
			LARGE_INTEGER offset{};
			offset.QuadPart = static_cast<LONGLONG>(start);
			if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN))
				return {};
			std::string text(static_cast<size_t>(total - start), '\0');
			DWORD read = 0;
			if (!ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr))
				return {};
			text.resize(read);
			return text;
		}

	}

	ProcessResult RunProcess(const ProcessSpec& spec)
	{
		ProcessResult result;
		static std::atomic<uint32_t> s_Counter{ 0 };
		wchar_t tempDirectory[MAX_PATH + 1] = {};
		if (!GetTempPathW(MAX_PATH + 1, tempDirectory))
		{
			result.Error = "cannot find the temporary directory: " + LastErrorMessage(GetLastError());
			return result;
		}
		const std::wstring outputPath = std::wstring(tempDirectory) + L"basalt-process-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(s_Counter++) + L".log";
		SECURITY_ATTRIBUTES inherit{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
		HandleGuard output{ CreateFileW(outputPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr) };
		if (output.Handle == INVALID_HANDLE_VALUE)
		{
			result.Error = "cannot create the output file: " + LastErrorMessage(GetLastError());
			return result;
		}
		HandleGuard input{ CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr) };

		std::wstring commandLine = QuoteArgument(spec.Executable.wstring());
		for (const std::string& argument : spec.Arguments)
			commandLine += L" " + QuoteArgument(Widen(argument));

		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = input.Handle == INVALID_HANDLE_VALUE ? nullptr : input.Handle;
		startup.hStdOutput = output.Handle;
		startup.hStdError = output.Handle;
		PROCESS_INFORMATION process{};
		const std::wstring directory = spec.WorkingDirectory.wstring();
		std::wstring environment = BuildEnvironment(spec.Environment);
		if (!CreateProcessW(spec.Executable.wstring().c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.empty() ? nullptr : environment.data(), directory.empty() ? nullptr : directory.c_str(), &startup, &process))
		{
			result.Error = "cannot start '" + spec.Executable.string() + "': " + LastErrorMessage(GetLastError());
			return result;
		}
		HandleGuard processHandle{ process.hProcess };
		HandleGuard threadHandle{ process.hThread };

		const auto timeout = static_cast<DWORD>(std::clamp<long long>(spec.Timeout.count(), 0, INFINITE - 1));
		const DWORD wait = WaitForSingleObject(process.hProcess, timeout);
		if (wait == WAIT_TIMEOUT)
		{
			TerminateProcess(process.hProcess, 1);
			WaitForSingleObject(process.hProcess, INFINITE);
			result.Outcome = ProcessOutcome::TimedOut;
			result.Error = "killed after " + std::to_string(spec.Timeout.count()) + " ms";
		}
		else if (wait != WAIT_OBJECT_0)
		{
			const DWORD error = GetLastError();
			TerminateProcess(process.hProcess, 1);
			result.Outcome = ProcessOutcome::Failed;
			result.Error = "cannot wait for the process: " + LastErrorMessage(error);
		}
		else
		{
			DWORD exitCode = 0;
			if (!GetExitCodeProcess(process.hProcess, &exitCode))
			{
				result.Outcome = ProcessOutcome::Failed;
				result.Error = "cannot read the exit code: " + LastErrorMessage(GetLastError());
			}
			else
			{
				result.Outcome = ProcessOutcome::Exited;
				result.ExitCode = static_cast<int>(exitCode);
			}
		}
		result.Output = ReadTail(output.Handle, spec.MaxOutputBytes);
		return result;
	}

#else

	namespace {

		// Closes a descriptor when the scope ends.
		struct FileDescriptor
		{
			int Value = -1;
			~FileDescriptor()
			{
				if (Value >= 0)
					close(Value);
			}
		};

		std::string ReadTail(int file, size_t maxBytes)
		{
			const off_t size = lseek(file, 0, SEEK_END);
			if (size <= 0)
				return {};
			const auto total = static_cast<size_t>(size);
			const size_t start = total > maxBytes ? total - maxBytes : 0;
			std::string text(total - start, '\0');
			size_t done = 0;
			while (done < text.size())
			{
				const ssize_t count = pread(file, text.data() + done, text.size() - done, static_cast<off_t>(start) + static_cast<off_t>(done));
				if (count < 0 && errno == EINTR)
					continue;
				if (count <= 0)
					break;
				done += static_cast<size_t>(count);
			}
			text.resize(done);
			return text;
		}

		bool SetCloseOnExec(int descriptor)
		{
			const int flags = fcntl(descriptor, F_GETFD);
			return flags >= 0 && fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
		}

	}

	ProcessResult RunProcess(const ProcessSpec& spec)
	{
		ProcessResult result;
		std::error_code error;
		std::string pattern = (std::filesystem::temp_directory_path(error) / "basalt-process-XXXXXX").string();
		if (error)
		{
			result.Error = "cannot find the temporary directory: " + error.message();
			return result;
		}
		FileDescriptor output{ mkstemp(pattern.data()) };
		if (output.Value < 0)
		{
			result.Error = std::string("cannot create the output file: ") + std::strerror(errno);
			return result;
		}
		// The open descriptor keeps the file alive; nothing is left behind if this process dies.
		unlink(pattern.c_str());

		// Everything the child needs is prepared before fork: between fork and exec only async-signal-safe calls
		// are allowed (other threads may hold the allocator's locks).
		const std::string executable = spec.Executable.string();
		const std::string directory = spec.WorkingDirectory.string();
		std::vector<std::string> arguments;
		arguments.reserve(spec.Arguments.size() + 1);
		arguments.push_back(executable);
		arguments.insert(arguments.end(), spec.Arguments.begin(), spec.Arguments.end());
		std::vector<char*> argv;
		argv.reserve(arguments.size() + 1);
		for (std::string& argument : arguments)
			argv.push_back(argument.data());
		argv.push_back(nullptr);
		// The caller's environment with spec.Environment replacing (or adding) its variables.
		std::vector<std::string> variables;
		for (char** entry = environ; entry && *entry; entry++)
		{
			const std::string_view text(*entry);
			const bool replaced = std::any_of(spec.Environment.begin(), spec.Environment.end(), [&](const auto& variable) {
				return text.size() > variable.first.size() && text[variable.first.size()] == '=' && text.starts_with(variable.first);
			});
			if (!replaced)
				variables.emplace_back(text);
		}
		for (const auto& [name, value] : spec.Environment)
			variables.push_back(name + "=" + value);
		std::vector<char*> envp;
		envp.reserve(variables.size() + 1);
		for (std::string& variable : variables)
			envp.push_back(variable.data());
		envp.push_back(nullptr);

		// The child reports a failed chdir/exec through this pipe; a successful exec closes it (FD_CLOEXEC), so
		// "could not start" never looks like "started and exited non-zero".
		int pipeEnds[2] = { -1, -1 };
		if (pipe(pipeEnds) != 0)
		{
			result.Error = std::string("cannot create a pipe: ") + std::strerror(errno);
			return result;
		}
		FileDescriptor statusRead{ pipeEnds[0] };
		FileDescriptor statusWrite{ pipeEnds[1] };
		if (!SetCloseOnExec(statusRead.Value) || !SetCloseOnExec(statusWrite.Value))
		{
			result.Error = std::string("cannot configure the status pipe: ") + std::strerror(errno);
			return result;
		}

		const pid_t pid = fork();
		if (pid < 0)
		{
			result.Error = std::string("cannot start '") + executable + "': " + std::strerror(errno);
			return result;
		}
		if (pid == 0)
		{
			int failure = 0;
			if (!directory.empty() && chdir(directory.c_str()) != 0)
				failure = errno;
			if (!failure)
			{
				const int input = open("/dev/null", O_RDONLY);
				if (input >= 0)
					dup2(input, STDIN_FILENO);
				dup2(output.Value, STDOUT_FILENO);
				dup2(output.Value, STDERR_FILENO);
				execve(executable.c_str(), argv.data(), envp.data());
				failure = errno;
			}
			[[maybe_unused]] const ssize_t written = write(statusWrite.Value, &failure, sizeof(failure));
			_exit(127);
		}

		close(statusWrite.Value);
		statusWrite.Value = -1;
		int childError = 0;
		ssize_t count = 0;
		do
			count = read(statusRead.Value, &childError, sizeof(childError));
		while (count < 0 && errno == EINTR);

		int status = 0;
		if (count == static_cast<ssize_t>(sizeof(childError)))
		{
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			{
			}
			result.Error = std::string("cannot start '") + executable + "': " + std::strerror(childError);
			return result;
		}

		const auto deadline = std::chrono::steady_clock::now() + spec.Timeout;
		for (;;)
		{
			const pid_t waited = waitpid(pid, &status, WNOHANG);
			if (waited == pid)
				break;
			if (waited < 0 && errno != EINTR)
			{
				result.Error = std::string("cannot wait for the process: ") + std::strerror(errno);
				kill(pid, SIGKILL);
				return result;
			}
			if (std::chrono::steady_clock::now() >= deadline)
			{
				kill(pid, SIGKILL);
				while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
				{
				}
				result.Outcome = ProcessOutcome::TimedOut;
				result.Error = "killed after " + std::to_string(spec.Timeout.count()) + " ms";
				result.Output = ReadTail(output.Value, spec.MaxOutputBytes);
				return result;
			}
			std::this_thread::sleep_for(PollInterval);
		}

		result.Outcome = ProcessOutcome::Exited;
		// A signal is reported like a shell does (128 + signal), so a crash is never mistaken for success.
		if (WIFEXITED(status))
			result.ExitCode = WEXITSTATUS(status);
		else if (WIFSIGNALED(status))
		{
			result.ExitCode = 128 + WTERMSIG(status);
			result.Error = "terminated by signal " + std::to_string(WTERMSIG(status));
		}
		result.Output = ReadTail(output.Value, spec.MaxOutputBytes);
		return result;
	}

#endif

}
