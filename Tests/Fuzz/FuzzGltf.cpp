// glTF / GLB import. The importer reads files (external buffers and images are resolved relative to the
// model), so each input is written to a scratch file inside a scratch project.
#include "FuzzCommon.h"

#include <Basalt/Asset/MeshImporter.h>
#include <Basalt/Core/FileSystem.h>
#include <Basalt/Project/Project.h>

#include <filesystem>
#include <string>
#include <unistd.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	// Per-process directory: libFuzzer may run several workers in parallel (-jobs/-workers).
	static const std::filesystem::path directory = std::filesystem::temp_directory_path() / ("BasaltFuzzGltf-" + std::to_string(getpid()));
	static const bool projectReady = [] {
		std::string error;
		Basalt::Ref<Basalt::Project> project = Basalt::Project::Create(directory, "FuzzGltf", error);
		if (!project)
			return false;
		Basalt::Project::SetActive(project);
		return true;
	}();
	if (!projectReady)
		return 0;

	// The leading byte picks the container so both the JSON (.gltf) and binary (.glb) parsers get input.
	if (size == 0)
		return 0;
	const bool binary = (data[0] & 1) != 0;
	const std::filesystem::path path = directory / (binary ? "Fuzz.glb" : "Fuzz.gltf");
	Basalt::FileSystem::WriteBinaryFile(path, data + 1, size - 1);
	std::string error;
	Basalt::MeshImporter::LoadGltf(path, binary ? "Fuzz.glb" : "Fuzz.gltf", error);
	return 0;
}
