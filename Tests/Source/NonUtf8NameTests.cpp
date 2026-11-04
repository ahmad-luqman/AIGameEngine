#include <doctest/doctest.h>

#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/AssetRegistry.h>
#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/FileWatcher.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Core/PackArchive.h>
#include <Basalt/Core/TextEncoding.h>
#include <Basalt/Project/Exporter.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Project/ProjectValidator.h>

#include "AutomationTestUtils.h"
#include "TestUtils.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace Basalt;
using BasaltTest::Contains;
using BasaltTest::EscapedNonUtf8Name;
using BasaltTest::NonUtf8Name;
using BasaltTest::Run;
using json = nlohmann::json;

namespace {

	void WriteRaw(const std::filesystem::path& path, const std::string& contents)
	{
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);
		REQUIRE_FALSE(error);
		std::ofstream file(path, std::ios::binary);
		file << contents;
		REQUIRE(file.good());
	}

	std::vector<std::string> Sorted(std::vector<std::string> entries)
	{
		std::sort(entries.begin(), entries.end());
		return entries;
	}

	size_t CountMessages(uint64_t since, std::string_view text)
	{
		uint64_t next = 0;
		const std::vector<LogMessage> messages = Log::GetHistory().GetMessagesSince(since, next);
		return static_cast<size_t>(std::count_if(messages.begin(), messages.end(), [&](const LogMessage& message) { return message.Text.find(text) != std::string::npos; }));
	}

	// A project holding keyed files beside every kind of entry no key can hold: a file with a sidecar that would index it,
	// a document that is not even JSON, a folder of files, and two names whose U+FFFD text is the same.
	class NonUtf8Project
	{
	public:
		explicit NonUtf8Project(const std::string& name)
			: m_Temp(name)
		{
			REQUIRE(m_Temp.IsValid());
			Available = BasaltTest::CanCreateNonUtf8Names(m_Temp.GetDirectory());
			if (!Available)
			{
				MESSAGE("this file system refuses such names; Linux and Windows run the test");
				return;
			}
			m_Temp.WriteFile("Assets/Good.png", "image");
			m_Temp.WriteFile("Assets/Scenes/Main.bscene", R"({"Entities":[]})");
			const std::filesystem::path assets = m_Temp.GetDirectory() / "Assets";
			WriteRaw(assets / NonUtf8Name("Bad", ".png"), "image");
			std::filesystem::path sidecar = assets / NonUtf8Name("Bad", ".png");
			sidecar += ".meta";
			WriteRaw(sidecar, R"({"Version":1,"UUID":"1234567"})");
			WriteRaw(assets / "Scenes" / NonUtf8Name("Broken", ".bscene"), "not a document");
			WriteRaw(assets / NonUtf8Name("Folder") / "Inner.lua", "return {}");
			WriteRaw(assets / NonUtf8Name("Folder") / "Deep" / "Inner.bscene", "not a document");
			WriteRaw(assets / NonUtf8Name("Clash", ".txt", 0), "first");
			WriteRaw(assets / NonUtf8Name("Clash", ".txt", 1), "second");
		}

		const BasaltTest::TempProject& GetTemp() const { return m_Temp; }

		// Every entry skipped, as scans report it, sorted. A folder counts once, whatever it holds.
		static std::vector<std::string> Expected()
		{
			return Sorted({ EscapedNonUtf8Name("Assets/Bad", ".png"), EscapedNonUtf8Name("Assets/Bad", ".png.meta"), EscapedNonUtf8Name("Assets/Scenes/Broken", ".bscene"), EscapedNonUtf8Name("Assets/Folder"), EscapedNonUtf8Name("Assets/Clash", ".txt", 0), EscapedNonUtf8Name("Assets/Clash", ".txt", 1) });
		}

		bool Available = false;

	private:
		BasaltTest::TempProject m_Temp;
	};

}

