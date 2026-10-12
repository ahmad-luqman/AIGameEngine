#include <doctest/doctest.h>

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/Platform.h>
#include <Basalt/Project/SaveData.h>

#include "TestUtils.h"

#include <filesystem>

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

		CHECK(data.Set("Score", 42));
		CHECK(data.Set("Name", "Ada"));
		CHECK(data.Set("Nested", { { "Level", 3 }, { "Flags", { true, false } } }));
		CHECK_FALSE(data.Set("", 1));
		CHECK(data.GetKeys() == std::vector<std::string>{ "Name", "Nested", "Score" });
		// The directory is created on the first save.
		REQUIRE_MESSAGE(data.Save(error), error);
		CHECK(std::filesystem::exists(path));

		SaveData reloaded(path);
		REQUIRE_MESSAGE(reloaded.Load(error), error);
		REQUIRE(reloaded.Find("Score"));
		CHECK(*reloaded.Find("Score") == 42);
		CHECK(*reloaded.Find("Nested") == nlohmann::json({ { "Level", 3 }, { "Flags", { true, false } } }));
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
			parsed.Set("Kept", 1);
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
		data.Set("Unsaved", true);
		std::string error;
		CHECK_FALSE(data.Load(error));
		CHECK(error.find("unreadable") != std::string::npos);
		CHECK(error.find("SaveData.corrupt.json") != std::string::npos);
		// The game starts afresh.
		CHECK(data.GetKeys().empty());
		CHECK_FALSE(std::filesystem::exists(path));
		CHECK(FileSystem::ReadTextFile(firstBackup) == "{ definitely not json");

		// It can save again, and a second damaged file gets its own backup.
		data.Set("Fresh", true);
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
		data.Set("Score", 1);
		CHECK_FALSE(data.Save(error));
		CHECK(error.find("not saving") != std::string::npos);
		CHECK(FileSystem::ReadTextFile(path) == newer);
		CHECK_FALSE(std::filesystem::exists(directory.GetPath() / "SaveData.corrupt.json"));

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

		const auto path = SaveData::GetDefaultPath("My Game: Deluxe", error);
		REQUIRE_MESSAGE(path.has_value(), error);
		CHECK(*path == *base / "My Game_ Deluxe" / SaveData::FileName);

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
}
