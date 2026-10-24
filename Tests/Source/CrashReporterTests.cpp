#include <doctest/doctest.h>

#include <Basalt/Core/CrashReporter.h>

#include <algorithm>
#include <csignal>
#include <memory>
#include <string>

using namespace Basalt;

// The handlers themselves are exercised by crashing child processes (Tests/Automation/CrashReport.py, ctest CrashReport);
// these cases check the report's parts, which the handlers format without allocating.
TEST_SUITE("CrashReporter")
{
	TEST_CASE("CrashText formats numbers and drops what does not fit")
	{
		auto text = std::make_unique<CrashText>();
		text->AppendDecimal(0);
		text->Append(" ");
		text->AppendDecimal(18446744073709551615ull);
		text->Append(" ");
		text->AppendHex(0xdeadbeefull);
		text->Append(" ");
		text->AppendHex(0);
		CHECK(text->View() == "0 18446744073709551615 0x00000000deadbeef 0x0000000000000000");
		CHECK(std::string(text->CStr()) == std::string(text->View()));

		text->Clear();
		CHECK(text->View().empty());
		const std::string fill(CrashText::Capacity - 3, 'x');
		text->Append(fill);
		text->AppendDecimal(123456);
		CHECK(text->View().size() == CrashText::Capacity);
		CHECK(text->View().substr(fill.size()) == "123");
		text->Append("more");
		text->AppendHex(1);
		CHECK(text->View().size() == CrashText::Capacity);
		CHECK(std::string(text->CStr()).size() == CrashText::Capacity);
	}

	TEST_CASE("Signals and Windows exception codes have readable names")
	{
		CHECK(std::string(CrashReporter::DescribeSignal(SIGSEGV)).starts_with("SIGSEGV"));
		CHECK(std::string(CrashReporter::DescribeSignal(SIGABRT)).starts_with("SIGABRT"));
		CHECK(std::string(CrashReporter::DescribeSignal(SIGILL)).starts_with("SIGILL"));
		CHECK(std::string(CrashReporter::DescribeSignal(SIGFPE)).starts_with("SIGFPE"));
#if !defined(_WIN32)
		CHECK(std::string(CrashReporter::DescribeSignal(SIGBUS)).starts_with("SIGBUS"));
		CHECK(std::string(CrashReporter::DescribeSignal(SIGTRAP)).starts_with("SIGTRAP"));
#endif
		CHECK(CrashReporter::DescribeSignal(SIGINT) == nullptr);
		CHECK(std::string(CrashReporter::DescribeException(0xC0000005)) == "EXCEPTION_ACCESS_VIOLATION (invalid memory access)");
		CHECK(std::string(CrashReporter::DescribeException(0xC00000FD)).starts_with("EXCEPTION_STACK_OVERFLOW"));
		CHECK(std::string(CrashReporter::DescribeException(0x80000003)).starts_with("EXCEPTION_BREAKPOINT"));
		CHECK(std::string(CrashReporter::DescribeException(0xC0000409)).starts_with("STATUS_STACK_BUFFER_OVERRUN"));
		CHECK(std::string(CrashReporter::DescribeException(0x12345678)) == "an unhandled exception");
	}

	TEST_CASE("The header names the application, context, platform, process, time and reason")
	{
		auto text = std::make_unique<CrashText>();
		CrashReporter::SetContext("Project: Header Test 2.0.1");
		CrashReporter::FormatHeader(*text, "SIGSEGV (invalid memory access)", 0x10, 1700000000, 4321);
		const std::string header(text->View());
		CHECK(header.starts_with("Basalt crash report\nApplication: "));
		CHECK(header.find("\nProject: Header Test 2.0.1\n") != std::string::npos);
#if defined(_WIN32)
		CHECK(header.find("\nPlatform: Windows\n") != std::string::npos);
#elif defined(__APPLE__)
		CHECK(header.find("\nPlatform: macOS\n") != std::string::npos);
#else
		CHECK(header.find("\nPlatform: Linux\n") != std::string::npos);
#endif
		CHECK(header.find("\nProcess: 4321\nTime: 1700000000 (seconds since 1970-01-01 UTC)\n") != std::string::npos);
		CHECK(header.ends_with("\nReason: SIGSEGV (invalid memory access) at 0x0000000000000010\n"));

		// Without an address or a context; a long context is cut.
		text->Clear();
		CrashReporter::SetContext(std::string(300, 'c'));
		CrashReporter::FormatHeader(*text, "SIGABRT", std::nullopt, 1, 2);
		CHECK(std::string(text->View()).find(std::string(255, 'c') + "\n") != std::string::npos);
		CHECK(std::string(text->View()).find(std::string(256, 'c')) == std::string::npos);
		CHECK(std::string(text->View()).ends_with("\nReason: SIGABRT\n"));
		text->Clear();
		CrashReporter::SetContext("");
		CrashReporter::FormatHeader(*text, "SIGABRT", std::nullopt, 1, 2);
		CHECK(std::string(text->View()).find("\n\n") == std::string::npos);
	}

	TEST_CASE("The last log lines are kept in order, cut and on one line each")
	{
		auto text = std::make_unique<CrashText>();
		for (size_t i = 0; i < CrashReporter::MaxLogLines + 5; i++)
			CrashReporter::RecordLogLine("info", "APP", "line " + std::to_string(i));
		CrashReporter::FormatLogLines(*text);
		std::string lines(text->View());
		// The oldest five fell out; the rest are oldest first.
		CHECK(lines.find("[info] APP: line 4\n") == std::string::npos);
		CHECK(lines.starts_with("[info] APP: line 5\n"));
		CHECK(lines.ends_with("[info] APP: line " + std::to_string(CrashReporter::MaxLogLines + 4) + "\n"));
		CHECK(std::count(lines.begin(), lines.end(), '\n') == static_cast<std::ptrdiff_t>(CrashReporter::MaxLogLines));

		CrashReporter::RecordLogLine("error", "BASALT", "two\nlines\r" + std::string(500, 'z'));
		text->Clear();
		CrashReporter::FormatLogLines(*text);
		lines = std::string(text->View());
		const std::string last = lines.substr(lines.rfind('\n', lines.size() - 2) + 1);
		CHECK(last.starts_with("[error] BASALT: two lines zzz"));
		CHECK(last.size() == CrashReporter::MaxLogLineLength + 1);
	}

	TEST_CASE("Install is skipped by the unit tests")
	{
		// The test process must not install handlers (a failing check would turn into a crash report); only the
		// crash-report child does (CrashReport.py).
		CHECK_FALSE(CrashReporter::IsInstalled());
		CHECK(CrashReporter::GetReportDirectory().empty());
	}
}