TEST_SUITE("Unicode")
{
	TEST_CASE("UTF-8 sequences are valid only in their shortest form, inside Unicode")
	{
		using TextEncoding::IsValidUtf8;
		using TextEncoding::Utf8SequenceLength;
		CHECK(IsValidUtf8(""));
		CHECK(IsValidUtf8("Assets/資産.png"));
		CHECK(IsValidUtf8("\xF0\x9F\x8E\xAE"));
		CHECK(Utf8SequenceLength("a", 0) == 1);
		CHECK(Utf8SequenceLength("\xC3\xA9", 0) == 2);
		CHECK(Utf8SequenceLength("\xE8\xB3\x87", 0) == 3);
		CHECK(Utf8SequenceLength("\xF4\x8F\xBF\xBF", 0) == 4); // U+10FFFF, the last there is.
		for (const char* invalid : {
				 "\xFF",             // Never a UTF-8 byte.
				 "\x80",             // A continuation with nothing before it.
				 "\xC3",             // Cut.
				 "\xE8\xB3",         // Cut.
				 "\xC3(",            // A lead without its continuation.
				 "\xC0\x80",         // Overlong.
				 "\xE0\x80\x80",     // Overlong.
				 "\xF0\x80\x80\x80", // Overlong.
				 "\xED\xA0\x80",     // An encoded surrogate.
				 "\xF4\x90\x80\x80", // Past U+10FFFF.
				 "\xF5\x80\x80\x80" })
		{
			CAPTURE(invalid);
			CHECK_FALSE(IsValidUtf8(invalid));
			CHECK(Utf8SequenceLength(invalid, 0) == 0);
		}
		CHECK_FALSE(IsValidUtf8("valid, then \xFF"));
		CHECK(TextEncoding::EscapeInvalidUtf8("A\xFF資\xC3") == "A\\xFF資\\xC3");
		CHECK(TextEncoding::EscapeInvalidUtf8("clean/資産") == "clean/資産");
		CHECK(TextEncoding::EscapeInvalidUtf8("\x80\x0A") == "\\x80\n");
	}

	TEST_CASE("UTF-16 surrogates count only in pairs")
	{
		const auto valid = [](std::initializer_list<char16_t> units) { return TextEncoding::IsValidUtf16(std::u16string_view(std::u16string(units))); };
		const auto escaped = [](std::initializer_list<char16_t> units) {
			const std::u16string text(units);
			return TextEncoding::EscapeUnpairedSurrogates(std::u16string_view(text));
		};
		CHECK(valid({}));
		CHECK(valid({ u'a', 0xD7FF, 0xE000, 0xFFFD }));
		CHECK(valid({ 0xD83C, 0xDFAE }));
		CHECK(valid({ 0xDBFF, 0xDFFF, u'a' }));
		CHECK_FALSE(valid({ 0xD800 }));                 // A high surrogate at the end.
		CHECK_FALSE(valid({ 0xDC00 }));                 // A low one alone.
		CHECK_FALSE(valid({ 0xDBFF, u'a' }));           // A high one before something else.
		CHECK_FALSE(valid({ 0xD800, 0xD800, 0xDC00 })); // Two highs, of which only the second pairs.
		CHECK_FALSE(valid({ 0xDC00, 0xD800 }));         // In the wrong order.

		CHECK(escaped({ u'a', 0xD800, u'b' }) == u"a\\uD800b");
		CHECK(escaped({ 0xDFFF }) == u"\\uDFFF");
		CHECK(escaped({ 0xD83C, 0xDFAE }) == std::u16string{ 0xD83C, 0xDFAE });
		CHECK(escaped({ 0xD800, 0xD800, 0xDC00 }) == std::u16string{ u'\\', u'u', u'D', u'8', u'0', u'0', 0xD800, 0xDC00 });
	}

	TEST_CASE("Paths know whether UTF-8 can name them, and always have text for reports")
	{
		CHECK(FileSystem::IsUtf8Path(FileSystem::PathFromUtf8("Assets/資産.png")));
		CHECK(FileSystem::IsUtf8Path(""));
		CHECK_FALSE(FileSystem::IsUtf8Path(NonUtf8Name("Bad", ".png")));
		CHECK_FALSE(FileSystem::IsUtf8Path(std::filesystem::path("Assets") / NonUtf8Name("Bad") / "Fine.png"));

		CHECK(FileSystem::PathToEscapedUtf8(FileSystem::PathFromUtf8("Assets/資産.png")) == "Assets/資産.png");
		CHECK(FileSystem::PathToEscapedUtf8(std::filesystem::path("Assets") / NonUtf8Name("Bad", ".png")) == EscapedNonUtf8Name("Assets/Bad", ".png"));
		// Two names one U+FFFD would merge stay apart, and the text is valid wherever it goes.
		CHECK(FileSystem::PathToEscapedUtf8(NonUtf8Name("Clash", "", 0)) != FileSystem::PathToEscapedUtf8(NonUtf8Name("Clash", "", 1)));
		CHECK(TextEncoding::IsValidUtf8(FileSystem::PathToEscapedUtf8(NonUtf8Name("資産"))));

		// An entry needs no file, so this runs where the file system would refuse the name. assign keeps the path of a missing
		// file; libstdc++'s constructor empties it.
		const auto entry = [](const std::filesystem::path& path) {
			std::filesystem::directory_entry result;
			std::error_code error;
			result.assign(path, error);
			return result;
		};
		const std::filesystem::path root = "Project";
		const std::filesystem::directory_entry bad = entry(root / "Assets" / NonUtf8Name("Bad", ".png"));
		const std::filesystem::directory_entry good = entry(root / "Assets" / "Good.png");
		// Only its own name counts: where the project lies is part of no key.
		const std::filesystem::directory_entry below = entry(NonUtf8Name("Folder") / root / "Good.png");
		std::vector<std::string> skipped;
		CHECK_FALSE(FileSystem::SkipNonUtf8Entry(good, root, skipped));
		CHECK_FALSE(FileSystem::SkipNonUtf8Entry(below, NonUtf8Name("Folder") / root, skipped));
		CHECK(skipped.empty());
		CHECK(FileSystem::SkipNonUtf8Entry(bad, root, skipped));
		CHECK(skipped == std::vector<std::string>{ EscapedNonUtf8Name("Assets/Bad", ".png") });
	}

	TEST_CASE("The asset registry indexes what has a key and lists the rest")
	{
		NonUtf8Project project("NonUtf8Registry");
		if (!project.Available)
			return;
		AssetRegistry registry(project.GetTemp().GetDirectory());
		std::string error;
		REQUIRE_MESSAGE(registry.Refresh(error), error);
		CHECK(registry.GetSkipped() == NonUtf8Project::Expected());
		// The skipped sidecar neither indexes its file under a key that leads nowhere nor counts as a problem or an orphan.
		CHECK(registry.GetProblems().empty());
		CHECK(registry.GetOrphans().empty());
		CHECK(registry.Find(UUID(1234567)).empty());
		// A keyed file still gets its identity beside them.
		CHECK(registry.Ensure("Assets/Good.png", error).has_value());
		CHECK(registry.GetSkipped() == NonUtf8Project::Expected());
		// Loading says so once, for hosts without a watcher.
		const uint64_t since = Log::GetHistory().GetTotalCount();
		std::string loadError;
		const Ref<Project> loaded = Project::Load(project.GetTemp().GetDirectory() / Project::FileName, loadError);
		REQUIRE_MESSAGE(loaded, loadError);
		CHECK(CountMessages(since, FileSystem::NonUtf8NameProblem) == NonUtf8Project::Expected().size());
	}

	TEST_CASE("The file watcher leaves such names out without giving up on the folder")
	{
		NonUtf8Project project("NonUtf8Watcher");
		if (!project.Available)
			return;
		const std::filesystem::path root = project.GetTemp().GetDirectory();
		const FileWatcher::Scan scan = FileWatcher::ScanDirectory(root, "Assets");
		CHECK(scan.Error.empty());
		CHECK(scan.Skipped == NonUtf8Project::Expected());
		CHECK(scan.Files.contains("Assets/Good.png"));
		CHECK(scan.Files.contains("Assets/Scenes/Main.bscene"));
		// Nothing below the skipped folder, and no entry made of a U+FFFD, or of a stray byte.
		CHECK(std::all_of(scan.Files.begin(), scan.Files.end(), [](const auto& file) { return !Contains(file.first, "Inner") && !Contains(file.first, "\xEF\xBF\xBD") && TextEncoding::IsValidUtf8(file.first); }));

		const uint64_t since = Log::GetHistory().GetTotalCount();
		FileWatcher watcher;
		watcher.Watch(root);
		// The baseline is not reported, as nothing else in it is, but it is known.
		CHECK(watcher.GetSkipped() == NonUtf8Project::Expected());
		CHECK(watcher.Refresh().empty());
		CHECK(CountMessages(since, FileSystem::NonUtf8NameProblem) == 0);

		// One more: logged once, however many scans find it, and the good file written with it is still reported.
		const std::filesystem::path later = root / "Assets" / NonUtf8Name("Later", ".png");
		WriteRaw(later, "image");
		WriteRaw(root / "Assets/Beside.png", "image");
		const std::vector<FileChange> changes = watcher.Refresh();
		CHECK(changes == std::vector<FileChange>{ { "Assets/Beside.png", FileChangeType::Added } });
		CHECK(watcher.GetLastError().empty());
		std::vector<std::string> withLater = NonUtf8Project::Expected();
		withLater.push_back(EscapedNonUtf8Name("Assets/Later", ".png"));
		CHECK(watcher.GetSkipped() == Sorted(withLater));
		CHECK(watcher.Refresh().empty());
		CHECK(CountMessages(since, EscapedNonUtf8Name("Assets/Later", ".png")) == 1);

		// Gone, then back: that is news again.
		std::error_code error;
		std::filesystem::remove(later, error);
		REQUIRE_FALSE(error);
		CHECK(watcher.Refresh().empty());
		CHECK(watcher.GetSkipped() == NonUtf8Project::Expected());
		WriteRaw(later, "image");
		CHECK(watcher.Refresh().empty());
		CHECK(CountMessages(since, EscapedNonUtf8Name("Assets/Later", ".png")) == 2);
	}

	TEST_CASE("Asset lists leave such names out and say which, for the types asked")
	{
		NonUtf8Project project("NonUtf8Lists");
		if (!project.Available)
			return;
		std::string error;
		std::vector<std::string> skipped;
		const std::vector<std::string> all = AssetManager::ListAssets({}, &error, &skipped);
		CHECK(error.empty());
		CHECK(all == std::vector<std::string>{ "Assets/Good.png", "Assets/Scenes/Main.bscene" });
		CHECK(skipped == NonUtf8Project::Expected());

		// The folder could hold anything; the image and the clashing text files were not asked for.
		CHECK(AssetManager::ListAssets({ ".bscene" }, &error, &skipped) == std::vector<std::string>{ "Assets/Scenes/Main.bscene" });
		CHECK(skipped == Sorted({ EscapedNonUtf8Name("Assets/Folder"), EscapedNonUtf8Name("Assets/Scenes/Broken", ".bscene") }));
		CHECK(AssetManager::ListAssets({ ".txt" }, &error, &skipped).empty());
		CHECK(skipped == Sorted({ EscapedNonUtf8Name("Assets/Folder"), EscapedNonUtf8Name("Assets/Clash", ".txt", 0), EscapedNonUtf8Name("Assets/Clash", ".txt", 1) }));
		// The list can be had without them.
		CHECK(AssetManager::ListAssets({ ".bscene" }).size() == 1);
	}

	TEST_CASE("Exact-case lookups do not take a key for a name it cannot lead back to")
	{
		NonUtf8Project project("NonUtf8Lookups");
		if (!project.Available)
			return;
		std::string actual, problem;
		const std::filesystem::path directory = project.GetTemp().GetDirectory();
		// What the name reads as with U+FFFD is simply missing, not a file that cannot be read.
		CHECK(FileSystem::FindExactCase(directory, FileSystem::PathFromUtf8("Assets/Bad\xEF\xBF\xBD.png"), actual, problem) == FileSystem::PathMatch::Missing);
		CHECK(problem.empty());
		CHECK(FileSystem::FindExactCase(directory, "Assets/Good.png", actual, problem) == FileSystem::PathMatch::Exact);
		CHECK(FileSystem::FindExactCase(directory, "Assets/good.png", actual, problem) == FileSystem::PathMatch::DifferentCase);
	}

	TEST_CASE("Validation warns about each such name without failing the project")
	{
		NonUtf8Project project("NonUtf8Validation");
		if (!project.Available)
			return;
		project.GetTemp().WriteFile("Assets/Scenes/Main.bscene", R"({"Entities":[]})");
		Project::GetActive()->GetConfig().StartScene = "Assets/Scenes/Main.bscene";
		const ValidationReport report = ValidateProject(*Project::GetActive());
		std::vector<std::string> warned;
		for (const auto& issue : report.GetIssues())
			if (issue.Message == FileSystem::NonUtf8NameProblem)
			{
				CHECK(issue.Severity == ValidationSeverity::Warning);
				warned.push_back(issue.File);
			}
		CHECK(warned == NonUtf8Project::Expected());
		// Inside the folder, the broken document would have been an error.
		CHECK_MESSAGE(report.IsValid(), "errors: ", report.GetErrorCount());
	}

	TEST_CASE("Archives leave such names out and say so, and folder exports copy them as they are")
	{
		NonUtf8Project project("NonUtf8Export");
		if (!project.Available)
			return;
		const BasaltTest::TempProject& temp = project.GetTemp();
		temp.WriteFile("FakeRuntime", "binary");
		const BasaltTest::ScopedFakeVulkanSdk sdk(temp);
		Project::GetActive()->GetConfig().StartScene = "Assets/Scenes/Main.bscene";
		const std::filesystem::path output = temp.GetDirectory().parent_path() / "NonUtf8ExportOut";
		std::error_code error;
		std::filesystem::remove_all(output, error);
		ExportOptions options;
		options.RuntimeExecutable = temp.GetDirectory() / "FakeRuntime";
		options.Archive = true;

		const ExportResult archived = Exporter::Export(*Project::GetActive(), output, options);
		REQUIRE_MESSAGE(archived.Success, archived.Error);
		std::vector<std::string> leftOut;
		for (const std::string& warning : archived.Warnings)
			if (Contains(warning, "is left out of the archive"))
				leftOut.push_back(warning);
		std::vector<std::string> expected;
		for (const std::string& entry : NonUtf8Project::Expected())
			expected.push_back("'" + entry + "' is left out of the archive: " + FileSystem::NonUtf8NameProblem);
		CHECK(leftOut == expected);
		std::string archiveError;
		const Ref<PackArchive> archive = PackArchive::Open(output / Project::ArchiveFileName, archiveError);
		REQUIRE_MESSAGE(archive, archiveError);
		std::vector<std::string> keys;
		for (const PackArchive::Entry& entry : archive->GetEntries())
			keys.push_back(entry.Path);
		CHECK(keys == std::vector<std::string>{ "Assets/Good.png", "Assets/Scenes/Main.bscene" });

		// A folder copy keeps the bytes it was given: the names still mean nothing to the game, which validation reports.
		options.Archive = false;
		const ExportResult folder = Exporter::Export(*Project::GetActive(), output, options);
		REQUIRE_MESSAGE(folder.Success, folder.Error);
		CHECK(std::filesystem::is_regular_file(output / "Assets" / NonUtf8Name("Bad", ".png"), error));
		CHECK(std::filesystem::is_regular_file(output / "Assets" / NonUtf8Name("Folder") / "Inner.lua", error));
		std::filesystem::remove_all(output, error);
	}

	TEST_CASE("Commands report what they skipped, and such a document does not block a move")
	{
		NonUtf8Project project("NonUtf8Commands");
		if (!project.Available)
			return;
		CommandRegistry registry;
		AutomationSession session;
		const auto warnings = [](const json& result) { return result.value("warnings", std::vector<std::string>{}); };

		const json list = Run(registry, session, "asset.list");
		CHECK(list["assets"] == json::array({ "Assets/Good.png", "Assets/Scenes/Main.bscene" }));
		std::vector<std::string> expected;
		for (const std::string& entry : NonUtf8Project::Expected())
			expected.push_back("'" + entry + "': " + FileSystem::NonUtf8NameProblem);
		CHECK(warnings(list) == expected);
		// Of a type nothing skipped has, only the folder is news: it could hold anything.
		CHECK(warnings(Run(registry, session, "asset.list", { { "extensions", json::array({ ".md" }) } })) == std::vector<std::string>{ "'" + EscapedNonUtf8Name("Assets/Folder") + "': " + FileSystem::NonUtf8NameProblem });

		const json refresh = Run(registry, session, "asset.refresh");
		CHECK(refresh["changed"].empty());
		CHECK(warnings(refresh) == expected);

		// A key that reads as the name is not that file.
		const std::string readError = BasaltTest::RunError(registry, session, "asset.read", { { "path", "Assets/Bad\xEF\xBF\xBD.png" } });
		CHECK_FALSE(readError.empty());

		const json moved = Run(registry, session, "asset.move", { { "from", "Assets/Good.png" }, { "to", "Assets/Moved.png" } });
		CHECK(FileSystem::Exists(project.GetTemp().GetDirectory() / "Assets/Moved.png"));
		const std::vector<std::string> moveWarnings = warnings(moved);
		REQUIRE(moveWarnings.size() == 2);
		CHECK(Contains(moveWarnings[0], "was not checked for references"));
		CHECK(std::is_sorted(moveWarnings.begin(), moveWarnings.end()));
	}
}
