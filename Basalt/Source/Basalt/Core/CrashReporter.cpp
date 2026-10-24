#include "Basalt/Core/CrashReporter.h"

#include "Basalt/Core/Base.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/Platform.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <exception>
#include <system_error>
#include <thread>

#if defined(BS_PLATFORM_WINDOWS)
	#include <windows.h>
	// After windows.h, which it needs.
	#include <dbghelp.h>
	#include <cstdlib>
	#include <cwchar>
	#include <process.h>
#else
	#include <fcntl.h>
	#include <sys/stat.h>
	#include <unistd.h>
	#if __has_include(<execinfo.h>)
		#include <execinfo.h>
		#define BS_CRASH_BACKTRACE 1
	#endif
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

		// Guards the log lines and the context. Writers wait for it; the crash handler gives up after a bounded number of
		// attempts (the crashed thread may be the holder) and reads whatever is there.
		std::atomic_flag s_Lock;
		std::atomic<bool> s_Installed = false;
		std::atomic<bool> s_InstallResult = false;
		std::atomic<bool> s_Handling = false;
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
		std::filesystem::path s_ReportDirectory;
		// The report folder's grandparent, parent and itself, created at the first crash and never before: a process that
		// does not crash leaves nothing behind (tests and tools never touch the real user data directory).
		constexpr size_t DirectoryLevels = 3;

		// The folders from the outermost to the report folder.
		std::array<std::filesystem::path, DirectoryLevels> DirectoryChain(const std::filesystem::path& directory)
		{
			return { directory.parent_path().parent_path(), directory.parent_path(), directory };
		}
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
			std::memcpy(destination, text.data(), length);
			destination[length] = '\0';
			return length;
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

		void OnTerminate();
		std::terminate_handler s_PreviousTerminate = nullptr;

		// Keeps an uncaught exception's message for the report, then ends the process as before (abort: SIGABRT).
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
		void FormatReportName(CrashText& out, uint64_t unixTime, uint64_t processId, std::string_view extension)
		{
			out.Append("crash-");
			out.AppendDecimal(unixTime);
			out.Append("-");
			out.AppendDecimal(processId);
			out.Append(extension);
		}

