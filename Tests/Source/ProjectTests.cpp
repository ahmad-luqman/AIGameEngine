#include <doctest/doctest.h>

#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/Platform.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Project/ProjectValidator.h>
#include <Basalt/Project/SaveData.h>
#include <Basalt/Project/SmokeTest.h>
#include <Basalt/Scene/Components.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/SceneSerializer.h>

#include "TestUtils.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>

#if !defined(_WIN32)
	#include <unistd.h>
#endif

using namespace Basalt;

namespace {

	nlohmann::json RunCommand(const CommandRegistry& registry, AutomationSession& session, const std::string& command, const nlohmann::json& params = nlohmann::json::object())
	{
		const nlohmann::json response = registry.HandleRequest(session, { { "command", command }, { "params", params } });
		INFO(command << ": " << response.dump());
		REQUIRE(response["ok"] == true);
		return response["result"];
	}

	std::string RunCommandError(const CommandRegistry& registry, AutomationSession& session, const std::string& command, const nlohmann::json& params = nlohmann::json::object())
	{
		const nlohmann::json response = registry.HandleRequest(session, { { "command", command }, { "params", params } });
		REQUIRE(response["ok"] == false);
		return response["error"].get<std::string>();
	}

	// The validation messages (file: message) of one severity.
	std::vector<std::string> Messages(const ValidationReport& report, ValidationSeverity severity)
	{
		std::vector<std::string> messages;
		for (const ValidationIssue& issue : report.GetIssues())
			if (issue.Severity == severity)
				messages.push_back(issue.File + ": " + issue.Message);
		return messages;
	}

	bool Contains(const std::vector<std::string>& messages, const std::string& text)
	{
		return std::any_of(messages.begin(), messages.end(), [&](const std::string& message) { return message.find(text) != std::string::npos; });
	}

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

	TEST_CASE("The project's fixed timestep is the default physics step, which scenes may override")
	{
		BasaltTest::TempProject temp("FixedTimestep");
		REQUIRE(temp.IsValid());
		const Ref<Project>& project = Project::GetActive();
		CHECK(project->GetConfig().FixedTimestep == DefaultFixedTimestep);

		Scene scene;
		CHECK(scene.GetFixedTimestep() == DefaultFixedTimestep);
		project->GetConfig().FixedTimestep = 0.01f;
		CHECK(scene.GetFixedTimestep() == 0.01f);
		// A session keeps the step it started with, as it keeps the seed.
		scene.OnSimulationStart();
		project->GetConfig().FixedTimestep = 0.02f;
		CHECK(scene.GetFixedTimestep() == 0.01f);
		scene.OnSimulationStop();
		CHECK(scene.GetFixedTimestep() == 0.02f);
		// The scene's own value wins.
		scene.GetPhysicsSettings().FixedTimestep = 0.005f;
		CHECK(scene.GetFixedTimestep() == 0.005f);

		// The project file keeps it; bad values are rejected on load.
		std::string error;
		REQUIRE(project->Save(error));
		Ref<Project> reloaded = Project::Load(temp.GetDirectory(), error);
		REQUIRE_MESSAGE(reloaded, error);
		CHECK(reloaded->GetConfig().FixedTimestep == 0.02f);
		for (const char* bad : { "0", "-0.01", "0.3", "\"fast\"" })
		{
			temp.WriteFile(Project::FileName, std::string(R"({"Format": "BasaltProject", "FixedTimestep": )") + bad + "}");
			CHECK_FALSE(Project::Load(temp.GetDirectory(), error));
			CHECK(error.find("FixedTimestep") != std::string::npos);
		}
		// Window sizes the runtime cannot open are rejected too (a negative one would wrap around).
		for (const char* bad : { R"("WindowWidth": -1)", R"("WindowWidth": 20)", R"("WindowHeight": 20000)", R"("WindowHeight": "tall")" })
		{
			temp.WriteFile(Project::FileName, std::string(R"({"Format": "BasaltProject", )") + bad + "}");
			CHECK_FALSE(Project::Load(temp.GetDirectory(), error));
			CHECK(error.find("must be an integer in [64, 16384]") != std::string::npos);
		}
	}

