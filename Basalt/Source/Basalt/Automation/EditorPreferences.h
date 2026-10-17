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
		static constexpr float MinCameraSpeed = 0.1f;
		static constexpr float MaxCameraSpeed = 10.0f;
		static constexpr size_t MaxRecentProjects = 10;
		static constexpr const char* FileName = "Preferences.json";
		static constexpr const char* LayoutFileName = "imgui.ini";

		// The camera's fly speed multiplier, in [MinCameraSpeed, MaxCameraSpeed].
		float GetCameraSpeed() const { return m_CameraSpeed; }
		// Clamped into range; a non-finite value is ignored.
		void SetCameraSpeed(float speed);
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
		// Applies the keys of `patch` (recentProjects replaces the list). Nothing changes when any key is invalid.
		bool Apply(const nlohmann::json& patch, std::string& outError);

		// "Dark" or "Light".
		static const char* ThemeName(EditorTheme theme);
		static std::optional<EditorTheme> ThemeFromName(const std::string& name);

		// <user data directory>/BasaltEditor (Platform::GetUserDataDirectory, so BASALT_USER_DATA_DIR moves it).
		static std::optional<std::filesystem::path> GetDirectory(std::string& outError);
		// The preferences in `file`. A missing file gives the defaults without an error; a file that cannot be read
		// or holds invalid preferences gives the defaults and sets outError.
		static EditorPreferences Load(const std::filesystem::path& file, std::string& outError);
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
