// Prefab files as read from disk: nested records and their overrides expand against a prefab in a scratch project, and
// what was accepted goes through the same steps saving it would take.
#include "FuzzCommon.h"

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Scene/Prefab.h>

#include <filesystem>
#include <random>
#include <string>

namespace {

	// Inputs can reach a real nested prefab (Tests/Fuzz/Corpus/FuzzPrefab names it). The directory is per process: fuzz
	// jobs run side by side.
	std::filesystem::path MakeProject()
	{
		std::error_code error;
		const auto directory = std::filesystem::temp_directory_path(error) / ("BasaltFuzzPrefab-" + std::to_string(std::random_device{}()));
		std::string createError;
		Basalt::Project::SetActive(Basalt::Project::Create(directory, "Fuzz", createError));
		Basalt::FileSystem::WriteTextFile(directory / "Assets/Prefabs/Base.bprefab", R"({ "Format": "BasaltPrefab", "Entities": [
			{ "ID": 1, "Name": "Base", "Parent": 0, "Components": { "PointLight": { "Intensity": 1 } } },
			{ "ID": 2, "Name": "Child", "Parent": 1, "Components": { "BoneAttachment": { "Target": 1 } } }
		] })");
		return directory;
	}

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	static const std::filesystem::path project = MakeProject();
	nlohmann::json entities;
	std::string error;
	Basalt::Prefab::ReadReport report;
	if (!Basalt::Prefab::ReadText(BasaltFuzz::AsText(data, size), "Fuzz.bprefab", entities, error, &report))
		return 0;
	if (!Basalt::Prefab::Normalize(entities, "Fuzz.bprefab", error))
		return 0;
	nlohmann::json file;
	Basalt::Prefab::ToFileEntities(entities, project / "Assets/Prefabs/Fuzz.bprefab", entities[0]["Components"].contains("Prefab"), file, error);
	return 0;
}
