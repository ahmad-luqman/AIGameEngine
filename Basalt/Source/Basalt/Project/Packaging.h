#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	class Project;
	struct ProjectConfig;

	// RGBA8 pixels, top row first.
	struct IconImage
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<uint8_t> Pixels;

		// Nonempty, with one RGBA pixel per Width x Height. Results of failed operations are not.
		bool IsValid() const { return Width > 0 && Height > 0 && Pixels.size() == static_cast<size_t>(Width) * Height * 4; }
	};

	// Largest icon side LoadIconImage accepts. Platform icons stop at 1024 px; larger sources only cost memory.
	constexpr uint32_t MaxIconSize = 4096;
	// Below this side an icon looks blurry in docks, taskbars and file managers (ProjectValidator warns).
	constexpr uint32_t RecommendedIconSize = 256;

	// Reads a PNG icon. nullopt, with the reason, when the file cannot be read, is not a PNG, is larger than MaxIconSize
	// or cannot be decoded.
	std::optional<IconImage> LoadIconImage(const std::filesystem::path& path, std::string& outError);
	// The project's icon (ProjectConfig::Icon) into outIcon. True with outIcon empty when the project has none; false with
	// the reason when one is set but cannot be loaded (its file is gone, unreadable, not a PNG or too large).
	bool LoadProjectIcon(const Project& project, std::optional<IconImage>& outIcon, std::string& outError);
	// A valid icon centered on a transparent square (never stretched) and scaled to size x size; an invalid image when
	// scaling fails (out of memory).
	IconImage ResizeIcon(const IconImage& icon, uint32_t size);
	// PNG file bytes of an icon; empty when the image is invalid or encoding fails (out of memory).
	std::vector<uint8_t> EncodeIconPng(const IconImage& icon);
	// Basalt's icon for packages that need one when the project has none (an AppImage must have an icon).
	IconImage MakeDefaultIcon(uint32_t size);

	// What a packaged export says about the game, derived from the project by MakePackageInfo.
	struct PackageInfo
	{
		std::string Name;
		// The executable's file name without extension (the exporter's sanitized project name).
		std::string ExecutableName;
		// The project's Version and its numbers (ParseProjectVersion), which always agree.
		std::string Version;
		std::array<uint16_t, 4> VersionNumbers{};
		std::string Company;
		// Reverse-DNS identifier (macOS CFBundleIdentifier) from Company and Name.
		std::string BundleIdentifier;
		// The icon's file name inside the package (Contents/Resources on macOS), set by the exporter when it writes one;
		// empty when the package has none.
		std::string IconFileName;
	};

	// nullopt, with the reason, when the project's Version does not parse (ParseProjectVersion) or its Name or Company is
	// longer than ProjectConfig::MaxTextLength (ValidateProjectText).
	std::optional<PackageInfo> MakePackageInfo(const ProjectConfig& config, const std::string& executableName, std::string& outError);
	// "com.<company>.<name>": each part keeps its ASCII letters and digits, with one hyphen for each inner run of anything
	// else ("basalt" for a company without any, "game" for such a name).
	std::string MakeBundleIdentifier(const std::string& company, const std::string& name);

	// macOS Contents/Info.plist (XML).
	std::string BuildInfoPlist(const PackageInfo& info);
	// macOS .icns: PNG representations from 16 to 1024 px, none larger than the source (but at least 16 px). nullopt
	// when an image cannot be scaled or encoded (out of memory).
	std::optional<std::vector<uint8_t>> BuildIcns(const IconImage& icon);
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
	// Sizes from 16 to 256 px, none larger than the source (but at least 16 px). nullopt when an image cannot be scaled
	// or encoded (out of memory).
	std::optional<WindowsIconResources> BuildWindowsIcon(const IconImage& icon);
	// The VS_VERSIONINFO resource (RT_VERSION): fixed file and product versions plus the U.S. English string table
	// Explorer's Details tab shows (company, description, versions, copyright, file names).
	std::vector<uint8_t> BuildVersionInfo(const PackageInfo& info);
	// Writes the version resource of a Windows executable and, when icon is given, its icon (other resources are kept).
	// The icon group is named GLFW_ICON, which GLFW's Win32 backend loads as the window class icon. Only available on
	// Windows (false with a reason elsewhere).
	bool WriteWindowsResources(const std::filesystem::path& executable, const PackageInfo& info, const IconImage* icon, std::string& outError);

}
