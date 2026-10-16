#include "Basalt/Core/Process.h"

#include "Basalt/Core/Base.h"
#include "Basalt/Core/FileSystem.h"

#if defined(BS_PLATFORM_WINDOWS)
	#include <windows.h>
#else
	#include <fcntl.h>
	#include <sys/wait.h>
	#include <unistd.h>

extern char** environ;
#endif

#include <algorithm>
#include <string_view>
#if defined(BS_PLATFORM_WINDOWS)
	#include <atomic>
	#include <cstddef>
	#include <cwchar>
#else
	#include <cerrno>
	#include <csignal>
	#include <cstring>
	#include <thread>
#endif

namespace Basalt {

#if defined(BS_PLATFORM_WINDOWS)

	namespace {

		// How long a terminated child may take to go away before the runner gives up on it.
		constexpr DWORD TerminateWaitMilliseconds = 10000;
		// NTSTATUS codes with both severity bits set are errors: an unhandled exception ended the process.
		constexpr DWORD ExceptionCodeMask = 0xC0000000u;

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

		// UTF-8 to UTF-16 without throwing; false for text that is not valid UTF-8.
		bool Widen(const std::string& text, std::wstring& outText)
		{
			outText.clear();
			if (text.empty())
				return true;
			const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
			if (length <= 0)
				return false;
			outText.resize(static_cast<size_t>(length));
			return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), outText.data(), length) == length;
		}

		// The name part of an environment entry ("NAME=value"; entries such as "=C:=C:\" start with '=').
		std::wstring_view VariableName(std::wstring_view entry)
		{
			const size_t equals = entry.find(L'=', 1);
			return entry.substr(0, equals == std::wstring_view::npos ? entry.size() : equals);
		}

		// Variable names are case-insensitive on Windows.
		int CompareNames(std::wstring_view a, std::wstring_view b)
		{
			return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE);
		}

		// The caller's environment block with the overrides applied, sorted by name as CreateProcess expects
		// (empty when there are no overrides, which makes the child inherit the block unchanged).
		bool BuildEnvironment(const std::vector<std::pair<std::string, std::string>>& overrides, std::wstring& outBlock, std::string& outError)
		{
			outBlock.clear();
			if (overrides.empty())
				return true;
			std::vector<std::wstring> entries;
			for (const auto& [name, value] : overrides)
			{
				std::wstring wideName, wideValue;
				if (name.empty() || name.find('=') != std::string::npos || !Widen(name, wideName) || !Widen(value, wideValue))
				{
					outError = "invalid environment variable '" + name + "'";
					return false;
				}
				entries.push_back(wideName + L"=" + wideValue);
			}
			if (wchar_t* current = GetEnvironmentStringsW())
			{
				for (const wchar_t* entry = current; *entry; entry += std::wcslen(entry) + 1)
				{
					const std::wstring_view text(entry);
					const bool replaced = std::any_of(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(overrides.size()), [&](const std::wstring& added) { return CompareNames(VariableName(text), VariableName(added)) == CSTR_EQUAL; });
					if (!replaced)
						entries.emplace_back(text);
				}
				FreeEnvironmentStringsW(current);
			}
			std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) { return CompareNames(VariableName(a), VariableName(b)) == CSTR_LESS_THAN; });
			for (const std::wstring& entry : entries)
				outBlock.append(entry).push_back(L'\0');
			outBlock.push_back(L'\0');
			return true;
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

		// The last maxBytes of the output file, or a note saying why it could not be read.
		std::string ReadTail(HANDLE file, size_t maxBytes)
		{
			LARGE_INTEGER size{};
			if (!GetFileSizeEx(file, &size))
				return "[output unavailable: " + LastErrorMessage(GetLastError()) + "]";
			if (size.QuadPart <= 0)
				return {};
			const auto total = static_cast<uint64_t>(size.QuadPart);
			const uint64_t start = total > maxBytes ? total - maxBytes : 0;
			LARGE_INTEGER offset{};
			offset.QuadPart = static_cast<LONGLONG>(start);
			std::string text(static_cast<size_t>(total - start), '\0');
			DWORD read = 0;
			if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) || !ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr))
				return "[output unavailable: " + LastErrorMessage(GetLastError()) + "]";
			text.resize(read);
			return text;
		}

		std::string Hex(DWORD value)
		{
			static constexpr char Digits[] = "0123456789ABCDEF";
			std::string text = "0x00000000";
			for (size_t i = 9; i >= 2; i--, value >>= 4)
				text[i] = Digits[value & 0xF];
			return text;
		}

	}

	ProcessResult RunProcess(const ProcessSpec& spec)
	{
		ProcessResult result;
		std::error_code error;
		const std::filesystem::path executable = std::filesystem::absolute(spec.Executable, error);
		if (error || spec.Executable.empty())
		{
			result.Error = "invalid executable path '" + FileSystem::PathToUtf8(spec.Executable) + "'";
			return result;
		}
		static std::atomic<uint32_t> s_Counter{ 0 };
		wchar_t tempDirectory[MAX_PATH + 1] = {};
		const DWORD tempLength = GetTempPathW(MAX_PATH + 1, tempDirectory);
		if (tempLength == 0 || tempLength > MAX_PATH)
		{
			result.Error = "cannot find the temporary directory: " + (tempLength == 0 ? LastErrorMessage(GetLastError()) : std::string("the path is too long"));
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
		if (input.Handle == INVALID_HANDLE_VALUE)
		{
			result.Error = "cannot open the null device: " + LastErrorMessage(GetLastError());
			return result;
		}

		std::wstring commandLine = QuoteArgument(executable.wstring());
		for (const std::string& argument : spec.Arguments)
		{
			std::wstring wide;
			if (!Widen(argument, wide))
			{
				result.Error = "an argument is not valid UTF-8 text";
				return result;
			}
			commandLine += L" " + QuoteArgument(wide);
		}
		std::wstring environment;
		if (!BuildEnvironment(spec.Environment, environment, result.Error))
			return result;

		// Only the output and input handles reach the child, not every inheritable handle of this process (an
		// editor's server sockets, for one).
		SIZE_T attributeSize = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
		std::vector<std::byte> attributeBuffer(attributeSize);
		auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
		HANDLE inherited[2] = { output.Handle, input.Handle };
		if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize))
		{
			result.Error = "cannot prepare the child's handles: " + LastErrorMessage(GetLastError());
			return result;
		}
		const bool listed = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);
		if (!listed)
		{
			result.Error = "cannot prepare the child's handles: " + LastErrorMessage(GetLastError());
			DeleteProcThreadAttributeList(attributes);
			return result;
		}

		STARTUPINFOEXW startup{};
		startup.StartupInfo.cb = sizeof(startup);
		startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup.StartupInfo.hStdInput = input.Handle;
		startup.StartupInfo.hStdOutput = output.Handle;
		startup.StartupInfo.hStdError = output.Handle;
		startup.lpAttributeList = attributes;
		PROCESS_INFORMATION process{};
		const std::wstring directory = spec.WorkingDirectory.wstring();
		// The child inherits the error mode: a crash then ends it instead of waiting on an error-report dialog
		// (which would turn every crash into a timeout).
		const UINT previousErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
		const BOOL created = CreateProcessW(executable.wstring().c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, environment.empty() ? nullptr : environment.data(), directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &process);
		const DWORD createError = GetLastError();
		SetErrorMode(previousErrorMode);
		DeleteProcThreadAttributeList(attributes);
		if (!created)
		{
			result.Error = "cannot start '" + FileSystem::PathToUtf8(executable) + "': " + LastErrorMessage(createError);
			return result;
		}
		HandleGuard processHandle{ process.hProcess };
		HandleGuard threadHandle{ process.hThread };

		const auto timeout = static_cast<DWORD>(std::clamp<long long>(spec.Timeout.count(), 0, INFINITE - 1));
		const DWORD wait = WaitForSingleObject(process.hProcess, timeout);
		if (wait != WAIT_OBJECT_0)
		{
			const DWORD waitError = GetLastError();
			const bool terminated = TerminateProcess(process.hProcess, 1) && WaitForSingleObject(process.hProcess, TerminateWaitMilliseconds) == WAIT_OBJECT_0;
			result.Outcome = wait == WAIT_TIMEOUT ? ProcessOutcome::TimedOut : ProcessOutcome::Failed;
			result.Error = wait == WAIT_TIMEOUT ? "killed after " + std::to_string(spec.Timeout.count()) + " ms" : "cannot wait for the process: " + LastErrorMessage(waitError);
			if (!terminated)
				result.Error += " (and it could not be terminated: " + LastErrorMessage(GetLastError()) + ")";
		}
		else if (DWORD exitCode = 0; !GetExitCodeProcess(process.hProcess, &exitCode))
		{
			result.Outcome = ProcessOutcome::Failed;
			result.Error = "cannot read the exit code: " + LastErrorMessage(GetLastError());
		}
		else
		{
			result.ExitCode = static_cast<int>(exitCode);
			result.Outcome = (exitCode & ExceptionCodeMask) == ExceptionCodeMask ? ProcessOutcome::Crashed : ProcessOutcome::Exited;
			if (result.Outcome == ProcessOutcome::Crashed)
				result.Error = "ended by an unhandled exception " + Hex(exitCode);
		}
		result.Output = ReadTail(output.Handle, spec.MaxOutputBytes);
		return result;
	}

