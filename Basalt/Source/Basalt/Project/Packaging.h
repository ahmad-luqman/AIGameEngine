#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	struct ProjectConfig;

	// RGBA8 pixels, top row first.
	struct IconImage
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<uint8_t> Pixels;
	};

	// Largest icon side LoadIconImage accepts. Platform icons stop at 1024 px; larger sources only cost memory.
	constexpr uint32_t MaxIconSize = 4096;
	// Below this side an icon looks blurry in docks, taskbars and file managers (ProjectValidator warns).
	constexpr uint32_t RecommendedIconSize = 256;

	// Reads a PNG icon (ProjectConfig::Icon). nullopt, with the reason, when the file cannot be read, is not a PNG or is
	// larger than MaxIconSize.
	std::optional<IconImage> LoadIconImage(const std::filesystem::path& path, std::string& outError);
	// The icon centered on a transparent square (never stretched) and scaled to size x size.
	IconImage ResizeIcon(const IconImage& icon, uint32_t size);
	// PNG file bytes of an icon.
	std::vector<uint8_t> EncodeIconPng(const IconImage& icon);
	// Basalt's icon for packages that need one when the project has none (an AppImage must have an icon).
	IconImage MakeDefaultIcon(uint32_t size);

	// What a packaged export says about the game, derived once from the project (MakePackageInfo).
	struct PackageInfo
	{
		std::string Name;
		// The executable's file name without extension (the exporter's sanitized project name).
		std::string ExecutableName;
		std::string Version;
		std::array<uint16_t, 4> VersionNumbers{};
		std::string Company;
		// Reverse-DNS identifier (macOS CFBundleIdentifier) from Company and Name.
		std::string BundleIdentifier;
		// The icon's file name inside the package (Contents/Resources on macOS); empty when the package has none.
		std::string IconFileName;
	};

	// nullopt, with the reason, when the project's Version does not parse (ParseProjectVersion).
	std::optional<PackageInfo> MakePackageInfo(const ProjectConfig& config, const std::string& executableName, std::string& outError);
	// "com.<company>.<name>", each part reduced to letters, digits and hyphens ("com.basalt.<name>" without a company).
	std::string MakeBundleIdentifier(const std::string& company, const std::string& name);

	// macOS Contents/Info.plist (XML).
	std::string BuildInfoPlist(const PackageInfo& info);
	// macOS .icns: PNG representations from 16 to 1024 px, none larger than the source (but at least 16 px).
	std::vector<uint8_t> BuildIcns(const IconImage& icon);
	// Linux .desktop entry for an AppImage's AppDir (the icon is named after the executable).
	std::string BuildDesktopEntry(const PackageInfo& info);

	// A Windows icon as resources: one RT_ICON per size (PNG data, ids FirstIconID upwards) and the RT_GROUP_ICON
	// directory that lists them.
	struct WindowsIconResources
	{
		static constexpr uint16_t FirstIconID = 1;
		std::vector<std::vector<uint8_t>> Images;
		std::vector<uint8_t> GroupDirectory;
	};
	// Sizes from 16 to 256 px, none larger than the source (but at least 16 px).
	WindowsIconResources BuildWindowsIcon(const IconImage& icon);
	// The VS_VERSIONINFO resource (RT_VERSION): fixed file and product versions plus the U.S. English string table
	// Explorer's Details tab shows (company, description, versions, copyright, file names).
	std::vector<uint8_t> BuildVersionInfo(const PackageInfo& info);
	// Replaces the icon and version resources of a Windows executable. The icon group is named GLFW_ICON, which GLFW
	// also uses as the window class icon. Only available on Windows (false with a reason elsewhere).
	bool WriteWindowsResources(const std::filesystem::path& executable, const PackageInfo& info, const IconImage* icon, std::string& outError);

}
