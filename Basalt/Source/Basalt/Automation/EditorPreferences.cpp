#include "Basalt/Automation/EditorPreferences.h"

#include "Basalt/Automation/CommandSchema.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Platform.h"

#include <algorithm>
#include <cmath>

namespace Basalt {

	namespace {

		// Absolute and normal, without a trailing separator ("/a/b/" and "/a/b" are one project).
		std::filesystem::path NormalProjectPath(const std::filesystem::path& directory)
		{
			std::error_code error;
			std::filesystem::path path = std::filesystem::absolute(directory, error);
			if (error)
				path = directory;
			path = path.lexically_normal();
			if (!path.has_filename() && path.has_parent_path() && path != path.root_path())
				path = path.parent_path();
			return path;
		}

	}

	float EditorPreferences::ClampCameraSpeed(float speed, float fallback)
	{
		return std::isfinite(speed) ? std::clamp(speed, MinCameraSpeed, MaxCameraSpeed) : fallback;
	}

	void EditorPreferences::AddRecentProject(const std::filesystem::path& directory)
	{
		if (directory.empty())
			return;
		const std::filesystem::path path = NormalProjectPath(directory);
		std::erase_if(m_RecentProjects, [&](const std::string& entry) { return NormalProjectPath(FileSystem::PathFromUtf8(entry)) == path; });
		m_RecentProjects.insert(m_RecentProjects.begin(), FileSystem::PathToUtf8(path));
		if (m_RecentProjects.size() > MaxRecentProjects)
			m_RecentProjects.resize(MaxRecentProjects);
	}

	nlohmann::json EditorPreferences::ToJson() const
	{
		return { { "cameraSpeed", m_CameraSpeed }, { "theme", ThemeName(m_Theme) }, { "recentProjects", m_RecentProjects }, { "autoReload", m_AutoReload } };
	}

	nlohmann::json EditorPreferences::Schema()
	{
		namespace S = CommandSchema;
		nlohmann::json theme = S::String("Dark or Light");
		theme["enum"] = { ThemeName(EditorTheme::Dark), ThemeName(EditorTheme::Light) };
		const nlohmann::json recent = S::Array("project directories, most recent first", S::String("", true));
		// The limits as written (0.1, not the float 0.1 widened to 0.10000000149...), so the documented minimum passes.
		return S::Object({ { "cameraSpeed", S::Number("fly speed multiplier", 0.1, 10.0) }, { "theme", theme }, { "recentProjects", recent }, { "autoReload", S::Boolean("reload files changed on disk (and hot reload scripts in Play)") } });
	}

	bool EditorPreferences::Apply(const nlohmann::json& patch, std::string& outError)
	{
		if (!CommandSchema::Validate(Schema(), patch, outError))
			return false;
		// Validated against the schema above; changes go to a copy, so a later rejection (cameraSpeed) leaves nothing
		// applied.
		EditorPreferences next = *this;
		if (const auto it = patch.find("cameraSpeed"); it != patch.end())
		{
			float speed = 0.0f;
			if (!JsonToFloat(*it, speed))
			{
				outError = "cameraSpeed: not a finite number";
				return false;
			}
			next.SetCameraSpeed(speed);
		}
		if (const auto it = patch.find("theme"); it != patch.end())
		{
			const std::optional<EditorTheme> theme = ThemeFromName(it->get<std::string>());
			BS_CORE_ASSERT(theme, "the schema only lets theme names through");
			next.SetTheme(theme.value_or(EditorTheme::Dark));
		}
		if (const auto it = patch.find("recentProjects"); it != patch.end())
		{
			next.m_RecentProjects.clear();
			// Added oldest first, so the first entry ends up in front.
			for (auto entry = it->rbegin(); entry != it->rend(); ++entry)
				next.AddRecentProject(FileSystem::PathFromUtf8(entry->get<std::string>()));
		}
		if (const auto it = patch.find("autoReload"); it != patch.end())
			next.SetAutoReload(it->get<bool>());
		*this = std::move(next);
		return true;
	}

	const char* EditorPreferences::ThemeName(EditorTheme theme)
	{
		return theme == EditorTheme::Light ? "Light" : "Dark";
	}

	std::optional<EditorTheme> EditorPreferences::ThemeFromName(const std::string& name)
	{
		if (name == "Dark")
			return EditorTheme::Dark;
		if (name == "Light")
			return EditorTheme::Light;
		return std::nullopt;
	}

	std::optional<std::filesystem::path> EditorPreferences::GetDirectory(std::string& outError)
	{
		const auto base = Platform::GetUserDataDirectory(outError);
		if (!base)
			return std::nullopt;
		return *base / "BasaltEditor";
	}

	EditorPreferences EditorPreferences::Load(const std::filesystem::path& file, std::string& outError)
	{
		outError.clear();
		EditorPreferences preferences;
		std::error_code error;
		if (!std::filesystem::exists(file, error) && !error)
			return preferences;
		const auto text = error ? std::nullopt : FileSystem::ReadTextFile(file);
		if (!text)
		{
			outError = "cannot read '" + FileSystem::PathToUtf8(file) + "'" + (error ? ": " + error.message() : std::string());
			return preferences;
		}
		std::string parseError;
		const nlohmann::json data = ParseJson(*text, parseError);
		if (data.is_discarded())
		{
			outError = FileSystem::PathToUtf8(file) + ": " + parseError;
			return preferences;
		}
		if (!preferences.Apply(data, parseError))
		{
			outError = FileSystem::PathToUtf8(file) + ": " + parseError;
			return EditorPreferences();
		}
		return preferences;
	}

	std::optional<std::filesystem::path> EditorPreferences::SetAside(const std::filesystem::path& file, std::string& outError)
	{
		std::filesystem::path backup = file;
		backup += ".invalid";
		std::error_code error;
		std::filesystem::rename(file, backup, error);
		if (error)
		{
			outError = "cannot rename '" + FileSystem::PathToUtf8(file) + "': " + error.message();
			return std::nullopt;
		}
		return backup;
	}

	bool EditorPreferences::Save(const std::filesystem::path& file, std::string& outError) const
	{
		return FileSystem::WriteTextFile(file, ToJson().dump(1, '\t') + "\n", outError);
	}

}
