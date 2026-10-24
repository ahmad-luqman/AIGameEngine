#include "Basalt/Core/CrashReporter.h"

#include "Basalt/Core/Base.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/Platform.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <exception>
#include <iterator>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#if defined(BS_PLATFORM_WINDOWS)
	#include <windows.h>
	// After windows.h, which it needs.
	#include <dbghelp.h>
	#include <cstdlib>
	#include <cwchar>
	#include <process.h>
#else
	#include <fcntl.h>
	#include <spawn.h>
	#include <sys/stat.h>
	#include <unistd.h>
	#if __has_include(<execinfo.h>)
		#include <execinfo.h>
		#define BS_CRASH_BACKTRACE 1
	#endif
// The environment the dialog program inherits (posix_spawn needs it explicitly).
extern char** environ;
#endif

// AddressSanitizer reports these crashes itself (with better traces) and needs the signals to do so.
#if defined(__SANITIZE_ADDRESS__)
	#define BS_CRASH_ASAN 1
#elif defined(__has_feature)
	#if __has_feature(address_sanitizer)
		#define BS_CRASH_ASAN 1
	#endif
#endif
#if !defined(BS_CRASH_ASAN)
	#define BS_CRASH_ASAN 0
#endif

namespace Basalt {

	namespace {

		constexpr size_t MaxPathLength = 1024;
		constexpr size_t MaxContextLength = 255;
		constexpr size_t MaxApplicationLength = 127;
		constexpr size_t MaxTerminateMessageLength = 511;
		// The missing folders above a report folder the handler may create (a fresh account without ~/.local, a nested
		// BASALT_USER_DATA_DIR that does not exist yet).
		constexpr size_t MaxDirectoryLevels = 8;
		constexpr std::string_view StackTraceHeading = "\nStack trace:\n";
		constexpr std::string_view LogHeading = "\nLast log lines (oldest first):\n";

		// Guards the log lines and the context. Writers wait for it; the crash handler gives up after a bounded number of
		// attempts (the crashed thread may be the holder) and reads whatever is there.
		std::atomic_flag s_Lock;
		std::once_flag s_InstallOnce;
		std::atomic<bool> s_InstallResult = false;
		std::atomic<bool> s_Handling = false;
		// Set on the thread writing the report: a crash inside the handler dies at once, while other threads that crash
		// meanwhile wait for the report to be finished.
		thread_local bool t_Handling = false;
		std::atomic<bool> s_DialogEnabled = false;

		char s_Application[MaxApplicationLength + 1] = "Basalt";
		char s_Context[MaxContextLength + 1] = {};
		char s_TerminateMessage[MaxTerminateMessageLength + 1] = {};
		struct LogLine
		{
			char Text[CrashReporter::MaxLogLineLength + 1] = {};
			size_t Length = 0;
		};
		LogLine s_Lines[CrashReporter::MaxLogLines];
		uint64_t s_LineCount = 0;
		// Written by Install, before other threads exist; read-only afterwards.
		std::filesystem::path s_ReportDirectory;
		// The handler's own text buffer (static storage: the handler may run on a small alternate stack).
		CrashText s_Text;

		class LockGuard
		{
		public:
			LockGuard()
			{
				// The handler takes what it can get rather than wait for a thread that may never release it.
				const bool crashing = s_Handling.load();
				for (int attempt = 0; s_Lock.test_and_set(std::memory_order_acquire); attempt++)
				{
					if (crashing && attempt > 100000)
						return;
					if (!crashing)
						std::this_thread::yield();
				}
				m_Held = true;
			}
			~LockGuard()
			{
				if (m_Held)
					s_Lock.clear(std::memory_order_release);
			}
			LockGuard(const LockGuard&) = delete;
			LockGuard& operator=(const LockGuard&) = delete;

		private:
			bool m_Held = false;
		};

		// Copies text into a fixed buffer of capacity + 1 bytes, NUL-terminated, returning the copied length.
		size_t CopyText(char* destination, size_t capacity, std::string_view text)
		{
			const size_t length = std::min(text.size(), capacity);
			std::copy_n(text.begin(), length, destination);
			destination[length] = '\0';
			return length;
		}

		// The application name without characters that would leave the report folder.
		std::string SanitizeApplicationName(std::string_view name)
		{
			std::string result;
			for (const char c : name)
				if (c != '/' && c != '\\' && c != ':' && c != '\0')
					result += c;
			return result.empty() || result == "." || result == ".." ? std::string("Basalt") : result;
		}

