#include <doctest/doctest.h>

#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/JsonUtils.h>
#include <Basalt/Project/Exporter.h>
#include <Basalt/Project/Packaging.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Project/ProjectValidator.h>
#include <Basalt/Scene/Scene.h>

#include <stb_image.h>

#include "AutomationTestUtils.h"
#include "TestUtils.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
	#include <windows.h>
#endif

using namespace Basalt;
using json = nlohmann::json;

namespace {

	// A solid icon of one color.
	IconImage SolidIcon(uint32_t width, uint32_t height, std::array<uint8_t, 4> color = { 200, 40, 30, 255 })
	{
		IconImage icon;
		icon.Width = width;
		icon.Height = height;
		for (uint32_t i = 0; i < width * height; ++i)
			icon.Pixels.insert(icon.Pixels.end(), color.begin(), color.end());
		return icon;
	}

	void WriteIcon(const std::filesystem::path& path, const IconImage& icon)
	{
		const std::vector<uint8_t> png = EncodeIconPng(icon);
		REQUIRE(FileSystem::WriteBinaryFile(path, png.data(), png.size()));
	}

	// Width and height of PNG bytes (0 x 0 when they do not decode).
	std::pair<int, int> PngSize(const uint8_t* data, size_t size)
	{
		int width = 0;
		int height = 0;
		int channels = 0;
		if (!stbi_info_from_memory(data, static_cast<int>(size), &width, &height, &channels))
			return { 0, 0 };
		return { width, height };
	}

	uint32_t BigEndian32(const std::vector<uint8_t>& bytes, size_t offset)
	{
		return (uint32_t{ bytes[offset] } << 24) | (uint32_t{ bytes[offset + 1] } << 16) | (uint32_t{ bytes[offset + 2] } << 8) | bytes[offset + 3];
	}

	uint16_t Word(const std::vector<uint8_t>& bytes, size_t offset)
	{
		return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
	}

	uint32_t DWord(const std::vector<uint8_t>& bytes, size_t offset)
	{
		return Word(bytes, offset) | (static_cast<uint32_t>(Word(bytes, offset + 2)) << 16);
	}

	// One block of a VS_VERSIONINFO resource, read back the way Windows does (32-bit aligned, lengths in bytes except a
	// text value's, which counts UTF-16 units).
	struct VersionBlock
	{
		std::u16string Key;
		uint16_t Type = 0;
		std::vector<uint8_t> Value;
		std::vector<VersionBlock> Children;

		const VersionBlock* Find(std::u16string_view key) const
		{
			const auto it = std::find_if(Children.begin(), Children.end(), [&](const VersionBlock& child) { return child.Key == key; });
			return it == Children.end() ? nullptr : &*it;
		}
		std::u16string Text() const
		{
			std::u16string text;
			for (size_t i = 0; i + 1 < Value.size(); i += 2)
				text += static_cast<char16_t>(Value[i] | (Value[i + 1] << 8));
			while (!text.empty() && text.back() == u'\0')
				text.pop_back();
			return text;
		}
	};

	size_t Align4(size_t offset)
	{
		return (offset + 3) & ~size_t{ 3 };
	}

	VersionBlock ParseVersionBlock(const std::vector<uint8_t>& bytes, size_t offset)
	{
		VersionBlock block;
		REQUIRE(offset + 6 <= bytes.size());
		REQUIRE(offset % 4 == 0);
		const size_t end = offset + Word(bytes, offset);
		REQUIRE(end <= bytes.size());
		const uint16_t valueLength = Word(bytes, offset + 2);
		block.Type = Word(bytes, offset + 4);
		size_t position = offset + 6;
		for (; position + 1 < end && Word(bytes, position) != 0; position += 2)
			block.Key += static_cast<char16_t>(Word(bytes, position));
		position = Align4(position + 2);
		const size_t valueBytes = block.Type == 1 ? size_t{ valueLength } * 2 : valueLength;
		REQUIRE(position + valueBytes <= end);
		block.Value.assign(bytes.begin() + static_cast<std::ptrdiff_t>(position), bytes.begin() + static_cast<std::ptrdiff_t>(position + valueBytes));
		position = Align4(position + valueBytes);
		while (position < end)
		{
			block.Children.push_back(ParseVersionBlock(bytes, position));
			position = Align4(position + Word(bytes, position));
		}
		return block;
	}

	std::map<std::u16string, std::u16string> VersionStrings(const VersionBlock& root)
	{
		std::map<std::u16string, std::u16string> strings;
		const VersionBlock* fileInfo = root.Find(u"StringFileInfo");
		REQUIRE(fileInfo);
		const VersionBlock* table = fileInfo->Find(u"040904B0");
		REQUIRE(table);
		for (const VersionBlock& entry : table->Children)
			strings[entry.Key] = entry.Text();
		return strings;
	}

	PackageInfo TestPackageInfo()
	{
		ProjectConfig config;
		config.Name = "Rock & Roll";
		config.Version = "2.10.300.4";
		config.Company = "Stone <Works>";
		std::string error;
		const auto info = MakePackageInfo(config, "Rock_Roll", error);
		REQUIRE_MESSAGE(info, error);
		return *info;
	}

}

