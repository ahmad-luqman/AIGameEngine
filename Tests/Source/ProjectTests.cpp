#include <doctest/doctest.h>

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/Platform.h>
#include <Basalt/Project/SaveData.h>

#include "TestUtils.h"

#include <cmath>
#include <filesystem>
#include <limits>

using namespace Basalt;

namespace {

	std::string SaveFile(const nlohmann::json& values, int version = SaveData::FormatVersion)
	{
		return nlohmann::json{ { "Format", "BasaltSaveData" }, { "Version", version }, { "Values", values } }.dump();
	}

}

TEST_SUITE("Project")
{
	TEST_CASE("Save data round-trips through its file and rejects malformed text")
	{
		BasaltTest::TempDirectory directory("SaveDataRoundTrip");
		const std::filesystem::path path = directory.GetPath() / "Game" / SaveData::FileName;
		std::string error;

		SaveData data(path);
		// No file yet: empty data, not an error.
		CHECK(data.Load(error));
		CHECK(data.GetKeys().empty());

		CHECK(data.Set("Score", 42, error));
		CHECK(data.Set("Name", "Ada", error));
		CHECK(data.Set("Nested", { { "Level", 3 }, { "Flags", { true, false } } }, error));
		// Only what a save writes back exactly is stored.
		CHECK_FALSE(data.Set("", 1, error));
		CHECK(error == "keys must not be empty");
		CHECK_FALSE(data.Set("NaN", std::nan(""), error));
		CHECK(error.find("numbers must be finite") != std::string::npos);
		CHECK_FALSE(data.Set("Deep", { 1, { { "x", -std::numeric_limits<double>::infinity() } } }, error));
		CHECK_FALSE(data.Set("Bytes", std::string("\xff"), error));
		CHECK(error == "strings must be valid UTF-8 text");
		CHECK_FALSE(data.Set("\xff", 1, error));
		CHECK_FALSE(data.Set("Keys", { { "\xfe", 1 } }, error));
		// Valid UTF-8 beyond ASCII is fine.
		CHECK(data.Set("Text", "Caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac", error));
		// null removes.
		CHECK(data.Set("Temporary", 1, error));
		CHECK(data.Set("Temporary", nullptr, error));
		CHECK(data.Find("Temporary") == nullptr);
		CHECK(data.GetKeys() == std::vector<std::string>{ "Name", "Nested", "Score", "Text" });
		// The directory is created on the first save.
		REQUIRE_MESSAGE(data.Save(error), error);
		CHECK(std::filesystem::exists(path));

		// Saving again replaces the existing file (atomically, on every platform).
		REQUIRE(data.Set("Score", 43, error));
		REQUIRE_MESSAGE(data.Save(error), error);
		REQUIRE(data.Set("Score", 42, error));
		REQUIRE_MESSAGE(data.Save(error), error);
		CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));

		SaveData reloaded(path);
		REQUIRE_MESSAGE(reloaded.Load(error), error);
		REQUIRE(reloaded.Find("Score"));
		CHECK(*reloaded.Find("Score") == 42);
		CHECK(*reloaded.Find("Nested") == nlohmann::json({ { "Level", 3 }, { "Flags", { true, false } } }));
		CHECK(*reloaded.Find("Text") == "Caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac");
		CHECK(reloaded.Remove("Score"));
		CHECK_FALSE(reloaded.Remove("Score"));
		CHECK(reloaded.Find("Score") == nullptr);
		reloaded.Clear();
		CHECK(reloaded.GetKeys().empty());

		const std::pair<const char*, const char*> invalid[] = {
			{ "{", "not valid JSON" },
			{ "[]", "JSON object" },
			{ R"({"Format": "BasaltScene", "Version": 1, "Values": {}})", "'Format'" },
			{ R"({"Format": "BasaltSaveData", "Version": 0, "Values": {}})", "'Version'" },
			{ R"({"Format": "BasaltSaveData", "Version": 99, "Values": {}})", "newer than supported" },
			{ R"({"Format": "BasaltSaveData", "Version": 1, "Values": []})", "'Values'" },
			{ R"({"Format": "BasaltSaveData", "Version": 1})", "'Values'" },
			{ R"({"Format": "BasaltSaveData", "Version": 1, "Values": {"": 1}})", "empty key" },
			{ R"({"Format": "BasaltSaveData", "Version": 1, "Values": {}, "Extra": 1})", "unknown key 'Extra'" },
		};
		for (const auto& [text, expected] : invalid)
		{
			SaveData parsed(path);
			REQUIRE(parsed.Set("Kept", 1, error));
			CHECK_FALSE(parsed.LoadFromString(text, error));
			CHECK_MESSAGE(error.find(expected) != std::string::npos, text, " -> ", error);
			// A failed load keeps the values it had.
			CHECK(parsed.GetKeys() == std::vector<std::string>{ "Kept" });
		}
		SaveData parsed(path);
		// A null value (a hand edit) counts as no value.
		CHECK(parsed.LoadFromString(R"({"Format": "BasaltSaveData", "Version": 1, "Values": {"Coins": 5, "Gone": null}})", error));
		CHECK(*parsed.Find("Coins") == 5);
		CHECK(parsed.Find("Gone") == nullptr);
	}

	TEST_CASE("A malformed save file is moved to a backup that is never overwritten")
	{
		BasaltTest::TempDirectory directory("SaveDataCorrupt");
		const std::filesystem::path path = directory.GetPath() / SaveData::FileName;
		const std::filesystem::path firstBackup = directory.GetPath() / "SaveData.corrupt.json";
		const std::filesystem::path secondBackup = directory.GetPath() / "SaveData.corrupt.2.json";
		REQUIRE(FileSystem::WriteTextFile(path, "{ definitely not json"));

		SaveData data(path);
		std::string error;
		REQUIRE(data.Set("Unsaved", true, error));
		CHECK_FALSE(data.Load(error));
		CHECK(error.find("unreadable") != std::string::npos);
		CHECK(error.find("SaveData.corrupt.json") != std::string::npos);
		// The game starts afresh.
		CHECK(data.GetKeys().empty());
		CHECK_FALSE(std::filesystem::exists(path));
		CHECK(FileSystem::ReadTextFile(firstBackup) == "{ definitely not json");

		// It can save again, and a second damaged file gets its own backup.
		REQUIRE(data.Set("Fresh", true, error));
		CHECK(data.Save(error));
		REQUIRE(FileSystem::WriteTextFile(path, "second damage"));
		CHECK_FALSE(data.Load(error));
		CHECK(FileSystem::ReadTextFile(firstBackup) == "{ definitely not json");
		CHECK(FileSystem::ReadTextFile(secondBackup) == "second damage");

		// Too large counts as malformed too.
		REQUIRE(FileSystem::WriteTextFile(path, std::string(SaveData::MaxFileSize + 1, ' ')));
		CHECK_FALSE(data.Load(error));
		CHECK(error.find("larger than 16 MB") != std::string::npos);
		CHECK(std::filesystem::exists(directory.GetPath() / "SaveData.corrupt.3.json"));
	}

	TEST_CASE("Save data that cannot be read or comes from a newer version is never overwritten")
	{
		BasaltTest::TempDirectory directory("SaveDataProtected");
		const std::filesystem::path path = directory.GetPath() / SaveData::FileName;
		std::string error;

		// A newer version's file stays as it is, and saving is refused until it can be loaded.
		const std::string newer = SaveFile({ { "Score", 99 } }, SaveData::FormatVersion + 1);
		REQUIRE(FileSystem::WriteTextFile(path, newer));
		SaveData data(path);
		CHECK_FALSE(data.Load(error));
		CHECK(error.find("newer version") != std::string::npos);
		REQUIRE(data.Set("Score", 1, error));
		CHECK_FALSE(data.Save(error));
		CHECK(error.find("not saving") != std::string::npos);
		CHECK(FileSystem::ReadTextFile(path) == newer);
		CHECK_FALSE(std::filesystem::exists(directory.GetPath() / "SaveData.corrupt.json"));

		// A newer version is recognized whatever else it changed (here a key this version does not know).
		const std::string newerLayout = R"({"Format": "BasaltSaveData", "Version": 2, "Metadata": {}, "Values": []})";
		REQUIRE(FileSystem::WriteTextFile(path, newerLayout));
		CHECK_FALSE(data.Load(error));
		CHECK(error.find("newer version") != std::string::npos);
		CHECK(FileSystem::ReadTextFile(path) == newerLayout);

		// A successful load unblocks saving.
		REQUIRE(FileSystem::WriteTextFile(path, SaveFile({ { "Score", 5 } })));
		CHECK(data.Load(error));
		CHECK(*data.Find("Score") == 5);
		CHECK(data.Save(error));

		// A path that cannot be read as a file (here a directory; on players' machines a locked or unreadable
		// file): the values are kept and nothing is written over it.
		std::filesystem::remove(path);
		std::filesystem::create_directories(path);
		CHECK_FALSE(data.Load(error));
		CHECK(error.find("cannot read") != std::string::npos);
		CHECK(*data.Find("Score") == 5);
		CHECK_FALSE(data.Save(error));
		CHECK(std::filesystem::is_directory(path));
	}

	TEST_CASE("Save data lives in a per-project directory under the user data directory")
	{
		// TestMain points the user data directory at a scratch directory.
		std::string error;
		const auto base = Platform::GetUserDataDirectory(error);
		REQUIRE_MESSAGE(base.has_value(), error);
		CHECK(*base == std::filesystem::path(*Platform::GetEnvVar(Platform::UserDataDirectoryVariable)));

		// A safe name is used as it is; one that sanitizing changed also gets a hash, so names that sanitize alike
		// keep separate save data.
		const auto plain = SaveData::GetDefaultPath("Tetris 2", error);
		REQUIRE_MESSAGE(plain.has_value(), error);
		CHECK(*plain == *base / "Tetris 2" / SaveData::FileName);
		const auto path = SaveData::GetDefaultPath("My Game: Deluxe", error);
		REQUIRE_MESSAGE(path.has_value(), error);
		const std::string directoryName = path->parent_path().filename().string();
		CHECK(directoryName.rfind("My Game_ Deluxe-", 0) == 0);
		CHECK(directoryName.size() == std::string("My Game_ Deluxe-").size() + 8);
		CHECK(SaveData::GetDefaultPath("My Game: Deluxe", error) == path);
		CHECK(SaveData::GetDefaultPath("Foo/Bar", error) != SaveData::GetDefaultPath("Foo?Bar", error));

		// A set but unusable override never falls back to the real user directory.
		{
			BasaltTest::ScopedEnvVar relative(Platform::UserDataDirectoryVariable, "relative/dir");
			CHECK_FALSE(Platform::GetUserDataDirectory(error));
			CHECK(error.find("BASALT_USER_DATA_DIR is set to 'relative/dir', which is not an absolute path") != std::string::npos);
			CHECK_FALSE(SaveData::GetDefaultPath("Game", error));
		}
		CHECK(Platform::GetUserDataDirectory(error) == base);

		CHECK(SaveData::SanitizeDirectoryName("Tetris") == "Tetris");
		CHECK(SaveData::SanitizeDirectoryName("a/b\\c:d*e?f\"g<h>i|j") == "a_b_c_d_e_f_g_h_i_j");
		CHECK(SaveData::SanitizeDirectoryName("  ..hidden. ") == "hidden");
		CHECK(SaveData::SanitizeDirectoryName("...") == "Untitled");
		CHECK(SaveData::SanitizeDirectoryName("") == "Untitled");
		CHECK(SaveData::SanitizeDirectoryName("..") == "Untitled");
		CHECK(SaveData::SanitizeDirectoryName("con") == "con_");
		CHECK(SaveData::SanitizeDirectoryName("COM0") == "COM0_");
		// The extension does not make a device name safe; the '_' goes after the stem.
		CHECK(SaveData::SanitizeDirectoryName("NUL.txt") == "NUL_.txt");
		CHECK(SaveData::SanitizeDirectoryName("Lpt3.tar.gz") == "Lpt3_.tar.gz");
		CHECK(SaveData::SanitizeDirectoryName("Console") == "Console");
		CHECK(SaveData::SanitizeDirectoryName("Caf\xc3\xa9") == "Caf__");
	}

	TEST_CASE("Save data that cannot be moved aside or saved within the limit is not written")
	{
		BasaltTest::TempDirectory directory("SaveDataLimits");
		const std::filesystem::path path = directory.GetPath() / SaveData::FileName;
		std::string error;

		// Every backup name taken: the malformed file stays, and nothing overwrites it.
		REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "SaveData.corrupt.json", "old"));
		for (int index = 2; index <= 1000; index++)
			REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / ("SaveData.corrupt." + std::to_string(index) + ".json"), "old"));
		REQUIRE(FileSystem::WriteTextFile(path, "damaged"));
		SaveData data(path);
		CHECK_FALSE(data.Load(error));
		CHECK(error.find("all 1000 backup names are taken") != std::string::npos);
		REQUIRE(data.Set("Score", 1, error));
		CHECK_FALSE(data.Save(error));
		CHECK(FileSystem::ReadTextFile(path) == "damaged");

		// More than Load would accept is never written.
		std::filesystem::remove(path);
		CHECK(data.Load(error));
		REQUIRE(data.Set("Big", std::string(SaveData::MaxFileSize, 'x'), error));
		CHECK_FALSE(data.Save(error));
		CHECK(error.find("more than the 16 MB limit") != std::string::npos);
		CHECK_FALSE(std::filesystem::exists(path));
	}

	TEST_CASE("Without an override, the user data directory comes from the platform's variables")
	{
		std::string error;
		BasaltTest::ScopedEnvVar noOverride(Platform::UserDataDirectoryVariable, std::nullopt);
#if defined(BS_PLATFORM_WINDOWS)
		BasaltTest::ScopedEnvVar appData("APPDATA", "C:\\Users\\Player\\AppData\\Roaming");
		CHECK(Platform::GetUserDataDirectory(error) == std::filesystem::path("C:\\Users\\Player\\AppData\\Roaming"));
#else
		BasaltTest::ScopedEnvVar home("HOME", "/home/player");
	#if defined(BS_PLATFORM_MACOS)
		CHECK(Platform::GetUserDataDirectory(error) == std::filesystem::path("/home/player/Library/Application Support"));
	#else
		{
			BasaltTest::ScopedEnvVar xdg("XDG_DATA_HOME", "/data/player");
			CHECK(Platform::GetUserDataDirectory(error) == std::filesystem::path("/data/player"));
		}
		{
			// The XDG spec ignores a relative XDG_DATA_HOME.
			BasaltTest::ScopedEnvVar xdg("XDG_DATA_HOME", "relative");
			CHECK(Platform::GetUserDataDirectory(error) == std::filesystem::path("/home/player/.local/share"));
		}
	#endif
		{
			BasaltTest::ScopedEnvVar relativeHome("HOME", "relative/home");
			BasaltTest::ScopedEnvVar noXdg("XDG_DATA_HOME", std::nullopt);
			CHECK_FALSE(Platform::GetUserDataDirectory(error));
			CHECK(error.find("HOME is set to 'relative/home', which is not an absolute path") != std::string::npos);
		}
#endif
	}
}