	TEST_CASE("Scenes store the fixed timestep only as an override, and null removes it")
	{
		BasaltTest::TempProject temp("FixedTimestepOverride");
		REQUIRE(temp.IsValid());
		Scene scene;
		CHECK_FALSE(SceneSerializer::SerializeScene(scene)["Physics"].contains("FixedTimestep"));
		scene.GetPhysicsSettings().FixedTimestep = 0.01f;
		CHECK(SceneSerializer::SerializeScene(scene)["Physics"]["FixedTimestep"] == 0.01f);

		std::string error;
		CHECK(SceneSerializer::DeserializeScene(scene, { { "Physics", { { "FixedTimestep", nullptr } } } }, error));
		CHECK(scene.GetPhysicsSettings().FixedTimestep == 0.0f);
		CHECK(SceneSerializer::DeserializeScene(scene, { { "Physics", { { "FixedTimestep", 0.02 } } } }, error));
		CHECK(scene.GetPhysicsSettings().FixedTimestep == 0.02f);
		for (const nlohmann::json& bad : { nlohmann::json(0), nlohmann::json(0.5), nlohmann::json("x") })
		{
			CHECK_FALSE(SceneSerializer::DeserializeScene(scene, { { "Physics", { { "FixedTimestep", bad } } } }, error));
			CHECK(error.find("FixedTimestep") != std::string::npos);
			CHECK(scene.GetPhysicsSettings().FixedTimestep == 0.02f);
		}

		// The state hash covers the step the simulation uses, whoever chose it.
		scene.GetPhysicsSettings().FixedTimestep = 0.0f;
		const std::string projectStep = SceneSerializer::ComputeStateHash(scene);
		Project::GetActive()->GetConfig().FixedTimestep = 0.01f;
		const std::string changedProject = SceneSerializer::ComputeStateHash(scene);
		CHECK(changedProject != projectStep);
		scene.GetPhysicsSettings().FixedTimestep = 0.01f;
		CHECK(SceneSerializer::ComputeStateHash(scene) == changedProject);
	}

	TEST_CASE("Version 1 scenes, which always stored the default step, follow the project's step")
	{
		BasaltTest::TempProject temp("FixedTimestepMigration");
		REQUIRE(temp.IsValid());
		// What the version 1 serializer wrote into every scene: the 1/60 s default as a float.
		const auto load = [](int version, double step) {
			Scene scene;
			std::string error;
			const nlohmann::json data = { { "Format", "BasaltScene" }, { "Version", version }, { "Physics", { { "FixedTimestep", step } } } };
			REQUIRE_MESSAGE(SceneSerializer::DeserializeScene(scene, data, error), error);
			return scene.GetPhysicsSettings().FixedTimestep;
		};
		CHECK(load(1, 0.01666666753590107) == 0.0f);
		// What the committed scenes store, and a hand-written approximation, mean the default too.
		CHECK(load(1, 0.016666668) == 0.0f);
		CHECK(load(1, 0.0166667) == 0.0f);
		// A version 1 scene that changed the step meant it; so does any version 2 value.
		CHECK(load(1, 0.01) == 0.01f);
		CHECK(load(2, 0.01666666753590107) == DefaultFixedTimestep);
		Scene scene;
		CHECK(SceneSerializer::SerializeScene(scene)["Version"] == SceneSerializer::FormatVersion);
		// A migrated scene saves without a step; a scene.settings patch (no Version) keeps an exact 1/60 s override.
		std::string error;
		REQUIRE(SceneSerializer::DeserializeScene(scene, { { "Format", "BasaltScene" }, { "Version", 1 }, { "Physics", { { "FixedTimestep", 0.016666668 } } } }, error));
		CHECK_FALSE(SceneSerializer::SerializeScene(scene)["Physics"].contains("FixedTimestep"));
		REQUIRE(SceneSerializer::DeserializeScene(scene, { { "Physics", { { "FixedTimestep", 1.0 / 60.0 } } } }, error));
		CHECK(scene.GetPhysicsSettings().FixedTimestep == DefaultFixedTimestep);
		CHECK(SceneSerializer::FormatVersion == 2);
	}