TEST_SUITE("Packaging")
{
	TEST_CASE("Project versions are three or four 16-bit numbers")
	{
		CHECK(ParseProjectVersion("1.0.0") == std::array<uint16_t, 4>{ 1, 0, 0, 0 });
		CHECK(ParseProjectVersion("2.10.300.4") == std::array<uint16_t, 4>{ 2, 10, 300, 4 });
		CHECK(ParseProjectVersion("65535.0.65535") == std::array<uint16_t, 4>{ 65535, 0, 65535, 0 });
		CHECK(ParseProjectVersion("0.10.100") == std::array<uint16_t, 4>{ 0, 10, 100, 0 });
		// Leading zeros would make the text ("01.002.3") disagree with the numbers packages carry (1.2.3).
		for (const char* invalid : { "", "1", "1.2", "1.2.3.4.5", "1..3", "1.2.3.", ".1.2.3", "a.b.c", "1.2.-3", "+1.2.3", "1.2.3 ", " 1.2.3", "65536.0.0", "1.2.999999", "1.2.3-beta", "1.2.3x", "01.2.3", "1.02.3", "1.2.3.00" })
			CHECK_MESSAGE(!ParseProjectVersion(invalid), invalid);
	}

	TEST_CASE("Version, company and icon round-trip through the project file")
	{
		BasaltTest::TempProject temp("PackagingRoundTrip");
		REQUIRE(temp.IsValid());
		const Ref<Project>& project = Project::GetActive();
		CHECK(project->GetConfig().Version == ProjectConfig::DefaultVersion);
		CHECK(project->GetConfig().Company.empty());
		CHECK(project->GetConfig().Icon.empty());

		WriteIcon(temp.GetDirectory() / "Assets/Textures/Icon.png", SolidIcon(256, 256));
		project->GetConfig().Version = "3.2.1.7";
		project->GetConfig().Company = "Caf\xC3\xA9 Studio";
		project->GetConfig().Icon = "Assets/Textures/Icon.png";
		std::string error;
		REQUIRE_MESSAGE(project->Save(error), error);
		// The icon is an asset reference, like the start scene: it follows the file's identity.
		const json saved = ParseJson(*FileSystem::ReadTextFile(project->GetProjectFilePath()));
		CHECK(saved["Icon"].is_object());
		CHECK(saved["Icon"]["Path"] == "Assets/Textures/Icon.png");

		Ref<Project> loaded = Project::Load(temp.GetDirectory(), error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetConfig().Version == "3.2.1.7");
		CHECK(loaded->GetConfig().Company == "Caf\xC3\xA9 Studio");
		CHECK(loaded->GetConfig().Icon == "Assets/Textures/Icon.png");

		// An icon whose file is gone keeps its identity through a load and save, for validation to report.
		std::filesystem::remove(temp.GetDirectory() / "Assets/Textures/Icon.png");
		loaded = Project::Load(temp.GetDirectory(), error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(AssetRegistry::ParseUnresolvedKey(loaded->GetConfig().Icon));
		REQUIRE_MESSAGE(loaded->Save(error), error);
		CHECK(ParseJson(*FileSystem::ReadTextFile(project->GetProjectFilePath()))["Icon"] == saved["Icon"]);

		// Bad values in the file stop the load with the reason.
		json broken = saved;
		broken["Version"] = "1.0";
		FileSystem::WriteTextFile(project->GetProjectFilePath(), broken.dump());
		CHECK_FALSE(Project::Load(temp.GetDirectory(), error));
		CHECK(error.find("Version '1.0' must be") != std::string::npos);
		broken = saved;
		broken["Company"] = 5;
		FileSystem::WriteTextFile(project->GetProjectFilePath(), broken.dump());
		CHECK_FALSE(Project::Load(temp.GetDirectory(), error));
		broken = saved;
		broken["Company"] = std::string(ProjectConfig::MaxTextLength + 1, 'c');
		FileSystem::WriteTextFile(project->GetProjectFilePath(), broken.dump());
		CHECK_FALSE(Project::Load(temp.GetDirectory(), error));
		CHECK(error.find("Company must be at most 256 bytes") != std::string::npos);
		broken = saved;
		broken["Name"] = std::string(ProjectConfig::MaxTextLength + 1, 'n');
		FileSystem::WriteTextFile(project->GetProjectFilePath(), broken.dump());
		CHECK_FALSE(Project::Load(temp.GetDirectory(), error));
		CHECK(error.find("Name must be at most 256 bytes") != std::string::npos);
		broken = saved;
		broken["Icon"] = 5;
		FileSystem::WriteTextFile(project->GetProjectFilePath(), broken.dump());
		CHECK_FALSE(Project::Load(temp.GetDirectory(), error));
		CHECK(error.find("Icon:") != std::string::npos);
	}

	TEST_CASE("project.set and project.info carry version, company and icon")
	{
		BasaltTest::TempProject temp("PackagingCommands");
		CommandRegistry registry;
		AutomationSession session;
		using BasaltTest::Run;
		using BasaltTest::RunError;
		WriteIcon(temp.GetDirectory() / "Assets/Textures/Icon.png", SolidIcon(512, 512));
		temp.WriteFile("Assets/Textures/NotAnImage.png", "text");
		temp.WriteFile("Outside.png", "text");

		Run(registry, session, "project.set", { { "version", "1.4.2" }, { "company", "Acme" }, { "icon", "Assets/Textures/Icon.png" } });
		json info = Run(registry, session, "project.info");
		CHECK(info["version"] == "1.4.2");
		CHECK(info["company"] == "Acme");
		CHECK(info["icon"] == "Assets/Textures/Icon.png");
		CHECK(info["bundleIdentifier"] == "com.Acme.PackagingCommands");

		// Every rejected value leaves the project as it was.
		CHECK(BasaltTest::Contains(RunError(registry, session, "project.set", { { "version", "1.4" } }), "parameter 'version' must be MAJOR.MINOR.PATCH"));
		CHECK(BasaltTest::Contains(RunError(registry, session, "project.set", { { "icon", "Outside.png" } }), "must be under Assets/"));
		CHECK(BasaltTest::Contains(RunError(registry, session, "project.set", { { "icon", "Assets/Textures/NotAnImage.png" } }), "is not a PNG file"));
		CHECK(BasaltTest::Contains(RunError(registry, session, "project.set", { { "icon", "Assets/Textures/Missing.png" } }), "cannot read icon"));
		CHECK(BasaltTest::Contains(RunError(registry, session, "project.set", { { "icon", "../Icon.png" } }), "'..' is not allowed"));
		CHECK(BasaltTest::Contains(RunError(registry, session, "project.set", { { "company", "Other" }, { "version", "x" } }), "version"));
		// Name and company end up in fixed-size package metadata.
		CHECK(BasaltTest::Contains(RunError(registry, session, "project.set", { { "company", std::string(ProjectConfig::MaxTextLength + 1, 'c') } }), "'company' must be at most 256 bytes"));
		CHECK(BasaltTest::Contains(RunError(registry, session, "project.set", { { "name", std::string(ProjectConfig::MaxTextLength + 1, 'n') } }), "'name' must be at most 256 bytes"));
		Run(registry, session, "project.set", { { "company", std::string(ProjectConfig::MaxTextLength, 'c') } });
		Run(registry, session, "project.set", { { "company", "Acme" } });
		info = Run(registry, session, "project.info");
		CHECK(info["version"] == "1.4.2");
		CHECK(info["company"] == "Acme");
		CHECK(info["icon"] == "Assets/Textures/Icon.png");

		// Moving the icon keeps the project pointing at it, in memory and on disk.
		Run(registry, session, "asset.move", { { "from", "Assets/Textures/Icon.png" }, { "to", "Assets/Branding/GameIcon.png" } });
		CHECK(Project::GetActive()->GetConfig().Icon == "Assets/Branding/GameIcon.png");
		std::string error;
		Ref<Project> reloaded = Project::Load(temp.GetDirectory(), error);
		REQUIRE_MESSAGE(reloaded, error);
		CHECK(reloaded->GetConfig().Icon == "Assets/Branding/GameIcon.png");

		// A legacy path string (a hand-written project file) is rewritten as the moved file's identity.
		json file = ParseJson(*FileSystem::ReadTextFile(reloaded->GetProjectFilePath()));
		file["Icon"] = "Assets/Branding/GameIcon.png";
		REQUIRE(FileSystem::WriteTextFile(reloaded->GetProjectFilePath(), file.dump()));
		reloaded = Project::Load(temp.GetDirectory(), error);
		REQUIRE_MESSAGE(reloaded, error);
		Project::SetActive(reloaded);
		Run(registry, session, "asset.move", { { "from", "Assets/Branding/GameIcon.png" }, { "to", "Assets/Icon.png" } });
		CHECK(reloaded->GetConfig().Icon == "Assets/Icon.png");
		CHECK(ParseJson(*FileSystem::ReadTextFile(reloaded->GetProjectFilePath()))["Icon"].is_object());
		reloaded = Project::Load(temp.GetDirectory(), error);
		REQUIRE_MESSAGE(reloaded, error);
		CHECK(reloaded->GetConfig().Icon == "Assets/Icon.png");
		Project::SetActive(reloaded);

		// With the icon file gone, other settings stay editable and the icon keeps its identity.
		std::filesystem::remove(temp.GetDirectory() / "Assets/Icon.png");
		reloaded = Project::Load(temp.GetDirectory(), error);
		REQUIRE_MESSAGE(reloaded, error);
		Project::SetActive(reloaded);
		const std::string missing = reloaded->GetConfig().Icon;
		CHECK(AssetRegistry::IsUnresolvedKey(missing));
		Run(registry, session, "project.set", { { "company", "Beta" } });
		CHECK(reloaded->GetConfig().Icon == missing);
		std::optional<IconImage> loadedIcon;
		CHECK_FALSE(LoadProjectIcon(*reloaded, loadedIcon, error));
		CHECK(BasaltTest::Contains(error, "the project icon is an unresolved asset UUID"));

		Run(registry, session, "project.set", { { "icon", "" } });
		CHECK(Run(registry, session, "project.info")["icon"] == "");
		CHECK(LoadProjectIcon(*reloaded, loadedIcon, error));
		CHECK_FALSE(loadedIcon);

		// Deleting the icon says so; the project keeps naming it until another is set.
		WriteIcon(temp.GetDirectory() / "Assets/Textures/Icon.png", SolidIcon(256, 256));
		Run(registry, session, "project.set", { { "icon", "Assets/Textures/Icon.png" } });
		const json deleted = Run(registry, session, "asset.delete", { { "path", "Assets/Textures/Icon.png" } });
		REQUIRE(deleted.contains("warnings"));
		CHECK(BasaltTest::Contains(deleted["warnings"][0].get<std::string>(), "this was the project's icon"));
		CHECK_FALSE(Run(registry, session, "asset.delete", { { "path", "Assets/Textures/NotAnImage.png" } }).contains("warnings"));
	}

	TEST_CASE("Validation checks the icon")
	{
		BasaltTest::TempProject temp("PackagingValidation");
		const Ref<Project>& project = Project::GetActive();
		const auto messages = [&](ValidationSeverity severity) {
			std::vector<std::string> result;
			// Kept in a variable: a range-for over a temporary's member would read it after it is destroyed.
			const ValidationReport report = ValidateProject(*project);
			for (const ValidationIssue& issue : report.GetIssues())
				if (issue.Severity == severity && issue.File == Project::FileName)
					result.push_back(issue.Message);
			return result;
		};
		const auto contains = [](const std::vector<std::string>& list, const std::string& text) {
			return std::any_of(list.begin(), list.end(), [&](const std::string& message) { return BasaltTest::Contains(message, text); });
		};

		WriteIcon(temp.GetDirectory() / "Assets/Textures/Small.png", SolidIcon(64, 32));
		project->GetConfig().Icon = "Assets/Textures/Small.png";
		CHECK(contains(messages(ValidationSeverity::Warning), "is 64 x 32 pixels; packaged icons look blurry"));

		WriteIcon(temp.GetDirectory() / "Assets/Textures/Large.png", SolidIcon(300, 256));
		project->GetConfig().Icon = "Assets/Textures/Large.png";
		CHECK_FALSE(contains(messages(ValidationSeverity::Warning), "Icon"));
		CHECK_FALSE(contains(messages(ValidationSeverity::Error), "Icon"));

		temp.WriteFile("Assets/Textures/Fake.png", "not a png");
		project->GetConfig().Icon = "Assets/Textures/Fake.png";
		CHECK(contains(messages(ValidationSeverity::Error), "the Icon: icon"));

		// The larger side counts, whichever it is.
		WriteIcon(temp.GetDirectory() / "Assets/Textures/Tall.png", SolidIcon(32, 300));
		project->GetConfig().Icon = "Assets/Textures/Tall.png";
		CHECK_FALSE(contains(messages(ValidationSeverity::Warning), "Icon"));

		project->GetConfig().Icon = "Assets/Textures/Missing.png";
		CHECK(contains(messages(ValidationSeverity::Error), "the Icon names the missing file"));

		// What a reload leaves when the icon's file is gone: its identity, reported as unresolved.
		project->GetConfig().Icon = "Assets/Textures/Tall.png";
		std::string error;
		REQUIRE_MESSAGE(project->Save(error), error);
		std::filesystem::remove(temp.GetDirectory() / "Assets/Textures/Tall.png");
		const Ref<Project> reloaded = Project::Load(temp.GetDirectory(), error);
		REQUIRE_MESSAGE(reloaded, error);
		std::vector<std::string> unresolved;
		const ValidationReport report = ValidateProject(*reloaded);
		for (const ValidationIssue& issue : report.GetIssues())
			if (issue.Severity == ValidationSeverity::Error)
				unresolved.push_back(issue.Message);
		CHECK(contains(unresolved, "the Icon names an unresolved asset UUID"));
	}

	TEST_CASE("Icons load from PNG only and resize onto a transparent square")
	{
		BasaltTest::TempDirectory directory("PackagingIcons");
		std::string error;
		FileSystem::WriteTextFile(directory.GetPath() / "text.png", "hello");
		CHECK_FALSE(LoadIconImage(directory.GetPath() / "text.png", error));
		CHECK(error.find("is not a PNG file") != std::string::npos);
		CHECK_FALSE(LoadIconImage(directory.GetPath() / "missing.png", error));
		CHECK(error.find("cannot read icon") != std::string::npos);
		// Too large is rejected from the header, before decoding.
		WriteIcon(directory.GetPath() / "wide.png", SolidIcon(MaxIconSize + 1, 1));
		CHECK_FALSE(LoadIconImage(directory.GetPath() / "wide.png", error));
		CHECK(error.find("at most 4096 pixels") != std::string::npos);
		WriteIcon(directory.GetPath() / "widest.png", SolidIcon(MaxIconSize, 1));
		CHECK(LoadIconImage(directory.GetPath() / "widest.png", error));
		// A PNG signature followed by garbage does not decode.
		const std::string truncated = std::string("\x89PNG\r\n\x1a\n") + "garbage";
		FileSystem::WriteTextFile(directory.GetPath() / "truncated.png", truncated);
		CHECK_FALSE(LoadIconImage(directory.GetPath() / "truncated.png", error));
		CHECK(error.find("cannot decode icon") != std::string::npos);

		WriteIcon(directory.GetPath() / "red.png", SolidIcon(4, 2));
		const auto icon = LoadIconImage(directory.GetPath() / "red.png", error);
		REQUIRE_MESSAGE(icon, error);
		CHECK(icon->Width == 4);
		CHECK(icon->Height == 2);

		// A wide icon is centered, never stretched: rows above and below are transparent.
		const IconImage square = ResizeIcon(*icon, 4);
		REQUIRE(square.Width == 4);
		REQUIRE(square.Height == 4);
		CHECK(square.Pixels[3] == 0);
		CHECK(square.Pixels[(1 * 4 + 1) * 4 + 0] == 200);
		CHECK(square.Pixels[(1 * 4 + 1) * 4 + 3] == 255);
		CHECK(square.Pixels[(3 * 4 + 3) * 4 + 3] == 0);

		// A tall icon is centered too: columns left and right are transparent.
		const IconImage tall = ResizeIcon(SolidIcon(2, 4), 4);
		for (size_t y = 0; y < 4; ++y)
		{
			CHECK(tall.Pixels[(y * 4 + 0) * 4 + 3] == 0);
			CHECK(tall.Pixels[(y * 4 + 1) * 4 + 3] == 255);
			CHECK(tall.Pixels[(y * 4 + 2) * 4 + 3] == 255);
			CHECK(tall.Pixels[(y * 4 + 3) * 4 + 3] == 0);
		}

		// Downscaling a padded icon blends its edge rows with the transparent padding: alpha drops, but the color stays
		// the icon's own (an unweighted filter would darken it toward the padding's black).
		const IconImage padded = ResizeIcon(SolidIcon(64, 32), 16);
		REQUIRE(padded.IsValid());
		bool sawEdge = false;
		for (size_t y = 0; y < 16; ++y)
		{
			const uint8_t* pixel = &padded.Pixels[(y * 16 + 8) * 4];
			if (pixel[3] > 8)
				CHECK_MESSAGE(std::abs(pixel[0] - 200) <= 6, "row ", y, " red ", int(pixel[0]), " alpha ", int(pixel[3]));
			sawEdge = sawEdge || (pixel[3] > 8 && pixel[3] < 247);
		}
		CHECK(sawEdge);

		const IconImage scaled = ResizeIcon(SolidIcon(64, 64), 16);
		CHECK(scaled.Width == 16);
		CHECK(scaled.Height == 16);
		CHECK(scaled.Pixels.size() == 16u * 16u * 4u);
		// Alpha-weighted filtering keeps an opaque color exact.
		CHECK(scaled.Pixels[0] == 200);
		CHECK(scaled.Pixels[3] == 255);

		const IconImage fallback = MakeDefaultIcon(64);
		CHECK(fallback.Width == 64);
		CHECK(fallback.Pixels[3] == 0);                        // a transparent corner
		CHECK(fallback.Pixels[(32 * 64 + 32) * 4 + 3] == 255); // an opaque center

		// Invalid images (a failed resize) encode to nothing rather than to a broken file.
		CHECK(EncodeIconPng(IconImage{}).empty());
		IconImage short_;
		short_.Width = 4;
		short_.Height = 4;
		short_.Pixels.resize(8);
		CHECK_FALSE(short_.IsValid());
		CHECK(EncodeIconPng(short_).empty());
	}

	TEST_CASE("Bundle identifiers are reverse-DNS from company and name")
	{
		CHECK(MakeBundleIdentifier("Acme Games", "Space Rocks!") == "com.Acme-Games.Space-Rocks");
		CHECK(MakeBundleIdentifier("", "Tetris") == "com.basalt.Tetris");
		CHECK(MakeBundleIdentifier("  ..  ", "\xE6\x97\xA5\xE6\x9C\xAC") == "com.basalt.game");
		CHECK(MakeBundleIdentifier("a--b", "x_y") == "com.a-b.x-y");
	}

	TEST_CASE("Info.plist escapes text and names the icon only when there is one")
	{
		PackageInfo info = TestPackageInfo();
		std::string plist = BuildInfoPlist(info);
		CHECK(plist.find("<string>Rock &amp; Roll</string>") != std::string::npos);
		CHECK(plist.find("<key>CFBundleExecutable</key>\n\t<string>Rock_Roll</string>") != std::string::npos);
		CHECK(plist.find("<key>CFBundleShortVersionString</key>\n\t<string>2.10.300</string>") != std::string::npos);
		// Apple documents both version keys as at most three numbers.
		CHECK(plist.find("<key>CFBundleVersion</key>\n\t<string>2.10.300</string>") != std::string::npos);
		CHECK(plist.find("2.10.300.4") == std::string::npos);
		CHECK(plist.find("<key>CFBundleIdentifier</key>\n\t<string>com.Stone-Works.Rock-Roll</string>") != std::string::npos);
		CHECK(plist.find("Copyright \xC2\xA9 Stone &lt;Works&gt;") != std::string::npos);
		CHECK(plist.find("CFBundleIconFile") == std::string::npos);
		CHECK(plist.find('\r') == std::string::npos);

		info.IconFileName = "Rock_Roll.icns";
		info.Company.clear();
		info.Name = "Bell\x07";
		plist = BuildInfoPlist(info);
		CHECK(plist.find("<key>CFBundleIconFile</key>\n\t<string>Rock_Roll.icns</string>") != std::string::npos);
		CHECK(plist.find("NSHumanReadableCopyright") == std::string::npos);
		// Control characters are not allowed in XML 1.0.
		CHECK(plist.find("<string>Bell</string>") != std::string::npos);
	}

	TEST_CASE("Desktop entries are single-line and escaped")
	{
		PackageInfo info = TestPackageInfo();
		info.Name = " Back\\slash\nNewline";
		const std::string entry = BuildDesktopEntry(info);
		CHECK(entry.find("Name=\\sBack\\\\slashNewline\n") != std::string::npos);
		CHECK(entry.find("Exec=Rock_Roll\n") != std::string::npos);
		CHECK(entry.find("Icon=Rock_Roll\n") != std::string::npos);
		CHECK(entry.find("X-AppImage-Version=2.10.300.4\n") != std::string::npos);
		CHECK(entry.starts_with("[Desktop Entry]\nType=Application\n"));
	}

	TEST_CASE("icns files hold PNG elements up to the source size")
	{
		const auto elements = [](const std::vector<uint8_t>& icns) {
			REQUIRE(icns.size() >= 8);
			CHECK(std::string(icns.begin(), icns.begin() + 4) == "icns");
			CHECK(BigEndian32(icns, 4) == icns.size());
			std::vector<std::pair<std::string, int>> found;
			for (size_t offset = 8; offset < icns.size();)
			{
				REQUIRE(offset + 8 <= icns.size());
				const uint32_t length = BigEndian32(icns, offset + 4);
				REQUIRE(length > 8);
				REQUIRE(offset + length <= icns.size());
				const auto [width, height] = PngSize(&icns[offset + 8], length - 8);
				CHECK(width == height);
				found.emplace_back(std::string(icns.begin() + static_cast<std::ptrdiff_t>(offset), icns.begin() + static_cast<std::ptrdiff_t>(offset + 4)), width);
				offset += length;
			}
			return found;
		};
		using Elements = std::vector<std::pair<std::string, int>>;
		CHECK(elements(*BuildIcns(SolidIcon(1024, 600))) == Elements{ { "icp4", 16 }, { "icp5", 32 }, { "ic11", 32 }, { "ic12", 64 }, { "ic07", 128 }, { "ic08", 256 }, { "ic13", 256 }, { "ic09", 512 }, { "ic14", 512 }, { "ic10", 1024 } });
		CHECK(elements(*BuildIcns(SolidIcon(100, 100))) == Elements{ { "icp4", 16 }, { "icp5", 32 }, { "ic11", 32 }, { "ic12", 64 } });
		// Never empty, even for a tiny source.
		CHECK(elements(*BuildIcns(SolidIcon(8, 8))) == Elements{ { "icp4", 16 } });
	}

	TEST_CASE("Windows icon resources list one PNG image per size")
	{
		const auto built = BuildWindowsIcon(SolidIcon(300, 300));
		REQUIRE(built);
		const WindowsIconResources& resources = *built;
		const std::vector<uint8_t>& directory = resources.GroupDirectory;
		const std::vector<int> sizes = { 16, 24, 32, 48, 64, 128, 256 };
		REQUIRE(resources.Images.size() == sizes.size());
		REQUIRE(directory.size() == 6 + 14 * sizes.size());
		CHECK(Word(directory, 0) == 0);
		CHECK(Word(directory, 2) == 1);
		CHECK(Word(directory, 4) == sizes.size());
		for (size_t i = 0; i < sizes.size(); ++i)
		{
			const size_t entry = 6 + 14 * i;
			// 256 is stored as 0 in the byte-sized fields.
			CHECK(directory[entry] == (sizes[i] == 256 ? 0 : sizes[i]));
			CHECK(directory[entry + 1] == directory[entry]);
			CHECK(Word(directory, entry + 4) == 1);
			CHECK(Word(directory, entry + 6) == 32);
			CHECK(DWord(directory, entry + 8) == resources.Images[i].size());
			CHECK(Word(directory, entry + 12) == WindowsIconResources::FirstIconID + i);
			CHECK(PngSize(resources.Images[i].data(), resources.Images[i].size()) == std::pair{ sizes[i], sizes[i] });
		}
		CHECK(BuildWindowsIcon(SolidIcon(40, 40))->Images.size() == 3);
	}

	TEST_CASE("Version resources hold the fixed versions and the string table")
	{
		const std::vector<uint8_t> bytes = BuildVersionInfo(TestPackageInfo());
		const VersionBlock root = ParseVersionBlock(bytes, 0);
		CHECK(Word(bytes, 0) == bytes.size());
		CHECK(root.Key == u"VS_VERSION_INFO");
		CHECK(root.Type == 0);
		REQUIRE(root.Value.size() == 52);
		CHECK(DWord(root.Value, 0) == 0xFEEF04BD);
		CHECK(DWord(root.Value, 8) == ((2u << 16) | 10u));    // file version, most significant
		CHECK(DWord(root.Value, 12) == ((300u << 16) | 4u));  // file version, least significant
		CHECK(DWord(root.Value, 16) == DWord(root.Value, 8)); // product version
		CHECK(DWord(root.Value, 20) == DWord(root.Value, 12));
		CHECK(DWord(root.Value, 36) == 0x1); // VFT_APP

		const auto strings = VersionStrings(root);
		CHECK(strings.at(u"CompanyName") == u"Stone <Works>");
		CHECK(strings.at(u"FileDescription") == u"Rock & Roll");
		CHECK(strings.at(u"ProductName") == u"Rock & Roll");
		CHECK(strings.at(u"FileVersion") == u"2.10.300.4");
		CHECK(strings.at(u"ProductVersion") == u"2.10.300.4");
		CHECK(strings.at(u"InternalName") == u"Rock_Roll");
		CHECK(strings.at(u"OriginalFilename") == u"Rock_Roll.exe");
		CHECK(strings.at(u"LegalCopyright") == u"Copyright © Stone <Works>");

		const VersionBlock* variables = root.Find(u"VarFileInfo");
		REQUIRE(variables);
		const VersionBlock* translation = variables->Find(u"Translation");
		REQUIRE(translation);
		CHECK(translation->Value == std::vector<uint8_t>{ 0x09, 0x04, 0xB0, 0x04 });

		// Text beyond the Basic Multilingual Plane becomes a surrogate pair; malformed UTF-8 replacement characters.
		PackageInfo info = TestPackageInfo();
		info.Company = "Rocket \xF0\x9F\x9A\x80 \xC3";
		info.Name = "Caf\xC3\xA9";
		const auto unicode = VersionStrings(ParseVersionBlock(BuildVersionInfo(info), 0));
		CHECK(unicode.at(u"CompanyName") == u"Rocket \U0001F680 �");
		CHECK(unicode.at(u"ProductName") == u"Café");
		// Each byte that starts no valid sequence becomes U+FFFD, and what follows it decodes normally: an overlong NUL, an
		// encoded surrogate, a value past U+10FFFF, a lone continuation byte and a truncated sequence.
		info.Company = "A\xC0\x80"
					   "B\xED\xA0\x80"
					   "C\xF4\x90\x80\x80"
					   "D\x80"
					   "E\xE6\x97"
					   "F";
		const auto malformed = VersionStrings(ParseVersionBlock(BuildVersionInfo(info), 0));
		CHECK(malformed.at(u"CompanyName") == u"A\uFFFD\uFFFDB\uFFFD\uFFFD\uFFFDC\uFFFD\uFFFD\uFFFD\uFFFDD\uFFFDE\uFFFD\uFFFDF");
		// Without a company there is no company or copyright string.
		info.Company.clear();
		const auto anonymous = VersionStrings(ParseVersionBlock(BuildVersionInfo(info), 0));
		CHECK(anonymous.count(u"CompanyName") == 0);
		CHECK(anonymous.count(u"LegalCopyright") == 0);
	}

	TEST_CASE("Package exports read the version and icon before writing anything")
	{
		BasaltTest::TempProject temp("PackagingExportChecks");
		const Ref<Project>& project = Project::GetActive();
		temp.WriteFile("Assets/Scenes/Main.bscene", R"({"Entities":[]})");
		temp.WriteFile("FakeRuntime", "binary");
		project->GetConfig().StartScene = "Assets/Scenes/Main.bscene";
		const std::filesystem::path output = temp.GetDirectory().parent_path() / "PackagingExportOut";
		std::error_code error;
		std::filesystem::remove_all(output, error);
		// A previous package export of this game (either platform's layout), which a refused export must leave alone.
		const std::filesystem::path app = output / "PackagingExportChecks.app";
		const std::filesystem::path appDir = output / "PackagingExportChecks.AppDir";
		for (const std::filesystem::path& projectDirectory : { app / "Contents" / "Resources", appDir })
		{
			std::filesystem::create_directories(projectDirectory);
			REQUIRE(FileSystem::WriteTextFile(projectDirectory / Project::FileName, "{}"));
			REQUIRE(FileSystem::WriteTextFile(projectDirectory / "Sentinel.txt", "previous export"));
		}
		const auto untouched = [&]() {
			return FileSystem::Exists(app / "Contents" / "Resources" / "Sentinel.txt") && FileSystem::Exists(appDir / "Sentinel.txt");
		};

		ExportOptions options;
		options.RuntimeExecutable = temp.GetDirectory() / "FakeRuntime";
		options.Package = true;
		project->GetConfig().Version = "1";
		ExportResult result = Exporter::Export(*project, output, options);
		CHECK_FALSE(result.Success);
		CHECK(result.Error.find("Version '1' must be") != std::string::npos);
		CHECK(result.PackagePath.empty());
		CHECK(untouched());

		project->GetConfig().Version = "1.0.0";
		temp.WriteFile("Assets/Textures/Broken.png", "not a png");
		project->GetConfig().Icon = "Assets/Textures/Broken.png";
		result = Exporter::Export(*project, output, options);
		CHECK(result.Error.find("is not a PNG file") != std::string::npos);
		CHECK(untouched());

		project->GetConfig().Icon = "asset-uuid://123/Assets/Textures/Gone.png";
		result = Exporter::Export(*project, output, options);
		CHECK(result.Error.find("the project icon is an") != std::string::npos);
		CHECK(untouched());

		// A folder export never reads them.
		options.Package = false;
		project->GetConfig().Version = "1";
		const std::filesystem::path folder = temp.GetDirectory().parent_path() / "PackagingExportFolder";
		std::filesystem::remove_all(folder, error);
		result = Exporter::Export(*project, folder, options);
#if defined(__APPLE__)
		// macOS exports also need the Vulkan libraries, which a test machine may lack; nothing else may fail.
		if (!result.Success)
			CHECK_MESSAGE(BasaltTest::Contains(result.Error, "libMoltenVK"), result.Error);
#else
		CHECK_MESSAGE(result.Success, result.Error);
		CHECK(result.PackagePath == folder);
#endif
		std::filesystem::remove_all(folder, error);
		std::filesystem::remove_all(output, error);
	}

	TEST_CASE("Only an earlier export of the same game may be replaced")
	{
		BasaltTest::TempProject temp("PackagingPrevious");
		const Ref<Project>& project = Project::GetActive();
		temp.WriteFile("Assets/Scenes/Main.bscene", R"({"Entities":[]})");
		temp.WriteFile("FakeRuntime", "binary");
		project->GetConfig().StartScene = "Assets/Scenes/Main.bscene";
		const std::filesystem::path output = temp.GetDirectory().parent_path() / "PackagingPreviousOut";
		std::error_code error;
		std::filesystem::remove_all(output, error);
		ExportOptions options;
		options.RuntimeExecutable = temp.GetDirectory() / "FakeRuntime";

		// Another game's package (a folder of downloaded games) is not an export of this one: its Assets survive.
		std::filesystem::create_directories(output / "Other.app" / "Contents" / "Resources");
		REQUIRE(FileSystem::WriteTextFile(output / "Other.app" / "Contents" / "Resources" / Project::FileName, "{}"));
		REQUIRE(FileSystem::WriteTextFile(output / "Assets" / "Precious.txt", "keep"));
		ExportResult result = Exporter::Export(*project, output, options);
		CHECK(BasaltTest::Contains(result.Error, "not a previous export of this game"));
		CHECK(FileSystem::Exists(output / "Assets" / "Precious.txt"));

		// This game's own package is.
		std::filesystem::create_directories(output / "PackagingPrevious.AppDir");
		REQUIRE(FileSystem::WriteTextFile(output / "PackagingPrevious.AppDir" / Project::FileName, "{}"));
		result = Exporter::Export(*project, output, options);
		CHECK_FALSE(BasaltTest::Contains(result.Error, "not a previous export"));
		std::filesystem::remove_all(output, error);
	}

#if defined(__linux__)
	TEST_CASE("Linux packages become AppImages through appimagetool")
	{
		BasaltTest::TempProject temp("AppRun");
		const Ref<Project>& project = Project::GetActive();
		temp.WriteFile("Assets/Scenes/Main.bscene", R"({"Entities":[]})");
		temp.WriteFile("FakeRuntime", "binary");
		project->GetConfig().StartScene = "Assets/Scenes/Main.bscene";
		project->GetConfig().Version = "2.1.0";
		const std::filesystem::path output = temp.GetDirectory().parent_path() / "PackagingAppImageOut";
		std::error_code error;
		std::filesystem::remove_all(output, error);
		std::filesystem::create_directories(output);
		// An older version's AppImage of this game goes; another game's ("AppRun-Demo") stays.
		REQUIRE(FileSystem::WriteTextFile(output / "AppRun-1.9.0-x86_64.AppImage", "old"));
		REQUIRE(FileSystem::WriteTextFile(output / "AppRun-Demo-1.0.0-x86_64.AppImage", "other game"));
		REQUIRE(FileSystem::WriteTextFile(output / "AppRun.AppDir" / Project::FileName, "{}"));

		// Records its arguments and environment, then writes the AppImage it was asked for.
		const std::filesystem::path tool = temp.GetDirectory().parent_path() / "fake-appimagetool";
		const std::filesystem::path record = temp.GetDirectory().parent_path() / "fake-appimagetool.log";
		REQUIRE(FileSystem::WriteTextFile(tool, "#!/bin/sh\necho \"$ARCH $APPIMAGE_EXTRACT_AND_RUN $*\" > \"" + record.string() + "\"\nfor last; do :; done\necho image > \"$last\"\n"));
		std::filesystem::permissions(tool, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
		ExportOptions options;
		options.RuntimeExecutable = temp.GetDirectory() / "FakeRuntime";
		options.Package = true;
		options.AppImageTool = tool;
		ExportResult result = Exporter::Export(*project, output, options);
		REQUIRE_MESSAGE(result.Success, result.Error);
		const std::filesystem::path appDir = output / "AppRun.AppDir";
	#if defined(__aarch64__)
		const std::string architecture = "aarch64";
	#else
		const std::string architecture = "x86_64";
	#endif
		CHECK(result.PackagePath == output / ("AppRun-2.1.0-" + architecture + ".AppImage"));
		CHECK(FileSystem::ReadTextFile(result.PackagePath) == "image\n");
		CHECK(FileSystem::ReadTextFile(record) == architecture + " 1 --no-appstream " + appDir.string() + " " + result.PackagePath.string() + "\n");
		CHECK_FALSE(std::filesystem::exists(output / "AppRun-1.9.0-x86_64.AppImage"));
		CHECK(std::filesystem::exists(output / "AppRun-Demo-1.0.0-x86_64.AppImage"));
		// A game named AppRun is its own entry point: the executable, not a link to itself.
		CHECK(std::filesystem::is_regular_file(std::filesystem::symlink_status(appDir / "AppRun")));
		CHECK(std::filesystem::read_symlink(appDir / ".DirIcon") == "AppRun.png");
		// Without a project icon the AppImage gets Basalt's, and says so.
		REQUIRE(result.Warnings.size() == 1);
		CHECK(BasaltTest::Contains(result.Warnings[0], "uses Basalt's default icon"));
		int width = 0;
		int height = 0;
		int channels = 0;
		CHECK(stbi_info((appDir / "AppRun.png").string().c_str(), &width, &height, &channels));
		CHECK(width == 256);

		// A failing appimagetool fails the export with its output.
		REQUIRE(FileSystem::WriteTextFile(tool, "#!/bin/sh\necho broken runtime\nexit 3\n"));
		result = Exporter::Export(*project, output, options);
		CHECK_FALSE(result.Success);
		CHECK(BasaltTest::Contains(result.Error, "appimagetool failed (exit code 3): broken runtime"));
		std::filesystem::remove_all(output, error);
	}
#endif

	TEST_CASE("Exported runtimes find the project next to them or in their bundle's resources")
	{
		BasaltTest::TempDirectory directory("PackagingProjectLookup");
		const std::filesystem::path macOS = directory.GetPath() / "Game.app" / "Contents" / "MacOS";
		const std::filesystem::path resources = directory.GetPath() / "Game.app" / "Contents" / "Resources";
		std::filesystem::create_directories(macOS);
		std::filesystem::create_directories(resources);
		CHECK(Exporter::GetExportedProjectDirectory(macOS) == macOS);
		FileSystem::WriteTextFile(resources / Project::FileName, "{}");
		CHECK(Exporter::GetExportedProjectDirectory(macOS) == resources);
		// A project next to the executable wins (a --project-less run of a folder export).
		FileSystem::WriteTextFile(macOS / Project::FileName, "{}");
		CHECK(Exporter::GetExportedProjectDirectory(macOS) == macOS);
		// Only a bundle's MacOS directory looks at ../Resources.
		const std::filesystem::path plain = directory.GetPath() / "Plain" / "bin";
		std::filesystem::create_directories(plain);
		std::filesystem::create_directories(plain.parent_path() / "Resources");
		FileSystem::WriteTextFile(plain.parent_path() / "Resources" / Project::FileName, "{}");
		CHECK(Exporter::GetExportedProjectDirectory(plain) == plain);
	}

	TEST_CASE("Game.GetInfo returns the project's name, version and company")
	{
		BasaltTest::TempProject temp("PackagingLua");
		Project::GetActive()->GetConfig().Version = "4.5.6";
		Project::GetActive()->GetConfig().Company = "Studio";
		Scene scene;
		scene.OnRuntimeStart();
		CHECK(BasaltTest::Eval(scene, "local info = Game.GetInfo() return info.Name .. '|' .. info.Version .. '|' .. info.Company") == "PackagingLua|4.5.6|Studio");
	}

	TEST_CASE("Game.GetInfo returns the defaults without a project")
	{
		Project::SetActive(nullptr);
		Scene scene;
		scene.OnRuntimeStart();
		CHECK(BasaltTest::Eval(scene, "local info = Game.GetInfo() return info.Name .. '|' .. info.Version .. '|' .. info.Company") == "Untitled|1.0.0|");
	}

	TEST_CASE("Windows resources are written only on Windows")
	{
		const PackageInfo info = TestPackageInfo();
		BasaltTest::TempDirectory directory("PackagingWindowsResources");
		std::string error;
#if defined(_WIN32)
		// Any PE file takes resources; the test executable is one.
		const std::filesystem::path executable = directory.GetPath() / "Game.exe";
		wchar_t self[MAX_PATH] = {};
		REQUIRE(GetModuleFileNameW(nullptr, self, MAX_PATH) > 0);
		std::filesystem::copy_file(self, executable);
		const IconImage icon = SolidIcon(64, 64);
		REQUIRE_MESSAGE(WriteWindowsResources(executable, info, &icon, error), error);

		HMODULE module = LoadLibraryExW(executable.wstring().c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
		REQUIRE(module);
		const auto resource = [&](LPCWSTR type, LPCWSTR name) {
			HRSRC found = FindResourceW(module, name, type);
			REQUIRE(found);
			const auto* data = static_cast<const uint8_t*>(LockResource(LoadResource(module, found)));
			return std::vector<uint8_t>(data, data + SizeofResource(module, found));
		};
		// Wide resource types: the RT_* macros are narrow strings without UNICODE.
		CHECK(resource(MAKEINTRESOURCEW(16), MAKEINTRESOURCEW(1)) == BuildVersionInfo(info));
		const auto expected = BuildWindowsIcon(icon);
		REQUIRE(expected);
		CHECK(resource(MAKEINTRESOURCEW(14), L"GLFW_ICON") == expected->GroupDirectory);
		for (size_t i = 0; i < expected->Images.size(); ++i)
			CHECK(resource(MAKEINTRESOURCEW(3), MAKEINTRESOURCEW(WindowsIconResources::FirstIconID + i)) == expected->Images[i]);
		FreeLibrary(module);
#else
		CHECK_FALSE(WriteWindowsResources(directory.GetPath() / "Game.exe", info, nullptr, error));
		CHECK(error.find("only Windows") != std::string::npos);
#endif
	}
}
