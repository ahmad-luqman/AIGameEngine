#include <doctest/doctest.h>

#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/ImageUtils.h>
#include <Basalt/Asset/TextureSource.h>
#include <Basalt/Asset/MeshImporter.h>
#include <Basalt/Scene/Scene.h>

#include "TestUtils.h"

#include <stb_image_write.h>

#include <cstring>
#include <glm/gtc/epsilon.hpp>

using namespace Basalt;

namespace {

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
			{ { "byteLength", buffer.size() }, { "uri", "data:application/octet-stream;base64," + BasaltTest::Base64(buffer.data(), buffer.size()) } },
			{ { { "uri", "data:image/png;base64," + BasaltTest::Base64(redPng.data(), redPng.size()) } }, { { "uri", "normal.png" } } });
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

	// Regression: external URIs in a glTF were resolved without checks, so a model could make the engine
	// read any file (absolute paths, '..' escapes, file: URIs).
	TEST_CASE("glTF external files must stay inside the project")
	{
		BasaltTest::TempProject project("GltfSandbox");
		const std::string png = MakePng(0xFF0000FF);
		project.WriteFile("Assets/Textures/ok.png", png);
		project.WriteFile("Assets/Models/sub/ok.png", png);
		const std::string buffer = MakeQuadBuffer();
		const nlohmann::json embedded = { { "byteLength", buffer.size() }, { "uri", "data:application/octet-stream;base64," + BasaltTest::Base64(buffer.data(), buffer.size()) } };

		auto load = [&](const std::string& name, const nlohmann::json& bufferJson, const std::string& imageUri) {
			project.WriteFile("Assets/Models/" + name, MakeGltfJson(bufferJson, { { { "uri", imageUri } }, { { "uri", imageUri } } }).dump());
			AssetManager::Clear();
			return AssetManager::GetMesh("Assets/Models/" + name) != nullptr;
		};

		// Relative paths anywhere inside the project are fine, including '..' that stays inside.
		CHECK(load("A.gltf", embedded, "sub/ok.png"));
		CHECK(load("B.gltf", embedded, "../Textures/ok.png"));
		CHECK(load("C.gltf", embedded, "sub%2Fok.png")); // percent-encoded

		CHECK_FALSE(load("D.gltf", embedded, "../../../../../../../../etc/hosts"));
		CHECK(AssetManager::GetError("Assets/Models/D.gltf").find("outside the project") != std::string::npos);
		CHECK_FALSE(load("E.gltf", embedded, "/etc/hosts"));
		CHECK_FALSE(load("F.gltf", embedded, "file:///etc/hosts"));
		CHECK_FALSE(load("G.gltf", embedded, "C:/Windows/win.ini"));
		CHECK_FALSE(load("H.gltf", embedded, "..%2F..%2F..%2F..%2F..%2F..%2Fetc%2Fhosts"));
		CHECK_FALSE(load("I.gltf", { { "byteLength", buffer.size() }, { "uri", "../../../../../../../../etc/hosts" } }, "sub/ok.png"));
		AssetManager::Clear();
	}

	// Regression (FuzzGltf): cgltf_validate read 16-bit indices through a misaligned pointer when an accessor's
	// byte offset was odd. The glTF spec forbids that, so such files are rejected before validation.
	TEST_CASE("glTF accessors must be aligned to their component size")
	{
		BasaltTest::TempProject project("GltfAlignment");
		const std::string buffer = MakeQuadBuffer() + std::string(4, '\0');
		nlohmann::json gltf = MakeGltfJson({ { "byteLength", buffer.size() }, { "uri", "data:application/octet-stream;base64," + BasaltTest::Base64(buffer.data(), buffer.size()) } }, nlohmann::json::array());
		gltf.erase("textures");
		gltf["materials"][0]["pbrMetallicRoughness"].erase("baseColorTexture");
		gltf["materials"][0].erase("normalTexture");
		gltf["images"] = nlohmann::json::array();
		project.WriteFile("Assets/Models/Aligned.gltf", gltf.dump());
		gltf["accessors"][2]["byteOffset"] = 1; // uint16 indices starting at an odd address
		project.WriteFile("Assets/Models/Misaligned.gltf", gltf.dump());

		AssetManager::Clear();
		CHECK_MESSAGE(AssetManager::GetMesh("Assets/Models/Aligned.gltf"), AssetManager::GetError("Assets/Models/Aligned.gltf"));
		CHECK_FALSE(AssetManager::GetMesh("Assets/Models/Misaligned.gltf"));
		CHECK(AssetManager::GetError("Assets/Models/Misaligned.gltf").find("not aligned") != std::string::npos);
		AssetManager::Clear();
	}

	// Regression: a 4-byte header could claim a gigantic image and make stb_image allocate gigabytes.
	TEST_CASE("Images larger than the dimension limit are rejected before allocating")
	{
		// Minimal TGA header (uncompressed true-color) claiming 65535 x 65535 pixels, with no pixel data.
		const uint8_t header[18] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 32, 8 };
		std::string error;
		CHECK_FALSE(TextureSource::LoadFromMemory(header, sizeof(header), error));
		CHECK_FALSE(error.empty());
	}

	// Regression (FuzzTexture, CI): stb_image accepts a Radiance header whose width parses as 0 and still
	// returns a buffer; copying the empty result passed a null pointer to memcpy (UB on glibc).
	TEST_CASE("Images without pixels are rejected")
	{
		static const uint8_t zeroWidthHdr[] = {
			0x23,
			0x3F,
			0x52,
			0x41,
			0x44,
			0x49,
			0x41,
			0x4E,
			0x43,
			0x45,
			0x0A,
			0x46,
			0x4F,
			0x52,
			0x4D,
			0x41,
			0x54,
			0x3D,
			0x33,
			0x32,
			0x2D,
			0x62,
			0x69,
			0x74,
			0x5F,
			0x72,
			0x6C,
			0x65,
			0x5F,
			0x72,
			0x67,
			0x62,
			0x65,
			0x0A,
			0x0A,
			0x2D,
			0x59,
			0x20,
			0x32,
			0x20,
			0x2B,
			0x58,
			0x20,
			0x20,
			0x81,
			0x0A,
			0x32,
			0x40,
			0x80,
			0x80,
			0x20,
			0x08,
			0x04,
			0x41,
			0xCA,
			0xA1,
			0xFF,
			0xA2,
			0x68,
			0x43,
			0x3B,
			0xC0,
			0x58,
			0xD8,
			0xC0,
			0x90,
			0xC0,
			0x8D,
			0x6E,
			0x3E,
			0xAA,
			0x00,
			0x00,
			0x00,
			0x00,
			0x8B,
			0x01,
			0xA6,
			0x7F,
			0xF0,
			0x75,
			0x0F,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x6C,
			0x06,
			0x50,
			0x82,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x80,
			0x3D,
			0x40,
			0x50,
			0x82,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x80,
			0x3D,
			0x40,
			0x09,
			0x02,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0xF6,
			0x00,
			0x25,
			0x08,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0xD8,
			0x03,
			0x94,
			0x20,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x60,
			0x0F,
			0x50,
			0x82,
			0x01,
			0x00,
			0x20,
			0x81,
			0x80,
			0x40,
			0x20,
			0x81,
		};
		std::string error;
		CHECK_FALSE(TextureSource::LoadFromMemory(zeroWidthHdr, sizeof(zeroWidthHdr), error));
		CHECK(error == "image has no pixels");
	}

	// Regression (FuzzTexture): a BMP with height INT32_MIN made stb_image evaluate abs(INT32_MIN), which
	// overflows. Rejected before decoding; other heights (including negative, top-down BMPs) still load.
	TEST_CASE("BMP headers with an unrepresentable height are rejected")
	{
		static const uint8_t intMinHeight[] = {
			0x42,
			0x4D,
			0x46,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x36,
			0x00,
			0x00,
			0x00,
			0x00,
			0x28,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x80,
			0x01,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0xFF,
			0xFF,
			0xFF,
			0xFF,
			0xFF,
		};
		std::string error;
		CHECK_FALSE(TextureSource::LoadFromMemory(intMinHeight, sizeof(intMinHeight), error));
		CHECK(error == "invalid BMP height");

		// A valid 2x2 top-down BMP (negative height) is unaffected.
		std::vector<uint8_t> bmp = { 'B', 'M' };
		auto put32 = [&bmp](uint32_t value) {
			for (int i = 0; i < 4; i++)
				bmp.push_back(static_cast<uint8_t>(value >> (8 * i)));
		};
		auto put16 = [&bmp](uint16_t value) {
			bmp.push_back(static_cast<uint8_t>(value));
			bmp.push_back(static_cast<uint8_t>(value >> 8));
		};
		put32(14 + 40 + 16);
		put32(0);
		put32(14 + 40);
		put32(40);
		put32(2);
		put32(static_cast<uint32_t>(-2));
		put16(1);
		put16(32);
		for (int i = 0; i < 6; i++)
			put32(0);
		for (int i = 0; i < 16; i++)
			bmp.push_back(0x80);
		Ref<TextureSource> image = TextureSource::LoadFromMemory(bmp.data(), bmp.size(), error);
		REQUIRE_MESSAGE(image, error);
		CHECK(image->Width == 2);
		CHECK(image->Height == 2);
	}

	// Regression (FuzzTexture, CI): a PNG whose first IDAT chunk is empty made stb_image call
	// memcpy(NULL, src, 0), which glibc declares invalid; the ASan+UBSan job aborted on such images.
	TEST_CASE("PNGs with an empty first IDAT chunk decode without undefined behaviour")
	{
		static const uint8_t emptyFirstIdat[] = {
			0x89,
			0x50,
			0x4E,
			0x47,
			0x0D,
			0x0A,
			0x1A,
			0x0A,
			0x00,
			0x00,
			0x00,
			0x0D,
			0x49,
			0x48,
			0x44,
			0x52,
			0x00,
			0x00,
			0x00,
			0x40,
			0x00,
			0x00,
			0x00,
			0x40,
			0x08,
			0x06,
			0x00,
			0x00,
			0x00,
			0xAA,
			0x69,
			0x71,
			0xDE,
			0x00,
			0x00,
			0x00,
			0x00,
			0x49,
			0x44,
			0x41,
			0x54,
			0x78,
			0xDA,
			0xED,
			0xD8,
			0x00,
			0xC0,
			0x58,
			0xD8,
			0xC0,
			0x90,
			0xC0,
			0x8D,
			0x6E,
			0x3E,
			0xDE,
			0x00,
			0x00,
			0x00,
			0x7D,
			0x00,
			0x6C,
			0x06,
			0x50,
			0x82,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x80,
			0x3D,
			0x40,
			0x50,
			0x4E,
			0x47,
			0x0D,
			0x0A,
			0x1A,
			0x0A,
			0x00,
			0x00,
			0x00,
			0x00,
			0x40,
			0xFF,
			0xFF,
			0xFF,
			0xFF,
			0xFF,
			0xFF,
			0xFF,
			0xFF,
			0xFF,
			0xFF,
			0x08,
			0x06,
			0x00,
			0x00,
			0x0F,
			0x60,
			0x00,
			0x50,
			0x82,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x80,
			0x3D,
			0x40,
			0x09,
			0x09,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			0xCF,
			0x00,
			0x2E,
			0xA9,
			0x76,
			0x42,
			0xB1,
			0x11,
			0x00,
			0x4F,
			0xED,
			0x00,
			0x00,
			0x00,
			0xA9,
			0x76,
			0x42,
			0xB1,
			0x11,
			0x00,
			0x4F,
			0xED,
			0x00,
			0x00,
			0x00,
			0x00,
			0x49,
			0x45,
			0x4E,
			0x44,
			0xAE,
			0x42,
			0x60,
			0x82,
		};
		std::string error;
		Ref<TextureSource> image = TextureSource::LoadFromMemory(emptyFirstIdat, sizeof(emptyFirstIdat), error);
		// The image data itself is corrupt; what matters is a clean failure (or result), not the outcome.
		CHECK((image != nullptr || !error.empty()));
	}

	TEST_CASE("CompareImages measures per-pixel differences against a tolerance")
	{
		// 10x10 gray image; tests change individual pixels to probe the thresholds.
		auto makeImage = [](uint8_t value) {
			TextureSource image;
			image.Width = 10;
			image.Height = 10;
			image.Pixels.assign(size_t{ 10 } * 10 * 4, value);
			return image;
		};
		const TextureSource reference = makeImage(100);

		ImageCompareOptions options;
		options.PixelThreshold = 8;
		options.MaxDifferingPercent = 1.0;

		ImageCompareResult same = CompareImages(reference, reference, options, true);
		CHECK(same.SizeMatches);
		CHECK(same.Matches);
		CHECK(same.MaxDelta == 0);
		CHECK(same.DifferingPercent == 0.0);
		REQUIRE(same.DiffImage.size() == 10 * 10 * 4);
		CHECK(same.DiffImage[0] == 0);

		// A delta equal to the threshold does not count; one above it does.
		TextureSource image = makeImage(100);
		image.Pixels[0] = 108;
		CHECK(CompareImages(image, reference, options, false).DifferingPercent == 0.0);
		image.Pixels[0] = 109;
		ImageCompareResult onePixel = CompareImages(image, reference, options, true);
		CHECK(onePixel.MaxDelta == 9);
		CHECK(onePixel.DifferingPercent == doctest::Approx(1.0));
		CHECK(onePixel.Matches); // exactly at MaxDifferingPercent
		CHECK(onePixel.DiffImage[0] == 255);
		CHECK(onePixel.DiffImage[1] == 0); // differing pixels are red

		// A second differing pixel exceeds 1% of 100 pixels.
		image.Pixels[4 + 2] = 0;
		ImageCompareResult twoPixels = CompareImages(image, reference, options, false);
		CHECK(twoPixels.DifferingPercent == doctest::Approx(2.0));
		CHECK_FALSE(twoPixels.Matches);
		CHECK(twoPixels.MeanDelta == doctest::Approx((9.0 + 100.0) / 100.0));

		// Alpha is ignored: captures are opaque, but a reference may have been saved with alpha.
		TextureSource alpha = makeImage(100);
		alpha.Pixels[3] = 0;
		CHECK(CompareImages(alpha, reference, options, false).MaxDelta == 0);
	}

	TEST_CASE("CompareImages never matches images of different sizes")
	{
		TextureSource a;
		a.Width = 4;
		a.Height = 4;
		a.Pixels.assign(size_t{ 4 } * 4 * 4, 0);
		TextureSource b = a;
		b.Width = 2;
		b.Height = 8;
		const ImageCompareResult result = CompareImages(a, b, {}, true);
		CHECK_FALSE(result.SizeMatches);
		CHECK_FALSE(result.Matches);
		CHECK(result.DiffImage.empty());

		TextureSource hdr = a;
		hdr.Format = TextureFormat::RGBA32F;
		CHECK_FALSE(CompareImages(hdr, a, {}, false).Matches);
	}
}