	TEST_CASE("Fixed timestep commands: project.set and project.info, scene.settings and scene.info")
	{
		BasaltTest::TempProject temp("FixedTimestepCommands");
		REQUIRE(temp.IsValid());
		CommandRegistry registry;
		AutomationSession session;
		RunCommand(registry, session, "scene.new", { { "name", "Main" }, { "empty", true } });
		CHECK(RunCommand(registry, session, "project.info")["fixedTimestep"] == DefaultFixedTimestep);
		RunCommand(registry, session, "project.set", { { "fixedTimestep", 0.02 } });
		CHECK(RunCommand(registry, session, "project.info")["fixedTimestep"] == 0.02f);
		for (const nlohmann::json& bad : { nlohmann::json(0), nlohmann::json(-1), nlohmann::json(0.26), nlohmann::json("x") })
			CHECK(RunCommandError(registry, session, "project.set", { { "fixedTimestep", bad } }).find("fixedTimestep") != std::string::npos);
		CHECK(Project::GetActive()->GetConfig().FixedTimestep == 0.02f);

		nlohmann::json info = RunCommand(registry, session, "scene.info");
		CHECK(info["fixedTimestep"] == 0.02f);
		CHECK(info["fixedTimestepOverride"].is_null());
		CHECK_FALSE(RunCommand(registry, session, "project.set", { { "fixedTimestep", 0.02 } }).contains("warnings"));
		RunCommand(registry, session, "scene.settings", { { "Physics", { { "FixedTimestep", 0.005 } } } });
		// A scene with its own step ignores the project's: the command says so.
		const nlohmann::json warned = RunCommand(registry, session, "project.set", { { "fixedTimestep", 0.02 } });
		REQUIRE(warned.contains("warnings"));
		CHECK(warned["warnings"][0].get<std::string>().find("keeps its own physics step") != std::string::npos);
		// Patching other physics settings keeps the override.
		RunCommand(registry, session, "scene.settings", { { "Physics", { { "Gravity", { 0, -5, 0 } } } } });
		info = RunCommand(registry, session, "scene.info");
		CHECK(info["fixedTimestep"] == 0.005f);
		CHECK(info["fixedTimestepOverride"] == 0.005f);
		RunCommand(registry, session, "scene.settings", { { "Physics", { { "FixedTimestep", nullptr } } } });
		CHECK(RunCommand(registry, session, "scene.info")["fixedTimestepOverride"].is_null());
	}

	TEST_CASE("Project validation accepts a complete project")
	{
		BasaltTest::TempProject temp("ValidateClean");
		REQUIRE(temp.IsValid());
		temp.WriteFile("Assets/Scripts/Mover.lua", "local Mover = {}\nfunction Mover:OnUpdate(dt) end\nreturn Mover\n");
		// A module loaded with require may return anything; only Script components need a class table.
		temp.WriteFile("Assets/Scripts/Helpers.lua", "return function() return 1 end\n");
		temp.WriteFile("Assets/Textures/Wall.png", "not decoded by validation");
		temp.WriteFile("Assets/Models/Ship.glb", "not decoded by validation");
		// Non-ASCII names must match on every platform (UTF-8 in the scene, the file system's encoding on disk).
		REQUIRE(FileSystem::WriteTextFile(temp.GetDirectory() / std::filesystem::path(u8"Assets/Textures/\u00c4rger.png"), "x"));
		temp.WriteFile("Assets/Scenes/Main.bscene", "{\"Entities\": [{\"ID\": 1, \"Name\": \"Wall\", \"Components\": {"
													"\"Mesh\": {\"Mesh\": \"builtin://Cube\"}, \"Material\": {\"AlbedoMap\": \"Assets/Textures/Wall.png\", \"NormalMap\": \"Assets/Models/Ship.glb#image/0\", "
													"\"EmissiveMap\": \"Assets/Textures/\xc3\x84rger.png\"}, \"Script\": {\"Script\": \"./Assets/Scripts/Mover.lua\"}, \"RigidBody\": {\"Layer\": \"Default\"}}}]}");
		Project::GetActive()->GetConfig().StartScene = "Assets/Scenes/Main.bscene";
		const ValidationReport report = ValidateProject(*Project::GetActive());
		INFO(nlohmann::json(Messages(report, ValidationSeverity::Error)).dump(1));
		CHECK(report.IsValid());
		CHECK(report.GetWarningCount() == 0);
		CHECK(report.GetIssues().empty());
	}