#else

	namespace {

		// How often the parent checks whether the child has exited.
		constexpr std::chrono::milliseconds PollInterval{ 5 };

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

		// The last maxBytes of the output file, or a note saying why it could not be read.
		std::string ReadTail(int file, size_t maxBytes)
		{
			const off_t size = lseek(file, 0, SEEK_END);
			if (size < 0)
				return std::string("[output unavailable: ") + std::strerror(errno) + "]";
			const auto total = static_cast<size_t>(size);
			const size_t start = total > maxBytes ? total - maxBytes : 0;
			std::string text(total - start, '\0');
			size_t done = 0;
			while (done < text.size())
			{
				const ssize_t count = pread(file, text.data() + done, text.size() - done, static_cast<off_t>(start) + static_cast<off_t>(done));
				if (count < 0 && errno == EINTR)
					continue;
				if (count < 0)
					return std::string("[output unavailable: ") + std::strerror(errno) + "]";
				if (count == 0)
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

		// Reaps the child, retrying when a signal interrupts the wait.
		void WaitForChild(pid_t pid, int& status)
		{
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			{
			}
		}

	}

	ProcessResult RunProcess(const ProcessSpec& spec)
	{
		ProcessResult result;
		std::error_code error;
		const std::filesystem::path program = std::filesystem::absolute(spec.Executable, error);
		if (error || spec.Executable.empty())
		{
			result.Error = "invalid executable path '" + spec.Executable.string() + "'";
			return result;
		}
		const std::filesystem::path temporary = std::filesystem::temp_directory_path(error);
		if (error)
		{
			result.Error = "cannot find the temporary directory: " + error.message();
			return result;
		}
		std::string pattern = (temporary / "basalt-process-XXXXXX").string();
		// Close-on-exec: the child gets the file only as its stdout/stderr (dup2 clears the flag on those copies).
		FileDescriptor output{ mkostemp(pattern.data(), O_CLOEXEC) };
		if (output.Value < 0)
		{
			result.Error = std::string("cannot create the output file: ") + std::strerror(errno);
			return result;
		}
		// The open descriptor keeps the file alive; nothing is left behind if this process dies.
		unlink(pattern.c_str());
		FileDescriptor input{ open("/dev/null", O_RDONLY | O_CLOEXEC) };
		if (input.Value < 0)
		{
			result.Error = std::string("cannot open /dev/null: ") + std::strerror(errno);
			return result;
		}

		// Everything the child needs is prepared before fork: between fork and exec only async-signal-safe calls
		// are allowed (other threads may hold the allocator's locks).
		const std::string executable = program.string();
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
		for (const auto& [name, value] : spec.Environment)
			if (name.empty() || name.find('=') != std::string::npos)
			{
				result.Error = "invalid environment variable '" + name + "'";
				return result;
			}
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
			variables.emplace_back(name).append("=").append(value);
		std::vector<char*> envp;
		envp.reserve(variables.size() + 1);
		for (std::string& variable : variables)
			envp.push_back(variable.data());
		envp.push_back(nullptr);

		// The child reports a failed chdir/dup2/exec through this pipe; a successful exec closes it (FD_CLOEXEC),
		// so "could not start" never looks like "started and exited non-zero". (pipe2 is not portable to macOS; a
		// fork elsewhere between pipe and fcntl could inherit the write end, which this process never does.)
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
			// Each step only runs when the previous one worked; execve returns only on failure.
			const bool prepared = (directory.empty() || chdir(directory.c_str()) == 0) && dup2(input.Value, STDIN_FILENO) >= 0 && dup2(output.Value, STDOUT_FILENO) >= 0 && dup2(output.Value, STDERR_FILENO) >= 0;
			if (prepared)
				execve(executable.c_str(), argv.data(), envp.data());
			const int failure = errno;
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
		if (count != 0)
		{
			// Anything but end-of-file (the exec closed the pipe) means the child did not become the program.
			const int readError = errno;
			if (count < 0)
				kill(pid, SIGKILL);
			WaitForChild(pid, status);
			if (count == static_cast<ssize_t>(sizeof(childError)))
				result.Error = std::string("cannot start '") + executable + "': " + std::strerror(childError);
			else
				result.Error = std::string("cannot tell whether '") + executable + "' started: " + (count < 0 ? std::strerror(readError) : "a short status report");
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
				// ECHILD: someone else reaped it (SIGCHLD ignored); the pid may already be reused, so never kill it.
				const int waitError = errno;
				if (waitError != ECHILD)
				{
					kill(pid, SIGKILL);
					WaitForChild(pid, status);
				}
				result.Error = std::string("cannot wait for the process: ") + std::strerror(waitError);
				result.Output = ReadTail(output.Value, spec.MaxOutputBytes);
				return result;
			}
			if (std::chrono::steady_clock::now() >= deadline)
			{
				kill(pid, SIGKILL);
				WaitForChild(pid, status);
				result.Outcome = ProcessOutcome::TimedOut;
				result.Error = "killed after " + std::to_string(spec.Timeout.count()) + " ms";
				result.Output = ReadTail(output.Value, spec.MaxOutputBytes);
				return result;
			}
			std::this_thread::sleep_for(PollInterval);
		}

		if (WIFSIGNALED(status))
		{
			// Reported like a shell does (128 + signal), so a crash is never mistaken for success.
			result.Outcome = ProcessOutcome::Crashed;
			result.ExitCode = 128 + WTERMSIG(status);
			const char* name = strsignal(WTERMSIG(status));
			result.Error = "terminated by signal " + std::to_string(WTERMSIG(status)) + (name ? std::string(" (") + name + ")" : std::string());
		}
		else
		{
			result.Outcome = ProcessOutcome::Exited;
			result.ExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		}
		result.Output = ReadTail(output.Value, spec.MaxOutputBytes);
		return result;
	}

#endif

}
