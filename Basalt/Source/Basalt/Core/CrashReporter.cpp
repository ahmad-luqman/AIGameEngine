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
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <system_error>
#include <thread>
#include <type_traits>
#include <variant>
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
	#include <sys/mman.h>
	#include <sys/stat.h>
	#include <unistd.h>
	#if __has_include(<execinfo.h>)
		#include <execinfo.h>
		#define BS_CRASH_BACKTRACE 1
	#endif
	// addr2line names frames in the executable, whose load address dl_iterate_phdr gives.
	#if defined(__linux__) && defined(__GLIBC__) && defined(BS_CRASH_BACKTRACE)
		#define BS_CRASH_SYMBOLIZE 1
		#include <link.h>
		#include <sys/wait.h>
	#endif
// The environment spawned programs inherit (posix_spawn needs it explicitly).
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
		// How many levels (the report folder and its missing ancestors) the handler may create: a fresh account without
		// ~/.local, a nested BASALT_USER_DATA_DIR that does not exist yet.
		constexpr size_t MaxDirectoryLevels = 8;
		constexpr std::string_view StackTraceHeading = "\nStack trace:\n";
		constexpr std::string_view LogHeading = "\nLast log lines (oldest first):\n";
		constexpr std::string_view TruncationNote = "\n(cut: the rest did not fit the crash handler's buffer)\n";
		constexpr std::string_view OpenReportButton = "Open Report";
#if defined(BS_PLATFORM_WINDOWS)
		constexpr std::string_view FunctionNamesHeading = "\nFunction names:\n";
#endif
		// What a dialog shows: far more than any report's path and reason, and short enough that its AppleScript fits a
		// CrashText, even with every character escaped.
		using DialogText = BasicCrashText<size_t{ 4 } * 1024>;
		static_assert((DialogText::Capacity + MaxPathLength + MaxApplicationLength) * 2 + 1024 <= CrashText::Capacity);

		// Guards the log lines and the context. Writers wait for it; the crash handler gives up after a bounded number of
		// attempts (the crashed thread may be the holder) and reads whatever is there.
		std::atomic_flag s_Lock;
		std::once_flag s_InstallOnce;
		std::atomic<bool> s_InstallResult = false;
		std::atomic<bool> s_Handling = false;
		// The thread writing the report (0: none): a crash inside the handler dies at once, while other threads that crash
		// meanwhile wait for the report to be finished. Not a thread_local, whose first access on a thread may allocate
		// (macOS creates thread-local storage lazily).
		std::atomic<uint64_t> s_ReportingThread = 0;
		// Set once the report is written (Windows then may wait on its dialog): threads that crash meanwhile wait at most
		// ReportTimeoutSeconds for it, so a reporter stuck on a lock the crash left held cannot hang the process forever.
		std::atomic<bool> s_ReportWritten = false;
		constexpr unsigned ReportTimeoutSeconds = 30;
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

		// Copies text into a fixed buffer of capacity + 1 bytes, NUL-terminated, returning the copied length. Text that does
		// not fit is cut on a UTF-8 character boundary.
		size_t CopyText(char* destination, size_t capacity, std::string_view text)
		{
			size_t length = std::min(text.size(), capacity);
			if (length < text.size())
				length = CrashTextDetail::TrimPartialUtf8(text.substr(0, length), text[length]);
			std::copy_n(text.begin(), length, destination);
			destination[length] = '\0';
			return length;
		}

		// The application name as a folder name: without characters that would leave the folder or that Windows refuses,
		// cut at MaxApplicationLength bytes on a UTF-8 character boundary (Windows converts the name to UTF-16, which an
		// invalid sequence would break). Names that still cannot be folders (device names like CON) fail at the crash, and
		// the report then goes next to the executable.
		std::string SanitizeName(std::string_view name)
		{
			std::string result;
			for (const char c : name)
				if (static_cast<unsigned char>(c) >= 0x20 && std::string_view("/\\:<>\"|?*").find(c) == std::string_view::npos)
					result += c;
			if (result.size() > MaxApplicationLength)
			{
				size_t length = MaxApplicationLength;
				while (length > 0 && (static_cast<unsigned char>(result[length]) & 0xC0) == 0x80)
					length--;
				result.resize(length);
			}
			while (!result.empty() && (result.back() == '.' || result.back() == ' '))
				result.pop_back();
			return result.empty() ? std::string("Basalt") : result;
		}

		// This thread's identity, read without allocating (a register or a system call).
		uint64_t CurrentThreadKey()
		{
#if defined(BS_PLATFORM_WINDOWS)
			return GetCurrentThreadId();
#else
			// pthread_t is a pointer on macOS and an integer on Linux.
			const auto key = [](auto self) -> uint64_t {
				if constexpr (std::is_pointer_v<decltype(self)>)
					return reinterpret_cast<uintptr_t>(self);
				else
					return static_cast<uint64_t>(self);
			}(pthread_self());
			// pthread_self is never zero in practice; keep 0 free to mean "nobody" anyway.
			return key != 0 ? key : 1;
#endif
		}

#if defined(BS_PLATFORM_WINDOWS)
		void StartReportDeadline();
