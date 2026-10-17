#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	enum class EditorTheme : uint8_t
	{
		Dark,
		Light
	};

	// The editor settings that belong to the user rather than to a project, kept across sessions: the fly camera's
	// speed, the theme, the recently opened projects and whether changed files reload by themselves. Stored as JSON in
	// the per-user data directory (GetDirectory), next to ImGui's window layout (LayoutFileName). Every value is kept
	// valid: setters clamp or reject, and files and patches are checked against Schema before anything changes.
	// Headless; the editor applies the values and editor.preferences reads and patches them.
	class EditorPreferences
	{
	public:
		// As written in the schema (doubles: the float 0.1 widened is 0.10000000149..., above a JSON 0.1).
		static constexpr double CameraSpeedLimits[2] = { 0.1, 10.0 };
		static constexpr float MinCameraSpeed = static_cast<float>(CameraSpeedLimits[0]);
		static constexpr float MaxCameraSpeed = static_cast<float>(CameraSpeedLimits[1]);
		static constexpr size_t MaxRecentProjects = 10;
		static constexpr const char* FileName = "Preferences.json";
		static constexpr const char* LayoutFileName = "imgui.ini";

		// The camera's fly speed multiplier, in [MinCameraSpeed, MaxCameraSpeed].
		float GetCameraSpeed() const { return m_CameraSpeed; }
		// Clamped into range; a non-finite value is ignored.
		void SetCameraSpeed(float speed) { m_CameraSpeed = ClampCameraSpeed(speed, m_CameraSpeed); }
		// `speed` clamped into range, or `fallback` (clamped too) when it is not finite. The editor camera keeps the same
		// range.
		static float ClampCameraSpeed(float speed, float fallback);
		EditorTheme GetTheme() const { return m_Theme; }
		void SetTheme(EditorTheme theme) { m_Theme = theme; }
		// Project directories, most recently opened first, without duplicates, at most MaxRecentProjects.
		const std::vector<std::string>& GetRecentProjects() const { return m_RecentProjects; }
		// Moves a project directory to the front (made absolute and normal; trailing separators do not make another
		// entry), dropping the oldest beyond MaxRecentProjects.
		void AddRecentProject(const std::filesystem::path& directory);
		void ClearRecentProjects() { m_RecentProjects.clear(); }
		bool GetAutoReload() const { return m_AutoReload; }
		void SetAutoReload(bool enabled) { m_AutoReload = enabled; }

		// { cameraSpeed, theme, recentProjects, autoReload }: the file format and editor.preferences's result.
		nlohmann::json ToJson() const;
		// What a file or a patch may contain (every key optional, no others).
		static nlohmann::json Schema();
		// Applies the keys of `patch`: recentProjects replaces the list (its entries made absolute, de-duplicated and
		// capped at MaxRecentProjects). Nothing changes when any key is invalid.
		bool Apply(const nlohmann::json& patch, std::string& outError);

		// "Dark" or "Light".
		static const char* ThemeName(EditorTheme theme);
		static std::optional<EditorTheme> ThemeFromName(const std::string& name);

		// <user data directory>/BasaltEditor (Platform::GetUserDataDirectory, so BASALT_USER_DATA_DIR moves it).
		static std::optional<std::filesystem::path> GetDirectory(std::string& outError);
		enum class LoadStatus : uint8_t
		{
			Loaded,
			// No file yet: the defaults.
			Missing,
			// The file could not be read (it may be fine): the defaults, and outError.
			Unreadable,
			// The file holds no valid preferences (including keys this version does not know): the defaults, and
			// outError.
			Invalid
		};
		// The preferences in `file`, or the defaults when it is missing, unreadable or invalid (outStatus says which).
		static EditorPreferences Load(const std::filesystem::path& file, LoadStatus& outStatus, std::string& outError);
		// Moves a file Load found Invalid to "<file>.invalid" (replacing an older one), so saving the defaults does not
		// destroy what the user had. Returns the new path, or nullopt with the reason.
		static std::optional<std::filesystem::path> SetAside(const std::filesystem::path& file, std::string& outError);
		// Writes atomically (FileSystem::WriteTextFile), creating the directory.
		bool Save(const std::filesystem::path& file, std::string& outError) const;

		bool operator==(const EditorPreferences&) const = default;

	private:
		float m_CameraSpeed = 1.0f;
		EditorTheme m_Theme = EditorTheme::Dark;
		std::vector<std::string> m_RecentProjects;
		bool m_AutoReload = true;
	};

}