	TEST_CASE("Project validation reports what would break the exported game")
	{
		BasaltTest::TempProject temp("ValidateProblems");
		REQUIRE(temp.IsValid());
		const Ref<Project>& project = Project::GetActive();
		CHECK(Contains(Messages(ValidateProject(*project), ValidationSeverity::Error), "no StartScene"));

		temp.WriteFile("Assets/Textures/Wall.png", "x");
		temp.WriteFile("Assets/Scripts/Broken.lua", "local x = = 1\n");
		temp.WriteFile("Assets/Scripts/NoClass.lua", "return 42\n");
		temp.WriteFile("Assets/Scenes/Main.bscene", R"({"Entities": [
			{"ID": 1, "Name": "A", "Components": {"Mesh": {"Mesh": "Assets/Models/Missing.glb"}}},
			{"ID": 2, "Name": "B", "Components": {"Material": {"AlbedoMap": "Assets/textures/wall.png"}}},
			{"ID": 3, "Name": "C", "Components": {"Mesh": {"Mesh": "builtin://Teapot"}}},
			{"ID": 4, "Name": "D", "Components": {"Material": {"NormalMap": "../Outside.png"}}},
			{"ID": 5, "Name": "E", "Components": {"Script": {"Script": "./Assets/Scripts/NoClass.lua", "Properties": {"Shot": "Assets/Prefabs/Gone.bprefab"}}}},
			{"ID": 6, "Name": "F", "Components": {"RigidBody": {"Layer": "Ghost"}}},
			{"ID": 7, "Name": "G", "Components": {"UIText": {"Text": "See docs/help.png or https://example.com/help.png"}}},
			{"ID": 8, "Name": "H", "Components": {"Material": {"AlbedoMap": "/Absolute/Wall.png"}}},
			{"ID": 9, "Name": "I", "Components": {"Mesh": {"Mesh": "Ship.glb"}, "AudioSource": {"Clip": "Assets\\Audio\\Beep.wav"}}}]})");
		temp.WriteFile("Assets/Prefabs/Hollow.bprefab", R"({"Format": "BasaltPrefab", "Version": 1, "Entities": [
			{"ID": 1, "Name": "Root", "Parent": 0, "Components": {"Mesh": {"Mesh": "Assets/Models/Gone.glb"}}}]})");
		temp.WriteFile("Assets/Scenes/Corrupt.bscene", R"({"Entities": [{"ID": 1, "Components": {"Bogus": {}}}]})");
		temp.WriteFile("Assets/Prefabs/Bad.bprefab", "{not json");
		project->GetConfig().StartScene = "Assets/Scenes/Missing.bscene";
		project->GetConfig().WindowWidth = 10;

		const ValidationReport report = ValidateProject(*project);
		CHECK_FALSE(report.IsValid());
		const auto errors = Messages(report, ValidationSeverity::Error);
		INFO(nlohmann::json(errors).dump(1));
		CHECK(Contains(errors, "Project.bproject: the window size 10x900"));
		CHECK(Contains(errors, "Project.bproject: the StartScene names the missing file 'Assets/Scenes/Missing.bscene'"));
		CHECK(Contains(errors, "Assets/Scenes/Main.bscene: references the missing file 'Assets/Models/Missing.glb'"));
		CHECK(Contains(errors, "'Assets/textures/wall.png', but the file on disk is 'Assets/Textures/Wall.png'"));
		CHECK(Contains(errors, "unknown built-in mesh 'builtin://Teapot'"));
		CHECK(Contains(errors, "'../Outside.png', which is outside Assets/"));
		CHECK(Contains(errors, "Assets/Scripts/Broken.lua: does not compile"));
		// Spelled with ./ the script is still found, and class-checked.
		CHECK(Contains(errors, "Assets/Scripts/NoClass.lua: fails to load as a script class"));
		// A path field with a bare name is not inside Assets/ (the game resolves it against the project).
		CHECK(Contains(errors, "references 'Ship.glb', which is outside Assets/"));
		CHECK(Contains(errors, "which uses backslashes"));
		CHECK(Contains(errors, "Assets/Scenes/Corrupt.bscene: does not load"));
		CHECK(Contains(errors, "Assets/Prefabs/Bad.bprefab: does not load"));
		CHECK(Contains(errors, "Assets/Prefabs/Hollow.bprefab: references the missing file 'Assets/Models/Gone.glb'"));
		CHECK(Contains(errors, "'/Absolute/Wall.png', which is outside Assets/"));
		CHECK(report.GetErrorCount() == errors.size());
		const auto warnings = Messages(report, ValidationSeverity::Warning);
		CHECK(Contains(warnings, "'F' uses the undefined physics layer 'Ghost'"));
		// A script property naming a missing asset warns (it may be text); plain text is not checked at all.
		CHECK(Contains(warnings, "has a value naming the missing file 'Assets/Prefabs/Gone.bprefab'"));
		CHECK_FALSE(Contains(errors, "help.png"));
		CHECK_FALSE(Contains(warnings, "help.png"));
		CHECK(report.GetWarningCount() == 2);
		CHECK(report.GetOmittedCount() == 0);
	}

	TEST_CASE("Project validation caps the listed issues but counts them all")
	{
		BasaltTest::TempProject temp("ValidateCap");
		REQUIRE(temp.IsValid());
		std::string entities;
		for (size_t i = 0; i < ValidationReport::MaxIssues + 25; i++)
			entities += (i ? "," : "") + nlohmann::json{ { "ID", i + 1 }, { "Name", "E" }, { "Components", { { "Mesh", { { "Mesh", "Assets/M" + std::to_string(i) + ".glb" } } } } } }.dump();
		temp.WriteFile("Assets/Scenes/Main.bscene", "{\"Entities\": [" + entities + "]}");
		Project::GetActive()->GetConfig().StartScene = "Assets/Scenes/Main.bscene";
		const ValidationReport report = ValidateProject(*Project::GetActive());
		CHECK(report.GetIssues().size() == ValidationReport::MaxIssues);
		CHECK(report.GetErrorCount() == ValidationReport::MaxIssues + 25);
		CHECK(report.GetOmittedCount() == 25);

		// Errors that arrive after the list filled with warnings still show.
		ValidationReport mixed;
		for (size_t i = 0; i < ValidationReport::MaxIssues + 10; i++)
			mixed.Add(ValidationSeverity::Warning, "w", "warning");
		mixed.Add(ValidationSeverity::Error, "e", "error 1");
		mixed.Add(ValidationSeverity::Error, "e", "error 2");
		CHECK(mixed.GetIssues().size() == ValidationReport::MaxIssues);
		CHECK(mixed.GetErrorCount() == 2);
		CHECK(Messages(mixed, ValidationSeverity::Error) == std::vector<std::string>{ "e: error 1", "e: error 2" });
		CHECK(mixed.GetOmittedCount() == 12);
	}

	TEST_CASE("Project validation reports project file problems")
	{
		BasaltTest::TempProject temp("ValidateProjectFile");
		REQUIRE(temp.IsValid());
		const Ref<Project>& project = Project::GetActive();
		temp.WriteFile("Assets/Scenes/Main.bscene", R"({"Entities": []})");
		project->GetConfig().StartScene = "Assets/Scenes/Main.bscene";
		temp.WriteFile(Project::FileName, R"({"Name": "No format"})");
		auto report = ValidateProject(*project);
		CHECK(report.IsValid());
		CHECK(Contains(Messages(report, ValidationSeverity::Warning), "does not declare \"Format\""));
		temp.WriteFile(Project::FileName, "[1, 2]");
		CHECK(Contains(Messages(ValidateProject(*project), ValidationSeverity::Error), "the project file is not a JSON object"));
		project->GetConfig().StartScene = "Assets/Scenes/Main.lua";
		CHECK(Contains(Messages(ValidateProject(*project), ValidationSeverity::Error), "is not a .bscene file"));
		std::filesystem::remove_all(temp.GetDirectory() / "Assets");
		CHECK(Contains(Messages(ValidateProject(*project), ValidationSeverity::Error), "the Assets directory is missing"));
	}