#endif

		// Whether this thread may write the report: false for a crash inside the handler, and on Windows after
		// ReportTimeoutSeconds of waiting for another thread's report (the caller then ends the process). Otherwise a thread
		// that crashes meanwhile waits here until the process dies.
		bool BeginReport()
		{
			const uint64_t self = CurrentThreadKey();
			uint64_t reporting = 0;
			if (s_ReportingThread.compare_exchange_strong(reporting, self))
			{
				s_Handling = true;
#if !defined(BS_PLATFORM_WINDOWS)
				// A reporter stuck on a lock the crash left held (the loader's, inside backtrace_symbols_fd) is ended by
				// SIGALRM's default action instead of hanging the process.
				alarm(ReportTimeoutSeconds);
#else
				StartReportDeadline();
#endif
				return true;
			}
			if (reporting != self)
			{
				// Another thread is writing the report; it ends the process when done.
#if defined(BS_PLATFORM_WINDOWS)
				for (unsigned waited = 0; waited < ReportTimeoutSeconds * 10 && !s_ReportWritten; waited++)
					Sleep(100);
				// Written: the reporter may be waiting on its dialog, and ends the process when the player closes it.
				if (s_ReportWritten)
					Sleep(INFINITE);
#else
				// The reporter's alarm ends the process if the report never finishes.
				for (;;)
					pause();
#endif
			}
			return false;
		}

		// "<application> crashed: <reason>" and how the report went, for stderr and the dialog; the caller adds the report's
		// path (written) or the error code and ").".
		void AppendSummaryOpening(DialogText& out, std::string_view reason, bool written, bool complete)
		{
			out.Append(s_Application);
			out.Append(" crashed: ");
			out.Append(reason);
			if (!written)
				out.Append(". No crash report could be written (error ");
			else
				out.Append(complete ? ". Crash report: " : ". The crash report is incomplete (it could not be written whole): ");
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
			if (chain.size() == MaxDirectoryLevels && !std::filesystem::exists(chain.back(), error))
				BS_CORE_WARN("Crash reports: more than {} folders above '{}' are missing; reports may go next to the executable", MaxDirectoryLevels, FileSystem::PathToUtf8(directory));
			std::reverse(chain.begin(), chain.end());
			return chain;
		}