		const char* PlatformName()
		{
#if defined(BS_PLATFORM_WINDOWS)
			return "Windows";
#elif defined(BS_PLATFORM_MACOS)
			return "macOS";
#else
			return "Linux";
#endif
		}

		std::terminate_handler s_PreviousTerminate = nullptr;

		// Keeps an uncaught exception's message for the report, then ends the process as before (abort: SIGABRT). Per thread
		// on MSVC, whose terminate handler is per thread: exceptions on other threads report without their message.
		void OnTerminate()
		{
			if (const std::exception_ptr current = std::current_exception())
			{
				try
				{
					std::rethrow_exception(current);
				}
				catch (const std::exception& exception)
				{
					CopyText(s_TerminateMessage, MaxTerminateMessageLength, exception.what());
				}
				catch (...)
				{
					CopyText(s_TerminateMessage, MaxTerminateMessageLength, "an exception that is not a std::exception");
				}
			}
			if (s_PreviousTerminate)
				s_PreviousTerminate();
			std::abort();
		}

		// The report file name inside a folder: crash-<unix time>-<process>.<extension>.
		template<size_t Capacity>
		void FormatReportName(BasicCrashText<Capacity>& out, uint64_t unixTime, uint64_t processId, std::string_view extension)
		{
			out.Append("crash-");
			out.AppendDecimal(unixTime);
			out.Append("-");
			out.AppendDecimal(processId);
			out.Append(extension);
		}

		// The report folder and its missing ancestors, outermost first, for the handler to create; at most
		// MaxDirectoryLevels. Reads the disk at Install; writes nothing.
		std::vector<std::filesystem::path> MissingDirectoryChain(const std::filesystem::path& directory)
		{
			std::vector<std::filesystem::path> chain;
			std::error_code error;
			for (std::filesystem::path current = directory; !current.empty() && chain.size() < MaxDirectoryLevels; current = current.parent_path())
			{
				chain.push_back(current);
				if (std::filesystem::exists(current, error) || current == current.parent_path())
					break;
			}
			std::reverse(chain.begin(), chain.end());
			return chain;
		}

#if !defined(BS_PLATFORM_WINDOWS)
		char s_Directories[MaxDirectoryLevels][MaxPathLength + 1] = {};
		char s_FallbackDirectory[MaxPathLength + 1] = {};
		// Room for the handler on a stack overflow. Per thread: other threads' overflows are not reported.
		alignas(16) char s_AlternateStack[256 * 1024];
		constexpr int HandledSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
		struct sigaction s_PreviousActions[std::size(HandledSignals)] = {};

		// The dialog program found at Install. The crashed process starts it with posix_spawn, which (unlike fork) runs no
		// atfork handlers, so it cannot deadlock on a malloc lock the crash left held; its stdio goes to /dev/null, so it
		// never holds a parent's output pipes open.
		const char* s_DialogProgram = nullptr;
		enum class DialogKind
		{
			Osascript,
			Zenity,
			KDialog,
			XMessage
		};
		DialogKind s_DialogKind = DialogKind::Osascript;
		posix_spawn_file_actions_t s_DialogActions;
		char s_DialogApplication[MaxApplicationLength * 2 + 1] = {};

		// False when the text could not be written whole (a full disk, an I/O error).
		bool WriteAll(int fd, std::string_view text)
		{
			while (!text.empty())
			{
				const ssize_t written = write(fd, text.data(), text.size());
				if (written < 0 && errno == EINTR)
					continue;
				if (written <= 0)
					return false;
				text.remove_prefix(static_cast<size_t>(written));
			}
			return true;
		}

