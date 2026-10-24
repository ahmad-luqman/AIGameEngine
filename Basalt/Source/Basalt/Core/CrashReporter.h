#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Basalt {

	// Text built without allocating, for the crash handler: a crashed process may have a corrupt heap, or have crashed
	// while another thread held the allocator's lock. Text past the capacity is dropped. Small capacities fit on a
	// crashed thread's stack; the 32 KiB CrashText lives in static storage.
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
			const size_t length = std::min(text.size(), Capacity - m_Size);
			std::copy_n(text.begin(), length, m_Data + m_Size);
			m_Size += length;
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
		// The text NUL-terminated, for C APIs (open, exec).
		const char* CStr()
		{
			m_Data[m_Size] = '\0';
			return m_Data;
		}
		void Clear() { m_Size = 0; }

	private:
		char m_Data[Capacity + 1] = {};
		size_t m_Size = 0;
	};
	using CrashText = BasicCrashText<size_t{ 32 } * 1024>;

	struct CrashReporterOptions
	{
		// Names the report folder (<user data>/<name>/Crashes) and appears in reports. Characters that would leave that
		// folder ('/', '\', ':') are dropped and the name is cut at 127 bytes; "Basalt" when nothing usable is left.
		std::string ApplicationName;
	};

	// Writes a crash report when the process dies of a fatal signal (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP on
	// macOS and Linux) or an unhandled exception (Windows, plus abort, invalid C runtime parameters and pure virtual
	// calls): the reason, a stack trace, the application and its context (the project), an uncaught C++ exception's
	// message, and the last log lines; Windows adds a minidump. Reports go to <user data>/<application>/Crashes (per user,
	// so a signed app bundle or a read-only install is normally left alone); only when there is no user data directory, or
	// the report cannot be created there, are they tried in CrashReports next to the executable. Everything the handler
	// uses is prepared beforehand: it formats into fixed buffers and, on macOS and Linux, allocates nothing and never forks
	// (backtrace's unwinder is loaded at Install), on an alternate stack for the thread that installed it. Then the process
	// dies as it would have without the reporter: the same signal or exception code, or exit code 3 after abort, an invalid
	// C runtime parameter or a pure virtual call on Windows.
	class CrashReporter
	{
	public:
		static constexpr size_t MaxLogLines = 64;
		static constexpr size_t MaxLogLineLength = 240;

		// Installs the handlers (once per process; later and concurrent calls return the first call's result). False when
		// crash reports are unavailable: under AddressSanitizer, which reports these crashes itself and needs the signals
		// for that, or when a signal handler cannot be installed (everything installed so far is then undone).
		static bool Install(const CrashReporterOptions& options);
		static bool IsInstalled();
		// A line naming what was running, in every report from now on ("Project: Platformer 1.2.0"). Cut at 255 bytes.
		static void SetContext(std::string_view context);
		// Shows a message box with the report's path after writing it: the windowed runtime enables it; tools, headless
		// and automated runs never show one (nobody would close it).
		static void SetDialogEnabled(bool enabled);
		// The folder reports go to; empty before Install.
		static std::filesystem::path GetReportDirectory();

		// The parts of a report, shared by the handler and the tests.
		// "SIGSEGV (invalid memory access)" and the like; nullptr for a signal the reporter does not handle.
		static const char* DescribeSignal(int signal);
		// "EXCEPTION_ACCESS_VIOLATION (invalid memory access)" and the like, for Windows exception codes.
		static const char* DescribeException(uint32_t code);
		// The report's opening lines: application, context, platform, process, time and reason (with the faulting address
		// when known), plus the message of an uncaught C++ exception that ended the process.
		static void FormatHeader(CrashText& out, std::string_view reason, std::optional<uint64_t> address, uint64_t unixTime, uint64_t processId);
		// The last MaxLogLines log lines, oldest first, one per line.
		static void FormatLogLines(CrashText& out);
		// Keeps a log line for reports ("[level] logger: text", cut at MaxLogLineLength bytes, line breaks as spaces).
		// Install routes every log message here.
		static void RecordLogLine(std::string_view level, std::string_view logger, std::string_view text);
	};

}