#if !defined(BS_PLATFORM_WINDOWS)
		char s_Directories[MaxDirectoryLevels][MaxPathLength + 1] = {};
		char s_FallbackDirectory[MaxPathLength + 1] = {};
		// Room for the handler on a stack overflow: the installing thread's, and what a ThreadStack maps for its thread.
		constexpr size_t AlternateStackSize = size_t{ 256 } * 1024;
		alignas(16) char s_AlternateStack[AlternateStackSize];
		constexpr int HandledSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
		struct sigaction s_PreviousActions[std::size(HandledSignals)] = {};

		// The dialog program found at Install. The crashed process starts it, or the shell that waits for its answer, with
		// posix_spawn, which (unlike fork) runs no atfork handlers, so it cannot deadlock on a malloc lock the crash left held;
		// its stdio goes to /dev/null, so it never holds a parent's output pipes open.
		char s_DialogProgram[MaxPathLength + 1] = {};
		CrashDialogKind s_DialogKind = CrashDialogKind::Osascript;
		// What opens a report from the Linux dialogs; empty: they show the path only.
		char s_Opener[MaxPathLength + 1] = {};
		posix_spawn_file_actions_t s_DialogActions;
		posix_spawnattr_t s_SpawnAttributes;

	#if defined(BS_CRASH_BACKTRACE)
		constexpr int MaxStackFrames = 128;
	#endif
	#if defined(BS_CRASH_SYMBOLIZE)
		// addr2line can take long over large debug information; what it has written by then stays in the report. The report's
		// alarm restarts with it, so this is below ReportTimeoutSeconds: its section cannot cost the process its signal.
		constexpr unsigned SymbolizeTimeoutSeconds = 15;
		constexpr const char* Addr2LineProgram = "/usr/bin/addr2line";
		constexpr std::string_view SourceLinesHeading = "\nSource lines (addr2line):\n";
		constexpr std::string_view SymbolizeTimeoutNote = "(addr2line did not finish in time)\n";
		// Empty: no addr2line, or nothing it could be pointed at.
		char s_Executable[MaxPathLength + 1] = {};
		// The executable's load bias and address range, read at Install because the loader's locks may be held at a crash.
		uintptr_t s_ImageBias = 0;
		uintptr_t s_ImageBegin = 0;
		uintptr_t s_ImageEnd = 0;
		// Reserved at Install: posix_spawn's file actions allocate, so they are prepared with this descriptor and the
		// handler points it at the report.
		int s_SymbolizeOutput = -1;
		posix_spawn_file_actions_t s_SymbolizeActions;
	#endif

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

		// Notes that text lost its end to its buffer's capacity; false when the note cannot be written.
		bool WriteTruncationNote(int fd, const CrashText& text)
		{
			return !text.Truncated() || WriteAll(fd, TruncationNote);
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

		// Each opens the report when the player chooses to. A shell waits for the answer, because the crashed process is gone
		// by then. Text only reaches them as arguments: $1 program, $2 title, $3 message, $4 report, $5 opener.
		const char* GetOpenReportScript(CrashDialogKind kind)
		{
			switch (kind)
			{
				case CrashDialogKind::Zenity:
					return R"("$1" --question --no-markup --title "$2" --text "$3" --ok-label 'Open Report' --cancel-label Close && "$5" "$4")";
				case CrashDialogKind::KDialog:
					return R"("$1" --title "$2" --yes-label 'Open Report' --no-label Close --yesno "$3" && "$5" "$4")";
				case CrashDialogKind::XMessage:
					return R"("$1" -center -buttons 'Close:1,Open Report:0' "$3" && "$5" "$4")";
				case CrashDialogKind::Osascript:
					break;
			}
			return nullptr;
		}

		// Starts the dialog program without waiting for it: the process dies at once, as the OS expects of a crash, and the
		// dialog stays until the player closes it. With a reportPath and an opener (macOS needs none) it offers to open that
		// file. False when the program could not start.
		bool ShowDialog(const char* title, const DialogText& message, const char* reportPath)
		{
			static CrashText s_Script;
			char* arguments[10] = {};
			const auto setArguments = [&arguments](std::initializer_list<const char*> texts) {
				size_t count = 0;
				for (const char* text : texts)
					arguments[count++] = const_cast<char*>(text);
			};
			const char* program = s_DialogProgram;
			const char* openScript = reportPath[0] != '\0' && s_Opener[0] != '\0' ? GetOpenReportScript(s_DialogKind) : nullptr;
			if (openScript)
			{
				program = "/bin/sh";
				// The second "sh" is the script's $0.
				setArguments({ "sh", "-c", openScript, "sh", s_DialogProgram, title, message.CStr(), reportPath, s_Opener });
			}
			else
			{
				switch (s_DialogKind)
				{
					case CrashDialogKind::Osascript:
						s_Script.Clear();
						CrashReporter::FormatAppleScript(s_Script, title, message.View(), reportPath);
						setArguments({ "osascript", "-e", s_Script.CStr() });
						break;
					case CrashDialogKind::Zenity:
						setArguments({ "zenity", "--error", "--no-markup", "--title", title, "--text", message.CStr() });
						break;
					case CrashDialogKind::KDialog:
						setArguments({ "kdialog", "--title", title, "--error", message.CStr() });
						break;
					case CrashDialogKind::XMessage:
						setArguments({ "xmessage", "-center", message.CStr() });
						break;
				}
			}
			pid_t child = 0;
			return posix_spawn(&child, program, &s_DialogActions, &s_SpawnAttributes, arguments, environ) == 0;
		}

		// What spawned programs share, prepared at Install (setting it up allocates): signals as a new process has them
		// rather than the handler's blocked mask and, where the platform can, no inherited descriptors but stdio.
		bool PrepareSpawnAttributes()
		{
			sigset_t none;
			sigset_t all;
			sigemptyset(&none);
			sigfillset(&all);
			short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
	#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
			flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
	#endif
			return posix_spawnattr_init(&s_SpawnAttributes) == 0 && posix_spawnattr_setsigmask(&s_SpawnAttributes, &none) == 0 && posix_spawnattr_setsigdefault(&s_SpawnAttributes, &all) == 0 && posix_spawnattr_setflags(&s_SpawnAttributes, flags) == 0;
		}

		// Other descriptors stay closed in a spawned program (a parent's pipes stay closable).
		bool AddCloseOthers([[maybe_unused]] posix_spawn_file_actions_t& actions)
		{
	#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
			return posix_spawn_file_actions_addclosefrom_np(&actions, 3) == 0;
	#else
			return true;
	#endif
		}

		// The dialog's process setup: stdio on /dev/null.
		bool PrepareDialogActions()
		{
			return posix_spawn_file_actions_init(&s_DialogActions) == 0 && posix_spawn_file_actions_addopen(&s_DialogActions, 0, "/dev/null", O_RDONLY, 0) == 0 && posix_spawn_file_actions_addopen(&s_DialogActions, 1, "/dev/null", O_WRONLY, 0) == 0 && posix_spawn_file_actions_adddup2(&s_DialogActions, 1, 2) == 0 && AddCloseOthers(s_DialogActions);
		}

	#if defined(BS_CRASH_SYMBOLIZE)
		int FindExecutableImage(dl_phdr_info* info, size_t, void*)
		{
			// The first object listed is the executable.
			s_ImageBias = info->dlpi_addr;
			for (ElfW(Half) i = 0; i < info->dlpi_phnum; i++)
			{
				const ElfW(Phdr) & header = info->dlpi_phdr[i];
				if (header.p_type != PT_LOAD)
					continue;
				const uintptr_t begin = info->dlpi_addr + header.p_vaddr;
				s_ImageBegin = s_ImageBegin == 0 ? begin : std::min(s_ImageBegin, begin);
				s_ImageEnd = std::max(s_ImageEnd, begin + header.p_memsz);
			}
			return 1;
		}

		void PrepareSymbolizer()
		{
			if (access(Addr2LineProgram, X_OK) != 0)
			{
				BS_CORE_INFO("Crash reports: no {}, so frames stay module and offset", Addr2LineProgram);
				return;
			}
			const std::string executable = FileSystem::PathToUtf8(FileSystem::GetExecutablePath());
			dl_iterate_phdr(&FindExecutableImage, nullptr);
			s_SymbolizeOutput = open("/dev/null", O_WRONLY | O_CLOEXEC);
			if (executable.size() > MaxPathLength || s_ImageBegin == 0 || s_SymbolizeOutput < 0 || posix_spawn_file_actions_init(&s_SymbolizeActions) != 0 || posix_spawn_file_actions_addopen(&s_SymbolizeActions, 0, "/dev/null", O_RDONLY, 0) != 0 || posix_spawn_file_actions_adddup2(&s_SymbolizeActions, s_SymbolizeOutput, 1) != 0 || posix_spawn_file_actions_addopen(&s_SymbolizeActions, 2, "/dev/null", O_WRONLY, 0) != 0 || !AddCloseOthers(s_SymbolizeActions))
			{
				BS_CORE_WARN("Crash reports: no source lines (cannot prepare addr2line's process)");
				return;
			}
			CopyText(s_Executable, MaxPathLength, executable);
		}

		uint64_t MonotonicSeconds()
		{
			timespec now = {};
			clock_gettime(CLOCK_MONOTONIC, &now);
			return static_cast<uint64_t>(now.tv_sec);
		}

		// Names the frames inside the executable (the engine and game code; it is static) into the report. Runs last, so a
		// slow or stuck addr2line costs only this section. Other modules' frames are in the stack trace as module + offset.
		// False when something could not be written to the report.
		bool AppendSourceLines(int fd, void* const* frames, int count)
		{
			if (s_Executable[0] == '\0')
				return true;
			static BasicCrashText<18> s_Addresses[MaxStackFrames];
			static char* s_Arguments[MaxStackFrames + 9];
			size_t argumentCount = 0;
			for (const char* text : { "addr2line", "-a", "-f", "-C", "-i", "-p", "-e", static_cast<const char*>(s_Executable) })
				s_Arguments[argumentCount++] = const_cast<char*>(text);
			const size_t firstAddress = argumentCount;
			for (int i = 0; i < count; i++)
			{
				const auto address = reinterpret_cast<uintptr_t>(frames[i]);
				if (address <= s_ImageBegin || address > s_ImageEnd)
					continue;
				// A return address: the call is the instruction before it.
				BasicCrashText<18>& text = s_Addresses[argumentCount - firstAddress];
				text.Clear();
				text.AppendHex(address - 1 - s_ImageBias);
				s_Arguments[argumentCount++] = const_cast<char*>(text.CStr());
			}
			s_Arguments[argumentCount] = nullptr;
			if (argumentCount == firstAddress)
				return true;
			if (!WriteAll(fd, SourceLinesHeading))
				return false;
			alarm(ReportTimeoutSeconds);
			pid_t child = 0;
			if (dup3(fd, s_SymbolizeOutput, O_CLOEXEC) < 0 || posix_spawn(&child, Addr2LineProgram, &s_SymbolizeActions, &s_SpawnAttributes, s_Arguments, environ) != 0)
				return WriteAll(fd, "(addr2line could not start)\n");
			// The descriptor's position, which addr2line's writes move: it may lose them to a full disk without saying so.
			const off_t before = lseek(fd, 0, SEEK_CUR);
			const uint64_t deadline = MonotonicSeconds() + SymbolizeTimeoutSeconds;
			for (;;)
			{
				int status = 0;
				const pid_t finished = waitpid(child, &status, WNOHANG);
				if (finished == child)
				{
					if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
						return WriteAll(fd, "(addr2line failed)\n");
					return lseek(fd, 0, SEEK_CUR) != before || WriteAll(fd, "(addr2line wrote nothing)\n");
				}
				// With SIGCHLD ignored there is nothing to wait for.
				if (finished < 0 && errno != EINTR)
					return true;
				if (MonotonicSeconds() >= deadline)
				{
					// The note first: the child may be stuck in the kernel, past what SIGKILL ends at once.
					const bool noted = WriteAll(fd, SymbolizeTimeoutNote);
					kill(child, SIGKILL);
					waitpid(child, nullptr, 0);
					return noted;
				}
				constexpr long PollNanoseconds = 20L * 1000 * 1000;
				const timespec pollTime = { 0, PollNanoseconds };
				nanosleep(&pollTime, nullptr);
			}
		}
	#endif

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
			if (!BeginReport())
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
			// Every missing level, outermost first; the report folder is the last one.
			const char* reportDirectory = "";
			for (const char* directory : s_Directories)
			{
				if (directory[0] != '\0')
				{
					mkdir(directory, 0755);
					reportDirectory = directory;
				}
			}
			// OpenReport fails without touching errno when no folder is set.
			errno = 0;
			int fd = OpenReport(reportDirectory, unixTime, processId, s_Path);
			int openError = fd < 0 ? errno : 0;
			if (fd < 0 && s_FallbackDirectory[0] != '\0')
			{
				mkdir(s_FallbackDirectory, 0755);
				errno = 0;
				fd = OpenReport(s_FallbackDirectory, unixTime, processId, s_Path);
				if (fd < 0)
					openError = errno;
			}
			const bool written = fd >= 0;
			bool complete = true;
			if (written)
			{
				s_Text.Clear();
				CrashReporter::FormatHeader(s_Text, reason, address, unixTime, processId);
				s_Text.Append(StackTraceHeading);
				complete = WriteAll(fd, s_Text.View());
				complete = WriteTruncationNote(fd, s_Text) && complete;
	#if defined(BS_CRASH_BACKTRACE)
				void* frames[MaxStackFrames];
				const int count = backtrace(frames, MaxStackFrames);
				backtrace_symbols_fd(frames, count, fd);
	#else
				complete = WriteAll(fd, "(not available on this platform)\n") && complete;
	#endif
				s_Text.Clear();
				s_Text.Append(LogHeading);
				CrashReporter::FormatLogLines(s_Text);
				complete = WriteAll(fd, s_Text.View()) && complete;
				complete = WriteTruncationNote(fd, s_Text) && complete;
	#if defined(BS_CRASH_SYMBOLIZE)
				complete = AppendSourceLines(fd, frames, count) && complete;
	#endif
				complete = close(fd) == 0 && complete;
			}
			// What the player and stderr are told.
			static DialogText s_Summary;
			s_Summary.Clear();
			AppendSummaryOpening(s_Summary, reason, written, complete);
			if (written)
				s_Summary.Append(s_Path.View());
			else
			{
				s_Summary.Append("errno ");
				s_Summary.AppendDecimal(static_cast<uint64_t>(openError));
				s_Summary.Append(").");
			}
			WriteAll(STDERR_FILENO, s_Summary.View());
			WriteAll(STDERR_FILENO, "\n");
			s_ReportWritten = true;
			static BasicCrashText<MaxApplicationLength + 8> s_Title;
			s_Title.Clear();
			s_Title.Append(s_Application);
			s_Title.Append(" crashed");
			if (s_DialogEnabled && s_DialogProgram[0] != '\0' && !ShowDialog(s_Title.CStr(), s_Summary, written ? s_Path.CStr() : ""))
				WriteAll(STDERR_FILENO, "The crash dialog could not be shown.\n");
			ResetAndRaise(signal);
		}

		void RestoreSignals(size_t count)
		{
			for (size_t i = 0; i < count; i++)
				sigaction(HandledSignals[i], &s_PreviousActions[i], nullptr);
		}

		void UseDialogTools(const CrashDialogTools& tools)
		{
			const std::string program = FileSystem::PathToUtf8(tools.Program);
			const std::string opener = FileSystem::PathToUtf8(tools.Opener);
			if (program.size() > MaxPathLength || opener.size() > MaxPathLength || access(program.c_str(), X_OK) != 0)
			{
				BS_CORE_WARN("Crash reports: no crash dialog ('{}' cannot be used)", program);
				return;
			}
			CopyText(s_DialogProgram, MaxPathLength, program);
			s_DialogKind = tools.Kind;
			if (!opener.empty() && access(opener.c_str(), X_OK) != 0)
				BS_CORE_WARN("Crash reports: dialogs cannot open reports ('{}' cannot be used)", opener);
			else
				CopyText(s_Opener, MaxPathLength, opener);
		}

		void FindDialogProgram()
		{
	#if defined(BS_PLATFORM_MACOS)
			if (access("/usr/bin/osascript", X_OK) == 0)
			{
				CopyText(s_DialogProgram, MaxPathLength, "/usr/bin/osascript");
				s_DialogKind = CrashDialogKind::Osascript;
			}
	#else
			// Only with a display to show it on.
			if (!Platform::GetEnvVar("DISPLAY") && !Platform::GetEnvVar("WAYLAND_DISPLAY"))
				return;
			if (access("/usr/bin/xdg-open", X_OK) == 0)
				CopyText(s_Opener, MaxPathLength, "/usr/bin/xdg-open");
			for (const auto& [program, kind] : { std::pair{ "/usr/bin/zenity", CrashDialogKind::Zenity }, std::pair{ "/usr/bin/kdialog", CrashDialogKind::KDialog }, std::pair{ "/usr/bin/xmessage", CrashDialogKind::XMessage } })
			{
				if (access(program, X_OK) == 0)
				{
					CopyText(s_DialogProgram, MaxPathLength, program);
					s_DialogKind = kind;
					return;
				}
			}
			BS_CORE_INFO("Crash reports: no zenity, kdialog or xmessage, so dialogs will not show");
	#endif
		}

		bool InstallPlatform(const CrashReporterOptions& options)
		{
			// The report folder is the longest path of the chain: when it fits, every level fits.
			const auto chain = MissingDirectoryChain(s_ReportDirectory);
			if (FileSystem::PathToUtf8(chain.back()).size() > MaxPathLength)
				BS_CORE_WARN("Crash reports: the folder '{}' is longer than {} bytes; reports go next to the executable", FileSystem::PathToUtf8(chain.back()), MaxPathLength);
			else
				for (size_t i = 0; i < chain.size(); i++)
					CopyText(s_Directories[i], MaxPathLength, FileSystem::PathToUtf8(chain[i]));
			const std::string fallback = FileSystem::PathToUtf8(FileSystem::GetExecutableDirectory() / "CrashReports");
			if (fallback.size() <= MaxPathLength)
				CopyText(s_FallbackDirectory, MaxPathLength, fallback);
			else
				BS_CORE_WARN("Crash reports: the fallback folder '{}' is longer than {} bytes; it will not be used", fallback, MaxPathLength);
	#if defined(BS_CRASH_BACKTRACE)
			// The first call loads the unwinder (glibc dlopens libgcc_s), which must not happen inside the handler.
			void* warmUp[4];
			backtrace(warmUp, 4);
	#endif
			const bool spawnPrepared = PrepareSpawnAttributes();
			if (!spawnPrepared)
				BS_CORE_WARN("Crash reports: no crash dialog or source lines (cannot prepare their processes)");
	#if defined(BS_CRASH_SYMBOLIZE)
			if (spawnPrepared)
				PrepareSymbolizer();
	#endif
			if (options.DialogTools)
				UseDialogTools(*options.DialogTools);
			else
				FindDialogProgram();
			if (s_DialogProgram[0] != '\0' && (!spawnPrepared || !PrepareDialogActions()))
			{
				BS_CORE_WARN("Crash reports: no crash dialog (cannot prepare its process)");
				s_DialogProgram[0] = '\0';
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
		// Stack for the filter after a stack overflow, out of the thread's usual stack (1 MiB by default): symbol lookup needs
		// room.
		constexpr ULONG StackGuarantee = 128 * 1024;
		// How long the dump may take, a lock the crash left held being able to stop it. RequestDump also keeps it inside
		// ReportTimeoutSeconds, with ReportTailMilliseconds to spare for the rest of the report.
		constexpr DWORD DumpTimeoutMilliseconds = 20000;
		constexpr DWORD ReportTailMilliseconds = 3000;
		// How long opening the report, and an ended watchdog's exit, may take.
		constexpr DWORD OpenTimeoutMilliseconds = 5000;
		constexpr USHORT MaxWindowsFrames = 62;

		// Work the crashed thread must not do itself: writing a minidump takes more stack than a stack overflow leaves, and
		// starting a process takes the process heap, which a crash may have left locked, so it runs where a timeout can
		// abandon it. The watchdog waits from Install on; a request is written before s_WatchdogWake is set, and its outputs
		// are read after s_WatchdogDone is.
		struct WatchdogDump
		{
			HANDLE File = INVALID_HANDLE_VALUE;
			EXCEPTION_POINTERS* Exception = nullptr;
			DWORD ThreadId = 0;
			bool Written = false;
			DWORD Error = 0;
		};
		struct WatchdogOpen
		{
			const wchar_t* File = nullptr;
		};
		using WatchdogRequest = std::variant<WatchdogDump, WatchdogOpen>;
		WatchdogRequest s_Request;
		HANDLE s_WatchdogWake = nullptr;
		HANDLE s_WatchdogDone = nullptr;
		HANDLE s_WatchdogThread = nullptr;
		// A report's time limit, a thread of its own (POSIX reports have their alarm): the watchdog may be ended, or stalled,
		// in the case this exists for.
		HANDLE s_ReportBegan = nullptr;
		HANDLE s_ReportFinished = nullptr;
		// When the report began (GetTickCount64).
		std::atomic<uint64_t> s_ReportStart = 0;
		// How the process ends if the report does not finish: abort's code unless an exception set its own.
		std::atomic<DWORD> s_CrashExitCode = 3;
		wchar_t s_Explorer[MAX_PATH + 16] = {};

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

		// Notes that text lost its end to its buffer's capacity; false when the note cannot be written.
		bool WriteTruncationNote(HANDLE file, const CrashText& text)
		{
			return !text.Truncated() || WriteAll(file, TruncationNote);
		}

		template<size_t Capacity>
		void AppendUtf8(BasicCrashText<Capacity>& out, const wchar_t* text)
		{
			static char s_Buffer[MaxPathLength * 3 + 1];
			const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, s_Buffer, static_cast<int>(sizeof(s_Buffer)), nullptr, nullptr);
			if (length > 1)
				out.Append(std::string_view(s_Buffer, static_cast<size_t>(length - 1)));
			else if (text[0] != L'\0')
				out.Append("(a path that cannot be shown as UTF-8)");
		}

		// The addresses, which need nothing but memory.
		void AppendStackAddresses(CrashText& out, void* const* frames, USHORT count)
		{
			for (USHORT i = 0; i < count; i++)
			{
				out.Append("  ");
				out.AppendHex(reinterpret_cast<DWORD64>(frames[i]));
				out.Append("\n");
			}
		}

		// The frames' function names. May load symbol files (SYMOPT_DEFERRED_LOADS), which can take long, so a report asks
		// for them last.
		void AppendStackNames(CrashText& out, void* const* frames, USHORT count)
		{
			alignas(SYMBOL_INFO) static char s_SymbolStorage[sizeof(SYMBOL_INFO) + 256];
			auto* symbol = reinterpret_cast<SYMBOL_INFO*>(s_SymbolStorage);
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

		// A minidump of the process; with an exception, request.ThreadId is the crashed thread and the dump holds its context.
		void WriteDump(WatchdogDump& request)
		{
			MINIDUMP_EXCEPTION_INFORMATION information = { request.ThreadId, request.Exception, FALSE };
			request.Written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), request.File, static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo), request.Exception ? &information : nullptr, nullptr, nullptr) != FALSE;
			if (!request.Written)
				request.Error = GetLastError();
		}

		// Opens a file with its default program: explorer.exe, rather than ShellExecute with the COM it brings.
		void OpenWithDefaultProgram(const wchar_t* file)
		{
			static wchar_t s_Command[MaxPathLength + std::size(s_Explorer) + 80];
			s_Command[0] = L'\0';
			AppendWide(s_Command, std::size(s_Command) - 1, L"explorer.exe \"");
			AppendWide(s_Command, std::size(s_Command) - 1, file);
			AppendWide(s_Command, std::size(s_Command) - 1, L"\"");
			STARTUPINFOW startup = {};
			startup.cb = sizeof(startup);
			PROCESS_INFORMATION process = {};
			if (CreateProcessW(s_Explorer, s_Command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
			{
				CloseHandle(process.hThread);
				CloseHandle(process.hProcess);
			}
		}

		void RunRequest()
		{
			if (auto* dumpRequest = std::get_if<WatchdogDump>(&s_Request))
				WriteDump(*dumpRequest);
			else if (const auto* openRequest = std::get_if<WatchdogOpen>(&s_Request))
				OpenWithDefaultProgram(openRequest->File);
		}

		DWORD WINAPI RunWatchdog(void*)
		{
			while (WaitForSingleObject(s_WatchdogWake, INFINITE) == WAIT_OBJECT_0)
			{
				RunRequest();
				SetEvent(s_WatchdogDone);
			}
			return 1;
		}

		// The report's thread is stuck (symbol files on a dead share, a lock the crash left held).
		DWORD WINAPI RunReportDeadline(void*)
		{
			if (WaitForSingleObject(s_ReportBegan, INFINITE) != WAIT_OBJECT_0)
				return 1;
			if (WaitForSingleObject(s_ReportFinished, ReportTimeoutSeconds * 1000) == WAIT_TIMEOUT)
				TerminateProcess(GetCurrentProcess(), s_CrashExitCode);
			return 0;
		}

		void StartReportDeadline()
		{
			s_ReportStart = GetTickCount64();
			if (s_ReportBegan)
				SetEvent(s_ReportBegan);
		}

		// False when the watchdog did not finish in time, and it is then ended: its request must not be read, and it may hold
		// a lock. A request runs on this thread only when no watchdog started (WriteReport asks for no second one after an
		// ending).
		bool RunOnWatchdog(WatchdogRequest request, DWORD timeoutMilliseconds)
		{
			s_Request = request;
			if (!s_WatchdogThread)
			{
				RunRequest();
				return true;
			}
			SetEvent(s_WatchdogWake);
			if (WaitForSingleObject(s_WatchdogDone, timeoutMilliseconds) == WAIT_OBJECT_0)
				return true;
			TerminateThread(s_WatchdogThread, 1);
			WaitForSingleObject(s_WatchdogThread, OpenTimeoutMilliseconds);
			s_WatchdogThread = nullptr;
			// The wait can also expire because the dump writer suspended this thread: a dump that did finish counts.
			return WaitForSingleObject(s_WatchdogDone, 0) == WAIT_OBJECT_0;
		}

		// A dump for this report: whether it was written, or why not in outError and outTimedOut.
		bool RequestDump(HANDLE dump, EXCEPTION_POINTERS* exception, DWORD& outError, bool& outTimedOut)
		{
			// Without a watchdog the dump runs on this thread, which has no stack for it after an overflow.
			if (!s_WatchdogThread && exception && exception->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
			{
				outError = ERROR_STACK_OVERFLOW;
				return false;
			}
			const uint64_t elapsed = GetTickCount64() - s_ReportStart;
			const uint64_t limit = uint64_t{ ReportTimeoutSeconds } * 1000 - ReportTailMilliseconds;
			const uint64_t left = elapsed < limit ? limit - elapsed : 0;
			if (!RunOnWatchdog(WatchdogDump{ dump, exception, GetCurrentThreadId() }, static_cast<DWORD>(std::min<uint64_t>(DumpTimeoutMilliseconds, left))))
			{
				outTimedOut = true;
				return false;
			}
			const auto* result = std::get_if<WatchdogDump>(&s_Request);
			outError = result->Error;
			return result->Written;
		}

		// Writes the text report and a minidump (with the exception's context when there is one), tells stderr and, when
		// enabled, the player. Everything that needs nothing but memory goes out first, and function names last: what stalls a
		// report is symbol files, and the report's deadline may then end the process at any point.
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
			const bool written = file != INVALID_HANDLE_VALUE;
			const DWORD openError = written ? 0 : GetLastError();
			bool complete = true;
			bool timedOut = false;
			static void* s_Frames[MaxWindowsFrames];
			const USHORT frameCount = CaptureStackBackTrace(0, MaxWindowsFrames, s_Frames, nullptr);
			if (written)
			{
				s_Text.Clear();
				CrashReporter::FormatHeader(s_Text, reason, address, unixTime, processId);
				s_Text.Append(StackTraceHeading);
				AppendStackAddresses(s_Text, s_Frames, frameCount);
				s_Text.Append(LogHeading);
				CrashReporter::FormatLogLines(s_Text);
				complete = WriteAll(file, s_Text.View());
				complete = WriteTruncationNote(file, s_Text) && complete;
				bool dumped = false;
				const HANDLE dump = CreateReportFile(s_Base, L".dmp", s_DumpPath, std::size(s_DumpPath) - 1);
				DWORD dumpError = dump == INVALID_HANDLE_VALUE ? GetLastError() : 0;
				bool dumpLeftBehind = false;
				if (dump != INVALID_HANDLE_VALUE)
				{
					dumped = RequestDump(dump, exception, dumpError, timedOut);
					CloseHandle(dump);
					// No empty or partial dump beside the report.
					if (!dumped)
						dumpLeftBehind = DeleteFileW(s_DumpPath) == FALSE;
				}
				s_Text.Clear();
				if (dumped)
				{
					s_Text.Append("\nMinidump: ");
					AppendUtf8(s_Text, s_DumpPath);
				}
				else if (timedOut)
					s_Text.Append("\nMinidump: not written (the watchdog thread did not finish in time)");
				else
				{
					// Hexadecimal: the writer's errors are HRESULTs.
					s_Text.Append("\nMinidump: not written (error ");
					s_Text.AppendHex(dumpError);
					s_Text.Append(")");
				}
				if (dumpLeftBehind)
					s_Text.Append("; what was written of it could not be deleted");
				s_Text.Append("\n");
				complete = WriteAll(file, s_Text.View()) && complete;
			}
			static DialogText s_Summary;
			s_Summary.Clear();
			AppendSummaryOpening(s_Summary, reason, written, complete);
			if (written)
				AppendUtf8(s_Summary, s_TextPath);
			else
			{
				s_Summary.AppendDecimal(openError);
				s_Summary.Append(").");
			}
			s_Summary.Append("\n");
			WriteAll(GetStdHandle(STD_ERROR_HANDLE), s_Summary.View());
			if (written)
			{
				s_Text.Clear();
				s_Text.Append(FunctionNamesHeading);
				AppendStackNames(s_Text, s_Frames, frameCount);
				WriteAll(file, s_Text.View());
				CloseHandle(file);
			}
			s_ReportWritten = true;
			if (s_ReportFinished)
				SetEvent(s_ReportFinished);
			// After a dump that had to be abandoned a lock may be held for good, and nothing would end a box that hangs on it.
			if (s_DialogEnabled && !timedOut)
			{
				// Opening needs the watchdog: on this thread it could hang on a lock the crash left held.
				const bool offerReport = written && s_WatchdogThread && s_Explorer[0] != L'\0';
				static wchar_t s_Message[MaxPathLength + 512];
				s_Message[0] = L'\0';
				AppendWide(s_Message, std::size(s_Message) - 1, s_WideApplication);
				AppendWide(s_Message, std::size(s_Message) - 1, L" crashed.");
				if (written)
				{
					AppendWide(s_Message, std::size(s_Message) - 1, complete ? L"\n\nA crash report was saved to:\n" : L"\n\nAn incomplete crash report was saved to:\n");
					AppendWide(s_Message, std::size(s_Message) - 1, s_TextPath);
					if (offerReport)
						AppendWide(s_Message, std::size(s_Message) - 1, L"\n\nOpen the report?");
				}
				else
					AppendWide(s_Message, std::size(s_Message) - 1, L"\n\nNo crash report could be written.");
				if (MessageBoxW(nullptr, s_Message, s_WideApplication, (offerReport ? MB_YESNO : MB_OK) | MB_ICONERROR | MB_TASKMODAL) == IDYES)
					RunOnWatchdog(WatchdogOpen{ s_TextPath }, OpenTimeoutMilliseconds);
			}
		}

		LONG WINAPI HandleException(EXCEPTION_POINTERS* exception)
		{
			const EXCEPTION_RECORD& record = *exception->ExceptionRecord;
			// The C runtime's own filter turns an uncaught C++ exception into std::terminate, where OnTerminate keeps its
			// message and abort then reports (HandleAbort).
			if (record.ExceptionCode == CppExceptionCode && s_PreviousFilter && !s_Handling)
			{
				// A filter that handles the exception itself (EXECUTE_HANDLER) still ends the process, so that is reported
				// below; only resumed execution is no crash.
				const LONG previous = s_PreviousFilter(exception);
				if (previous == EXCEPTION_CONTINUE_EXECUTION)
					return previous;
			}
			if (!BeginReport())
				return EXCEPTION_CONTINUE_SEARCH;
			s_CrashExitCode = record.ExceptionCode;
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

		void StartWatchdog()
		{
			wchar_t windows[MAX_PATH] = {};
			const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
			if (length > 0 && length < MAX_PATH)
			{
				AppendWide(s_Explorer, std::size(s_Explorer) - 1, windows);
				AppendWide(s_Explorer, std::size(s_Explorer) - 1, L"\\explorer.exe");
			}
			s_WatchdogWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			s_WatchdogDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (s_WatchdogWake && s_WatchdogDone)
				s_WatchdogThread = CreateThread(nullptr, 0, &RunWatchdog, nullptr, 0, nullptr);
			if (!s_WatchdogThread)
				BS_CORE_WARN("Crash reports: no watchdog thread (error {}); dialogs cannot open reports, and a stack overflow leaves no minidump", GetLastError());
			s_ReportBegan = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			s_ReportFinished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			const HANDLE deadline = s_ReportBegan && s_ReportFinished ? CreateThread(nullptr, 0, &RunReportDeadline, nullptr, 0, nullptr) : nullptr;
			if (deadline)
				CloseHandle(deadline);
			else
				BS_CORE_WARN("Crash reports: no deadline thread (error {}); a report that hangs will hang the process", GetLastError());
		}

		bool InstallPlatform(const CrashReporterOptions&)
		{
			// The report folder is the longest path of the chain: when it fits, every level fits.
			const auto chain = MissingDirectoryChain(s_ReportDirectory);
			if (chain.back().wstring().size() > MaxPathLength)
				BS_CORE_WARN("Crash reports: the folder '{}' is longer than {} characters; reports go next to the executable", FileSystem::PathToUtf8(chain.back()), MaxPathLength);
			else
				for (size_t i = 0; i < chain.size(); i++)
					wcsncpy_s(s_Directories[i], chain[i].wstring().c_str(), _TRUNCATE);
			const std::wstring fallback = (FileSystem::GetExecutableDirectory() / "CrashReports").wstring();
			if (fallback.size() <= MaxPathLength)
				wcsncpy_s(s_FallbackDirectory, fallback.c_str(), _TRUNCATE);
			else
				BS_CORE_WARN("Crash reports: the fallback folder '{}' is longer than {} characters; it will not be used", FileSystem::PathToUtf8(fallback), MaxPathLength);
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
			ULONG guarantee = StackGuarantee;
			if (!SetThreadStackGuarantee(&guarantee))
				BS_CORE_WARN("Crash reports: no stack guarantee on this thread (error {}); its stack overflow may not be reported", GetLastError());
			StartWatchdog();
			// Replaces any earlier filter (a C runtime's, or a third party's crash handler); only C++ exceptions are passed back
			// to it, so the C runtime turns them into std::terminate with their message.
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
			const std::string application = CrashReporter::SanitizeApplicationName(options.ApplicationName);
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
			if (!InstallPlatform(options))
			{
				std::set_terminate(s_PreviousTerminate);
				Log::GetHistory().RemoveListener(listener);
				s_ReportDirectory.clear();
				return false;
			}
			BS_CORE_INFO("Crash reports: '{}'", FileSystem::PathToUtf8(s_ReportDirectory));
			return true;
		}

	}

#if !defined(BS_PLATFORM_WINDOWS)
	CrashReporter::ThreadStack::ThreadStack()
	{
		if (!s_InstallResult)
			return;
		// The installing thread's, or an outer ThreadStack's: destroying this one must not take it away.
		stack_t current = {};
		if (sigaltstack(nullptr, &current) == 0 && (current.ss_flags & SS_DISABLE) == 0)
			return;
		void* memory = mmap(nullptr, AlternateStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		stack_t stack = {};
		stack.ss_sp = memory;
		stack.ss_size = AlternateStackSize;
		if (memory == MAP_FAILED || sigaltstack(&stack, nullptr) != 0)
		{
			BS_CORE_WARN("Crash reports: no alternate signal stack on this thread ({}); its stack overflow will not be reported", std::generic_category().message(errno));
			if (memory != MAP_FAILED)
				munmap(memory, AlternateStackSize);
			return;
		}
		m_Stack = memory;
	}

	CrashReporter::ThreadStack::~ThreadStack()
	{
		if (!m_Stack)
			return;
		stack_t disabled = {};
		disabled.ss_flags = SS_DISABLE;
		sigaltstack(&disabled, nullptr);
		munmap(m_Stack, AlternateStackSize);
	}

	void CrashReporter::ShowErrorDialog(std::string_view message)
	{
		if (!s_DialogEnabled)
			return;
		if (s_DialogProgram[0] == '\0')
		{
			BS_CORE_WARN("No dialog program to show this with: {}", message);
			return;
		}
		static DialogText s_Message;
		s_Message.Clear();
		s_Message.Append(message);
		if (!ShowDialog(s_Application, s_Message, ""))
			BS_CORE_WARN("The dialog program '{}' could not start", s_DialogProgram);
	}
#else
	CrashReporter::ThreadStack::ThreadStack()
	{
		if (!s_InstallResult)
			return;
		ULONG guarantee = StackGuarantee;
		if (!SetThreadStackGuarantee(&guarantee))
			BS_CORE_WARN("Crash reports: no stack guarantee on this thread (error {}); its stack overflow may not be reported", GetLastError());
	}

	CrashReporter::ThreadStack::~ThreadStack() = default;

	void CrashReporter::ShowErrorDialog(std::string_view message)
	{
		if (!s_DialogEnabled)
			return;
		static DialogText s_Message;
		s_Message.Clear();
		s_Message.Append(message);
		if (MessageBoxW(nullptr, Platform::Utf8ToWideReplacing(s_Message.View()).c_str(), s_WideApplication, MB_OK | MB_ICONERROR | MB_TASKMODAL) == 0)
			BS_CORE_WARN("The message box could not show (error {}): {}", GetLastError(), s_Message.View());
	}
#endif

	void CrashReporter::FormatAppleScript(CrashText& out, std::string_view title, std::string_view message, std::string_view reportPath)
	{
		const bool offersReport = !reportPath.empty();
		out.Append(offersReport ? "set answer to button returned of (display alert \"" : "display alert \"");
		AppendEscaped(out, title);
		out.Append("\" message \"");
		AppendEscaped(out, message);
		if (!offersReport)
		{
			out.Append("\" as critical buttons {\"OK\"}");
			return;
		}
		out.Append("\" as critical buttons {\"Close\", \"");
		out.Append(OpenReportButton);
		out.Append("\"})\nif answer is \"");
		out.Append(OpenReportButton);
		out.Append("\" then do shell script \"/usr/bin/open \" & quoted form of \"");
		AppendEscaped(out, reportPath);
		out.Append("\"");
	}

	std::string CrashReporter::SanitizeApplicationName(std::string_view name)
	{
		return SanitizeName(name);
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
		bool full = false;
		const std::string_view parts[] = { "[", level, "] ", logger, ": ", text };
		for (const std::string_view part : parts)
		{
			for (const char c : part)
			{
				if (length == MaxLogLineLength)
				{
					length = CrashTextDetail::TrimPartialUtf8(std::string_view(line.Text, length), c);
					full = true;
					break;
				}
				line.Text[length++] = c == '\n' || c == '\r' ? ' ' : c;
			}
			if (full)
				break;
		}
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
