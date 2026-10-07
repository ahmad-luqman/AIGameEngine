#pragma once

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Project/Project.h>

#include <filesystem>
#include <string>

namespace BasaltTest {

	// Creates an empty, active project in a fresh temporary directory; removed and deactivated on scope exit.
	class TempProject
	{
	public:
		explicit TempProject(const std::string& name)
		{
			m_Directory = std::filesystem::temp_directory_path() / "BasaltTests" / name;
			std::filesystem::remove_all(m_Directory);
			std::string error;
			m_Project = Basalt::Project::Create(m_Directory, name, error);
			Basalt::Project::SetActive(m_Project);
		}

		~TempProject()
		{
			Basalt::Project::SetActive(nullptr);
			std::error_code error;
			std::filesystem::remove_all(m_Directory, error);
		}

		TempProject(const TempProject&) = delete;
		TempProject& operator=(const TempProject&) = delete;

		// Writes a project-relative file and returns its project-relative path.
		std::string WriteFile(const std::string& relativePath, const std::string& contents) const
		{
			Basalt::FileSystem::WriteTextFile(m_Directory / relativePath, contents);
			return relativePath;
		}

		const std::filesystem::path& GetDirectory() const { return m_Directory; }
		bool IsValid() const { return m_Project != nullptr; }

	private:
		std::filesystem::path m_Directory;
		Basalt::Ref<Basalt::Project> m_Project;
	};

}