		// Opens a new report in directory; path receives its name. -1 when it cannot be created.
		int OpenReport(const char* directory, uint64_t unixTime, uint64_t processId, CrashText& path)
		{
			if (directory[0] == '\0')
				return -1;
			path.Clear();
			path.Append(directory);
			path.Append("/");
			FormatReportName(path, unixTime, processId, ".txt");
			return open(path.CStr(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
		}

		// Escapes a string for an AppleScript string literal ('"' and '\').
		template<size_t Capacity>
		void AppendEscaped(BasicCrashText<Capacity>& out, std::string_view text)
		{
			for (const char c : text)
			{
				if (c == '"' || c == '\\')
					out.Append("\\");
				out.Append(std::string_view(&c, 1));
			}
		}

		// Starts the dialog program without waiting for it: the crashed process dies at once, as the OS expects, and the
		// dialog stays until the player closes it.
		void ShowDialog(std::string_view message)
		{
			if (!s_DialogEnabled || !s_DialogProgram)
				return;
			static CrashText s_Message;
			static CrashText s_Script;
			s_Message.Clear();
			s_Message.Append(message);
			char* arguments[8] = {};
			const auto argument = [](const char* text) { return const_cast<char*>(text); };
			switch (s_DialogKind)
			{
				case DialogKind::Osascript:
					s_Script.Clear();
					s_Script.Append("display alert \"");
					s_Script.Append(s_DialogApplication);
					s_Script.Append(" crashed\" message \"");
					AppendEscaped(s_Script, message);
					s_Script.Append("\" as critical buttons {\"OK\"}");
					arguments[0] = argument("osascript");
					arguments[1] = argument("-e");
					arguments[2] = argument(s_Script.CStr());
					break;
				case DialogKind::Zenity:
					arguments[0] = argument("zenity");
					arguments[1] = argument("--error");
					arguments[2] = argument("--no-markup");
					arguments[3] = argument("--title");
					arguments[4] = argument(s_Application);
					arguments[5] = argument("--text");
					arguments[6] = argument(s_Message.CStr());
					break;
				case DialogKind::KDialog:
					arguments[0] = argument("kdialog");
					arguments[1] = argument("--title");
					arguments[2] = argument(s_Application);
					arguments[3] = argument("--error");
					arguments[4] = argument(s_Message.CStr());
					break;
				case DialogKind::XMessage:
					arguments[0] = argument("xmessage");
					arguments[1] = argument("-center");
					arguments[2] = argument(s_Message.CStr());
					break;
			}
			pid_t child = 0;
			posix_spawn(&child, s_DialogProgram, &s_DialogActions, nullptr, arguments, environ);
		}

		void ResetAndRaise(int signal)
		{
			struct sigaction action = {};
			action.sa_handler = SIG_DFL;
			sigemptyset(&action.sa_mask);
			sigaction(signal, &action, nullptr);
			raise(signal);
		}

		void HandleSignal(int signal, siginfo_t* info, void*)
		{
			if (s_Handling.exchange(true))
			{
				// A crash inside the handler dies at once; another thread waits for the report, then dies with the process.
				if (t_Handling)
				{
					ResetAndRaise(signal);
					return;
				}
				for (;;)
					pause();
			}
			t_Handling = true;
			const auto unixTime = static_cast<uint64_t>(time(nullptr));
			const auto processId = static_cast<uint64_t>(getpid());
			const char* described = CrashReporter::DescribeSignal(signal);
			const std::string_view reason = described ? described : "a fatal signal";
			std::optional<uint64_t> address;
			if (info && (signal == SIGSEGV || signal == SIGBUS || signal == SIGILL || signal == SIGFPE))
				address = reinterpret_cast<uintptr_t>(info->si_addr);

			static CrashText s_Path;
			for (const char* directory : s_Directories)
				if (directory[0] != '\0')
					mkdir(directory, 0755);
			// The report folder is the last level in use.
			const char* reportDirectory = "";
			for (const char* directory : s_Directories)
				if (directory[0] != '\0')
					reportDirectory = directory;
			int fd = OpenReport(reportDirectory, unixTime, processId, s_Path);
			int openError = fd < 0 ? errno : 0;
			if (fd < 0 && s_FallbackDirectory[0] != '\0')
			{
				mkdir(s_FallbackDirectory, 0755);
				fd = OpenReport(s_FallbackDirectory, unixTime, processId, s_Path);
				if (fd < 0)
					openError = errno;
			}
			bool complete = fd >= 0;
			if (fd >= 0)
			{
				s_Text.Clear();
				CrashReporter::FormatHeader(s_Text, reason, address, unixTime, processId);
				s_Text.Append(StackTraceHeading);
				complete = WriteAll(fd, s_Text.View()) && complete;
	#if defined(BS_CRASH_BACKTRACE)
				void* frames[128];
				const int count = backtrace(frames, 128);
				backtrace_symbols_fd(frames, count, fd);
	#else
				complete = WriteAll(fd, "(not available on this platform)\n") && complete;
	#endif
				s_Text.Clear();
				s_Text.Append(LogHeading);
				CrashReporter::FormatLogLines(s_Text);
				complete = WriteAll(fd, s_Text.View()) && complete;
				close(fd);
			}
			// What the player and stderr are told.
			static CrashText s_Summary;
			s_Summary.Clear();
			s_Summary.Append(s_Application);
			s_Summary.Append(" crashed: ");
			s_Summary.Append(reason);
			if (fd >= 0)
			{
				s_Summary.Append(complete ? ". Crash report: " : ". The crash report is incomplete (it could not be written whole): ");
				s_Summary.Append(s_Path.View());
			}
			else
			{
				s_Summary.Append(". No crash report could be written (errno ");
				s_Summary.AppendDecimal(static_cast<uint64_t>(openError));
				s_Summary.Append(").");
			}
			WriteAll(STDERR_FILENO, s_Summary.View());
			WriteAll(STDERR_FILENO, "\n");
			ShowDialog(s_Summary.View());
			ResetAndRaise(signal);
		}

		void RestoreSignals(size_t count)
		{
			for (size_t i = 0; i < count; i++)
				sigaction(HandledSignals[i], &s_PreviousActions[i], nullptr);
		}

		bool InstallPlatform()
		{
			const auto chain = MissingDirectoryChain(s_ReportDirectory);
			for (size_t i = 0; i < chain.size(); i++)
			{
				const std::string directory = FileSystem::PathToUtf8(chain[i]);
				if (directory.size() <= MaxPathLength)
					CopyText(s_Directories[i], MaxPathLength, directory);
				else
					BS_CORE_WARN("Crash reports: the folder '{}' is longer than {} bytes; reports go next to the executable", directory, MaxPathLength);
			}
			const std::string fallback = FileSystem::PathToUtf8(FileSystem::GetExecutableDirectory() / "CrashReports");
			if (fallback.size() <= MaxPathLength)
				CopyText(s_FallbackDirectory, MaxPathLength, fallback);
	#if defined(BS_CRASH_BACKTRACE)
			// The first call loads the unwinder (glibc dlopens libgcc_s), which must not happen inside the handler.
			void* warmUp[4];
			backtrace(warmUp, 4);
	#endif
	#if defined(BS_PLATFORM_MACOS)
			if (access("/usr/bin/osascript", X_OK) == 0)
			{
				s_DialogProgram = "/usr/bin/osascript";
				s_DialogKind = DialogKind::Osascript;
			}
			BasicCrashText<MaxApplicationLength * 2> escaped;
			AppendEscaped(escaped, s_Application);
			CopyText(s_DialogApplication, sizeof(s_DialogApplication) - 1, escaped.View());
	#else
			// Only with a display to show it on.
			if (Platform::GetEnvVar("DISPLAY") || Platform::GetEnvVar("WAYLAND_DISPLAY"))
			{
				for (const auto& [program, kind] : { std::pair{ "/usr/bin/zenity", DialogKind::Zenity }, std::pair{ "/usr/bin/kdialog", DialogKind::KDialog }, std::pair{ "/usr/bin/xmessage", DialogKind::XMessage } })
				{
					if (access(program, X_OK) == 0)
					{
						s_DialogProgram = program;
						s_DialogKind = kind;
						break;
					}
				}
			}
	#endif
			// Prepared now: setting up file actions allocates.
			if (s_DialogProgram && (posix_spawn_file_actions_init(&s_DialogActions) != 0 || posix_spawn_file_actions_addopen(&s_DialogActions, 0, "/dev/null", O_RDONLY, 0) != 0 || posix_spawn_file_actions_addopen(&s_DialogActions, 1, "/dev/null", O_WRONLY, 0) != 0 || posix_spawn_file_actions_adddup2(&s_DialogActions, 1, 2) != 0))
			{
				BS_CORE_WARN("Crash reports: no crash dialog (cannot prepare its process)");
				s_DialogProgram = nullptr;
			}
			stack_t stack = {};
			stack.ss_sp = s_AlternateStack;
			stack.ss_size = sizeof(s_AlternateStack);
			if (sigaltstack(&stack, nullptr) != 0)
				BS_CORE_WARN("Crash reports: no alternate signal stack ({}); a stack overflow will not be reported", std::generic_category().message(errno));
			struct sigaction action = {};
			action.sa_sigaction = &HandleSignal;
			sigemptyset(&action.sa_mask);
			// No SA_RESETHAND: a second thread's crash must reach the handler (and wait) rather than end the process mid-report.
			action.sa_flags = SA_SIGINFO | SA_ONSTACK;
			for (size_t i = 0; i < std::size(HandledSignals); i++)
			{
				if (sigaction(HandledSignals[i], &action, &s_PreviousActions[i]) != 0)
				{
					BS_CORE_ERROR("Crash reports: cannot handle signal {}: {}", HandledSignals[i], std::generic_category().message(errno));
					RestoreSignals(i);
					return false;
				}
			}
			return true;
		}
#else
		wchar_t s_Directories[MaxDirectoryLevels][MaxPathLength + 1] = {};
		wchar_t s_FallbackDirectory[MaxPathLength + 1] = {};
		wchar_t s_WideApplication[MaxApplicationLength + 1] = {};
		LPTOP_LEVEL_EXCEPTION_FILTER s_PreviousFilter = nullptr;
		// MSVC's code for C++ exceptions.
		constexpr DWORD CppExceptionCode = 0xE06D7363;

		// Appends text to a NUL-terminated wide buffer of capacity + 1 characters.
		void AppendWide(wchar_t* destination, size_t capacity, const wchar_t* text)
		{
			size_t length = wcslen(destination);
			for (; *text && length < capacity; text++)
				destination[length++] = *text;
			destination[length] = L'\0';
		}

		void AppendAscii(wchar_t* destination, size_t capacity, std::string_view text)
		{
			size_t length = wcslen(destination);
			for (size_t i = 0; i < text.size() && length < capacity; i++)
				destination[length++] = static_cast<wchar_t>(text[i]);
			destination[length] = L'\0';
		}

		// The report's path without extension (<directory>\crash-<time>-<process>) in a wide buffer; false when the folder is
		// unusable.
		bool BuildReportBase(const wchar_t* directory, uint64_t unixTime, uint64_t processId, wchar_t* out, size_t capacity)
		{
			if (directory[0] == L'\0')
				return false;
			out[0] = L'\0';
			AppendWide(out, capacity, directory);
			AppendWide(out, capacity, L"\\");
			// Small enough for the stack left after a stack overflow.
			BasicCrashText<64> name;
			FormatReportName(name, unixTime, processId, "");
			AppendAscii(out, capacity, name.View());
			return true;
		}

		HANDLE CreateReportFile(const wchar_t* base, const wchar_t* extension, wchar_t* path, size_t capacity)
		{
			path[0] = L'\0';
			AppendWide(path, capacity, base);
			AppendWide(path, capacity, extension);
			return CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		}

		// False when the text could not be written whole.
		bool WriteAll(HANDLE file, std::string_view text)
		{
			while (!text.empty())
			{
				DWORD written = 0;
				if (!WriteFile(file, text.data(), static_cast<DWORD>(std::min<size_t>(text.size(), 1u << 20)), &written, nullptr) || written == 0)
					return false;
				text.remove_prefix(written);
			}
			return true;
		}

		template<size_t Capacity>
		void AppendUtf8(BasicCrashText<Capacity>& out, const wchar_t* text)
		{
			static char s_Buffer[MaxPathLength * 3 + 1];
			const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, s_Buffer, static_cast<int>(sizeof(s_Buffer)), nullptr, nullptr);
			if (length > 1)
				out.Append(std::string_view(s_Buffer, static_cast<size_t>(length - 1)));
		}

		void AppendStackTrace(CrashText& out)
		{
			void* frames[62];
			const USHORT count = CaptureStackBackTrace(0, 62, frames, nullptr);
			static char s_SymbolStorage[sizeof(SYMBOL_INFO) + 256];
			auto* symbol = reinterpret_cast<SYMBOL_INFO*>(s_SymbolStorage);
			for (USHORT i = 0; i < count; i++)
			{
				const auto address = reinterpret_cast<DWORD64>(frames[i]);
				out.Append("  ");
				out.AppendHex(address);
				symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
				symbol->MaxNameLen = 255;
				DWORD64 displacement = 0;
				// May load symbol files (SYMOPT_DEFERRED_LOADS): heavier than the POSIX handler, but only names frames.
				if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol))
				{
					out.Append(" ");
					out.Append(std::string_view(symbol->Name, symbol->NameLen));
					out.Append(" + ");
					out.AppendDecimal(displacement);
				}
				out.Append("\n");
			}
		}

