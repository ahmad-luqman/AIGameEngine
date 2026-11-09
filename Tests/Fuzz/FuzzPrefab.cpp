// Prefab files as read from disk: nested records and their overrides expand against a prefab in a scratch project, and
// what was accepted goes through the same steps saving it would take.
#include "FuzzCommon.h"

#include <Breccia/Core/FileSystem.h>
#include <Breccia/Project/Project.h>
#include <Breccia/Scene/Prefab.h>
#include <Breccia/Scene/SceneSerializer.h>

#include <cstdlib>
#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

	// Inputs can reach a real nested prefab (Tests/Fuzz/Corpus/FuzzPrefab names it). The directory is per process: fuzz
	// jobs run side by side.
	std::filesystem::path MakeProject()
	{
		std::error_code error;
		const auto directory = std::filesystem::temp_directory_path(error) / ("BrecciaFuzzPrefab-" + std::to_string(std::random_device{}()));
		std::string createError;
		Breccia::Project::SetActive(Breccia::Project::Create(directory, "Fuzz", createError));
		Breccia::FileSystem::WriteTextFile(directory / "Assets/Prefabs/Base.bprefab", R"({ "Format": "BrecciaPrefab", "Entities": [
			{ "ID": 1, "Name": "Base", "Parent": 0, "Components": { "PointLight": { "Intensity": 1 } } },
			{ "ID": 2, "Name": "Child", "Parent": 1, "Components": { "BoneAttachment": { "Target": 1 } } }
		] })");
		Breccia::FileSystem::WriteTextFile(directory / "Assets/Prefabs/Nest.bprefab", R"({ "Format": "BrecciaPrefab", "Version": 4, "Entities": [
			{ "ID": 1, "Name": "Nest", "Parent": 0, "Components": {} },
			{ "ID": 2, "Parent": 1, "Components": { "Prefab": { "Prefab": "Assets/Prefabs/Base.bprefab" } } }
		] })");
		return directory;
	}

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BrecciaFuzz::Init();
	static const std::filesystem::path project = MakeProject();
	nlohmann::json entities;
	std::string error;
	Breccia::Prefab::ReadReport report;
	const std::filesystem::path path = project / "Assets/Prefabs/Fuzz.bprefab";
	if (!Breccia::Prefab::ReadText(BrecciaFuzz::AsText(data, size), path, entities, error, &report))
		return 0;
	if (!Breccia::Prefab::Normalize(entities, "Fuzz.bprefab", error))
		return 0;
	nlohmann::json file;
	std::vector<std::string> warnings;
	const auto root = entities[0]["Components"].contains("Prefab") ? Breccia::PrefabFileRoot::Variant : Breccia::PrefabFileRoot::Plain;
	if (!Breccia::Prefab::ToFileEntities(entities, path, root, file, warnings, error))
		return 0;
	// What a prefab reads as, it writes as the same entities, and what it writes it can read: an ID lost here is an entity
	// lost from every instance.
	const nlohmann::json document = { { "Format", Breccia::SceneSerializer::PrefabFormatName }, { "Version", Breccia::SceneSerializer::FormatVersion }, { "Entities", std::move(file) } };
	nlohmann::json reread;
	if (!Breccia::Prefab::ReadText(document.dump(), path, reread, error) || !Breccia::Prefab::Normalize(reread, "Fuzz.bprefab", error))
		abort();
	const auto ids = [](const nlohmann::json& records) {
		std::set<uint64_t> found;
		for (const nlohmann::json& record : records)
			found.insert(record["ID"].get<uint64_t>());
		return found;
	};
	if (ids(entities) != ids(reread))
		abort();
	return 0;
}
