#include <doctest/doctest.h>

#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/AssetRegistry.h>
#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/FileWatcher.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Core/PackArchive.h>
#include <Basalt/Core/Platform.h>
#include <Basalt/Core/TextEncoding.h>
#include <Basalt/Project/Exporter.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Project/ProjectValidator.h>

#include "AutomationTestUtils.h"
#include "TestUtils.h"

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace Basalt;
using BasaltTest::Contains;
using BasaltTest::EscapedNonUtf8Name;
using BasaltTest::NonUtf8Name;
using BasaltTest::Run;
using json = nlohmann::json;
using Entries = std::set<std::string>;

namespace {

	void Write(const std::filesystem::path& path, const std::string& contents)
	{
		REQUIRE(FileSystem::WriteTextFile(path, contents));
	}

	void Rename(const std::filesystem::path& from, const std::filesystem::path& to)
	{
		std::error_code error;
		std::filesystem::rename(from, to, error);
		REQUIRE_MESSAGE(!error, FileSystem::DescribeError(error));
	}

	size_t CountMessages(uint64_t since, std::string_view text)
	{
		uint64_t next = 0;
		const std::vector<LogMessage> messages = Log::GetHistory().GetMessagesSince(since, next);
		return static_cast<size_t>(std::count_if(messages.begin(), messages.end(), [&](const LogMessage& message) { return message.Text.find(text) != std::string::npos; }));
	}

	std::vector<std::string> Keys(const FileWatcher::Snapshot& files)
	{
		std::vector<std::string> keys;
		for (const auto& [key, stamp] : files)
			keys.push_back(key);
		return keys;
	}

	// What asset.list and asset.refresh say about an entry.
	std::string SkippedWarning(const std::string& entry)
	{
		return "'" + entry + "': " + FileSystem::NonUtf8NameProblem;
	}

	// A valid name holding U+FFFD itself: that is not a sign of a bad one.
	const std::string RealReplacementKey = "Assets/Real\xEF\xBF\xBD.png";

