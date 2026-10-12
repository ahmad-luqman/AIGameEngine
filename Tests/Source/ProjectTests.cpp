#include <doctest/doctest.h>

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/Platform.h>
#include <Basalt/Project/SaveData.h>

#include "TestUtils.h"

#include <filesystem>

using namespace Basalt;

namespace {

	std::filesystem::path ScratchDirectory(const std::string& name)
	{
		const std::filesystem::path directory = std::filesystem::temp_directory_path() / "BasaltTests" / name;
		std::filesystem::remove_all(directory);
		return directory;
	}

}

TEST_SUITE("Project")
{
	TEST_CASE("Save data round-trips through its file and rejects malformed files")
	{
		const std::filesystem::path directory = ScratchDirectory("SaveDataRoundTrip");
		const std::filesystem::path path = directory / "Game" / SaveData::FileName;
		std::string error;

		SaveData data(path);
		// No file yet: empty data, not an error.
		CHECK(data.Load(error));
		CHECK(data.GetKeys().empty());

		data.Set("Score", 42);
		data.Set("Name", "Ada");
		data.Set("Nested", { { "Level", 3 }, { "Flags", { true, false } } });
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

		// Lua strings are bytes: invalid UTF-8 is replaced instead of failing the save.
		data.Set("Bytes", std::string("\xff\xfe"));
		CHECK(data.Save(error));

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
			parsed.Set("Stale", 1);
			CHECK_FALSE(parsed.LoadFromString(text, error));
			CHECK_MESSAGE(error.find(expected) != std::string::npos, text, " -> ", error);
			// A failed load never keeps earlier values around.
			CHECK(parsed.GetKeys().empty());
		}
		SaveData parsed(path);
		CHECK(parsed.LoadFromString(R"({"Format": "BasaltSaveData", "Version": 1, "Values": {"Coins": 5}})", error));
		CHECK(*parsed.Find("Coins") == 5);
		std::filesystem::remove_all(directory);
	}

	TEST_CASE("A corrupt save file is moved aside so the next save cannot destroy it")
	{
		const std::filesystem::path directory = ScratchDirectory("SaveDataCorrupt");
		const std::filesystem::path path = directory / SaveData::FileName;
		REQUIRE(FileSystem::WriteTextFile(path, "{ definitely not json"));

		SaveData data(path);
		std::string error;
		CHECK_FALSE(data.Load(error));
		CHECK(error.find("unreadable") != std::string::npos);
		CHECK(error.find(SaveData::CorruptFileName) != std::string::npos);
		CHECK(data.GetKeys().empty());
		CHECK_FALSE(std::filesystem::exists(path));
		CHECK(FileSystem::ReadTextFile(directory / SaveData::CorruptFileName) == "{ definitely not json");

		// The game keeps going with empty data and can save again.
		data.Set("Fresh", true);
		CHECK(data.Save(error));
		CHECK(data.Load(error));
		CHECK(*data.Find("Fresh") == true);
		std::filesystem::remove_all(directory);
	}

	TEST_CASE("Save data lives in a per-project directory under the user data directory")
	{
		// TestMain points the user data directory at a scratch directory.
		const auto base = Platform::GetUserDataDirectory();
		REQUIRE(base.has_value());
		CHECK(base->is_absolute());
		CHECK(base->filename() == "UserData");

		std::string error;
		const auto path = SaveData::GetDefaultPath("My Game: Deluxe", error);
		REQUIRE_MESSAGE(path.has_value(), error);
		CHECK(*path == *base / "My Game_ Deluxe" / SaveData::FileName);

		CHECK(SaveData::SanitizeDirectoryName("Tetris") == "Tetris");
		CHECK(SaveData::SanitizeDirectoryName("a/b\\c:d*e?f\"g<h>i|j") == "a_b_c_d_e_f_g_h_i_j");
		CHECK(SaveData::SanitizeDirectoryName("  ..hidden. ") == "hidden");
		CHECK(SaveData::SanitizeDirectoryName("...") == "Untitled");
		CHECK(SaveData::SanitizeDirectoryName("") == "Untitled");
		CHECK(SaveData::SanitizeDirectoryName("..") == "Untitled");
		CHECK(SaveData::SanitizeDirectoryName("con") == "con_");
		CHECK(SaveData::SanitizeDirectoryName("NUL.txt") == "NUL.txt_");
		CHECK(SaveData::SanitizeDirectoryName("Console") == "Console");
		CHECK(SaveData::SanitizeDirectoryName("Caf\xc3\xa9") == "Caf__");
	}
}