		// Writes the text report (and, with exception pointers, a minidump), tells stderr and, when enabled, the player.
		void WriteReport(std::string_view reason, std::optional<uint64_t> address, EXCEPTION_POINTERS* exception)
		{
			const auto unixTime = static_cast<uint64_t>(_time64(nullptr));
			const auto processId = static_cast<uint64_t>(GetCurrentProcessId());
			static wchar_t s_Base[MaxPathLength + 64];
			static wchar_t s_TextPath[MaxPathLength + 80];
			static wchar_t s_DumpPath[MaxPathLength + 80];
			const wchar_t* reportDirectory = L"";
			for (const wchar_t* directory : s_Directories)
			{
				if (directory[0] != L'\0')
				{
					CreateDirectoryW(directory, nullptr);
					reportDirectory = directory;
				}
			}
			HANDLE file = INVALID_HANDLE_VALUE;
			if (BuildReportBase(reportDirectory, unixTime, processId, s_Base, std::size(s_Base) - 1))
				file = CreateReportFile(s_Base, L".txt", s_TextPath, std::size(s_TextPath) - 1);
			if (file == INVALID_HANDLE_VALUE && BuildReportBase(s_FallbackDirectory, unixTime, processId, s_Base, std::size(s_Base) - 1))
			{
				CreateDirectoryW(s_FallbackDirectory, nullptr);
				file = CreateReportFile(s_Base, L".txt", s_TextPath, std::size(s_TextPath) - 1);
			}
			const DWORD openError = file == INVALID_HANDLE_VALUE ? GetLastError() : 0;
			const bool written = file != INVALID_HANDLE_VALUE;
			bool complete = written;
			if (written)
			{
				bool dumped = false;
				const HANDLE dump = CreateReportFile(s_Base, L".dmp", s_DumpPath, std::size(s_DumpPath) - 1);
				if (dump != INVALID_HANDLE_VALUE)
				{
					MINIDUMP_EXCEPTION_INFORMATION information = { GetCurrentThreadId(), exception, FALSE };
					dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo), exception ? &information : nullptr, nullptr, nullptr) != FALSE;
					CloseHandle(dump);
					// No empty or partial dump beside the report.
					if (!dumped)
						DeleteFileW(s_DumpPath);
				}
				s_Text.Clear();
				CrashReporter::FormatHeader(s_Text, reason, address, unixTime, processId);
				if (dumped)
				{
					s_Text.Append("Minidump: ");
					AppendUtf8(s_Text, s_DumpPath);
					s_Text.Append("\n");
				}
				s_Text.Append(StackTraceHeading);
				AppendStackTrace(s_Text);
				s_Text.Append(LogHeading);
				CrashReporter::FormatLogLines(s_Text);
				complete = WriteAll(file, s_Text.View());
				CloseHandle(file);
			}
			static CrashText s_Summary;
			s_Summary.Clear();
			s_Summary.Append(s_Application);
			s_Summary.Append(" crashed: ");
			s_Summary.Append(reason);
			if (written)
			{
				s_Summary.Append(complete ? ". Crash report: " : ". The crash report is incomplete (it could not be written whole): ");
				AppendUtf8(s_Summary, s_TextPath);
			}
			else
			{
				s_Summary.Append(". No crash report could be written (error ");
				s_Summary.AppendDecimal(openError);
				s_Summary.Append(").");
			}
			s_Summary.Append("\n");
			WriteAll(GetStdHandle(STD_ERROR_HANDLE), s_Summary.View());
			if (s_DialogEnabled)
			{
				static wchar_t s_Message[MaxPathLength + 512];
				s_Message[0] = L'\0';
				AppendWide(s_Message, std::size(s_Message) - 1, s_WideApplication);
				AppendWide(s_Message, std::size(s_Message) - 1, L" crashed.");
				if (written)
				{
					AppendWide(s_Message, std::size(s_Message) - 1, complete ? L"\n\nA crash report was saved to:\n" : L"\n\nAn incomplete crash report was saved to:\n");
					AppendWide(s_Message, std::size(s_Message) - 1, s_TextPath);
				}
				else
					AppendWide(s_Message, std::size(s_Message) - 1, L"\n\nNo crash report could be written.");
				MessageBoxW(nullptr, s_Message, s_WideApplication, MB_OK | MB_ICONERROR | MB_TASKMODAL);
			}
		}

		// Whether this thread may write the report: false for a crash inside the handler; another thread that crashes
		// meanwhile waits here for the report, then dies with the process.
		bool BeginReport()
		{
			if (!s_Handling.exchange(true))
			{
				t_Handling = true;
				return true;
			}
			if (!t_Handling)
				Sleep(INFINITE);
			return false;
		}

		LONG WINAPI HandleException(EXCEPTION_POINTERS* exception)
		{
			const EXCEPTION_RECORD& record = *exception->ExceptionRecord;
			// The C runtime's own filter turns an uncaught C++ exception into std::terminate, where OnTerminate keeps its
			// message and abort then reports (HandleAbort).
			if (record.ExceptionCode == CppExceptionCode && s_PreviousFilter && !s_Handling)
			{
				const LONG previous = s_PreviousFilter(exception);
				if (previous != EXCEPTION_CONTINUE_SEARCH)
					return previous;
			}
			if (!BeginReport())
				return EXCEPTION_CONTINUE_SEARCH;
			std::optional<uint64_t> address = reinterpret_cast<uintptr_t>(record.ExceptionAddress);
			// For access violations, the address that could not be read or written.
			if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && record.NumberParameters >= 2)
				address = static_cast<uint64_t>(record.ExceptionInformation[1]);
			WriteReport(CrashReporter::DescribeException(record.ExceptionCode), address, exception);
			// The process ends with the exception code, as without the reporter (but without Windows Error Reporting's
			// dialog, which SEM_NOGPFAULTERRORBOX suppresses).
			return EXCEPTION_EXECUTE_HANDLER;
		}

		// abort() (failed checks, std::terminate) and C runtime failures bypass the unhandled exception filter: report, then
		// exit with abort's code.
		[[noreturn]] void ReportAndExit(std::string_view reason)
		{
			if (BeginReport())
				WriteReport(reason, std::nullopt, nullptr);
			_exit(3);
		}

		void HandleAbort(int)
		{
			ReportAndExit(CrashReporter::DescribeSignal(SIGABRT));
		}

		void HandleInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
		{
			ReportAndExit("an invalid parameter passed to a C runtime function");
		}

		void HandlePureCall()
		{
			ReportAndExit("a pure virtual function call");
		}

		bool InstallPlatform()
		{
			const auto chain = MissingDirectoryChain(s_ReportDirectory);
			for (size_t i = 0; i < chain.size(); i++)
			{
				const std::wstring directory = chain[i].wstring();
				if (directory.size() <= MaxPathLength)
					wcsncpy_s(s_Directories[i], directory.c_str(), _TRUNCATE);
				else
					BS_CORE_WARN("Crash reports: the folder '{}' is longer than {} characters; reports go next to the executable", FileSystem::PathToUtf8(chain[i]), MaxPathLength);
			}
			const std::wstring fallback = (FileSystem::GetExecutableDirectory() / "CrashReports").wstring();
			if (fallback.size() <= MaxPathLength)
				wcsncpy_s(s_FallbackDirectory, fallback.c_str(), _TRUNCATE);
			const std::wstring application = FileSystem::PathFromUtf8(s_Application).wstring();
			wcsncpy_s(s_WideApplication, application.c_str(), _TRUNCATE);
			SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
			if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE))
				BS_CORE_WARN("Crash reports: stack traces will have no function names (SymInitialize failed: {})", GetLastError());
			SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
			_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
			std::signal(SIGABRT, &HandleAbort);
			_set_invalid_parameter_handler(&HandleInvalidParameter);
			_set_purecall_handler(&HandlePureCall);
			// Stack for the filter after a stack overflow on this thread (the minidump writer and symbol lookup need room).
			ULONG guarantee = 128 * 1024;
			SetThreadStackGuarantee(&guarantee);
			s_PreviousFilter = SetUnhandledExceptionFilter(&HandleException);
			return true;
		}
