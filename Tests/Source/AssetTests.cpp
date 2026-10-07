#include <doctest/doctest.h>

#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/MeshImporter.h>
#include <Basalt/Scene/Scene.h>

#include "TestUtils.h"

#include <stb_image_write.h>

#include <cstring>
#include <glm/gtc/epsilon.hpp>

using namespace Basalt;

namespace {

	std::string Base64(const void* data, size_t size)
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

	std::string MakePng(uint32_t rgba)
	{
		const uint8_t pixel[4] = { static_cast<uint8_t>(rgba >> 24), static_cast<uint8_t>(rgba >> 16), static_cast<uint8_t>(rgba >> 8), static_cast<uint8_t>(rgba) };
		uint8_t pixels[16];
		for (int i = 0; i < 4; i++)
			std::memcpy(pixels + static_cast<ptrdiff_t>(i) * 4, pixel, 4);
		std::string png;
		stbi_write_png_to_func([](void* context, void* data, int size) { static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<size_t>(size)); }, &png, 2, 2, 4, pixels, 8);
		return png;
	}

	// Quad geometry: 4 positions (no normals), 4 UVs, 6 uint16 indices.
	std::string MakeQuadBuffer()
	{
		const float positions[12] = { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0 };
		const float uvs[8] = { 0, 1, 1, 1, 1, 0, 0, 0 };
		const uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
		std::string buffer;
		buffer.append(reinterpret_cast<const char*>(positions), sizeof(positions));
		buffer.append(reinterpret_cast<const char*>(uvs), sizeof(uvs));
		buffer.append(reinterpret_cast<const char*>(indices), sizeof(indices));
		return buffer;
	}

	nlohmann::json MakeGltfJson(const nlohmann::json& buffer, const nlohmann::json& images)
	{
		return {
			{ "asset", { { "version", "2.0" } } },
			{ "buffers", { buffer } },
			{ "bufferViews", { { { "buffer", 0 }, { "byteOffset", 0 }, { "byteLength", 48 } }, { { "buffer", 0 }, { "byteOffset", 48 }, { "byteLength", 32 } }, { { "buffer", 0 }, { "byteOffset", 80 }, { "byteLength", 12 } } } },
			{ "accessors", {
							   { { "bufferView", 0 }, { "componentType", 5126 }, { "count", 4 }, { "type", "VEC3" }, { "min", { 0, 0, 0 } }, { "max", { 1, 1, 0 } } },
							   { { "bufferView", 1 }, { "componentType", 5126 }, { "count", 4 }, { "type", "VEC2" } },
							   { { "bufferView", 2 }, { "componentType", 5123 }, { "count", 6 }, { "type", "SCALAR" } },
						   } },
			{ "images", images },
			{ "textures", { { { "source", 0 } }, { { "source", 1 } } } },
			{ "materials", { { { "name", "Painted" }, { "pbrMetallicRoughness", { { "baseColorFactor", { 1.0, 0.5, 0.25, 1.0 } }, { "metallicFactor", 0.8 }, { "roughnessFactor", 0.3 }, { "baseColorTexture", { { "index", 0 } } } } }, { "normalTexture", { { "index", 1 } } }, { "emissiveFactor", { 1.0, 0.0, 0.0 } } } } },
			{ "meshes", { { { "name", "QuadMesh" }, { "primitives", { { { "attributes", { { "POSITION", 0 }, { "TEXCOORD_0", 1 } } }, { "indices", 2 }, { "material", 0 } } } } } } },
			{ "nodes", { { { "name", "Root" }, { "children", { 1 } } }, { { "name", "Quad" }, { "mesh", 0 }, { "translation", { 1.0, 2.0, 3.0 } } } } },
			{ "scenes", { { { "nodes", { 0 } } } } },
			{ "scene", 0 },
		};
	}

}

