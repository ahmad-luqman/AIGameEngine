#pragma once

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Project/Project.h>

#include <cstddef>
#include <cstdint>
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

	// Standard base64 (with padding), for data: URIs in generated glTF files.
	inline std::string Base64(const void* data, size_t size)
	{
		static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		const auto* bytes = static_cast<const uint8_t*>(data);
		std::string out;
		for (size_t i = 0; i < size; i += 3)
		{
			const uint32_t chunk = (bytes[i] << 16) | ((i + 1 < size ? bytes[i + 1] : 0) << 8) | (i + 2 < size ? bytes[i + 2] : 0);
			out += alphabet[(chunk >> 18) & 63];
			out += alphabet[(chunk >> 12) & 63];
			out += i + 1 < size ? alphabet[(chunk >> 6) & 63] : '=';
			out += i + 2 < size ? alphabet[chunk & 63] : '=';
		}
		return out;
	}

}
