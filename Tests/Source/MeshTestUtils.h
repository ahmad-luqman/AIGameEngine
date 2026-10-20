#pragma once

#include <Basalt/Asset/MeshSource.h>
#include <Basalt/Core/FileSystem.h>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace BasaltTest {

	// A dense static glTF with real interleaved attributes, exercising the import path before publication.
	inline std::filesystem::path WriteSphereGltf(const std::filesystem::path& root, bool blend = false)
	{
		const auto mesh = Basalt::Primitives::CreateSphere(32, 16);
		const auto vertexBytes = mesh->Vertices.size() * sizeof(Basalt::Vertex);
		std::string buffer(reinterpret_cast<const char*>(mesh->Vertices.data()), vertexBytes);
		buffer.append(reinterpret_cast<const char*>(mesh->Indices.data()), mesh->Indices.size() * sizeof(uint32_t));
		REQUIRE(Basalt::FileSystem::WriteBinaryFile(root / "Sphere.bin", buffer.data(), buffer.size()));
		using json = nlohmann::json;
		const json data = {
			{ "asset", { { "version", "2.0" } } },
			{ "buffers", { { { "uri", "Sphere.bin" }, { "byteLength", buffer.size() } } } },
			{ "bufferViews", { { { "buffer", 0 }, { "byteLength", vertexBytes }, { "byteStride", sizeof(Basalt::Vertex) } }, { { "buffer", 0 }, { "byteOffset", vertexBytes }, { "byteLength", mesh->Indices.size() * sizeof(uint32_t) } } } },
			{ "accessors", { { { "bufferView", 0 }, { "byteOffset", 0 }, { "componentType", 5126 }, { "count", mesh->Vertices.size() }, { "type", "VEC3" } }, { { "bufferView", 0 }, { "byteOffset", 12 }, { "componentType", 5126 }, { "count", mesh->Vertices.size() }, { "type", "VEC3" } }, { { "bufferView", 0 }, { "byteOffset", 24 }, { "componentType", 5126 }, { "count", mesh->Vertices.size() }, { "type", "VEC4" } }, { { "bufferView", 0 }, { "byteOffset", 40 }, { "componentType", 5126 }, { "count", mesh->Vertices.size() }, { "type", "VEC2" } }, { { "bufferView", 1 }, { "componentType", 5125 }, { "count", mesh->Indices.size() }, { "type", "SCALAR" } } } },
			{ "materials", { { { "alphaMode", blend ? "BLEND" : "OPAQUE" } } } },
			{ "meshes", { { { "primitives", { { { "attributes", { { "POSITION", 0 }, { "NORMAL", 1 }, { "TANGENT", 2 }, { "TEXCOORD_0", 3 } } }, { "indices", 4 }, { "material", 0 } } } } } } },
			{ "nodes", { { { "mesh", 0 } } } },
			{ "scenes", { { { "nodes", { 0 } } } } },
			{ "scene", 0 }
		};
		const auto path = root / "Sphere.gltf";
		REQUIRE(Basalt::FileSystem::WriteTextFile(path, data.dump()));
		return path;
	}

}