#endif

		bool InstallOnce(const CrashReporterOptions& options)
		{
			// Not #if: sanitizer builds still compile the handlers (keeping them referenced for -Werror).
			if constexpr (BS_CRASH_ASAN != 0)
			{
				BS_CORE_INFO("Crash reports: off under AddressSanitizer, which reports crashes itself");
				return false;
			}
			const std::string application = SanitizeApplicationName(options.ApplicationName);
			{
				LockGuard lock;
				CopyText(s_Application, MaxApplicationLength, application);
			}
			std::string error;
			if (const auto userData = Platform::GetUserDataDirectory(error))
				s_ReportDirectory = *userData / FileSystem::PathFromUtf8(s_Application) / "Crashes";
			else
			{
				BS_CORE_WARN("Crash reports: {}; reports go next to the executable", error);
				s_ReportDirectory = FileSystem::GetExecutableDirectory() / "CrashReports";
			}
			const uint64_t listener = Log::GetHistory().AddListener([](const LogMessage& message) { CrashReporter::RecordLogLine(LogLevelName(message.Level), message.LoggerName, message.Text); });
			s_PreviousTerminate = std::set_terminate(&OnTerminate);
			if (!InstallPlatform())
			{
				std::set_terminate(s_PreviousTerminate);
				Log::GetHistory().RemoveListener(listener);
				return false;
			}
			BS_CORE_INFO("Crash reports: '{}'", FileSystem::PathToUtf8(s_ReportDirectory));
			return true;
		}

	}

	const char* CrashReporter::DescribeSignal(int signal)
	{
		switch (signal)
		{
			case SIGSEGV:
				return "SIGSEGV (invalid memory access)";
			case SIGILL:
				return "SIGILL (illegal instruction)";
			case SIGFPE:
				return "SIGFPE (arithmetic error)";
			case SIGABRT:
				return "SIGABRT (abort: a failed check or an uncaught exception)";
#if !defined(BS_PLATFORM_WINDOWS)
			case SIGBUS:
				return "SIGBUS (invalid memory access)";
			case SIGTRAP:
				return "SIGTRAP (trap: a failed check or a breakpoint)";
#endif
			default:
				return nullptr;
		}
	}

	const char* CrashReporter::DescribeException(uint32_t code)
	{
		// Fail-fast codes (stack buffer overruns, heap corruption) end the process without reaching any filter, so they
		// are not listed.
		switch (code)
		{
			case 0xC0000005:
				return "EXCEPTION_ACCESS_VIOLATION (invalid memory access)";
			case 0xC0000006:
				return "EXCEPTION_IN_PAGE_ERROR (a memory-mapped file could not be read)";
			case 0xC00000FD:
				return "EXCEPTION_STACK_OVERFLOW (stack overflow)";
			case 0xC000001D:
				return "EXCEPTION_ILLEGAL_INSTRUCTION (illegal instruction)";
			case 0xC0000096:
				return "EXCEPTION_PRIV_INSTRUCTION (privileged instruction)";
			case 0xC0000094:
				return "EXCEPTION_INT_DIVIDE_BY_ZERO (integer division by zero)";
			case 0xC0000095:
				return "EXCEPTION_INT_OVERFLOW (integer overflow)";
			case 0xC000008C:
				return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED (array bounds exceeded)";
			case 0x80000003:
				return "EXCEPTION_BREAKPOINT (a failed check or a breakpoint)";
			case 0xE06D7363:
				return "an uncaught C++ exception";
			default:
				return "an unhandled exception";
		}
	}

	void CrashReporter::FormatHeader(CrashText& out, std::string_view reason, std::optional<uint64_t> address, uint64_t unixTime, uint64_t processId)
	{
		LockGuard lock;
		out.Append("Basalt crash report\nApplication: ");
		out.Append(s_Application);
		out.Append("\n");
		if (s_Context[0] != '\0')
		{
			out.Append(s_Context);
			out.Append("\n");
		}
		out.Append("Platform: ");
		out.Append(PlatformName());
		out.Append("\nProcess: ");
		out.AppendDecimal(processId);
		out.Append("\nTime: ");
		out.AppendDecimal(unixTime);
		out.Append(" (seconds since 1970-01-01 UTC)\nReason: ");
		out.Append(reason);
		if (address)
		{
			out.Append(" at ");
			out.AppendHex(*address);
		}
		out.Append("\n");
		if (s_TerminateMessage[0] != '\0')
		{
			out.Append("Uncaught exception: ");
			out.Append(s_TerminateMessage);
			out.Append("\n");
		}
	}

	void CrashReporter::FormatLogLines(CrashText& out)
	{
		LockGuard lock;
		const uint64_t first = s_LineCount > MaxLogLines ? s_LineCount - MaxLogLines : 0;
		for (uint64_t i = first; i < s_LineCount; i++)
		{
			const LogLine& line = s_Lines[i % MaxLogLines];
			out.Append(std::string_view(line.Text, std::min(line.Length, MaxLogLineLength)));
			out.Append("\n");
		}
	}

	void CrashReporter::RecordLogLine(std::string_view level, std::string_view logger, std::string_view text)
	{
		LockGuard lock;
		LogLine& line = s_Lines[s_LineCount % MaxLogLines];
		size_t length = 0;
		const auto append = [&](std::string_view part) {
			for (const char c : part)
			{
				if (length == MaxLogLineLength)
					return;
				line.Text[length++] = c == '\n' || c == '\r' ? ' ' : c;
			}
		};
		append("[");
		append(level);
		append("] ");
		append(logger);
		append(": ");
		append(text);
		line.Text[length] = '\0';
		line.Length = length;
		s_LineCount++;
	}

	void CrashReporter::SetContext(std::string_view context)
	{
		LockGuard lock;
		CopyText(s_Context, MaxContextLength, context);
	}

	void CrashReporter::SetDialogEnabled(bool enabled)
	{
		s_DialogEnabled = enabled;
	}

	bool CrashReporter::IsInstalled()
	{
		return s_InstallResult;
	}

	std::filesystem::path CrashReporter::GetReportDirectory()
	{
		return s_ReportDirectory;
	}

	bool CrashReporter::Install(const CrashReporterOptions& options)
	{
		std::call_once(s_InstallOnce, [&]() { s_InstallResult = InstallOnce(options); });
		return s_InstallResult;
	}

}