TEST_SUITE("Assets")
{
	TEST_CASE("Built-in primitives have consistent geometry, normals and bounds")
	{
		for (const char* key : { "builtin://Cube", "builtin://Sphere", "builtin://Plane", "builtin://Cylinder", "builtin://Capsule" })
		{
			INFO(key);
			Ref<MeshSource> mesh = Primitives::CreateFromKey(key);
			REQUIRE(mesh);
			CHECK(mesh->Indices.size() % 3 == 0);
			REQUIRE(mesh->Meshes.size() == 1);
			for (uint32_t index : mesh->Indices)
				REQUIRE(index < mesh->Vertices.size());
			for (const Vertex& vertex : mesh->Vertices)
			{
				CHECK(glm::length(vertex.Normal) == doctest::Approx(1.0f).epsilon(1e-3));
				CHECK(glm::abs(glm::dot(glm::vec3(vertex.Tangent), vertex.Normal)) < 1e-3f);
			}
			CHECK(mesh->Bounds.IsValid());
		}

		Ref<MeshSource> cube = Primitives::CreateCube();
		CHECK(glm::all(glm::epsilonEqual(cube->Bounds.Min, glm::vec3(-0.5f), 1e-5f)));
		CHECK(glm::all(glm::epsilonEqual(cube->Bounds.Max, glm::vec3(0.5f), 1e-5f)));
		Ref<MeshSource> capsule = Primitives::CreateCapsule();
		CHECK(capsule->Bounds.Max.y == doctest::Approx(1.0f));

		// Every cube triangle winds counter-clockwise when seen from outside.
		for (size_t i = 0; i < cube->Indices.size(); i += 3)
		{
			const Vertex& a = cube->Vertices[cube->Indices[i]];
			const Vertex& b = cube->Vertices[cube->Indices[i + 1]];
			const Vertex& c = cube->Vertices[cube->Indices[i + 2]];
			const glm::vec3 faceNormal = glm::cross(b.Position - a.Position, c.Position - a.Position);
			CHECK(glm::dot(faceNormal, a.Normal) > 0.0f);
		}
		CHECK_FALSE(Primitives::CreateFromKey("builtin://Teapot"));
	}

	TEST_CASE("glTF import: geometry, materials, embedded and external textures, hierarchy")
	{
		BasaltTest::TempProject project("GltfImport");
		const std::string redPng = MakePng(0xFF0000FF);
		const std::string bluePng = MakePng(0x0000FFFF);
		project.WriteFile("Assets/Models/normal.png", bluePng);

		const std::string buffer = MakeQuadBuffer();
		const nlohmann::json gltf = MakeGltfJson(
			{ { "byteLength", buffer.size() }, { "uri", "data:application/octet-stream;base64," + Base64(buffer.data(), buffer.size()) } },
			{ { { "uri", "data:image/png;base64," + Base64(redPng.data(), redPng.size()) } }, { { "uri", "normal.png" } } });
		project.WriteFile("Assets/Models/Quad.gltf", gltf.dump());

		AssetManager::Clear();
		Ref<MeshSource> mesh = AssetManager::GetMesh("Assets/Models/Quad.gltf");
		REQUIRE_MESSAGE(mesh, AssetManager::GetError("Assets/Models/Quad.gltf"));
		CHECK(mesh->Vertices.size() == 4);
		CHECK(mesh->Indices.size() == 6);
		REQUIRE(mesh->Meshes.size() == 1);
		CHECK(mesh->Meshes[0].Name == "QuadMesh");
		// Normals were generated (+Z for this quad) since the file has none.
		CHECK(mesh->Vertices[0].Normal.z == doctest::Approx(1.0f));
		CHECK(mesh->Bounds.Max.x == doctest::Approx(1.0f));

		REQUIRE(mesh->Materials.size() == 2);
		const MaterialData& material = mesh->Materials[0];
		CHECK(material.Name == "Painted");
		CHECK(material.AlbedoColor.g == doctest::Approx(0.5f));
		CHECK(material.Metallic == doctest::Approx(0.8f));
		CHECK(material.Roughness == doctest::Approx(0.3f));
		CHECK(material.EmissiveColor.r == doctest::Approx(1.0f));
		CHECK(material.AlbedoMap == "Assets/Models/Quad.gltf#image/0");
		CHECK(material.NormalMap == "Assets/Models/normal.png");

		Ref<TextureSource> albedo = AssetManager::GetTexture(material.AlbedoMap);
		REQUIRE_MESSAGE(albedo, AssetManager::GetError(material.AlbedoMap));
		CHECK(albedo->Width == 2);
		CHECK(albedo->Pixels[0] == 255);
		CHECK(albedo->Pixels[2] == 0);
		Ref<TextureSource> normal = AssetManager::GetTexture(material.NormalMap);
		REQUIRE(normal);
		CHECK(normal->Pixels[2] == 255);

		// Cached: same object on the second request.
		CHECK(AssetManager::GetMesh("Assets/Models/Quad.gltf") == mesh);

		Scene scene;
		std::string error;
		Entity root = AssetManager::ImportModel(scene, "Assets/Models/Quad.gltf", {}, error);
		REQUIRE_MESSAGE(root, error);
		CHECK(root.GetName() == "Quad");
		REQUIRE(root.GetChildren().size() == 1);
		Entity gltfRoot = root.GetChildren()[0];
		CHECK(gltfRoot.GetName() == "Root");
		REQUIRE(gltfRoot.GetChildren().size() == 1);
		Entity quad = gltfRoot.GetChildren()[0];
		CHECK(quad.GetTransform().Translation == glm::vec3(1.0f, 2.0f, 3.0f));
		REQUIRE(quad.HasComponent<MeshComponent>());
		CHECK(quad.GetComponent<MeshComponent>().Mesh == "Assets/Models/Quad.gltf");
		CHECK_FALSE(gltfRoot.HasComponent<MeshComponent>());
	}

	TEST_CASE("Binary glTF (.glb) with an image in a buffer view")
	{
		BasaltTest::TempProject project("GlbImport");
		std::string binary = MakeQuadBuffer();
		const std::string png = MakePng(0x00FF00FF);
		const size_t imageOffset = binary.size();
		binary += png;
		while (binary.size() % 4 != 0)
			binary += '\0';

		nlohmann::json gltf = MakeGltfJson({ { "byteLength", binary.size() } }, { { { "bufferView", 3 }, { "mimeType", "image/png" } }, { { "bufferView", 3 }, { "mimeType", "image/png" } } });
		gltf["bufferViews"].push_back({ { "buffer", 0 }, { "byteOffset", imageOffset }, { "byteLength", png.size() } });
		std::string json = gltf.dump();
		while (json.size() % 4 != 0)
			json += ' ';

		auto put32 = [](std::string& out, uint32_t value) { out.append(reinterpret_cast<const char*>(&value), 4); };
		std::string glb = "glTF";
		put32(glb, 2);
		put32(glb, static_cast<uint32_t>(12 + 8 + json.size() + 8 + binary.size()));
		put32(glb, static_cast<uint32_t>(json.size()));
		glb += "JSON";
		glb += json;
		put32(glb, static_cast<uint32_t>(binary.size()));
		glb.append("BIN\0", 4);
		glb += binary;
		project.WriteFile("Assets/Models/Quad.glb", glb);

		AssetManager::Clear();
		Ref<MeshSource> mesh = AssetManager::GetMesh("Assets/Models/Quad.glb");
		REQUIRE_MESSAGE(mesh, AssetManager::GetError("Assets/Models/Quad.glb"));
		CHECK(mesh->Indices.size() == 6);
		Ref<TextureSource> texture = AssetManager::GetTexture(mesh->Materials[0].AlbedoMap);
		REQUIRE_MESSAGE(texture, AssetManager::GetError(mesh->Materials[0].AlbedoMap));
		CHECK(texture->Pixels[1] == 255);
	}

	TEST_CASE("Asset failures are reported and cached until reloaded")
	{
		BasaltTest::TempProject project("AssetFailures");
		AssetManager::Clear();
		CHECK_FALSE(AssetManager::GetMesh("Assets/Models/Missing.gltf"));
		CHECK(AssetManager::GetError("Assets/Models/Missing.gltf").find("not found") != std::string::npos);
		CHECK_FALSE(AssetManager::GetMesh("Assets/Models/model.obj"));
		CHECK_FALSE(AssetManager::GetMesh("builtin://Teapot"));
		CHECK_FALSE(AssetManager::GetTexture("Assets/Textures/none.png"));
		CHECK_FALSE(AssetManager::GetTexture("Assets/Models/x.glb#image/abc"));

		project.WriteFile("Assets/Textures/bad.png", "not an image");
		CHECK_FALSE(AssetManager::GetTexture("Assets/Textures/bad.png"));
		CHECK_FALSE(AssetManager::GetError("Assets/Textures/bad.png").empty());

		project.WriteFile("Assets/Textures/bad.png", MakePng(0xFFFFFFFF));
		CHECK_FALSE(AssetManager::GetTexture("Assets/Textures/bad.png"));
		AssetManager::Reload("Assets/Textures/bad.png");
		CHECK(AssetManager::GetTexture("Assets/Textures/bad.png"));

		project.WriteFile("Assets/Models/Broken.gltf", R"({"asset":{"version":"2.0"},"meshes":[]})");
		CHECK_FALSE(AssetManager::GetMesh("Assets/Models/Broken.gltf"));
	}

	TEST_CASE("HDR images decode to float RGBA and asset listing filters by extension")
	{
		BasaltTest::TempProject project("HdrAndListing");
		const float pixels[2 * 1 * 3] = { 4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 0.125f };
		std::string hdr;
		stbi_write_hdr_to_func([](void* context, void* data, int size) { static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<size_t>(size)); }, &hdr, 2, 1, 3, pixels);
		project.WriteFile("Assets/Environments/Sky.hdr", hdr);
		project.WriteFile("Assets/Scripts/A.lua", "return {}");

		AssetManager::Clear();
		Ref<TextureSource> texture = AssetManager::GetTexture("Assets/Environments/Sky.hdr");
		REQUIRE(texture);
		CHECK(texture->Format == TextureFormat::RGBA32F);
		const float* values = reinterpret_cast<const float*>(texture->Pixels.data());
		CHECK(values[0] == doctest::Approx(4.0f).epsilon(0.02));
		CHECK(values[3] == doctest::Approx(1.0f));

		const auto all = AssetManager::ListAssets();
		CHECK(all.size() == 2);
		const auto scripts = AssetManager::ListAssets({ ".lua" });
		REQUIRE(scripts.size() == 1);
		CHECK(scripts[0] == "Assets/Scripts/A.lua");
		CHECK(AssetManager::IsTextureFile("x.HDR"));
		CHECK(AssetManager::IsMeshFile("Ship.GLB"));
	}
}