	// A project holding keyed files beside every kind of entry no key can hold: a file with a sidecar that would index it,
	// a document that is not even JSON, a folder with a file, a sidecar and a subfolder, two names whose U+FFFD text is the
	// same, a name with (on Windows) the other half of a surrogate pair, a sidecar in capitals, a sidecar without a source,
	// and a folder that only looks like a sidecar, beside a file with the name it would shorten to.
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
			m_Temp.WriteFile(RealReplacementKey, "image");
			m_Temp.WriteFile("Assets/Scenes/Main.bscene", R"({"Entities":[]})");
			const std::filesystem::path assets = m_Temp.GetDirectory() / "Assets";
			Write(assets / NonUtf8Name("Bad", ".png"), "image");
			Write(assets / NonUtf8Name("Bad", ".png.meta"), R"({"Version":1,"UUID":"1234567"})");
			Write(assets / "Scenes" / NonUtf8Name("Broken", ".bscene"), "not a document");
			Write(assets / NonUtf8Name("Folder") / "Inner.lua", "return {}");
			Write(assets / NonUtf8Name("Folder") / "Inner.lua.meta", R"({"Version":1,"UUID":"7654321"})");
			Write(assets / NonUtf8Name("Folder") / "Deep" / "Inner.bscene", "not a document");
			Write(assets / NonUtf8Name("Clash", ".txt", 0), "first");
			Write(assets / NonUtf8Name("Clash", ".txt", 1), "second");
			Write(assets / NonUtf8Name("Low", ".txt", 2), "third");
			Write(assets / NonUtf8Name("Upper", ".png"), "image");
			Write(assets / NonUtf8Name("Upper", ".png.META"), R"({"Version":1,"UUID":"2345678"})");
			Write(assets / NonUtf8Name("Orphan", ".png.meta"), R"({"Version":1,"UUID":"3456789"})");
			Write(assets / NonUtf8Name("Art"), "a file");
			Write(assets / NonUtf8Name("Art", ".meta") / "Inside.png", "image");
		}

		const BasaltTest::TempProject& GetTemp() const { return m_Temp; }
		std::filesystem::path GetAssets() const { return m_Temp.GetDirectory() / "Assets"; }

		// Folders, which every filter reports, as they may hold anything.
		static Entries Folders()
		{
			return { EscapedNonUtf8Name("Assets/Folder"), EscapedNonUtf8Name("Assets/Art", ".meta") };
		}

		static Entries Images()
		{
			return { EscapedNonUtf8Name("Assets/Bad", ".png"), EscapedNonUtf8Name("Assets/Upper", ".png") };
		}

		static Entries Texts()
		{
			return { EscapedNonUtf8Name("Assets/Clash", ".txt", 0), EscapedNonUtf8Name("Assets/Clash", ".txt", 1), EscapedNonUtf8Name("Assets/Low", ".txt", 2) };
		}

		// Every entry skipped, as scans report it. A folder counts once, whatever it holds, a sidecar beside its source as that
		// source, and an orphan and the folder named like a sidecar as themselves.
		static Entries Expected()
		{
			Entries all = Folders();
			all.merge(Images());
			all.merge(Texts());
			all.insert({ EscapedNonUtf8Name("Assets/Scenes/Broken", ".bscene"), EscapedNonUtf8Name("Assets/Orphan", ".png.meta"), EscapedNonUtf8Name("Assets/Art") });
			return all;
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
		CHECK(Utf8SequenceLength("a", 1) == 0);                // Past the end.
		// Cut by the view, with the rest of a valid sequence still in memory behind it.
		CHECK(Utf8SequenceLength(std::string_view("\xC3\xA9", 1), 0) == 0);
		CHECK(Utf8SequenceLength(std::string_view("\xE8\xB3\x87", 2), 0) == 0);
		CHECK(Utf8SequenceLength(std::string_view("\xF0\x9F\x8E\xAE", 3), 0) == 0);

		// The first and last of each range of the standard's table of well-formed sequences.
		for (const char* valid : { "\xC2\x80", "\xDF\xBF", "\xE0\xA0\x80", "\xE0\xBF\xBF", "\xE1\x80\x80", "\xEC\xBF\xBF", "\xED\x80\x80", "\xED\x9F\xBF", "\xEE\x80\x80", "\xEF\xBF\xBF", "\xF0\x90\x80\x80", "\xF0\xBF\xBF\xBF", "\xF1\x80\x80\x80", "\xF3\xBF\xBF\xBF", "\xF4\x80\x80\x80", "\xF4\x8F\xBF\xBF" })
		{
			CAPTURE(valid);
			CHECK(IsValidUtf8(valid));
			CHECK(Utf8SequenceLength(valid, 0) == std::string_view(valid).size());
		}
		for (const char* invalid : {
				 "\xFF",             // Never a UTF-8 byte.
				 "\x80",             // A continuation with nothing before it.
				 "\xC3",             // Cut.
				 "\xE8\xB3",         // Cut.
				 "\xF0\x9F\x8E",     // Cut.
				 "\xC3(",            // A lead without its continuation.
				 "\xE8(\x87",        // The same, one byte on.
				 "\xE8\xB3(",        // The same, at the last byte.
				 "\xF0\x9F\x8E(",    // The same, at the last byte.
				 "\xC0\x80",         // Overlong.
				 "\xC1\xBF",         // Overlong.
				 "\xE0\x80\x80",     // Overlong.
				 "\xE0\x9F\xBF",     // Overlong.
				 "\xF0\x80\x80\x80", // Overlong.
				 "\xF0\x8F\xBF\xBF", // Overlong.
				 "\xED\xA0\x80",     // An encoded surrogate.
				 "\xED\xBF\xBF",     // An encoded surrogate.
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

	TEST_CASE("Every lead and second byte follows the UTF-8 rules, at run time")
	{
		// Built at run time so no compiler can fold them, unlike the literals above. The rule is decoded here, on purpose not as
		// the table the code follows. Later bytes only need each side of the continuation range.
		constexpr std::array<unsigned, 4> Tails = { 0x7F, 0x80, 0xBF, 0xC0 };
		const auto continues = [](unsigned byte) { return (byte & 0xC0) == 0x80; };
		const auto expectedLength = [&](unsigned lead, unsigned second, unsigned third, unsigned fourth) -> size_t {
			if (lead < 0x80)
				return 1;
			if (lead >= 0xC2 && lead < 0xE0)
				return continues(second) ? 2 : 0;
			if (lead >= 0xE0 && lead < 0xF0 && continues(second) && continues(third))
			{
				const unsigned code = ((lead & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F);
				return code >= 0x800 && (code < 0xD800 || code > 0xDFFF) ? 3 : 0;
			}
			if (lead >= 0xF0 && lead < 0xF8 && continues(second) && continues(third) && continues(fourth))
			{
				const unsigned code = ((lead & 0x07) << 18) | ((second & 0x3F) << 12) | ((third & 0x3F) << 6) | (fourth & 0x3F);
				return code >= 0x10000 && code <= 0x10FFFF ? 4 : 0;
			}
			return 0;
		};
		std::string text(4, '\0');
		std::string wrong;
		for (unsigned lead = 0; lead < 256; lead++)
			for (unsigned second = 0; second < 256; second++)
				for (unsigned third : Tails)
					for (unsigned fourth : Tails)
					{
						text[0] = static_cast<char>(lead);
						text[1] = static_cast<char>(second);
						text[2] = static_cast<char>(third);
						text[3] = static_cast<char>(fourth);
						const size_t expected = expectedLength(lead, second, third, fourth);
						const bool valid = expected != 0 && TextEncoding::IsValidUtf8(std::string_view(text).substr(0, expected));
						if ((TextEncoding::Utf8SequenceLength(text, 0) != expected || valid != (expected != 0)) && wrong.empty())
							wrong = TextEncoding::EscapeInvalidUtf8(text);
					}
		CHECK_MESSAGE(wrong.empty(), "first wrong bytes: ", wrong);
	}

	TEST_CASE("UTF-16 surrogates count only in pairs")
	{
		const auto valid = [](std::initializer_list<char16_t> units) { return TextEncoding::IsValidUtf16(std::u16string(units)); };
		const auto escaped = [](std::initializer_list<char16_t> units) { return TextEncoding::EscapeUnpairedSurrogates(std::u16string(units)); };
		CHECK(valid({}));
		CHECK(valid({ u'a', 0xD7FF, 0xE000, 0xFFFD }));
		CHECK(valid({ 0xD83C, 0xDFAE }));
		CHECK(valid({ 0xDBFF, 0xDFFF, u'a' }));
		CHECK_FALSE(valid({ 0xD800 }));                 // A high surrogate at the end.
		CHECK_FALSE(valid({ 0xDC00 }));                 // A low one alone.
		CHECK_FALSE(valid({ 0xDBFF, u'a' }));           // A high one before something else.
		CHECK_FALSE(valid({ 0xD800, 0xD800, 0xDC00 })); // Two highs, of which only the second pairs.
		CHECK_FALSE(valid({ 0xDC00, 0xD800 }));         // In the wrong order.
		CHECK(TextEncoding::Utf16SequenceLength(std::u16string(u"a"), 1) == 0);

		CHECK(escaped({ u'a', 0xD800, u'b' }) == u"a\\uD800b");
		CHECK(escaped({ 0xDFFF }) == u"\\uDFFF");
		CHECK(escaped({ 0xD83C, 0xDFAE }) == std::u16string{ 0xD83C, 0xDFAE });
		CHECK(escaped({ 0xD800, 0xD800, 0xDC00 }) == std::u16string{ u'\\', u'u', u'D', u'8', u'0', u'0', 0xD800, 0xDC00 });
	}

	TEST_CASE("Paths know whether UTF-8 can name them, and always have text for messages")
	{
		CHECK(FileSystem::HasUtf8Form(FileSystem::PathFromUtf8("Assets/資産.png")));
		CHECK(FileSystem::HasUtf8Form(""));
		// U+FFFD itself is a character like any other.
		CHECK(FileSystem::HasUtf8Form(FileSystem::PathFromUtf8(RealReplacementKey)));
		CHECK_FALSE(FileSystem::HasUtf8Form(NonUtf8Name("Bad", ".png")));
		CHECK_FALSE(FileSystem::HasUtf8Form(std::filesystem::path("Assets") / NonUtf8Name("Bad") / "Fine.png"));

		CHECK(FileSystem::DescribePath(FileSystem::PathFromUtf8("Assets/資産.png")) == "Assets/資産.png");
		CHECK(FileSystem::DescribePath(std::filesystem::path("Assets") / NonUtf8Name("Bad", ".png")) == EscapedNonUtf8Name("Assets/Bad", ".png"));
		// Two names one U+FFFD would merge stay apart, and the text is valid wherever it goes.
		CHECK(FileSystem::DescribePath(NonUtf8Name("Clash", "", 0)) != FileSystem::DescribePath(NonUtf8Name("Clash", "", 1)));
		CHECK(TextEncoding::IsValidUtf8(FileSystem::DescribePath(NonUtf8Name("資産"))));

		// An entry needs no file, so this runs where the file system would refuse the name. assign keeps the path of a missing
		// file; libstdc++'s constructor empties it.
		const auto entry = [](const std::filesystem::path& path) {
			std::filesystem::directory_entry result;
			std::error_code error;
			result.assign(path, error);
			return result;
		};
		const std::filesystem::path root = "Project";
		Entries skipped;
		CHECK_FALSE(FileSystem::RecordNonUtf8Entry(entry(root / "Assets" / "Good.png"), root, skipped));
		// Only its own name counts: where the project lies is part of no key.
		CHECK_FALSE(FileSystem::RecordNonUtf8Entry(entry(NonUtf8Name("Folder") / root / "Good.png"), NonUtf8Name("Folder") / root, skipped));
		CHECK(skipped.empty());
		CHECK(FileSystem::RecordNonUtf8Entry(entry(root / "Assets" / NonUtf8Name("Bad", ".png")), root, skipped));
		CHECK(skipped == Entries{ EscapedNonUtf8Name("Assets/Bad", ".png") });
		// With no source beside it, a sidecar is only itself.
		CHECK(FileSystem::RecordNonUtf8Entry(entry(root / "Assets" / NonUtf8Name("Bad", ".png.meta")), root, skipped));
		CHECK(skipped == Entries{ EscapedNonUtf8Name("Assets/Bad", ".png"), EscapedNonUtf8Name("Assets/Bad", ".png.meta") });
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
		// A skipped sidecar neither indexes its file under a key that leads nowhere nor counts as a problem or an orphan, even
		// inside a skipped folder.
		CHECK(registry.GetProblems().empty());
		CHECK(registry.GetOrphans().empty());
		for (const uint64_t id : { 1234567, 2345678, 3456789, 7654321 })
			CHECK(registry.Find(UUID(id)).empty());
		// A keyed file still gets its identity beside them.
		CHECK(registry.Ensure("Assets/Good.png", error).has_value());
		CHECK(registry.Ensure(RealReplacementKey, error).has_value());
		CHECK(registry.GetSkipped() == NonUtf8Project::Expected());

		// Loading says so once per entry, since a watcher's baseline stays silent.
		const uint64_t since = Log::GetHistory().GetTotalCount();
		std::string loadError;
		const Ref<Project> loaded = Project::Load(project.GetTemp().GetDirectory() / Project::FileName, loadError);
		REQUIRE_MESSAGE(loaded, loadError);
		CHECK(CountMessages(since, FileSystem::NonUtf8NameProblem) == NonUtf8Project::Expected().size());
	}

	TEST_CASE("Renaming a skipped asset with its sidecar brings its identity back")
	{
		NonUtf8Project project("NonUtf8Rename");
		if (!project.Available)
			return;
		AssetRegistry registry(project.GetTemp().GetDirectory());
		std::string error;
		const std::filesystem::path assets = project.GetAssets();

		// The source alone: its old sidecar is now an orphan that names itself, and the file is a new asset.
		Rename(assets / NonUtf8Name("Bad", ".png"), assets / "Fixed.png");
		REQUIRE_MESSAGE(registry.Refresh(error), error);
		CHECK(registry.GetSkipped().contains(EscapedNonUtf8Name("Assets/Bad", ".png.meta")));
		CHECK_FALSE(registry.GetSkipped().contains(EscapedNonUtf8Name("Assets/Bad", ".png")));
		CHECK(registry.Find(UUID(1234567)).empty());

		// Then the sidecar: the UUID every reference holds is back, and nothing is left to report.
		Rename(assets / NonUtf8Name("Bad", ".png.meta"), assets / "Fixed.png.meta");
		REQUIRE_MESSAGE(registry.Refresh(error), error);
		CHECK_FALSE(registry.GetSkipped().contains(EscapedNonUtf8Name("Assets/Bad", ".png.meta")));
		CHECK(registry.Find(UUID(1234567)) == "Assets/Fixed.png");

		// A folder's contents come back the same way.
		Rename(assets / NonUtf8Name("Folder"), assets / "Folder");
		REQUIRE_MESSAGE(registry.Refresh(error), error);
		CHECK(registry.Find(UUID(7654321)) == "Assets/Folder/Inner.lua");
	}

	TEST_CASE("The file watcher leaves such names out without giving up on the folder")
	{
		NonUtf8Project project("NonUtf8Watcher");
		if (!project.Available)
			return;
		const std::filesystem::path root = project.GetTemp().GetDirectory();
		const std::filesystem::path assets = project.GetAssets();
		const FileWatcher::Scan scan = FileWatcher::ScanDirectory(root, "Assets");
		CHECK(scan.Error.empty());
		CHECK(scan.Skipped == NonUtf8Project::Expected());
		CHECK(Keys(scan.Files) == std::vector<std::string>{ "Assets/Good.png", RealReplacementKey, "Assets/Scenes/Main.bscene" });

		const uint64_t since = Log::GetHistory().GetTotalCount();
		FileWatcher watcher;
		watcher.Watch(root);
		// The baseline is not reported, as nothing else in it is, but it is known.
		CHECK(watcher.GetSkipped() == NonUtf8Project::Expected());
		CHECK(watcher.Refresh().empty());
		CHECK(CountMessages(since, FileSystem::NonUtf8NameProblem) == 0);

		// One more: logged once, however many scans find it, and the good file written with it is still reported.
		const std::filesystem::path later = assets / NonUtf8Name("Later", ".png");
		const std::string laterEntry = EscapedNonUtf8Name("Assets/Later", ".png");
		Write(later, "image");
		Write(assets / "Beside.png", "image");
		CHECK(watcher.Refresh() == std::vector<FileChange>{ { "Assets/Beside.png", FileChangeType::Added } });
		CHECK(watcher.GetLastError().empty());
		Entries withLater = NonUtf8Project::Expected();
		withLater.insert(laterEntry);
		CHECK(watcher.GetSkipped() == withLater);
		CHECK(watcher.Refresh().empty());
		CHECK(CountMessages(since, laterEntry) == 1);

		// Gone, then back: that is news again.
		std::error_code error;
		std::filesystem::remove(later, error);
		REQUIRE_FALSE(error);
		CHECK(watcher.Refresh().empty());
		CHECK(watcher.GetSkipped() == NonUtf8Project::Expected());
		Write(later, "image");
		CHECK(watcher.Refresh().empty());
		CHECK(CountMessages(since, laterEntry) == 2);

		// A key that loses its name is a removal, and one that gains a good one an addition; so are a folder's files.
		Rename(assets / "Beside.png", assets / NonUtf8Name("Beside", ".png"));
		CHECK(watcher.Refresh() == std::vector<FileChange>{ { "Assets/Beside.png", FileChangeType::Removed } });
		CHECK(watcher.GetSkipped().contains(EscapedNonUtf8Name("Assets/Beside", ".png")));
		Rename(assets / NonUtf8Name("Beside", ".png"), assets / "Beside.png");
		CHECK(watcher.Refresh() == std::vector<FileChange>{ { "Assets/Beside.png", FileChangeType::Added } });
		Rename(assets / NonUtf8Name("Folder"), assets / "Folder");
		const std::vector<FileChange> folder = watcher.Refresh();
		CHECK(folder == std::vector<FileChange>{ { "Assets/Folder/Deep/Inner.bscene", FileChangeType::Added }, { "Assets/Folder/Inner.lua", FileChangeType::Added }, { "Assets/Folder/Inner.lua.meta", FileChangeType::Added } });
	}

#if !defined(_WIN32)
	namespace {

		// Locks a folder for its scope, so that a failed check cannot leave it behind.
		class LockedFolder
		{
		public:
			explicit LockedFolder(std::filesystem::path path)
				: m_Path(std::move(path))
			{
				std::filesystem::permissions(m_Path, std::filesystem::perms::none);
			}
			~LockedFolder() { Unlock(); }
			LockedFolder(const LockedFolder&) = delete;
			LockedFolder& operator=(const LockedFolder&) = delete;

			void Unlock() const
			{
				std::error_code error;
				std::filesystem::permissions(m_Path, std::filesystem::perms::owner_all, error);
			}
			// Running as root reads it anyway: nothing to test then.
			bool Holds() const
			{
				std::error_code probe;
				const std::filesystem::directory_iterator unreadable(m_Path, probe);
				if (!probe)
					MESSAGE("a locked folder can be read here (root?), so the test has nothing to do");
				return static_cast<bool>(probe);
			}

		private:
			std::filesystem::path m_Path;
		};

	}

	TEST_CASE("A scan that cannot finish neither reports nor forgets skipped names")
	{
		NonUtf8Project project("NonUtf8PartialScan");
		if (!project.Available)
			return;
		FileWatcher watcher;
		watcher.Watch(project.GetTemp().GetDirectory());
		LockedFolder locked(project.GetAssets() / "Scenes");
		if (!locked.Holds())
			return;
		const uint64_t since = Log::GetHistory().GetTotalCount();
		const std::string later = EscapedNonUtf8Name("Assets/Later", ".png");
		Write(project.GetAssets() / NonUtf8Name("Later", ".png"), "image");
		CHECK(watcher.Refresh().empty());
		CHECK_FALSE(watcher.GetLastError().empty());
		// The entries of a partial scan say nothing: the unreadable folder's own entry would look gone.
		CHECK(watcher.GetSkipped() == NonUtf8Project::Expected());
		CHECK(CountMessages(since, later) == 0);

		locked.Unlock();
		CHECK(watcher.Refresh().empty());
		CHECK(watcher.GetLastError().empty());
		CHECK(watcher.GetSkipped().contains(later));
		CHECK(CountMessages(since, later) == 1);
	}

	TEST_CASE("A late baseline reports what it skipped, as the first scan the watcher could read")
	{
		NonUtf8Project project("NonUtf8LateBaseline");
		if (!project.Available)
			return;
		LockedFolder locked(project.GetAssets() / "Scenes");
		if (!locked.Holds())
			return;
		const uint64_t since = Log::GetHistory().GetTotalCount();
		FileWatcher watcher;
		watcher.Watch(project.GetTemp().GetDirectory());
		CHECK(watcher.GetSkipped().empty());
		locked.Unlock();
		CHECK(watcher.Refresh().empty());
		CHECK(watcher.TakeLateBaselineNotice());
		CHECK(watcher.GetSkipped() == NonUtf8Project::Expected());
		CHECK(CountMessages(since, FileSystem::NonUtf8NameProblem) == NonUtf8Project::Expected().size());
	}
#endif

	TEST_CASE("Asset lists leave such names out and say which, for the types asked")
	{
		NonUtf8Project project("NonUtf8Lists");
		if (!project.Available)
			return;
		std::string error;
		Entries skipped;
		const std::vector<std::string> all = AssetManager::ListAssets({}, &error, &skipped);
		CHECK(error.empty());
		CHECK(all == std::vector<std::string>{ "Assets/Good.png", RealReplacementKey, "Assets/Scenes/Main.bscene" });
		CHECK(skipped == NonUtf8Project::Expected());

		// The folders could hold anything; what is not of the types asked for is not reported, an orphan sidecar included, and
		// a file's type is its extension in any case.
		Write(project.GetAssets() / NonUtf8Name("Shout", ".PNG"), "image");
		CHECK(AssetManager::ListAssets({ ".bscene" }, &error, &skipped) == std::vector<std::string>{ "Assets/Scenes/Main.bscene" });
		CHECK(error.empty());
		Entries expected = NonUtf8Project::Folders();
		expected.insert(EscapedNonUtf8Name("Assets/Scenes/Broken", ".bscene"));
		CHECK(skipped == expected);

		CHECK(AssetManager::ListAssets({ ".txt" }, &error, &skipped).empty());
		CHECK(error.empty());
		expected = NonUtf8Project::Folders();
		expected.merge(NonUtf8Project::Texts());
		CHECK(skipped == expected);

		CHECK(AssetManager::ListAssets({ ".png" }, &error, &skipped).size() == 2);
		expected = NonUtf8Project::Folders();
		expected.merge(NonUtf8Project::Images());
		expected.insert(EscapedNonUtf8Name("Assets/Shout", ".PNG"));
		CHECK(skipped == expected);
		// The list can be had without them.
		CHECK(AssetManager::ListAssets({ ".bscene" }).size() == 1);

		// What it held before never survives a call, even one that finds no folder.
		std::error_code removeError;
		std::filesystem::remove_all(project.GetAssets(), removeError);
		REQUIRE_FALSE(removeError);
		CHECK(AssetManager::ListAssets({}, &error, &skipped).empty());
		CHECK_FALSE(error.empty());
		CHECK(skipped.empty());
	}

	TEST_CASE("A folder listing shows folders, then files, without sidecars or such names")
	{
		NonUtf8Project project("NonUtf8Folder");
		if (!project.Available)
			return;
		project.GetTemp().WriteFile("Assets/Good.png.meta", R"({"Version":1,"UUID":"99"})");
		// Sidecars are files: a folder of that name is shown.
		project.GetTemp().WriteFile("Assets/Shown.meta/Inside.png", "image");
		const AssetManager::FolderListing listing = AssetManager::ListFolder(project.GetAssets());
		std::vector<std::string> names;
		names.reserve(listing.Entries.size());
		for (const std::filesystem::directory_entry& entry : listing.Entries)
			names.push_back(FileSystem::PathToUtf8(entry.path().filename()));
		CHECK(listing.Error.empty());
		// After the folders every project starts with.
		REQUIRE(names.size() >= 4);
		const auto files = names.end() - 2;
		CHECK(std::is_sorted(names.begin(), files));
		CHECK(std::find(names.begin(), files, "Scenes") != files);
		CHECK(std::find(names.begin(), files, "Shown.meta") != files);
		CHECK(std::vector<std::string>(files, names.end()) == std::vector<std::string>{ "Good.png", "Real\xEF\xBF\xBD.png" });
		// Project-relative, as every other report, and only what this folder holds.
		Entries expected = NonUtf8Project::Expected();
		expected.erase(EscapedNonUtf8Name("Assets/Scenes/Broken", ".bscene"));
		CHECK(listing.Skipped == expected);
		CHECK(AssetManager::ListFolder(project.GetAssets() / "Scenes").Skipped == Entries{ EscapedNonUtf8Name("Assets/Scenes/Broken", ".bscene") });

		const AssetManager::FolderListing missing = AssetManager::ListFolder(project.GetAssets() / "Nothing");
		CHECK(missing.Entries.empty());
		CHECK_FALSE(missing.Error.empty());
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
		CHECK(FileSystem::FindExactCase(directory, FileSystem::PathFromUtf8(RealReplacementKey), actual, problem) == FileSystem::PathMatch::Exact);
		CHECK(actual == RealReplacementKey);
#if !defined(_WIN32)
		// Here the bytes themselves could match, were they listed.
		CHECK(FileSystem::FindExactCase(directory, std::filesystem::path("Assets") / NonUtf8Name("Bad", ".png"), actual, problem) == FileSystem::PathMatch::Missing);
#endif
	}

	TEST_CASE("Validation warns about each such name without failing the project")
	{
		NonUtf8Project project("NonUtf8Validation");
		if (!project.Available)
			return;
		Project::GetActive()->GetConfig().StartScene = "Assets/Scenes/Main.bscene";
		const ValidationReport report = ValidateProject(*Project::GetActive());
		Entries warned;
		for (const ValidationIssue& issue : report.GetIssues())
			if (Contains(issue.Message, FileSystem::NonUtf8NameProblem))
			{
				CHECK(issue.Severity == ValidationSeverity::Warning);
				// Text that names no file stays out of the field agents read a path from.
				CHECK(issue.File == Project::AssetDirectoryName);
				warned.insert(issue.Message);
			}
		Entries expected;
		for (const std::string& entry : NonUtf8Project::Expected())
			expected.insert(SkippedWarning(entry));
		CHECK(warned == expected);
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
		const BasaltTest::TempDirectory directory("NonUtf8ExportOut");
		const std::filesystem::path output = directory.GetPath() / "Build";
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
		// Here a raw string could have opened one from a folder, so the warnings say what an archive changes.
		const bool warnsAboutRawBytes = std::any_of(archived.Warnings.begin(), archived.Warnings.end(), [](const std::string& warning) { return Contains(warning, "raw bytes"); });
#if defined(_WIN32)
		CHECK_FALSE(warnsAboutRawBytes);
#else
		CHECK(warnsAboutRawBytes);
#endif
		{
			// Closed before the next export replaces the file: Windows cannot delete an open one.
			std::string archiveError;
			const Ref<PackArchive> archive = PackArchive::Open(output / Project::ArchiveFileName, archiveError);
			REQUIRE_MESSAGE(archive, archiveError);
			std::vector<std::string> keys;
			for (const PackArchive::Entry& entry : archive->GetEntries())
				keys.push_back(entry.Path);
			CHECK(keys == std::vector<std::string>{ "Assets/Good.png", RealReplacementKey, "Assets/Scenes/Main.bscene" });
		}

		// A folder copy keeps the bytes it was given, and says nothing itself: validation reports the names.
		options.Archive = false;
		const ExportResult folder = Exporter::Export(*Project::GetActive(), output, options);
		REQUIRE_MESSAGE(folder.Success, folder.Error);
		CHECK(std::none_of(folder.Warnings.begin(), folder.Warnings.end(), [](const std::string& warning) { return Contains(warning, FileSystem::NonUtf8NameProblem); }));
		std::error_code error;
		for (const std::filesystem::path& copied : { NonUtf8Name("Bad", ".png"), NonUtf8Name("Bad", ".png.meta"), NonUtf8Name("Clash", ".txt", 0), NonUtf8Name("Clash", ".txt", 1), NonUtf8Name("Folder") / "Inner.lua" })
			CHECK(std::filesystem::is_regular_file(output / "Assets" / copied, error));

		CommandRegistry registry;
		AutomationSession session;
		const json result = Run(registry, session, "project.export", { { "output", FileSystem::PathToUtf8(output) } });
		CHECK(result.at("validation").at("warnings").get<size_t>() >= NonUtf8Project::Expected().size());
	}

	TEST_CASE("Links with such names are skipped before anything follows them")
	{
		NonUtf8Project project("NonUtf8Links");
		if (!project.Available)
			return;
		const BasaltTest::TempProject& temp = project.GetTemp();
		const std::filesystem::path assets = project.GetAssets();
		std::error_code linkError;
		std::filesystem::create_directory_symlink(assets / "Scenes", assets / NonUtf8Name("LinkedFolder"), linkError);
		if (linkError)
		{
			// A developer's Windows may not allow links; CI must, or this case would quietly stop running there.
			REQUIRE_MESSAGE(!Platform::GetEnvVar("CI"), "links are needed here: ", FileSystem::DescribeError(linkError));
			WARN_MESSAGE(false, "Symlink creation unavailable: ", FileSystem::DescribeError(linkError));
			return;
		}
		std::filesystem::create_symlink(assets / "Good.png", assets / NonUtf8Name("Linked", ".png"), linkError);
		REQUIRE_FALSE(linkError);
		std::filesystem::create_symlink(assets / "Nothing.png", assets / NonUtf8Name("Dangling", ".png"), linkError);
		REQUIRE_FALSE(linkError);
		// A link to itself has a type no call can read.
		std::filesystem::create_symlink(assets / NonUtf8Name("Loop", ".png"), assets / NonUtf8Name("Loop", ".png"), linkError);
		REQUIRE_FALSE(linkError);

		Entries expected = NonUtf8Project::Expected();
		expected.insert({ EscapedNonUtf8Name("Assets/LinkedFolder"), EscapedNonUtf8Name("Assets/Linked", ".png"), EscapedNonUtf8Name("Assets/Dangling", ".png"), EscapedNonUtf8Name("Assets/Loop", ".png") });
		CHECK(FileWatcher::ScanDirectory(temp.GetDirectory(), "Assets").Skipped == expected);
		AssetRegistry registry(temp.GetDirectory());
		std::string error;
		REQUIRE_MESSAGE(registry.Refresh(error), error);
		CHECK(registry.GetSkipped() == expected);
		// A link to a folder, or one whose type cannot be read, may hold anything; a link to nothing is a file of its name's
		// type.
		Entries skipped;
		CHECK(AssetManager::ListAssets({ ".lua" }, &error, &skipped).empty());
		CHECK(error.empty());
		Entries folders = NonUtf8Project::Folders();
		folders.insert({ EscapedNonUtf8Name("Assets/LinkedFolder"), EscapedNonUtf8Name("Assets/Loop", ".png") });
		CHECK(skipped == folders);

		// A linked folder or a broken link would stop an archive, were they not skipped first.
		temp.WriteFile("FakeRuntime", "binary");
		const BasaltTest::ScopedFakeVulkanSdk sdk(temp);
		Project::GetActive()->GetConfig().StartScene = "Assets/Scenes/Main.bscene";
		const BasaltTest::TempDirectory output("NonUtf8LinksOut");
		ExportOptions options;
		options.RuntimeExecutable = temp.GetDirectory() / "FakeRuntime";
		options.Archive = true;
		const ExportResult result = Exporter::Export(*Project::GetActive(), output.GetPath() / "Build", options);
		CHECK_MESSAGE(result.Success, result.Error);
		// A folder copy follows links, whatever their names, so a broken one stops it.
		options.Archive = false;
		CHECK_FALSE(Exporter::Export(*Project::GetActive(), output.GetPath() / "Build", options).Success);
	}

	TEST_CASE("Links to nothing do not stop an asset list, and an unreadable one does not stop a folder listing")
	{
		BasaltTest::TempProject temp("BrokenLinkLists");
		REQUIRE(temp.IsValid());
		const std::filesystem::path assets = temp.GetDirectory() / "Assets";
		temp.WriteFile("Assets/Good.png", "image");
		std::error_code linkError;
		std::filesystem::create_symlink(assets / "Nothing.png", assets / "Dangling.png", linkError);
		if (linkError)
		{
			REQUIRE_MESSAGE(!Platform::GetEnvVar("CI"), "links are needed here: ", FileSystem::DescribeError(linkError));
			WARN_MESSAGE(false, "Symlink creation unavailable: ", FileSystem::DescribeError(linkError));
			return;
		}
		std::string error;
		CHECK(AssetManager::ListAssets({ ".png" }, &error) == std::vector<std::string>{ "Assets/Good.png" });
		CHECK(error.empty());

		// A link to itself has a type no call can read: an asset list stops there and says where, and a folder listing shows it.
		std::filesystem::create_symlink(assets / "Loop.png", assets / "Loop.png", linkError);
		REQUIRE_FALSE(linkError);
		const std::vector<std::string> partial = AssetManager::ListAssets({ ".png" }, &error);
		CHECK(std::find(partial.begin(), partial.end(), "Assets/Loop.png") == partial.end());
		CHECK(Contains(error, "Assets/Loop.png"));
		AssetManager::FolderListing listing;
		REQUIRE_NOTHROW(listing = AssetManager::ListFolder(assets));
		CHECK(listing.Error.empty());
		CHECK(std::any_of(listing.Entries.begin(), listing.Entries.end(), [](const std::filesystem::directory_entry& entry) { return entry.path().filename() == "Loop.png"; }));
	}

	TEST_CASE("asset.list says when it could not list everything")
	{
		BasaltTest::TempProject temp("AssetListNoFolder");
		REQUIRE(temp.IsValid());
		CommandRegistry registry;
		AutomationSession session;
		std::error_code error;
		std::filesystem::remove_all(temp.GetDirectory() / "Assets", error);
		REQUIRE_FALSE(error);
		const json list = Run(registry, session, "asset.list");
		CHECK(list.at("assets").empty());
		REQUIRE(list.contains("warnings"));
		CHECK(Contains(list.at("warnings").at(0).get<std::string>(), "cannot scan Assets/"));
	}

	TEST_CASE("A project under a folder with such a name keeps its keys")
	{
		BasaltTest::TempProject temp("NonUtf8Parent");
		REQUIRE(temp.IsValid());
		if (!BasaltTest::CanCreateNonUtf8Names(temp.GetDirectory()))
			return;
		const std::filesystem::path directory = temp.GetDirectory() / NonUtf8Name("Parent") / "Game";
		std::string error;
		const Ref<Project> project = Project::Create(directory, "Game", error);
		REQUIRE_MESSAGE(project, error);
		Project::SetActive(project);
		Write(directory / "Assets/Good.png", "image");

		const FileWatcher::Scan scan = FileWatcher::ScanDirectory(directory, "Assets");
		CHECK(scan.Skipped.empty());
		CHECK(scan.Files.contains("Assets/Good.png"));
		AssetRegistry registry(directory);
		REQUIRE_MESSAGE(registry.Refresh(error), error);
		CHECK(registry.GetSkipped().empty());
		Entries skipped;
		CHECK(AssetManager::ListAssets({ ".png" }, &error, &skipped) == std::vector<std::string>{ "Assets/Good.png" });
		CHECK(skipped.empty());
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
		CHECK(list["assets"] == json::array({ "Assets/Good.png", RealReplacementKey, "Assets/Scenes/Main.bscene" }));
		std::vector<std::string> expected;
		for (const std::string& entry : NonUtf8Project::Expected())
			expected.push_back(SkippedWarning(entry));
		CHECK(warnings(list) == expected);
		// Of a type nothing skipped has, only the folders are news: they could hold anything.
		std::vector<std::string> folders;
		for (const std::string& entry : NonUtf8Project::Folders())
			folders.push_back(SkippedWarning(entry));
		CHECK(warnings(Run(registry, session, "asset.list", { { "extensions", json::array({ ".md" }) } })) == folders);

		json refresh = Run(registry, session, "asset.refresh");
		CHECK(refresh.at("changed").empty());
		CHECK(warnings(refresh) == expected);

		// Refreshes follow what is there now.
		std::error_code error;
		std::filesystem::remove(project.GetAssets() / NonUtf8Name("Low", ".txt", 2), error);
		REQUIRE_FALSE(error);
		refresh = Run(registry, session, "asset.refresh");
		std::erase_if(expected, [](const std::string& warning) { return Contains(warning, "Low"); });
		CHECK(warnings(refresh) == expected);

		const json moved = Run(registry, session, "asset.move", { { "from", "Assets/Good.png" }, { "to", "Assets/Moved.png" } });
		CHECK(FileSystem::Exists(project.GetTemp().GetDirectory() / "Assets/Moved.png"));
		Entries documents = NonUtf8Project::Folders();
		documents.insert(EscapedNonUtf8Name("Assets/Scenes/Broken", ".bscene"));
		std::vector<std::string> expectedMove;
		for (const std::string& entry : documents)
			expectedMove.push_back("'" + entry + "' was not checked for references to this asset: " + FileSystem::NonUtf8NameProblem);
		CHECK(warnings(moved) == expectedMove);
	}
}
