#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Basalt {

	namespace CrashTextDetail {
		// How much of kept, the part of a cut text before its first dropped byte next, to keep so it does not end inside a
		// UTF-8 sequence (which a report viewer would show as a broken character).
		inline size_t TrimPartialUtf8(std::string_view kept, char next)
		{
			const auto continuation = [](char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; };
			size_t length = kept.size();
			if (!continuation(next))
				return length;
			while (length > 0 && continuation(kept[length - 1]))
				length--;
			// The sequence's lead byte goes too.
			return length > 0 && (static_cast<unsigned char>(kept[length - 1]) & 0xC0) == 0xC0 ? length - 1 : length;
		}
	}

	// Text built without allocating, for the crash handler: a crashed process may have a corrupt heap, or have crashed
	// while another thread held the allocator's lock. Text past the capacity is dropped, cut on a UTF-8 character
	// boundary, and Truncated says so. Small capacities fit on a crashed thread's stack; the 32 KiB CrashText lives in
	// static storage.
	template<size_t CapacityValue>
	class BasicCrashText
	{
	public:
		static constexpr size_t Capacity = CapacityValue;

		BasicCrashText() = default;
		BasicCrashText(const BasicCrashText&) = delete;
		BasicCrashText& operator=(const BasicCrashText&) = delete;

		void Append(std::string_view text)
		{
			size_t length = std::min(text.size(), Capacity - m_Size);
			if (length < text.size())
			{
				m_Truncated = true;
				length = CrashTextDetail::TrimPartialUtf8(text.substr(0, length), text[length]);
			}
			std::copy_n(text.begin(), length, m_Data + m_Size);
			m_Size += length;
			m_Data[m_Size] = '\0';
		}
		void AppendDecimal(uint64_t value)
		{
			char digits[20];
			size_t start = sizeof(digits);
			do
			{
				digits[--start] = static_cast<char>('0' + value % 10);
				value /= 10;
			} while (value != 0);
			Append(std::string_view(digits + start, sizeof(digits) - start));
		}
		// "0x" and 16 hex digits.
		void AppendHex(uint64_t value)
		{
			static constexpr char Digits[] = "0123456789abcdef";
			char text[18] = { '0', 'x' };
			for (int i = 17; i >= 2; i--, value >>= 4)
				text[i] = Digits[value & 0xF];
			Append(std::string_view(text, sizeof(text)));
		}
		std::string_view View() const { return { m_Data, m_Size }; }
		// Whether some appended text was dropped since construction or the last Clear.
		bool Truncated() const { return m_Truncated; }
		// The text NUL-terminated, for C APIs (open, exec).
		const char* CStr() const { return m_Data; }
		void Clear()
		{
			m_Size = 0;
			m_Data[0] = '\0';
			m_Truncated = false;
		}

	private:
		char m_Data[Capacity + 1] = {};
		size_t m_Size = 0;
		bool m_Truncated = false;
	};
	using CrashText = BasicCrashText<size_t{ 32 } * 1024>;

	// The dialog programs of macOS and Linux.
	enum class CrashDialogKind
	{
		Osascript,
		Zenity,
		KDialog,
		XMessage
	};

	// A dialog program and what opens a report from it. Install warns and shows no dialogs when Program cannot run or either
	// path is over 1024 bytes.
	struct CrashDialogTools
	{
		std::filesystem::path Program;
		CrashDialogKind Kind = CrashDialogKind::Zenity;
		// Empty: the dialog shows the report's path only. Osascript does not use it.
		std::filesystem::path Opener;
	};

	struct CrashReporterOptions
	{
		// Names the report folder (<user data>/<name>/Crashes) and appears in reports. Control characters and characters
		// that would leave that folder or that Windows refuses (/ \ : < > " | ? *) are dropped, trailing dots and spaces
		// too, and the name is cut at 127 bytes on a UTF-8 character boundary; "Basalt" when nothing is left.
		std::string ApplicationName;
		// Stand-ins for the system's dialog program and opener, on macOS and Linux (tests); ignored on Windows.
		std::optional<CrashDialogTools> DialogTools;
	};

	// Writes a crash report when the process dies of a fatal signal (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP
	// on macOS and Linux) or an unhandled exception (Windows, plus abort, invalid C runtime parameters and pure virtual
	// calls): the reason, a stack trace, the application and its context (the project), an uncaught C++ exception's
	// message, and the last log lines; Linux adds addr2line's names for the executable's frames when /usr/bin/addr2line
	// exists, and Windows a minidump written by a watchdog thread, which keeps working when the crashed thread has no
	// stack left. Reports go to <user data>/<application>/Crashes (per user, so a signed app bundle or a read-only
	// install is normally left alone); only when there is no user data directory, or the report cannot be created there,
	// are they tried in CrashReports next to the executable. Everything the handler uses is prepared beforehand: it
	// formats into fixed buffers and, on macOS and Linux, allocates nothing and never forks (the dialog and addr2line
	// start through posix_spawn, and backtrace's unwinder is loaded at Install), on an alternate stack for the thread
	// that installed it and for threads holding a ThreadStack. Then the process ends: by the same signal or exception
	// code, or with the C runtime's abort code 3 after abort, an invalid C runtime parameter or a pure virtual call on
	// Windows (where it would otherwise fail fast with 0xC0000409).
	class CrashReporter
	{
	public:
		static constexpr size_t MaxLogLines = 64;
		static constexpr size_t MaxLogLineLength = 240;

		// Makes a stack overflow on the calling thread reportable: an alternate signal stack on macOS and Linux, a stack
		// guarantee on Windows, each only per thread. Hold one for the life of an engine thread started after Install. Does
		// nothing before Install or when crash reports are unavailable, and threads the engine does not start (miniaudio's,
		// a driver's) have none: their overflows end the process without a report.
		// Create and destroy it on one thread. On a thread that already has an alternate stack it does nothing.
		class [[nodiscard]] ThreadStack
		{
		public:
			ThreadStack();
			~ThreadStack();
			ThreadStack(const ThreadStack&) = delete;
			ThreadStack& operator=(const ThreadStack&) = delete;

		private:
			// The alternate stack (macOS, Linux; unused on Windows); null when this object set up none.
			void* m_Stack = nullptr;
		};

		// Installs the handlers (once per process; later and concurrent calls return the first call's result). False when
		// crash reports are unavailable: under AddressSanitizer, which reports these crashes itself and needs the signals
		// for that, or when a signal handler cannot be installed (the signal handlers, terminate handler and log listener
		// installed so far are then removed).
		static bool Install(const CrashReporterOptions& options);
		static bool IsInstalled();
		// A line naming what was running, in every report from now on ("Project: Platformer 1.2.0"). Cut at 255 bytes, on
		// a UTF-8 character boundary.
		static void SetContext(std::string_view context);
		// Allows message boxes: one after writing a report, with its path and an offer to open it (Linux needs xdg-open), and
		// ShowErrorDialog's. The windowed runtime enables them; tools, headless and automated runs never show one (nobody
		// would close it).
		static void SetDialogEnabled(bool enabled);
		// Tells the player why the game cannot go on, for a failure that is no crash (a broken archive): a message box when
		// dialogs are enabled, nothing otherwise; a warning in the log when the box cannot show. Text past 4 KiB is cut.
		// Windows returns when it is closed; elsewhere it stays after the process ends.
		static void ShowErrorDialog(std::string_view message);
		// The folder reports go to; empty before Install, under AddressSanitizer and after a failed Install.
		static std::filesystem::path GetReportDirectory();

		// The parts of a report and its dialog, shared by the handler and the tests.
		// The AppleScript of the macOS dialog: an alert with the message, and with a reportPath a button that opens that file.
		static void FormatAppleScript(CrashText& out, std::string_view title, std::string_view message, std::string_view reportPath);
		// The folder name Install makes of CrashReporterOptions::ApplicationName (the rules are there).
		static std::string SanitizeApplicationName(std::string_view name);
		// "SIGSEGV (invalid memory access)" and the like; nullptr for a signal the reporter does not handle.
		static const char* DescribeSignal(int signal);
		// "EXCEPTION_ACCESS_VIOLATION (invalid memory access)" and the like, for Windows exception codes.
		static const char* DescribeException(uint32_t code);
		// The report's opening lines: application, context, platform, process, time and reason (with the faulting address
		// when known), plus the message of an uncaught C++ exception that ended the process.
		static void FormatHeader(CrashText& out, std::string_view reason, std::optional<uint64_t> address, uint64_t unixTime, uint64_t processId);
		// The last MaxLogLines log lines, oldest first, one per line.
		static void FormatLogLines(CrashText& out);
		// Keeps a log line for reports ("[level] logger: text", cut at MaxLogLineLength bytes on a UTF-8 character boundary,
		// line breaks as spaces). Install routes every log message here.
		static void RecordLogLine(std::string_view level, std::string_view logger, std::string_view text);
	};

}