#if !defined(_WIN32)
	TEST_CASE("Project validation reports a folder it cannot read instead of skipping it")
	{
		// Permissions do not stop root, which then reads the folder.
		if (geteuid() == 0)
			return;
		BasaltTest::TempProject temp("ValidateUnreadable");
		REQUIRE(temp.IsValid());
		temp.WriteFile("Assets/Scenes/Main.bscene", R"({"Entities": []})");
		temp.WriteFile("Assets/Locked/Broken.lua", "local x = = 1\n");
		Project::GetActive()->GetConfig().StartScene = "Assets/Scenes/Main.bscene";
		const std::filesystem::path locked = temp.GetDirectory() / "Assets" / "Locked";
		std::filesystem::permissions(locked, std::filesystem::perms::none);
		const ValidationReport report = ValidateProject(*Project::GetActive());
		std::filesystem::permissions(locked, std::filesystem::perms::owner_all);
		CHECK_FALSE(report.IsValid());
		const auto errors = Messages(report, ValidationSeverity::Error);
		INFO(nlohmann::json(errors).dump(1));
		CHECK(Contains(errors, "cannot list all the assets"));
	}
#endif

	TEST_CASE("project.validate and project.export refuse a broken project; project.smoke_test needs an executable")
	{
		BasaltTest::TempProject temp("ValidateCommands");
		REQUIRE(temp.IsValid());
		CommandRegistry registry;
		AutomationSession session;
		// One error, and a warning that error messages leave out.
		temp.WriteFile("Assets/Scenes/Main.bscene", R"({"Entities": [{"ID": 1, "Name": "A", "Components": {"Mesh": {"Mesh": "Assets/Missing.glb"}, "RigidBody": {"Layer": "Ghost"}}}]})");
		RunCommand(registry, session, "project.set", { { "startScene", "Assets/Scenes/Main.bscene" } });

		const nlohmann::json report = RunCommand(registry, session, "project.validate");
		CHECK(report["valid"] == false);
		CHECK(report["errors"] == 1);
		CHECK(report["warnings"] == 1);
		REQUIRE(report["issues"].size() == 2);
		const auto listedError = std::find_if(report["issues"].begin(), report["issues"].end(), [](const nlohmann::json& issue) { return issue["severity"] == "error"; });
		REQUIRE(listedError != report["issues"].end());
		CHECK((*listedError)["file"] == "Assets/Scenes/Main.bscene");
		const std::string assertError = RunCommandError(registry, session, "project.validate", { { "assertValid", true } });
		CHECK(assertError.find("found 1 error(s)\nAssets/Scenes/Main.bscene: references the missing file 'Assets/Missing.glb'") != std::string::npos);
		CHECK(assertError.find("Ghost") == std::string::npos);

		BasaltTest::TempDirectory exportDirectory("ValidateCommandsExport");
		const auto output = exportDirectory.GetPath() / "Build";
		const std::string error = RunCommandError(registry, session, "project.export", { { "output", output.string() } });
		CHECK(error.find("not exported: project validation found 1 error") != std::string::npos);
		CHECK_FALSE(std::filesystem::exists(output));
		CHECK(RunCommandError(registry, session, "project.export", { { "output", output.string() }, { "smokeTestFrames", -1 } }).find("smokeTestFrames") != std::string::npos);
		CHECK(RunCommandError(registry, session, "project.export", { { "output", output.string() }, { "smokeTestTimeout", 0 } }).find("smokeTestTimeout") != std::string::npos);
		// validate: false exports anyway (BasaltRuntime is built next to the tests). On macOS the exporter bundles
		// the Vulkan loader and MoltenVK: point it at stand-ins so the test does not need the Vulkan SDK.
#if defined(__APPLE__)
		temp.WriteFile("FakeVulkanSDK/lib/libvulkan.1.dylib", "stand-in");
		temp.WriteFile("FakeVulkanSDK/lib/libMoltenVK.dylib", "stand-in");
		const BasaltTest::ScopedEnvVar sdk("VULKAN_SDK", (temp.GetDirectory() / "FakeVulkanSDK").string());
#endif
		const nlohmann::json exported = RunCommand(registry, session, "project.export", { { "output", output.string() }, { "validate", false } });
		CHECK(exported["validation"].is_null());
		CHECK(std::filesystem::exists(exported["executable"].get<std::string>()));

		CHECK(RunCommandError(registry, session, "project.smoke_test", { { "executable", (temp.GetDirectory() / "NoGame").string() } }).find("no executable") != std::string::npos);
		CHECK(RunCommandError(registry, session, "project.smoke_test", { { "executable", "x" }, { "frames", 0 } }).find("frames") != std::string::npos);
		CHECK(RunCommandError(registry, session, "project.export", { { "output", output.string() }, { "smokeTestTimeout", 5 } }).find("smokeTestTimeout needs smokeTestFrames") != std::string::npos);

		// A file that is not a program cannot run: a failed smoke test, not an exit code.
		temp.WriteFile("NotAProgram.txt", "text");
		const std::string notAProgram = (temp.GetDirectory() / "NotAProgram.txt").string();
		const nlohmann::json notRun = RunCommand(registry, session, "project.smoke_test", { { "executable", notAProgram }, { "frames", 1 } });
		CHECK(notRun["passed"] == false);
		CHECK(notRun["outcome"] == "failed");
		CHECK(notRun["exitCode"].is_null());
		CHECK(notRun["summary"].get<std::string>().find("could not be run") != std::string::npos);
		CHECK(RunCommandError(registry, session, "project.smoke_test", { { "executable", notAProgram }, { "frames", 1 }, { "assertPassed", true } }).find("smoke test failed") != std::string::npos);
	}

	TEST_CASE("A smoke test reports a game that crashes")
	{
		// The test binary stands in for an exported game: BASALT_TEST_CHILD_ACTIONS makes it act whatever its
		// arguments (TestMain.cpp).
		const BasaltTest::ScopedEnvVar actions("BASALT_TEST_CHILD_ACTIONS", "print playing crash");
#if defined(_WIN32)
		const std::filesystem::path game = FileSystem::GetExecutableDirectory() / "BasaltTests.exe";
#else
		const std::filesystem::path game = FileSystem::GetExecutableDirectory() / "BasaltTests";
#endif
		const SmokeTestResult result = RunSmokeTest(game, 10, std::chrono::seconds(30));
		CHECK_FALSE(result.Passed);
		CHECK(result.Process.Outcome == ProcessOutcome::Crashed);
		CHECK(result.Summary.rfind("the game crashed: ", 0) == 0);
		CHECK(result.Process.Output.find("playing") != std::string::npos);

		CommandRegistry registry;
		AutomationSession session;
		const nlohmann::json report = RunCommand(registry, session, "project.smoke_test", { { "executable", FileSystem::PathToUtf8(game) }, { "frames", 10 } });
		CHECK(report["outcome"] == "crashed");
		CHECK_FALSE(report["exitCode"].is_null());
		CHECK(report["passed"] == false);
		CHECK_FALSE(RunSmokeTest(game, 0, std::chrono::seconds(30)).Passed);
	}

	TEST_CASE("LastLines keeps the end of a log")
	{
		CHECK(LastLines("a\nb\nc\n", 2) == "b\nc\n");
		CHECK(LastLines("a\nb\nc", 2) == "b\nc");
		CHECK(LastLines("a\nb\nc\n", 5) == "a\nb\nc\n");
		CHECK(LastLines("single", 1) == "single");
		CHECK(LastLines("", 3).empty());
		CHECK(LastLines("a\n\n\n", 2) == "\n\n");
		CHECK(LastLines("\nabc", 5) == "\nabc");
		CHECK(LastLines("abc\n", 0).empty());
	}
}