#if !defined(BS_PLATFORM_WINDOWS)
		char s_Directories[DirectoryLevels][MaxPathLength + 1] = {};
		char s_FallbackDirectory[MaxPathLength + 1] = {};
		// Room for the handler on a stack overflow. Per thread: other threads' overflows are not reported.
		alignas(16) char s_AlternateStack[256 * 1024];
		constexpr int HandledSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };

		// The dialog program found at Install, with its arguments filled in at the crash (fork + exec are
		// async-signal-safe; a GUI toolkit is not).
		enum class DialogProgram
		{
			None,
			Osascript,
			Zenity,
			KDialog,
			XMessage
		};
		DialogProgram s_DialogProgram = DialogProgram::None;
		char s_DialogApplication[MaxApplicationLength * 2 + 1] = {};

		void WriteAll(int fd, std::string_view text)
		{
			while (!text.empty())
			{
				const ssize_t written = write(fd, text.data(), text.size());
				if (written <= 0)
					return;
				text.remove_prefix(static_cast<size_t>(written));
			}
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
		void AppendEscaped(CrashText& out, std::string_view text)
		{
			for (const char c : text)
			{
				if (c == '"' || c == '\\')
					out.Append("\\");
				out.Append(std::string_view(&c, 1));
			}
		}

		// Starts the dialog program in a child process without waiting for it: the crashed process dies at once, as the
		// OS expects, and the dialog stays until the player closes it.
		void ShowDialog(std::string_view reportPath, bool written)
		{
			if (!s_DialogEnabled || s_DialogProgram == DialogProgram::None)
				return;
			static CrashText s_Message;
			static CrashText s_Script;
			s_Message.Clear();
			s_Message.Append(s_Application);
			s_Message.Append(" crashed.");
			if (written)
			{
				s_Message.Append(" A crash report was saved to ");
				s_Message.Append(reportPath);
			}
			const char* message = s_Message.CStr();
			char title[MaxApplicationLength + 1];
			CopyText(title, MaxApplicationLength, s_Application);
			if (s_DialogProgram == DialogProgram::Osascript)
			{
				s_Script.Clear();
				s_Script.Append("display alert \"");
				s_Script.Append(s_DialogApplication);
				s_Script.Append(" crashed\" message \"");
				AppendEscaped(s_Script, s_Message.View());
				s_Script.Append("\" as critical buttons {\"OK\"}");
			}
			const pid_t child = fork();
			if (child != 0)
				return;
			// The child: only async-signal-safe calls until exec.
			switch (s_DialogProgram)
			{
				case DialogProgram::Osascript:
					execl("/usr/bin/osascript", "osascript", "-e", s_Script.CStr(), static_cast<char*>(nullptr));
					break;
				case DialogProgram::Zenity:
					execl("/usr/bin/zenity", "zenity", "--error", "--no-markup", "--title", title, "--text", message, static_cast<char*>(nullptr));
					break;
				case DialogProgram::KDialog:
					execl("/usr/bin/kdialog", "kdialog", "--title", title, "--error", message, static_cast<char*>(nullptr));
					break;
				case DialogProgram::XMessage:
					execl("/usr/bin/xmessage", "xmessage", "-center", message, static_cast<char*>(nullptr));
					break;
				case DialogProgram::None:
					break;
			}
			_exit(127);
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
			// A crash inside the handler (or on a second thread at the same time) dies at once.
			if (s_Handling.exchange(true))
			{
				ResetAndRaise(signal);
				return;
			}
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
			int fd = OpenReport(s_Directories[DirectoryLevels - 1], unixTime, processId, s_Path);
			if (fd < 0 && s_FallbackDirectory[0] != '\0')
			{
				mkdir(s_FallbackDirectory, 0755);
				fd = OpenReport(s_FallbackDirectory, unixTime, processId, s_Path);
			}
			const std::string_view path = s_Path.View();
			if (fd >= 0)
			{
				s_Text.Clear();
				CrashReporter::FormatHeader(s_Text, reason, address, unixTime, processId);
				s_Text.Append("\nStack trace:\n");
				WriteAll(fd, s_Text.View());
	#if defined(BS_CRASH_BACKTRACE)
				void* frames[128];
				const int count = backtrace(frames, 128);
				backtrace_symbols_fd(frames, count, fd);
	#else
				WriteAll(fd, "(not available on this platform)\n");
	#endif
				s_Text.Clear();
				s_Text.Append("\nLast log lines (oldest first):\n");
				CrashReporter::FormatLogLines(s_Text);
				WriteAll(fd, s_Text.View());
				close(fd);
			}
			s_Text.Clear();
			s_Text.Append(s_Application);
			s_Text.Append(" crashed: ");
			s_Text.Append(reason);
			if (fd >= 0)
			{
				s_Text.Append(". Crash report: ");
				s_Text.Append(path);
			}
			else
				s_Text.Append(". The crash report could not be written.");
			s_Text.Append("\n");
			WriteAll(STDERR_FILENO, s_Text.View());
			ShowDialog(path, fd >= 0);
			ResetAndRaise(signal);
		}

		bool InstallPlatform()
		{
			const auto chain = DirectoryChain(s_ReportDirectory);
			for (size_t i = 0; i < DirectoryLevels; i++)
			{
				const std::string directory = FileSystem::PathToUtf8(chain[i]);
				CopyText(s_Directories[i], MaxPathLength, directory.size() <= MaxPathLength ? directory : std::string());
			}
			const std::string fallback = FileSystem::PathToUtf8(FileSystem::GetExecutableDirectory() / "CrashReports");
			CopyText(s_FallbackDirectory, MaxPathLength, fallback.size() <= MaxPathLength ? fallback : std::string());
	#if defined(BS_CRASH_BACKTRACE)
			// The first call loads the unwinder (glibc dlopens libgcc_s), which must not happen inside the handler.
			void* warmUp[4];
			backtrace(warmUp, 4);
	#endif
	#if defined(BS_PLATFORM_MACOS)
			s_DialogProgram = access("/usr/bin/osascript", X_OK) == 0 ? DialogProgram::Osascript : DialogProgram::None;
			CrashText escaped;
			AppendEscaped(escaped, s_Application);
			CopyText(s_DialogApplication, sizeof(s_DialogApplication) - 1, escaped.View());
	#else
			// Only with a display to show it on.
			if (Platform::GetEnvVar("DISPLAY") || Platform::GetEnvVar("WAYLAND_DISPLAY"))
			{
				if (access("/usr/bin/zenity", X_OK) == 0)
					s_DialogProgram = DialogProgram::Zenity;
				else if (access("/usr/bin/kdialog", X_OK) == 0)
					s_DialogProgram = DialogProgram::KDialog;
				else if (access("/usr/bin/xmessage", X_OK) == 0)
					s_DialogProgram = DialogProgram::XMessage;
			}
	#endif
			stack_t stack = {};
			stack.ss_sp = s_AlternateStack;
			stack.ss_size = sizeof(s_AlternateStack);
			if (sigaltstack(&stack, nullptr) != 0)
				BS_CORE_WARN("Crash reports: no alternate signal stack ({}); a stack overflow will not be reported", std::generic_category().message(errno));
			struct sigaction action = {};
			action.sa_sigaction = &HandleSignal;
			sigemptyset(&action.sa_mask);
			action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
			for (const int signal : HandledSignals)
			{
				if (sigaction(signal, &action, nullptr) != 0)
				{
					BS_CORE_ERROR("Crash reports: cannot handle signal {}: {}", signal, std::generic_category().message(errno));
					return false;
				}
			}
			return true;
		}
#else
		wchar_t s_Directories[DirectoryLevels][MaxPathLength + 1] = {};
		wchar_t s_FallbackDirectory[MaxPathLength + 1] = {};
		wchar_t s_WideApplication[MaxApplicationLength + 1] = {};

		// Appends text to a NUL-terminated wide buffer of capacity + 1 characters.
		void AppendWide(wchar_t* destination, size_t capacity, const wchar_t* text)
		{
			size_t length = wcslen(destination);
			for (; *text && length < capacity; text++)
				destination[length++] = *text;
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
			CrashText name;
			FormatReportName(name, unixTime, processId, "");
			wchar_t wide[64] = {};
			for (size_t i = 0; i < name.View().size() && i < 63; i++)
				wide[i] = static_cast<wchar_t>(name.View()[i]);
			AppendWide(out, capacity, wide);
			return true;
		}

		HANDLE CreateReportFile(const wchar_t* base, const wchar_t* extension, wchar_t* path, size_t capacity)
		{
			path[0] = L'\0';
			AppendWide(path, capacity, base);
			AppendWide(path, capacity, extension);
			return CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		}

		void WriteAll(HANDLE file, std::string_view text)
		{
			while (!text.empty())
			{
				DWORD written = 0;
				if (!WriteFile(file, text.data(), static_cast<DWORD>(std::min<size_t>(text.size(), 1u << 20)), &written, nullptr) || written == 0)
					return;
				text.remove_prefix(written);
			}
		}

		void AppendUtf8(CrashText& out, const wchar_t* text)
		{
			char buffer[MaxPathLength * 3 + 1] = {};
			const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, static_cast<int>(sizeof(buffer)), nullptr, nullptr);
			if (length > 1)
				out.Append(std::string_view(buffer, static_cast<size_t>(length - 1)));
		}

		void AppendStackTrace(CrashText& out)
		{
			void* frames[62];
			const USHORT count = CaptureStackBackTrace(0, 62, frames, nullptr);
			alignas(SYMBOL_INFO) char symbolStorage[sizeof(SYMBOL_INFO) + 256] = {};
			auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolStorage);
			for (USHORT i = 0; i < count; i++)
			{
				const auto address = reinterpret_cast<DWORD64>(frames[i]);
				out.Append("  ");
				out.AppendHex(address);
				symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
				symbol->MaxNameLen = 255;
				DWORD64 displacement = 0;
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
			HANDLE file = INVALID_HANDLE_VALUE;
			for (const wchar_t* directory : s_Directories)
				if (directory[0] != L'\0')
					CreateDirectoryW(directory, nullptr);
			if (BuildReportBase(s_Directories[DirectoryLevels - 1], unixTime, processId, s_Base, MaxPathLength + 63))
				file = CreateReportFile(s_Base, L".txt", s_TextPath, MaxPathLength + 79);
			if (file == INVALID_HANDLE_VALUE && BuildReportBase(s_FallbackDirectory, unixTime, processId, s_Base, MaxPathLength + 63))
			{
				CreateDirectoryW(s_FallbackDirectory, nullptr);
				file = CreateReportFile(s_Base, L".txt", s_TextPath, MaxPathLength + 79);
			}
			const bool written = file != INVALID_HANDLE_VALUE;
			if (written)
			{
				bool dumped = false;
				const HANDLE dump = CreateReportFile(s_Base, L".dmp", s_DumpPath, MaxPathLength + 79);
				if (dump != INVALID_HANDLE_VALUE)
				{
					MINIDUMP_EXCEPTION_INFORMATION information = { GetCurrentThreadId(), exception, FALSE };
					dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo), exception ? &information : nullptr, nullptr, nullptr) != FALSE;
					CloseHandle(dump);
				}
				s_Text.Clear();
				CrashReporter::FormatHeader(s_Text, reason, address, unixTime, processId);
				if (dumped)
				{
					s_Text.Append("Minidump: ");
					AppendUtf8(s_Text, s_DumpPath);
					s_Text.Append("\n");
				}
				s_Text.Append("\nStack trace:\n");
				AppendStackTrace(s_Text);
				s_Text.Append("\nLast log lines (oldest first):\n");
				CrashReporter::FormatLogLines(s_Text);
				WriteAll(file, s_Text.View());
				CloseHandle(file);
			}
			s_Text.Clear();
			s_Text.Append(s_Application);
			s_Text.Append(" crashed: ");
			s_Text.Append(reason);
			if (written)
			{
				s_Text.Append(". Crash report: ");
				AppendUtf8(s_Text, s_TextPath);
			}
			else
				s_Text.Append(". The crash report could not be written.");
			s_Text.Append("\n");
			WriteAll(GetStdHandle(STD_ERROR_HANDLE), s_Text.View());
			if (s_DialogEnabled)
			{
				static wchar_t s_Message[MaxPathLength + 512];
				s_Message[0] = L'\0';
				AppendWide(s_Message, MaxPathLength + 511, s_WideApplication);
				AppendWide(s_Message, MaxPathLength + 511, L" crashed.");
				if (written)
				{
					AppendWide(s_Message, MaxPathLength + 511, L"\n\nA crash report was saved to:\n");
					AppendWide(s_Message, MaxPathLength + 511, s_TextPath);
				}
				MessageBoxW(nullptr, s_Message, s_WideApplication, MB_OK | MB_ICONERROR | MB_TASKMODAL);
			}
		}

		LONG WINAPI HandleException(EXCEPTION_POINTERS* exception)
		{
			if (s_Handling.exchange(true))
				return EXCEPTION_CONTINUE_SEARCH;
			const EXCEPTION_RECORD& record = *exception->ExceptionRecord;
			std::optional<uint64_t> address = reinterpret_cast<uintptr_t>(record.ExceptionAddress);
			// For access violations, the address that could not be read or written.
			if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && record.NumberParameters >= 2)
				address = static_cast<uint64_t>(record.ExceptionInformation[1]);
			WriteReport(CrashReporter::DescribeException(record.ExceptionCode), address, exception);
			// The process ends with the exception code, as without the reporter (but without Windows Error Reporting's
			// dialog, which SEM_NOGPFAULTERRORBOX suppresses).
			return EXCEPTION_EXECUTE_HANDLER;
		}

		// abort() (failed checks, std::terminate) and C runtime failures bypass the unhandled exception filter.
		void HandleAbort(int)
		{
			if (!s_Handling.exchange(true))
				WriteReport(CrashReporter::DescribeSignal(SIGABRT), std::nullopt, nullptr);
			_exit(3);
		}

		void HandleInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
		{
			if (!s_Handling.exchange(true))
				WriteReport("an invalid parameter passed to a C runtime function", std::nullopt, nullptr);
			_exit(3);
		}

		void HandlePureCall()
		{
			if (!s_Handling.exchange(true))
				WriteReport("a pure virtual function call", std::nullopt, nullptr);
			_exit(3);
		}

		bool InstallPlatform()
		{
			const auto chain = DirectoryChain(s_ReportDirectory);
			for (size_t i = 0; i < DirectoryLevels; i++)
			{
				const std::wstring directory = chain[i].wstring();
				if (directory.size() <= MaxPathLength)
					wcsncpy_s(s_Directories[i], directory.c_str(), _TRUNCATE);
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
			// Stack for the filter after a stack overflow on this thread.
			ULONG guarantee = 64 * 1024;
			SetThreadStackGuarantee(&guarantee);
			SetUnhandledExceptionFilter(&HandleException);
			return true;
		}
#endif

	}

	void CrashText::Append(std::string_view text)
	{
		const size_t length = std::min(text.size(), Capacity - m_Size);
		std::memcpy(m_Data + m_Size, text.data(), length);
		m_Size += length;
	}

	void CrashText::AppendDecimal(uint64_t value)
	{
		char digits[20];
		size_t count = 0;
		do
		{
			digits[count++] = static_cast<char>('0' + value % 10);
			value /= 10;
		} while (value != 0);
		while (count > 0)
		{
			const char digit = digits[--count];
			Append(std::string_view(&digit, 1));
		}
	}

	void CrashText::AppendHex(uint64_t value)
	{
		static constexpr char Digits[] = "0123456789abcdef";
		char text[18] = { '0', 'x' };
		for (int i = 17; i >= 2; i--, value >>= 4)
			text[i] = Digits[value & 0xF];
		Append(std::string_view(text, sizeof(text)));
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
			case 0xC0000409:
				return "STATUS_STACK_BUFFER_OVERRUN (fail fast: a security check failed)";
			case 0xC0000374:
				return "STATUS_HEAP_CORRUPTION (heap corruption)";
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
		if (s_Installed.exchange(true))
			return s_InstallResult;
		// Not #if: sanitizer builds still compile (and warn about) the handlers.
		if constexpr (BS_CRASH_ASAN != 0)
		{
			BS_CORE_INFO("Crash reports: off under AddressSanitizer, which reports crashes itself");
			return false;
		}
		{
			LockGuard lock;
			CopyText(s_Application, MaxApplicationLength, options.ApplicationName.empty() ? std::string_view("Basalt") : std::string_view(options.ApplicationName));
		}
		std::string error;
		if (const auto userData = Platform::GetUserDataDirectory(error))
			s_ReportDirectory = *userData / FileSystem::PathFromUtf8(s_Application) / "Crashes";
		else
		{
			BS_CORE_INFO("Crash reports: {}; reports go next to the executable", error);
			s_ReportDirectory = FileSystem::GetExecutableDirectory() / "CrashReports";
		}
		Log::GetHistory().AddListener([](const LogMessage& message) { RecordLogLine(LogLevelName(message.Level), message.LoggerName, message.Text); });
		s_PreviousTerminate = std::set_terminate(&OnTerminate);
		if (!InstallPlatform())
			return false;
		s_InstallResult = true;
		BS_CORE_INFO("Crash reports: '{}'", FileSystem::PathToUtf8(s_ReportDirectory));
		return true;
	}

}
