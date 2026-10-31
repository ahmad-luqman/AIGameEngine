#include <doctest/doctest.h>

#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Core/JobSystem.h>
#include <Basalt/Asset/ImageUtils.h>
#include <Basalt/Asset/TextureSource.h>
#include <Basalt/Asset/MeshImporter.h>
#include <Basalt/Asset/MeshOptimizer.h>
#include <Basalt/Asset/Thumbnails.h>
#include <Basalt/Scene/Scene.h>

#include "TestUtils.h"

#include <stb_image_write.h>

#include <cstring>
#include <limits>
#include <thread>
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

	// A buffer embedding the given bytes as a base64 data URI.
	nlohmann::json EmbeddedBufferJson(const std::string& buffer)
	{
		return { { "byteLength", buffer.size() }, { "uri", "data:application/octet-stream;base64," + BasaltTest::Base64(buffer.data(), buffer.size()) } };
	}

	// A binary glTF container. The caller pads the JSON chunk (or leaves it unpadded on purpose); no binary chunk when
	// binary is empty.
	std::string MakeGlb(const std::string& json, const std::string& binary)
	{
		auto put32 = [](std::string& out, uint32_t value) { out.append(reinterpret_cast<const char*>(&value), 4); };
		std::string glb = "glTF";
		put32(glb, 2);
		put32(glb, static_cast<uint32_t>(12 + 8 + json.size() + (binary.empty() ? 0 : 8 + binary.size())));
		put32(glb, static_cast<uint32_t>(json.size()));
		glb += "JSON";
		glb += json;
		if (!binary.empty())
		{
			put32(glb, static_cast<uint32_t>(binary.size()));
			glb.append("BIN\0", 4);
			glb += binary;
		}
		return glb;
	}

	// The quad without textures, for tests that only exercise buffers and accessors.
	nlohmann::json MakeUntexturedGltfJson(const nlohmann::json& buffer)
	{
		nlohmann::json gltf = MakeGltfJson(buffer, nlohmann::json::array());
		gltf.erase("textures");
		gltf["materials"][0]["pbrMetallicRoughness"].erase("baseColorTexture");
		gltf["materials"][0].erase("normalTexture");
		return gltf;
	}

}

TEST_SUITE("Assets")
{
	TEST_CASE("glTF extension materials retain independent texture transforms and UV1")
	{
		BasaltTest::TempProject project("GltfExtensions");
		auto binary = MakeQuadBuffer();
		const float uv1[] = { 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f };
		const auto uv1Offset = binary.size();
		binary.append(reinterpret_cast<const char*>(uv1), sizeof(uv1));
		project.WriteFile("Assets/quad.bin", binary);
		// One image per material slot (slot order), so a permuted slot table is caught.
		const char* slotImages[] = { "color.ktx2", "normal.png", "mr.png", "ao.png", "emissive.png", "coat.png", "coatrough.png", "coatnormal.png", "glass.png" };
		nlohmann::json images = nlohmann::json::array(), textures = nlohmann::json::array();
		for (size_t slot = 0; slot < MaterialTextureSlotCount; slot++)
		{
			images.push_back({ { "uri", slotImages[slot] } });
			textures.push_back({ { "source", slot } });
		}
		auto gltf = MakeGltfJson({ { "uri", "quad.bin" }, { "byteLength", binary.size() } }, images);
		gltf["textures"] = textures;
		gltf["bufferViews"].push_back({ { "buffer", 0 }, { "byteOffset", uv1Offset }, { "byteLength", sizeof(uv1) } });
		gltf["accessors"].push_back({ { "bufferView", 3 }, { "componentType", 5126 }, { "count", 4 }, { "type", "VEC2" } });
		gltf["extensionsRequired"] = { "KHR_texture_transform", "KHR_materials_emissive_strength", "KHR_materials_clearcoat", "KHR_materials_transmission", "KHR_texture_basisu", "KHR_mesh_quantization" };
		gltf["textures"][0] = { { "extensions", { { "KHR_texture_basisu", { { "source", 0 } } } } } };
		auto& material = gltf["materials"][0];
		material["extensions"] = {
			{ "KHR_materials_emissive_strength", { { "emissiveStrength", 3 } } },
			{ "KHR_materials_clearcoat", { { "clearcoatFactor", 0.8 }, { "clearcoatRoughnessFactor", 0.2 }, { "clearcoatTexture", { { "index", 5 } } }, { "clearcoatRoughnessTexture", { { "index", 6 } } }, { "clearcoatNormalTexture", { { "index", 7 }, { "scale", 0.5 } } } } },
			{ "KHR_materials_transmission", { { "transmissionFactor", 0.6 }, { "transmissionTexture", { { "index", 8 } } } } }
		};
		material["normalTexture"] = { { "index", 1 }, { "scale", 0.75 } };
		material["pbrMetallicRoughness"]["metallicRoughnessTexture"] = { { "index", 2 } };
		material["occlusionTexture"] = { { "index", 3 }, { "strength", 0.25 } };
		material["emissiveTexture"] = { { "index", 4 } };
		// Every slot gets its own offset; slot 0 also exercises the rest of the transform and UV1.
		nlohmann::json* views[] = { &material["pbrMetallicRoughness"]["baseColorTexture"], &material["normalTexture"], &material["pbrMetallicRoughness"]["metallicRoughnessTexture"], &material["occlusionTexture"], &material["emissiveTexture"], &material["extensions"]["KHR_materials_clearcoat"]["clearcoatTexture"], &material["extensions"]["KHR_materials_clearcoat"]["clearcoatRoughnessTexture"], &material["extensions"]["KHR_materials_clearcoat"]["clearcoatNormalTexture"], &material["extensions"]["KHR_materials_transmission"]["transmissionTexture"] };
		for (size_t slot = 1; slot < MaterialTextureSlotCount; slot++)
			(*views[slot])["extensions"]["KHR_texture_transform"] = { { "offset", { 0.5 + 0.0625 * double(slot), 0 } } };
		material["pbrMetallicRoughness"]["baseColorTexture"]["extensions"]["KHR_texture_transform"] = { { "offset", { 0.2, 0.3 } }, { "scale", { 2, -3 } }, { "rotation", 0.5 }, { "texCoord", 1 } };
		gltf["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_1"] = 3;
		project.WriteFile("Assets/quad.gltf", gltf.dump());
		std::string error;
		auto mesh = MeshImporter::LoadGltf(project.GetDirectory() / "Assets/quad.gltf", "Assets/quad.gltf", error);
		REQUIRE_MESSAGE(mesh, error);
		const auto& m = mesh->Materials[0];
		CHECK(m.Clearcoat == doctest::Approx(0.8));
		CHECK(m.ClearcoatRoughness == doctest::Approx(0.2));
		CHECK(m.Transmission == doctest::Approx(0.6));
		CHECK(m.EmissiveIntensity == 3);
		const std::string maps[] = { m.AlbedoMap, m.NormalMap, m.MetallicRoughnessMap, m.OcclusionMap, m.EmissiveMap, m.ClearcoatMap, m.ClearcoatRoughnessMap, m.ClearcoatNormalMap, m.TransmissionMap };
		for (size_t slot = 0; slot < MaterialTextureSlotCount; slot++)
		{
			INFO("slot " << slot);
			CHECK(maps[slot] == std::string("Assets/") + slotImages[slot]);
			if (slot > 0)
			{
				CHECK(m.TextureTransforms[slot].Offset == glm::vec2(0.5f + 0.0625f * float(slot), 0));
				CHECK(m.TextureTransforms[slot].Scale == glm::vec2(1));
				CHECK(m.TextureTransforms[slot].TexCoord == 0);
			}
		}
		CHECK(m.NormalScale == 0.75f);
		CHECK(m.OcclusionStrength == 0.25f);
		CHECK(m.ClearcoatNormalScale == 0.5f);
		CHECK(m.TextureTransforms[0].Offset == glm::vec2(0.2f, 0.3f));
		CHECK(m.TextureTransforms[0].Scale == glm::vec2(2, -3));
		CHECK(m.TextureTransforms[0].Rotation == 0.5f);
		CHECK(m.TextureTransforms[0].TexCoord == 1);
		// Each vertex keeps its own UV1, whatever order the optimizer leaves the vertices in.
		const glm::vec2 uv0[] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
		REQUIRE(mesh->Vertices.size() == 4);
		for (const auto& vertex : mesh->Vertices)
		{
			const auto source = std::find(std::begin(uv0), std::end(uv0), vertex.TexCoord);
			REQUIRE(source != std::end(uv0));
			const size_t index = static_cast<size_t>(source - std::begin(uv0));
			CHECK(vertex.TexCoord1 == glm::vec2(uv1[index * 2], uv1[index * 2 + 1]));
		}
		auto rejects = [&](const char* expected) {
			project.WriteFile("Assets/quad.gltf", gltf.dump());
			CHECK_FALSE(MeshImporter::LoadGltf(project.GetDirectory() / "Assets/quad.gltf", "Assets/quad.gltf", error));
			CHECK_MESSAGE(error.find(expected) != std::string::npos, error);
		};
		gltf["meshes"][0]["primitives"][0]["attributes"].erase("TEXCOORD_1");
		rejects("material 'Painted' uses TEXCOORD_1 but mesh 'QuadMesh' has no TEXCOORD_1");
		gltf["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_1"] = 3;
		gltf["extensionsRequired"] = { "KHR_not_supported" };
		rejects("KHR_not_supported");
		gltf.erase("extensionsRequired");
		material["extensions"]["KHR_materials_transmission"]["transmissionFactor"] = 2;
		rejects("material 'Painted': transmissionFactor 2 must be in [0, 1]");
		material["extensions"]["KHR_materials_transmission"]["transmissionFactor"] = 0.6;
		material["occlusionTexture"]["strength"] = 1.5;
		rejects("occlusionTexture.strength 1.5 must be in [0, 1]");
		material["occlusionTexture"]["strength"] = 0.25;
		material["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"] = -1;
		rejects("emissiveStrength -1 must be finite and nonnegative");
		material["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"] = 3;
		material["emissiveTexture"]["texCoord"] = 2;
		rejects("material 'Painted' emissiveTexture: TEXCOORD_2 is unsupported");
	}

	TEST_CASE("Quantized glTF positions and UVs decode without changing topology")
	{
		BasaltTest::TempProject project("GltfQuantized");
		const uint16_t positions[] = { 0, 0, 0, 0, 65535, 0, 0, 0, 65535, 65535, 0, 0, 0, 65535, 0, 0 };
		const uint16_t uv[] = { 0, 65535, 65535, 65535, 65535, 0, 0, 0 };
		const uint16_t indices[] = { 0, 1, 2, 0, 2, 3 };
		std::string binary(reinterpret_cast<const char*>(positions), sizeof(positions));
		binary.append(reinterpret_cast<const char*>(uv), sizeof(uv));
		binary.append(reinterpret_cast<const char*>(indices), sizeof(indices));
		project.WriteFile("Assets/quant.bin", binary);
		auto gltf = MakeGltfJson({ { "uri", "quant.bin" }, { "byteLength", binary.size() } }, { { { "uri", "color.png" } }, { { "uri", "normal.png" } } });
		gltf["extensionsRequired"] = { "KHR_mesh_quantization" };
		gltf["bufferViews"][0]["byteLength"] = 32;
		gltf["bufferViews"][0]["byteStride"] = 8;
		gltf["bufferViews"][1]["byteOffset"] = 32;
		gltf["bufferViews"][1]["byteLength"] = 16;
		gltf["bufferViews"][2]["byteOffset"] = 48;
		gltf["accessors"][0]["componentType"] = 5123;
		gltf["accessors"][0]["normalized"] = true;
		gltf["accessors"][1]["componentType"] = 5123;
		gltf["accessors"][1]["normalized"] = true;
		project.WriteFile("Assets/quant.gltf", gltf.dump());
		std::string error;
		auto mesh = MeshImporter::LoadGltf(project.GetDirectory() / "Assets/quant.gltf", "Assets/quant.gltf", error);
		REQUIRE_MESSAGE(mesh, error);
		CHECK(mesh->Vertices.size() == 4);
		CHECK(mesh->Submeshes[0].IndexCount == 6);
		CHECK(mesh->Bounds.Min == glm::vec3(0));
		CHECK(mesh->Bounds.Max == glm::vec3(1, 1, 0));
		for (const auto& vertex : mesh->Vertices)
		{
			CHECK(vertex.TexCoord.x == vertex.Position.x);
			CHECK(vertex.TexCoord.y == 1 - vertex.Position.y);
		}
	}

	TEST_CASE("Quantized normals and unnormalized integer positions keep their scale in the node")
	{
		// gltfpack's default: integer positions with the dequantizing scale on the node, normalized int8 normals.
		BasaltTest::TempProject project("GltfQuantizedNormals");
		const uint16_t positions[] = { 0, 0, 0, 0, 1000, 0, 0, 0, 1000, 1000, 0, 0, 0, 1000, 0, 0 };
		const uint16_t uv[] = { 0, 65535, 65535, 65535, 65535, 0, 0, 0 };
		const uint16_t indices[] = { 0, 1, 2, 0, 2, 3 };
		const int8_t normals[] = { 0, 0, 127, 0, 0, 0, 127, 0, 0, -127, 0, 0, 127, 0, 0, 0 };
		std::string binary(reinterpret_cast<const char*>(positions), sizeof(positions));
		binary.append(reinterpret_cast<const char*>(uv), sizeof(uv));
		binary.append(reinterpret_cast<const char*>(indices), sizeof(indices));
		const auto normalOffset = binary.size();
		binary.append(reinterpret_cast<const char*>(normals), sizeof(normals));
		project.WriteFile("Assets/quant.bin", binary);
		auto gltf = MakeGltfJson({ { "uri", "quant.bin" }, { "byteLength", binary.size() } }, { { { "uri", "color.png" } }, { { "uri", "normal.png" } } });
		gltf["extensionsRequired"] = { "KHR_mesh_quantization" };
		gltf["bufferViews"][0]["byteLength"] = 32;
		gltf["bufferViews"][0]["byteStride"] = 8;
		gltf["bufferViews"][1]["byteOffset"] = 32;
		gltf["bufferViews"][1]["byteLength"] = 16;
		gltf["bufferViews"][2]["byteOffset"] = 48;
		gltf["bufferViews"].push_back({ { "buffer", 0 }, { "byteOffset", normalOffset }, { "byteLength", sizeof(normals) }, { "byteStride", 4 } });
		gltf["accessors"][0]["componentType"] = 5123;
		gltf["accessors"][0]["max"] = { 1000, 1000, 0 };
		gltf["accessors"][1]["componentType"] = 5123;
		gltf["accessors"][1]["normalized"] = true;
		gltf["accessors"].push_back({ { "bufferView", 3 }, { "componentType", 5120 }, { "normalized", true }, { "count", 4 }, { "type", "VEC3" } });
		gltf["meshes"][0]["primitives"][0]["attributes"]["NORMAL"] = 3;
		gltf["nodes"][1]["scale"] = { 0.001, 0.001, 0.001 };
		project.WriteFile("Assets/quant.gltf", gltf.dump());
		std::string error;
		auto mesh = MeshImporter::LoadGltf(project.GetDirectory() / "Assets/quant.gltf", "Assets/quant.gltf", error);
		REQUIRE_MESSAGE(mesh, error);
		CHECK(mesh->Bounds.Max == glm::vec3(1000, 1000, 0));
		const glm::vec3 expected[] = { { 0, 0, 1 }, { 0, 0, 1 }, { 0, -1, 0 }, { 1, 0, 0 } };
		const glm::vec2 uvs[] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
		for (const auto& vertex : mesh->Vertices)
		{
			const auto index = static_cast<size_t>(std::find(std::begin(uvs), std::end(uvs), vertex.TexCoord) - std::begin(uvs));
			REQUIRE(index < 4);
			CHECK(glm::length(vertex.Normal - expected[index]) < 1e-5f);
		}
		const auto node = std::find_if(mesh->Nodes.begin(), mesh->Nodes.end(), [](const MeshNode& n) { return n.Name == "Quad"; });
		REQUIRE(node != mesh->Nodes.end());
		CHECK(node->Scale == glm::vec3(0.001f));
	}

	TEST_CASE("KHR_texture_basisu external and embedded images reach the texture cache")
	{
		BasaltTest::TempProject project("GltfBasisImages");
		const auto bytes = FileSystem::ReadBinaryFile(std::filesystem::path(BASALT_TEST_DATA_DIR) / "Textures/UASTC.ktx2");
		REQUIRE(bytes);
		std::string binary = MakeQuadBuffer();
		const auto imageOffset = binary.size();
		binary.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
		project.WriteFile("Assets/quad.bin", binary);
		project.WriteFile("Assets/color.ktx2", std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
		auto gltf = MakeGltfJson({ { "uri", "quad.bin" }, { "byteLength", binary.size() } }, { { { "uri", "color.ktx2" } }, { { "bufferView", 3 }, { "mimeType", "image/ktx2" } } });
		gltf["bufferViews"].push_back({ { "buffer", 0 }, { "byteOffset", imageOffset }, { "byteLength", bytes->size() } });
		gltf["extensionsRequired"] = { "KHR_texture_basisu" };
		gltf["textures"][0] = { { "extensions", { { "KHR_texture_basisu", { { "source", 0 } } } } } };
		gltf["textures"][1] = { { "extensions", { { "KHR_texture_basisu", { { "source", 1 } } } } } };
		project.WriteFile("Assets/quad.gltf", gltf.dump());
		AssetManager::Clear();
		auto mesh = AssetManager::GetMesh("Assets/quad.gltf");
		REQUIRE(mesh);
		for (const auto& key : { mesh->Materials[0].AlbedoMap, mesh->Materials[0].NormalMap })
		{
			auto texture = AssetManager::GetTexture(key);
			REQUIRE_MESSAGE(texture, AssetManager::GetError(key));
			CHECK(texture->Format == TextureFormat::BC7);
			CHECK(texture->MipOffsets.size() == 4);
		}
		AssetManager::Clear();
	}

	TEST_CASE("Blocking mesh getters join dependency retries without publishing unrelated requests")
	{
		BasaltTest::TempProject project("BlockingAsyncRetry");
		AssetManager::Clear();
		const auto binary = MakeQuadBuffer();
		project.WriteFile("Assets/quad.bin", binary);
		project.WriteFile("Assets/normal.png", MakePng(0xFFFFFFFF));
		project.WriteFile("Assets/quad.gltf", MakeGltfJson({ { "uri", "quad.bin" }, { "byteLength", binary.size() } }, { { { "uri", "normal.png" } }, { { "uri", "normal.png" } } }).dump());
		AssetManager::RequestMesh("Assets/quad.gltf");
		AssetManager::RequestMesh("builtin://Sphere");
		JobSystem::Get().WaitIdle();
		const auto since = Log::GetHistory().GetTotalCount();
		CHECK(AssetManager::ReloadChanged({ "Assets/quad.bin" }).empty());
		REQUIRE(AssetManager::GetMesh("Assets/quad.gltf"));
		CHECK_FALSE(AssetManager::IsLoading("Assets/quad.gltf"));
		CHECK_FALSE(AssetManager::FindLoadedMesh("builtin://Sphere"));
		CHECK(BasaltTest::CountLogMessages(since, "Assets/quad.gltf': 'Assets/quad.bin' changed while loading; retrying") == 1);
		AssetManager::Clear();
	}

	TEST_CASE("Pending imports retry only when their discovered dependencies changed")
	{
		BasaltTest::TempProject project("AsyncDependencyChange");
		AssetManager::Clear();
		const auto binary = MakeQuadBuffer();
		project.WriteFile("Assets/quad.bin", binary);
		project.WriteFile("Assets/normal.png", MakePng(0xFFFFFFFF));
		const auto png = MakePng(0xFFFFFFFF);
		project.WriteFile("Assets/quad.gltf", MakeGltfJson({ { "uri", "quad.bin" }, { "byteLength", binary.size() } }, { { { "uri", "data:image/png;base64," + BasaltTest::Base64(png.data(), png.size()) } }, { { "uri", "normal.png" } } }).dump());
		AssetManager::RequestTexture("Assets/quad.gltf#image/0");
		JobSystem::Get().WaitIdle();
		// Dependency metadata is still private to the completed result at this point.
		std::filesystem::remove(project.GetDirectory() / "Assets/quad.bin");
		CHECK(AssetManager::ReloadChanged({ "Assets/quad.bin" }).empty());
		AssetManager::PublishCompleted();
		CHECK(AssetManager::IsLoading("Assets/quad.gltf"));
		CHECK_FALSE(AssetManager::FindLoadedMesh("Assets/quad.gltf"));
		CHECK_FALSE(AssetManager::RequestTexture("Assets/quad.gltf#image/0"));
		AssetManager::WaitForLoads();
		CHECK_FALSE(AssetManager::GetMesh("Assets/quad.gltf"));
		CHECK_FALSE(AssetManager::GetError("Assets/quad.gltf").empty());
		CHECK_FALSE(AssetManager::IsLoading("Assets/quad.gltf#image/0"));
		CHECK_FALSE(AssetManager::GetError("Assets/quad.gltf#image/0").empty());
		project.WriteFile("Assets/quad.bin", binary);
		CHECK(AssetManager::ReloadChanged({ "Assets/quad.bin" }) == std::vector<std::string>{ "Assets/quad.gltf", "Assets/quad.gltf#image/0" });
		CHECK(AssetManager::GetMesh("Assets/quad.gltf"));
		AssetManager::Clear();
	}

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
			EmbeddedBufferJson(buffer),
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

		project.WriteFile("Assets/Models/Quad.glb", MakeGlb(json, binary));

		AssetManager::Clear();
		Ref<MeshSource> mesh = AssetManager::GetMesh("Assets/Models/Quad.glb");
		REQUIRE_MESSAGE(mesh, AssetManager::GetError("Assets/Models/Quad.glb"));
		CHECK(mesh->Indices.size() == 6);
		Ref<TextureSource> texture = AssetManager::GetTexture(mesh->Materials[0].AlbedoMap);
		REQUIRE_MESSAGE(texture, AssetManager::GetError(mesh->Materials[0].AlbedoMap));
		CHECK(texture->Pixels[1] == 255);
	}

	TEST_CASE("FindLoadedMesh only reports meshes already loaded, and never loads one")
	{
		AssetManager::Clear();
		CHECK_FALSE(AssetManager::FindLoadedMesh("builtin://Cube"));
		// A lookup does not load (the content-browser tooltip relies on this).
		CHECK_FALSE(AssetManager::FindLoadedMesh("builtin://Cube"));
		const Ref<MeshSource> loaded = AssetManager::GetMesh("builtin://Cube");
		REQUIRE(loaded);
		CHECK(AssetManager::FindLoadedMesh("builtin://Cube") == loaded);
		// A failed load is not a loaded mesh.
		CHECK_FALSE(AssetManager::GetMesh("builtin://Teapot"));
		CHECK_FALSE(AssetManager::FindLoadedMesh("builtin://Teapot"));
		AssetManager::Reload("builtin://Cube");
		CHECK_FALSE(AssetManager::FindLoadedMesh("builtin://Cube"));
		AssetManager::Clear();
	}

	TEST_CASE("Changed files drop the loaded assets they affect")
	{
		BasaltTest::TempProject project("AssetReloadChanged");
		const std::string buffer = MakeQuadBuffer();
		project.WriteFile("Assets/Models/Quad.bin", buffer);
		project.WriteFile("Assets/Models/normal.png", MakePng(0x0000FFFF));
		const std::string redPng = MakePng(0xFF0000FF);
		const nlohmann::json gltf = MakeGltfJson({ { "byteLength", buffer.size() }, { "uri", "Quad.bin" } }, { { { "uri", "data:image/png;base64," + BasaltTest::Base64(redPng.data(), redPng.size()) } }, { { "uri", "normal.png" } } });
		project.WriteFile("Assets/Models/Quad.gltf", gltf.dump());
		project.WriteFile("Assets/Textures/Wall.png", MakePng(0xFFFFFFFF));

		AssetManager::Clear();
		const Ref<MeshSource> mesh = AssetManager::GetMesh("Assets/Models/Quad.gltf");
		REQUIRE_MESSAGE(mesh, AssetManager::GetError("Assets/Models/Quad.gltf"));
		CHECK(mesh->ExternalFiles == std::vector<std::string>{ "Assets/Models/Quad.bin", "Assets/Models/normal.png" });
		REQUIRE(AssetManager::GetTexture("Assets/Models/Quad.gltf#image/0"));
		REQUIRE(AssetManager::GetTexture("Assets/Models/normal.png"));
		REQUIRE(AssetManager::GetTexture("Assets/Textures/Wall.png"));
		REQUIRE(AssetManager::GetMesh("builtin://Cube"));
		CHECK_FALSE(AssetManager::GetTexture("Assets/Textures/Missing.png"));

		// A file nothing loaded drops nothing.
		CHECK(AssetManager::ReloadChanged({ "Assets/Textures/Other.png" }).empty());
		// A buffer only the model reads: the model goes, with its embedded images.
		CHECK(AssetManager::ReloadChanged({ "Assets/Models/Quad.bin" }) == std::vector<std::string>{ "Assets/Models/Quad.gltf", "Assets/Models/Quad.gltf#image/0" });
		CHECK_FALSE(AssetManager::FindLoadedMesh("Assets/Models/Quad.gltf"));
		const Ref<MeshSource> reloaded = AssetManager::GetMesh("Assets/Models/Quad.gltf");
		REQUIRE(reloaded);
		CHECK(reloaded != mesh);
		// An external image is its own texture and is read by the model.
		CHECK(AssetManager::ReloadChanged({ "Assets/Models/normal.png" }) == std::vector<std::string>{ "Assets/Models/Quad.gltf", "Assets/Models/normal.png" });
		// A failed load is dropped too, so the file is tried again; others stay.
		project.WriteFile("Assets/Textures/Missing.png", MakePng(0xFFFFFFFF));
		CHECK(AssetManager::ReloadChanged({ "Assets/Textures/Missing.png" }) == std::vector<std::string>{ "Assets/Textures/Missing.png" });
		CHECK(AssetManager::GetTexture("Assets/Textures/Missing.png"));
		CHECK(AssetManager::GetLoadedKeys() == std::vector<std::string>{ "Assets/Textures/Missing.png", "Assets/Textures/Wall.png", "builtin://Cube" });

		// A model that failed for want of its buffer is retried when the buffer appears (any change retries failed models).
		REQUIRE(AssetManager::GetMesh("Assets/Models/Quad.gltf"));
		REQUIRE(AssetManager::GetTexture("Assets/Models/Quad.gltf#image/0"));
		std::filesystem::remove(project.GetDirectory() / "Assets/Models/Quad.bin");
		CHECK(AssetManager::ReloadChanged({ "Assets/Models/Quad.bin" }) == std::vector<std::string>{ "Assets/Models/Quad.gltf", "Assets/Models/Quad.gltf#image/0" });
		CHECK_FALSE(AssetManager::GetMesh("Assets/Models/Quad.gltf"));
		project.WriteFile("Assets/Models/Quad.bin", buffer);
		CHECK(AssetManager::ReloadChanged({ "Assets/Models/Quad.bin" }) == std::vector<std::string>{ "Assets/Models/Quad.gltf" });
		CHECK(AssetManager::GetMesh("Assets/Models/Quad.gltf"));
		CHECK(AssetManager::GetSourceFile("Assets/Models/Quad.gltf#image/0") == "Assets/Models/Quad.gltf");
		// A model whose image lives in another folder is dropped when that image changes (keys like the watcher's).
		const nlohmann::json shared = MakeGltfJson({ { "byteLength", buffer.size() }, { "uri", "Quad.bin" } }, { { { "uri", "../Textures/Wall.png" } }, { { "uri", "normal.png" } } });
		project.WriteFile("Assets/Models/Shared.gltf", shared.dump());
		REQUIRE(AssetManager::GetMesh("Assets/Models/Shared.gltf"));
		CHECK(AssetManager::FindLoadedMesh("Assets/Models/Shared.gltf")->ExternalFiles == std::vector<std::string>{ "Assets/Models/Quad.bin", "Assets/Models/normal.png", "Assets/Textures/Wall.png" });
		const std::vector<std::string> dropped = AssetManager::ReloadChanged({ "Assets/Textures/Wall.png" });
		CHECK(std::find(dropped.begin(), dropped.end(), "Assets/Models/Shared.gltf") != dropped.end());
		CHECK(MeshImporter::GetExternalFileKey("Assets/Models/Ship.gltf", "../Textures/Hull.png") == "Assets/Textures/Hull.png");
		CHECK(MeshImporter::GetExternalFileKey("Assets/Models/Ship.gltf", "Ship%20Data.bin") == "Assets/Models/Ship Data.bin");
		CHECK(MeshImporter::GetExternalFileKey("Assets/Models/Ship.gltf", "./sub/../Ship.bin") == "Assets/Models/Ship.bin");
		CHECK_FALSE(MeshImporter::GetExternalFileKey("Assets/Models/Ship.gltf", "data:application/octet-stream;base64,AAAA"));
		CHECK_FALSE(MeshImporter::GetExternalFileKey("Assets/Models/Ship.gltf", nullptr));
		CHECK(AssetManager::GetSourceFile("Assets/Textures/Wall.png") == "Assets/Textures/Wall.png");
		AssetManager::Clear();
	}

	TEST_CASE("TemporaryLoads releases only the assets first loaded during its lifetime")
	{
		AssetManager::Clear();
		const Ref<MeshSource> cube = AssetManager::GetMesh("builtin://Cube");
		REQUIRE(cube);
		{
			AssetManager::TemporaryLoads temporary;
			CHECK(AssetManager::GetMesh("builtin://Cube") == cube);
			CHECK(AssetManager::GetMesh("builtin://Sphere"));
			CHECK_FALSE(AssetManager::GetMesh("builtin://Teapot"));
			CHECK(AssetManager::GetLoadedKeys().size() == 3);
		}
		CHECK(AssetManager::GetLoadedKeys() == std::vector<std::string>{ "builtin://Cube" });
		CHECK(AssetManager::FindLoadedMesh("builtin://Cube") == cube);

		// A model loaded only inside the scope goes with its embedded images; a texture loaded before stays.
		BasaltTest::TempProject project("TemporaryLoadsModel");
		const std::string buffer = MakeQuadBuffer();
		const std::string redPng = MakePng(0xFF0000FF);
		project.WriteFile("Assets/Models/normal.png", MakePng(0x0000FFFF));
		project.WriteFile("Assets/Models/Quad.gltf", MakeGltfJson(EmbeddedBufferJson(buffer), { { { "uri", "data:image/png;base64," + BasaltTest::Base64(redPng.data(), redPng.size()) } }, { { "uri", "normal.png" } } }).dump());
		REQUIRE(AssetManager::GetTexture("Assets/Models/normal.png"));
		{
			AssetManager::TemporaryLoads temporary;
			REQUIRE(AssetManager::GetMesh("Assets/Models/Quad.gltf"));
			REQUIRE(AssetManager::GetTexture("Assets/Models/Quad.gltf#image/0"));
		}
		CHECK(AssetManager::GetLoadedKeys() == std::vector<std::string>{ "Assets/Models/normal.png", "builtin://Cube" });
		AssetManager::Clear();
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
		const nlohmann::json embedded = EmbeddedBufferJson(buffer);

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
		// The sandbox check runs before buffers are sized, so the error says nothing about the outside file.
		CHECK_FALSE(load("I.gltf", { { "byteLength", 9999999999ull }, { "uri", "../../../../../../../../etc/hosts" } }, "sub/ok.png"));
		CHECK(AssetManager::GetError("Assets/Models/I.gltf").find("outside the project") != std::string::npos);
		CHECK(AssetManager::GetError("Assets/Models/I.gltf").find("declares") == std::string::npos);
		AssetManager::Clear();
	}

	// Regression (FuzzGltf): cgltf_validate read 16-bit indices through a misaligned pointer when an accessor's
	// byte offset was odd. The glTF spec forbids that, so such files are rejected before validation.
	TEST_CASE("glTF accessors must be aligned to their component size")
	{
		BasaltTest::TempProject project("GltfAlignment");
		const std::string buffer = MakeQuadBuffer() + std::string(4, '\0');
		nlohmann::json gltf = MakeUntexturedGltfJson(EmbeddedBufferJson(buffer));
		project.WriteFile("Assets/Models/Aligned.gltf", gltf.dump());
		// Overlapping elements: the positions' buffer view strides 4 bytes over 12-byte elements.
		nlohmann::json overlapping = gltf;
		overlapping["bufferViews"][0]["byteStride"] = 4;
		project.WriteFile("Assets/Models/Overlapping.gltf", overlapping.dump());
		gltf["accessors"][2]["byteOffset"] = 1; // uint16 indices starting at an odd address
		project.WriteFile("Assets/Models/Misaligned.gltf", gltf.dump());

		AssetManager::Clear();
		CHECK_MESSAGE(AssetManager::GetMesh("Assets/Models/Aligned.gltf"), AssetManager::GetError("Assets/Models/Aligned.gltf"));
		CHECK_FALSE(AssetManager::GetMesh("Assets/Models/Misaligned.gltf"));
		CHECK(AssetManager::GetError("Assets/Models/Misaligned.gltf").find("not aligned") != std::string::npos);
		CHECK_FALSE(AssetManager::GetMesh("Assets/Models/Overlapping.gltf"));
		CHECK(AssetManager::GetError("Assets/Models/Overlapping.gltf").find("accessor 0 has 12-byte elements, but its buffer view's byteStride is only 4") != std::string::npos);
		AssetManager::Clear();
	}

	// Regression (FuzzGltf, nightly): cgltf reads a GLB's binary chunk in place, so after a JSON chunk not padded to 4
	// bytes the uint16 indices (aligned within the buffer) were read through a misaligned pointer.
	TEST_CASE("GLB chunks must be padded to 4 bytes")
	{
		BasaltTest::TempProject project("GlbPadding");
		const std::string binary = MakeQuadBuffer();
		// A JSON chunk this many bytes past a multiple of 4, with or without a binary chunk holding the quad.
		auto glb = [&](size_t remainder, bool withBinary) {
			std::string json = MakeUntexturedGltfJson(withBinary ? nlohmann::json{ { "byteLength", binary.size() } } : EmbeddedBufferJson(binary)).dump();
			while (json.size() % 4 != remainder)
				json += ' ';
			return MakeGlb(json, withBinary ? binary : std::string());
		};
		auto load = [&](const std::string& name, const std::string& contents) {
			project.WriteFile("Assets/Models/" + name, contents);
			AssetManager::Clear();
			return AssetManager::GetMesh("Assets/Models/" + name) != nullptr;
		};
		CHECK_MESSAGE(load("Padded.glb", glb(0, true)), AssetManager::GetError("Assets/Models/Padded.glb"));
		for (size_t remainder = 1; remainder < 4; remainder++)
		{
			CAPTURE(remainder);
			CHECK_FALSE(load("Unpadded.glb", glb(remainder, true)));
			CHECK(AssetManager::GetError("Assets/Models/Unpadded.glb").find("not padded to a multiple of 4") != std::string::npos);
			// Without a binary chunk nothing is read in place, so the padding does not matter.
			CHECK_MESSAGE(load("UnpaddedWithoutBinary.glb", glb(remainder, false)), AssetManager::GetError("Assets/Models/UnpaddedWithoutBinary.glb"));
		}
		AssetManager::Clear();
	}

	// Regression (FuzzGltf, CI): cgltf_validate read a sparse accessor's indices through a buffer view past the end of
	// its buffer before checking the view's bounds.
	TEST_CASE("glTF buffer views must lie inside their buffers")
	{
		BasaltTest::TempProject project("GltfViewBounds");
		const std::string buffer = MakeQuadBuffer();
		nlohmann::json gltf = MakeUntexturedGltfJson(EmbeddedBufferJson(buffer));
		// A sparse override of the positions whose index view starts past the buffer.
		gltf["bufferViews"].push_back({ { "buffer", 0 }, { "byteOffset", buffer.size() + 600 }, { "byteLength", 4 } });
		gltf["accessors"][0]["sparse"] = { { "count", 1 }, { "indices", { { "bufferView", gltf["bufferViews"].size() - 1 }, { "componentType", 5123 } } }, { "values", { { "bufferView", 0 } } } };
		project.WriteFile("Assets/Models/PastBuffer.gltf", gltf.dump());
		AssetManager::Clear();
		CHECK_FALSE(AssetManager::GetMesh("Assets/Models/PastBuffer.gltf"));
		CHECK(AssetManager::GetError("Assets/Models/PastBuffer.gltf").find("buffer view 3 lies outside its buffer") != std::string::npos);
		AssetManager::Clear();
	}

	// Regression (FuzzGltf, nightly): cgltf allocates a buffer's declared byteLength before decoding its data URI or
	// reading its file, so a 134-byte glTF claiming 10 GB made the importer request 10 GB.
	TEST_CASE("glTF buffers may not declare more bytes than their data holds")
	{
		BasaltTest::TempProject project("GltfBufferLength");
		const std::string buffer = MakeQuadBuffer();
		project.WriteFile("Assets/Models/Quad.bin", buffer);
		const std::string base64Prefix = "data:application/octet-stream;base64,";
		const std::string dataUri = base64Prefix + BasaltTest::Base64(buffer.data(), buffer.size());
		constexpr uint64_t Huge = 9999999999;
		auto load = [&](const std::string& name, const nlohmann::json& gltf) {
			project.WriteFile("Assets/Models/" + name, gltf.dump());
			AssetManager::Clear();
			return AssetManager::GetMesh("Assets/Models/" + name) != nullptr;
		};
		auto error = [](const std::string& name) { return AssetManager::GetError("Assets/Models/" + name); };
		auto has = [&](const std::string& name, const std::string& text) { return error(name).find(text) != std::string::npos; };

		SUBCASE("Data URIs")
		{
			CHECK_MESSAGE(load("Embedded.gltf", MakeUntexturedGltfJson({ { "byteLength", buffer.size() }, { "uri", dataUri } })), error("Embedded.gltf"));
			CHECK_FALSE(load("EmbeddedHuge.gltf", MakeUntexturedGltfJson({ { "byteLength", Huge }, { "uri", dataUri } })));
			CHECK(has("EmbeddedHuge.gltf", "buffer 0 declares 9999999999 bytes but its data holds only"));

			// 93 bytes encode to 124 characters without padding, so the bound is exact: 93 loads, 94 is rejected here
			// rather than by cgltf's decoder.
			const std::string padded = buffer + '\0';
			const std::string unpaddedUri = base64Prefix + BasaltTest::Base64(padded.data(), padded.size());
			REQUIRE(unpaddedUri.back() != '=');
			CHECK_MESSAGE(load("Exact.gltf", MakeUntexturedGltfJson({ { "byteLength", padded.size() }, { "uri", unpaddedUri } })), error("Exact.gltf"));
			CHECK_FALSE(load("OneMore.gltf", MakeUntexturedGltfJson({ { "byteLength", padded.size() + 1 }, { "uri", unpaddedUri } })));
			CHECK(has("OneMore.gltf", "buffer 0 declares 94 bytes but its data holds only 93"));

			// cgltf only decodes base64 data URIs and would report anything else as an unknown format.
			CHECK_FALSE(load("NoComma.gltf", MakeUntexturedGltfJson({ { "byteLength", Huge }, { "uri", "data:application/octet-stream;base64AAAA" } })));
			CHECK(has("NoComma.gltf", "buffer 0 data URI is not base64"));
			CHECK_FALSE(load("Text.gltf", MakeUntexturedGltfJson({ { "byteLength", 2 }, { "uri", "data:text/plain,hi" } })));
			CHECK(has("Text.gltf", "buffer 0 data URI is not base64"));
		}

		SUBCASE("External files")
		{
			CHECK_MESSAGE(load("External.gltf", MakeUntexturedGltfJson({ { "byteLength", buffer.size() }, { "uri", "Quad.bin" } })), error("External.gltf"));
			CHECK_FALSE(load("ExternalHuge.gltf", MakeUntexturedGltfJson({ { "byteLength", Huge }, { "uri", "Quad.bin" } })));
			CHECK(has("ExternalHuge.gltf", "buffer 0 declares 9999999999 bytes but its data holds only " + std::to_string(buffer.size())));

			// A buffer may be a prefix of its file.
			project.WriteFile("Assets/Models/Padded.bin", buffer + std::string(16, '\0'));
			CHECK_MESSAGE(load("Prefix.gltf", MakeUntexturedGltfJson({ { "byteLength", buffer.size() }, { "uri", "Padded.bin" } })), error("Prefix.gltf"));

			// The URI is percent-decoded before sizing, as cgltf decodes it before reading.
			project.WriteFile("Assets/Models/Quad Data.bin", buffer);
			CHECK_MESSAGE(load("Encoded.gltf", MakeUntexturedGltfJson({ { "byteLength", buffer.size() }, { "uri", "Quad%20Data.bin" } })), error("Encoded.gltf"));
			CHECK_FALSE(load("EncodedHuge.gltf", MakeUntexturedGltfJson({ { "byteLength", Huge }, { "uri", "Quad%20Data.bin" } })));
			CHECK(has("EncodedHuge.gltf", "holds only " + std::to_string(buffer.size())));
		}

		SUBCASE("Files that cannot be sized")
		{
			// A directory inside the project passes the sandbox check, and cgltf opens one on Linux and macOS.
			CHECK_FALSE(load("Directory.gltf", MakeUntexturedGltfJson({ { "byteLength", Huge }, { "uri", "." } })));
			CHECK(has("Directory.gltf", "buffer 0 file '.' is not a readable file"));
			CHECK_FALSE(load("Missing.gltf", MakeUntexturedGltfJson({ { "byteLength", buffer.size() }, { "uri", "Missing.bin" } })));
			CHECK(has("Missing.gltf", "buffer 0 file 'Missing.bin' is not a readable file"));
		}

		SUBCASE("Every buffer is checked, past ones without a URI")
		{
			nlohmann::json gltf = MakeUntexturedGltfJson({ { "byteLength", buffer.size() }, { "uri", dataUri } });
			gltf["buffers"].push_back({ { "byteLength", 4 } });
			gltf["buffers"].push_back({ { "byteLength", Huge }, { "uri", "Quad.bin" } });
			CHECK_FALSE(load("Later.gltf", gltf));
			CHECK(has("Later.gltf", "buffer 2 declares 9999999999 bytes but its data holds only " + std::to_string(buffer.size())));
		}

		SUBCASE("Buffers sharing a file")
		{
			// glTF lets buffers share a file; cgltf reads it once per buffer.
			nlohmann::json gltf = MakeUntexturedGltfJson({ { "byteLength", buffer.size() }, { "uri", "Quad.bin" } });
			gltf["buffers"].push_back({ { "byteLength", buffer.size() }, { "uri", "Quad.bin" } });
			CHECK_MESSAGE(load("Twice.gltf", gltf), error("Twice.gltf"));

			// Rereads are limited, since a small glTF repeating one URI multiplied the file's size. Three buffers reading
			// a 32 MiB file reread it twice, exactly the 64 MiB allowed; a missing file after them stops the load before
			// anything is read. A spelling of the same path, or a buffer without a byteLength (the whole file), counts
			// the same.
			const auto big = project.GetDirectory() / "Assets/Models/Big.bin";
			REQUIRE(FileSystem::WriteTextFile(big, "x"));
			std::filesystem::resize_file(big, 32ull << 20);
			nlohmann::json shared = MakeUntexturedGltfJson(EmbeddedBufferJson(buffer));
			shared["buffers"].push_back({ { "byteLength", 32ull << 20 }, { "uri", "Big.bin" } });
			shared["buffers"].push_back({ { "uri", "./Big.bin" } });
			shared["buffers"].push_back({ { "byteLength", 32ull << 20 }, { "uri", "Big.bin" } });
			nlohmann::json allowed = shared;
			allowed["buffers"].push_back({ { "byteLength", 4 }, { "uri", "Missing.bin" } });
			CHECK_FALSE(load("Rereads.gltf", allowed));
			CHECK(has("Rereads.gltf", "buffer 4 file 'Missing.bin' is not a readable file"));
			shared["buffers"].push_back({ { "byteLength", 1 }, { "uri", "Big.bin" } });
			shared["buffers"].push_back({ { "byteLength", 4 }, { "uri", "Missing.bin" } });
			CHECK_FALSE(load("TooManyRereads.gltf", shared));
			CHECK(has("TooManyRereads.gltf", "buffers read files they share more than once, 1 bytes past the limit of 67108864"));

			// Every buffer reads from the start of its file, so a small first read leaves nothing for later ones: after a
			// 1-byte buffer, two 32 MiB reads and a 1-byte read are all rereads (Codex review).
			nlohmann::json prefix = MakeUntexturedGltfJson(EmbeddedBufferJson(buffer));
			prefix["buffers"].push_back({ { "byteLength", 1 }, { "uri", "Big.bin" } });
			prefix["buffers"].push_back({ { "byteLength", 32ull << 20 }, { "uri", "Big.bin" } });
			prefix["buffers"].push_back({ { "byteLength", 32ull << 20 }, { "uri", "Big.bin" } });
			prefix["buffers"].push_back({ { "byteLength", 1 }, { "uri", "Big.bin" } });
			prefix["buffers"].push_back({ { "byteLength", 4 }, { "uri", "Missing.bin" } });
			CHECK_FALSE(load("Prefixes.gltf", prefix));
			CHECK(has("Prefixes.gltf", "buffers read files they share more than once, 1 bytes past the limit of 67108864"));
		}

		SUBCASE("The bytes all buffers declare are capped")
		{
			// Hard links name one file through paths that count as different files, so only the 4 GiB total limits
			// them. With the 92-byte buffer, 63 links of 64 MiB fit and 64 do not; the missing file stops a load that
			// passes, before anything is read.
			const auto directory = project.GetDirectory() / "Assets/Models";
			REQUIRE(FileSystem::WriteTextFile(directory / "Link0.bin", "x"));
			std::filesystem::resize_file(directory / "Link0.bin", 64ull << 20);
			for (int i = 1; i < 64; i++)
				std::filesystem::create_hard_link(directory / "Link0.bin", directory / ("Link" + std::to_string(i) + ".bin"));
			auto links = [&](int count) {
				nlohmann::json gltf = MakeUntexturedGltfJson(EmbeddedBufferJson(buffer));
				for (int i = 0; i < count; i++)
					gltf["buffers"].push_back({ { "byteLength", 64ull << 20 }, { "uri", "Link" + std::to_string(i) + ".bin" } });
				gltf["buffers"].push_back({ { "byteLength", 4 }, { "uri", "Missing.bin" } });
				return gltf;
			};
			CHECK_FALSE(load("Links63.gltf", links(63)));
			CHECK(has("Links63.gltf", "file 'Missing.bin' is not a readable file"));
			CHECK_FALSE(load("Links64.gltf", links(64)));
			CHECK(has("Links64.gltf", "buffers declare more than 4294967296 bytes in total"));
		}

		SUBCASE("A GLB's binary chunk is checked by cgltf")
		{
			std::string json = MakeUntexturedGltfJson({ { "byteLength", buffer.size() + 4 } }).dump();
			while (json.size() % 4 != 0)
				json += ' ';
			auto put32 = [](std::string& out, uint32_t value) { out.append(reinterpret_cast<const char*>(&value), 4); };
			std::string glb = "glTF";
			put32(glb, 2);
			put32(glb, static_cast<uint32_t>(12 + 8 + json.size() + 8 + buffer.size()));
			put32(glb, static_cast<uint32_t>(json.size()));
			glb += "JSON";
			glb += json;
			put32(glb, static_cast<uint32_t>(buffer.size()));
			glb.append("BIN\0", 4);
			glb += buffer;
			project.WriteFile("Assets/Models/Short.glb", glb);
			AssetManager::Clear();
			CHECK_FALSE(AssetManager::GetMesh("Assets/Models/Short.glb"));
			CHECK(AssetManager::GetError("Assets/Models/Short.glb").find("cannot load buffers (data too short)") != std::string::npos);
		}
		AssetManager::Clear();
	}

	// Regression (review of PR #51): cgltf_validate computes the bytes an accessor spans without overflow checks, so a
	// count of 2^62 + 1 four-byte indices wrapped to 4 bytes and its index scan read past the buffer (ASan
	// heap-buffer-overflow). A view into a buffer without a URI (no data outside a GLB) made the importer allocate
	// gigabytes of vertices before reading nothing.
	TEST_CASE("glTF accessors must fit their buffer views")
	{
		BasaltTest::TempProject project("GltfAccessorBounds");
		const std::string buffer = MakeQuadBuffer();
		const nlohmann::json quad = MakeUntexturedGltfJson(EmbeddedBufferJson(buffer));
		auto load = [&](const std::string& name, const nlohmann::json& json) {
			project.WriteFile("Assets/Models/" + name, json.dump());
			AssetManager::Clear();
			return AssetManager::GetMesh("Assets/Models/" + name) != nullptr;
		};
		auto has = [](const std::string& name, const std::string& text) { return AssetManager::GetError("Assets/Models/" + name).find(text) != std::string::npos; };
		constexpr uint64_t Wrapping = 4611686018427387905; // 2^62 + 1: times 4 bytes wraps to 4.

		nlohmann::json indices = quad;
		indices["accessors"][2]["componentType"] = 5125;
		indices["accessors"][2]["count"] = Wrapping;
		CHECK_FALSE(load("Indices.gltf", indices));
		CHECK(has("Indices.gltf", "accessor 2 (4611686018427387905 elements) does not fit its buffer views"));

		nlohmann::json sparse = quad;
		sparse["accessors"][0]["sparse"] = { { "count", Wrapping }, { "indices", { { "bufferView", 2 }, { "componentType", 5125 } } }, { "values", { { "bufferView", 0 } } } };
		CHECK_FALSE(load("Sparse.gltf", sparse));
		CHECK(has("Sparse.gltf", "accessor 0 (4 elements) does not fit its buffer views"));

		nlohmann::json dataless = quad;
		dataless["buffers"].push_back({ { "byteLength", 48 } });
		dataless["bufferViews"].push_back({ { "buffer", 1 }, { "byteLength", 48 } });
		dataless["accessors"][0]["bufferView"] = 3;
		CHECK_FALSE(load("Dataless.gltf", dataless));
		CHECK(has("Dataless.gltf", "buffer view 3 uses a buffer without data"));
		AssetManager::Clear();
	}

	// Regression (FuzzGltf, review of PR #51): the importer allocates per vertex and index before reading data, so a
	// 232-byte glTF declaring 400 million zero-filled positions asked for 35 GB, and primitives sharing one accessor
	// multiply it. The limits are checked over the whole model before anything is allocated; only the rejecting side
	// is pinned, since a model at a limit takes seconds to import.
	TEST_CASE("glTF geometry past the import limits is rejected before allocating")
	{
		BasaltTest::TempProject project("GltfGeometryLimits");
		const std::string buffer = MakeQuadBuffer();
		nlohmann::json gltf = MakeUntexturedGltfJson(EmbeddedBufferJson(buffer));
		auto load = [&](const std::string& name, const nlohmann::json& json) {
			project.WriteFile("Assets/Models/" + name, json.dump());
			AssetManager::Clear();
			return AssetManager::GetMesh("Assets/Models/" + name) != nullptr;
		};
		auto has = [](const std::string& name, const std::string& text) { return AssetManager::GetError("Assets/Models/" + name).find(text) != std::string::npos; };
		// Appends a buffer of the given size (in its own file), a view and an accessor over all of it, and n primitives
		// that use the accessor as their positions or indices.
		auto share = [&](nlohmann::json json, const std::string& file, size_t bytes, const nlohmann::json& accessor, bool indices, size_t primitives) {
			project.WriteFile("Assets/Models/" + file, std::string(bytes, '\0'));
			json["buffers"].push_back({ { "byteLength", bytes }, { "uri", file } });
			json["bufferViews"].push_back({ { "buffer", json["buffers"].size() - 1 }, { "byteLength", bytes } });
			nlohmann::json shared = accessor;
			shared["bufferView"] = json["bufferViews"].size() - 1;
			json["accessors"].push_back(shared);
			const size_t index = json["accessors"].size() - 1;
			const nlohmann::json primitive = indices ? nlohmann::json{ { "attributes", { { "POSITION", 0 } } }, { "indices", index } } : nlohmann::json{ { "attributes", { { "POSITION", index } } } };
			json["meshes"][0]["primitives"] = nlohmann::json::array();
			for (size_t i = 0; i < primitives; i++)
				json["meshes"][0]["primitives"].push_back(primitive);
			return json;
		};

		SUBCASE("Accessors without a buffer view")
		{
			// Zero-filled positions (what sparse morph targets build on) are valid. cgltf requires every attribute of a
			// primitive to have the same count, so the UVs go before the position count grows below.
			gltf["accessors"][0].erase("bufferView");
			gltf["meshes"][0]["primitives"][0]["attributes"].erase("TEXCOORD_0");
			CHECK_MESSAGE(load("ZeroFilled.gltf", gltf), AssetManager::GetError("Assets/Models/ZeroFilled.gltf"));
			// The indices use the first four positions. Nightly FuzzGltf found 4,000,008 zero-filled positions under the
			// old limit taking longer than its 10 s timeout to import.
			gltf["accessors"][0]["count"] = MaxZeroFilledElements;
			CHECK_MESSAGE(load("ZeroFilledLimit.gltf", gltf), AssetManager::GetError("Assets/Models/ZeroFilledLimit.gltf"));
			gltf["accessors"][0]["count"] = MaxZeroFilledElements + 1;
			CHECK_FALSE(load("ZeroFilledPastLimit.gltf", gltf));
			CHECK(has("ZeroFilledPastLimit.gltf", "more than " + std::to_string(MaxZeroFilledElements) + " vertices and indices in accessors without a buffer view"));
			// Each morph target can generate the normals again, so zero-filled elements count once more per target of their
			// primitive (64 targets over 65535 zero-filled positions took 10 s in the fuzz build).
			nlohmann::json morphed = gltf;
			morphed["accessors"][0]["count"] = MaxZeroFilledElements / 64;
			morphed["accessors"].push_back(morphed["accessors"][0]);
			morphed["meshes"][0]["primitives"][0]["targets"] = nlohmann::json(63, { { "POSITION", morphed["accessors"].size() - 1 } });
			CHECK_MESSAGE(load("ZeroFilledTargets.gltf", morphed), AssetManager::GetError("Assets/Models/ZeroFilledTargets.gltf"));
			morphed["meshes"][0]["primitives"][0]["targets"].push_back(morphed["meshes"][0]["primitives"][0]["targets"][0]);
			CHECK_FALSE(load("ZeroFilledPastTargets.gltf", morphed));
			CHECK(has("ZeroFilledPastTargets.gltf", "more than " + std::to_string(MaxZeroFilledElements) + " vertices and indices in accessors without a buffer view (each counted once more per morph target of its primitive) (reached at mesh 'QuadMesh' primitive 0)"));
			// The multiplier is per primitive: zero-filled positions without targets, then a quad with 64 targets, load.
			nlohmann::json scoped = MakeUntexturedGltfJson(EmbeddedBufferJson(buffer));
			scoped["accessors"].push_back({ { "componentType", 5126 }, { "count", 60000 }, { "type", "VEC3" } });
			scoped["accessors"].push_back({ { "componentType", 5126 }, { "count", 4 }, { "type", "VEC3" } });
			scoped["meshes"].push_back(scoped["meshes"][0]);
			scoped["meshes"][0]["primitives"][0] = { { "attributes", { { "POSITION", 3 } } } };
			scoped["meshes"][1]["primitives"][0]["targets"] = nlohmann::json(64, { { "POSITION", 4 } });
			scoped["nodes"].push_back({ { "mesh", 1 } });
			scoped["scenes"][0]["nodes"].push_back(scoped["nodes"].size() - 1);
			CHECK_MESSAGE(load("ZeroFilledScoped.gltf", scoped), AssetManager::GetError("Assets/Models/ZeroFilledScoped.gltf"));
			gltf["accessors"][0]["count"] = 400000000;
			CHECK_FALSE(load("ZeroFilledHuge.gltf", gltf));
			CHECK(has("ZeroFilledHuge.gltf", "more than " + std::to_string(MaxZeroFilledElements) + " vertices and indices in accessors without a buffer view"));
			// The totals must not wrap. cgltf parses counts as long long, and the limits are checked after each primitive,
			// but JSON allows duplicate keys: one primitive naming four positions of 2^62 + 1 adds up to 2^64 + 4.
			nlohmann::json wrapping = gltf;
			wrapping["accessors"][0]["count"] = 4611686018427387905ull;
			std::string json = wrapping.dump();
			const std::string position = "\"POSITION\":0";
			const size_t at = json.find(position);
			REQUIRE(at != std::string::npos);
			json.replace(at, position.size(), position + "," + position + "," + position + "," + position);
			project.WriteFile("Assets/Models/Wrapping.gltf", json);
			AssetManager::Clear();
			CHECK_FALSE(AssetManager::GetMesh("Assets/Models/Wrapping.gltf"));
			CHECK(has("Wrapping.gltf", "more than " + std::to_string(MaxZeroFilledElements) + " vertices and indices in accessors without a buffer view"));
		}

		SUBCASE("Primitives sharing one accessor")
		{
			// 257 primitives x 65536 positions pass the vertex limit by 65536; Positions.bin is only 768 KiB.
			const nlohmann::json positions = { { "componentType", 5126 }, { "count", 65536 }, { "type", "VEC3" }, { "min", { 0, 0, 0 } }, { "max", { 0, 0, 0 } } };
			const nlohmann::json vertices = share(gltf, "Positions.bin", size_t{ 65536 } * 12, positions, false, MaxModelVertices / 65536 + 1);
			CHECK_FALSE(load("Vertices.gltf", vertices));
			CHECK(has("Vertices.gltf", "its meshes have more than " + std::to_string(MaxModelVertices) + " vertices (counted once per primitive that uses them) (reached at mesh 'QuadMesh' primitive 256)"));

			// The limits are per model, not per mesh.
			nlohmann::json split = vertices;
			nlohmann::json second = { { "primitives", nlohmann::json::array() } };
			while (second["primitives"].size() < 128)
			{
				second["primitives"].push_back(split["meshes"][0]["primitives"].back());
				split["meshes"][0]["primitives"].erase(split["meshes"][0]["primitives"].size() - 1);
			}
			split["meshes"].push_back(second);
			split["nodes"].push_back({ { "mesh", 1 } });
			split["scenes"][0]["nodes"].push_back(split["nodes"].size() - 1);
			CHECK_FALSE(load("Meshes.gltf", split));
			CHECK(has("Meshes.gltf", "its meshes have more than " + std::to_string(MaxModelVertices) + " vertices (counted once per primitive that uses them) (reached at mesh 1 primitive 127)"));

			// Primitives the importer skips (points here) do not count; the quad still loads.
			nlohmann::json points = vertices;
			for (auto& primitive : points["meshes"][0]["primitives"])
				primitive["mode"] = 0;
			points["meshes"][0]["primitives"].push_back(gltf["meshes"][0]["primitives"][0]);
			CHECK_MESSAGE(load("Points.gltf", points), AssetManager::GetError("Assets/Models/Points.gltf"));
			// 65 primitives x 2^20 byte indices pass the index limit.
			const nlohmann::json indices = { { "componentType", 5121 }, { "count", 1 << 20 }, { "type", "SCALAR" } };
			CHECK_FALSE(load("Indices.gltf", share(gltf, "Indices.bin", 1 << 20, indices, true, MaxModelIndices / (1 << 20) + 1)));
			CHECK(has("Indices.gltf", "its meshes have more than " + std::to_string(MaxModelIndices) + " indices (counted once per primitive that uses them) (reached at mesh 'QuadMesh' primitive 64)"));
		}
		AssetManager::Clear();
	}

	// Regression (FuzzGltf, nightly): 46116 zero-filled indices name one vertex, and meshoptimizer's vertex cache order,
	// whose time grows with the squared valences, took over 10 s on them. The model keeps its triangle order instead. (The
	// fuzzer's file, Tests/Fuzz/Corpus/FuzzGltf/ZeroFilledIndexValence.gltf, only parses because cgltf's JSON reader is
	// lenient; this is its clean equivalent.)
	TEST_CASE("glTF primitives whose vertices are in too many triangles skip optimization")
	{
		BasaltTest::TempProject project("GltfValence");
		nlohmann::json gltf = MakeUntexturedGltfJson(EmbeddedBufferJson(MakeQuadBuffer()));
		gltf["accessors"][2].erase("bufferView");
		gltf["accessors"][2]["count"] = 46116;
		project.WriteFile("Assets/Models/Valence.gltf", gltf.dump());
		AssetManager::Clear();
		const auto since = Log::GetHistory().GetTotalCount();
		const auto mesh = AssetManager::GetMesh("Assets/Models/Valence.gltf");
		REQUIRE_MESSAGE(mesh, AssetManager::GetError("Assets/Models/Valence.gltf"));
		REQUIRE(mesh->Submeshes.size() == 1);
		CHECK(mesh->Submeshes[0].IndexCount == 46116);
		CHECK(mesh->Submeshes[0].VertexCount == 1); // the vertex fetch remap still drops the unused vertices
		CHECK(mesh->Submeshes[0].LODs.empty());
		CHECK(BasaltTest::CountLogMessages(since, "Valence.gltf: mesh 'QuadMesh' primitive 0: the squares of its vertices' triangle counts sum past " + std::to_string(MeshOptimizer::MaxValenceCostPerIndex) + " per index") == 1);

		// Primitives share the model's allowance: two whose busy vertex fits it alone, but not twice, sharing one index
		// accessor (eight such primitives took 12 s in the fuzz build before).
		gltf["accessors"][2]["count"] = 1800;
		gltf["meshes"][0]["primitives"].push_back(gltf["meshes"][0]["primitives"][0]);
		project.WriteFile("Assets/Models/Shared.gltf", gltf.dump());
		AssetManager::Clear();
		const auto sharedSince = Log::GetHistory().GetTotalCount();
		const auto shared = AssetManager::GetMesh("Assets/Models/Shared.gltf");
		REQUIRE_MESSAGE(shared, AssetManager::GetError("Assets/Models/Shared.gltf"));
		CHECK(BasaltTest::CountLogMessages(sharedSince, "Shared.gltf: mesh 'QuadMesh' primitive 0: the squares") == 0);
		CHECK(BasaltTest::CountLogMessages(sharedSince, "Shared.gltf: mesh 'QuadMesh' primitive 1: the squares") == 1);
		AssetManager::Clear();
	}

	TEST_CASE("glTF primitives with non-finite positions or empty indices are reported")
	{
		BasaltTest::TempProject project("GltfPrimitiveWarnings");
		std::string buffer = MakeQuadBuffer();
		const float nan = std::numeric_limits<float>::quiet_NaN();
		std::memcpy(buffer.data() + 4, &nan, sizeof(nan)); // the first position's y
		project.WriteFile("Assets/Models/NaN.gltf", MakeUntexturedGltfJson(EmbeddedBufferJson(buffer)).dump());
		AssetManager::Clear();
		auto since = Log::GetHistory().GetTotalCount();
		const auto nonFinite = AssetManager::GetMesh("Assets/Models/NaN.gltf");
		REQUIRE_MESSAGE(nonFinite, AssetManager::GetError("Assets/Models/NaN.gltf"));
		CHECK(nonFinite->Submeshes[0].LODs.empty());
		CHECK(BasaltTest::CountLogMessages(since, "NaN.gltf: mesh 'QuadMesh' primitive 0: optimization and LODs skipped for non-finite vertex positions") == 1);

		// cgltf accepts an index accessor without elements; that primitive is skipped and the next one still loads.
		nlohmann::json empty = MakeUntexturedGltfJson(EmbeddedBufferJson(MakeQuadBuffer()));
		empty["accessors"].push_back({ { "bufferView", 2 }, { "componentType", 5123 }, { "count", 0 }, { "type", "SCALAR" } });
		empty["meshes"][0]["primitives"].push_back(empty["meshes"][0]["primitives"][0]);
		empty["meshes"][0]["primitives"][0]["indices"] = 3;
		project.WriteFile("Assets/Models/Empty.gltf", empty.dump());
		AssetManager::Clear();
		since = Log::GetHistory().GetTotalCount();
		const auto skipped = AssetManager::GetMesh("Assets/Models/Empty.gltf");
		REQUIRE_MESSAGE(skipped, AssetManager::GetError("Assets/Models/Empty.gltf"));
		CHECK(skipped->Submeshes.size() == 1);
		CHECK(BasaltTest::CountLogMessages(since, "Empty.gltf: mesh 'QuadMesh' primitive 0: skipped, its index accessor is empty") == 1);
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

	TEST_CASE("Thumbnail kinds follow the file type")
	{
		CHECK(ClassifyThumbnail("Assets/Textures/Wall.PNG") == ThumbnailKind::Image);
		CHECK(ClassifyThumbnail("a.jpeg") == ThumbnailKind::Image);
		CHECK(ClassifyThumbnail("Assets/Textures/Wall.KTX2") == ThumbnailKind::Image);
		CHECK(ClassifyThumbnail("Assets/Sky.hdr") == ThumbnailKind::Environment);
		CHECK(ClassifyThumbnail("Ship.glb") == ThumbnailKind::Model);
		CHECK(ClassifyThumbnail("Ship.gltf") == ThumbnailKind::Model);
		CHECK(ClassifyThumbnail("Enemy.bprefab") == ThumbnailKind::Prefab);
		for (const char* none : { "Main.bscene", "Player.lua", "Beep.wav", "Font.ttf", "README", "Assets/Models" })
			CHECK(ClassifyThumbnail(none) == ThumbnailKind::None);
		CHECK(IsRenderedThumbnail(ThumbnailKind::Model));
		CHECK_FALSE(IsRenderedThumbnail(ThumbnailKind::Environment));
	}

	TEST_CASE("Image thumbnails average source pixels and keep the aspect ratio")
	{
		// 4x2: left half black, right half white; one red pixel.
		std::vector<uint8_t> pixels;
		for (uint32_t y = 0; y < 2; y++)
			for (uint32_t x = 0; x < 4; x++)
			{
				const uint8_t value = x < 2 ? 0 : 255;
				pixels.insert(pixels.end(), { value, value, value, 255 });
			}
		ThumbnailImage half = DownsampleImage(pixels, 4, 2, 2, 1);
		REQUIRE(half.Width == 2);
		REQUIRE(half.Height == 1);
		CHECK(half.Pixels == std::vector<uint8_t>{ 0, 0, 0, 255, 255, 255, 255, 255 });
		ThumbnailImage single = DownsampleImage(pixels, 4, 2, 1, 1);
		CHECK(single.Pixels[0] == 128);
		// Never upscaled; bad input gives an empty image.
		CHECK(DownsampleImage(pixels, 4, 2, 64, 64).Width == 4);
		CHECK(DownsampleImage(pixels, 4, 3, 2, 2).Pixels.empty());

		TextureSource wide;
		wide.Width = 300;
		wide.Height = 100;
		wide.Pixels.assign(static_cast<size_t>(300) * 100 * 4, 200);
		ThumbnailImage fitted = MakeImageThumbnail(wide, 128);
		CHECK(fitted.Width == 128);
		CHECK(fitted.Height == 43);
		CHECK(fitted.Pixels[0] == 200);
		TextureSource tall = wide;
		std::swap(tall.Width, tall.Height);
		fitted = MakeImageThumbnail(tall, 64);
		CHECK(fitted.Width == 21);
		CHECK(fitted.Height == 64);
		TextureSource tiny = wide;
		tiny.Width = tiny.Height = 2;
		tiny.Pixels.resize(16);
		CHECK(MakeImageThumbnail(tiny, 128).Width == 2);
	}

	TEST_CASE("HDR thumbnails tonemap linear radiance monotonically and ignore non-finite texels")
	{
		TextureSource hdr;
		hdr.Format = TextureFormat::RGBA32F;
		hdr.Width = 5;
		hdr.Height = 1;
		const float values[5] = { 0.0f, 0.18f, 1.0f, 16.0f, std::numeric_limits<float>::infinity() };
		for (const float value : values)
		{
			const float texel[4] = { value, value * 0.5f, std::nanf(""), 1.0f };
			const auto* bytes = reinterpret_cast<const uint8_t*>(texel);
			hdr.Pixels.insert(hdr.Pixels.end(), bytes, bytes + sizeof(texel));
		}
		const ThumbnailImage image = MakeImageThumbnail(hdr, 8);
		REQUIRE(image.Width == 5);
		CHECK(image.Pixels[0] == 0);
		CHECK(image.Pixels[4] > image.Pixels[0]);
		CHECK(image.Pixels[8] > image.Pixels[4]);
		CHECK(image.Pixels[12] > image.Pixels[8]);
		CHECK(image.Pixels[12] > 245); // bright light approaches white without wrapping
		CHECK(image.Pixels[16] == 0);  // infinity counts as black
		for (size_t pixel = 0; pixel < 5; pixel++)
		{
			CHECK(image.Pixels[pixel * 4 + 2] == 0); // NaN channel
			CHECK(image.Pixels[pixel * 4 + 3] == 255);
		}
		// Averaging happens in linear light before tonemapping: a black and a bright texel preview as their mean.
		TextureSource pair = hdr;
		pair.Width = 2;
		pair.Pixels.resize(32);
		const float dark[4] = { 0, 0, 0, 1 }, bright[4] = { 4, 4, 4, 1 };
		std::memcpy(pair.Pixels.data(), dark, 16);
		std::memcpy(pair.Pixels.data() + 16, bright, 16);
		TextureSource mean = hdr;
		mean.Width = 1;
		mean.Pixels.resize(16);
		const float middle[4] = { 2, 2, 2, 1 };
		std::memcpy(mean.Pixels.data(), middle, 16);
		CHECK(MakeImageThumbnail(pair, 1).Pixels == MakeImageThumbnail(mean, 1).Pixels);
	}

	TEST_CASE("The thumbnail cache stores previews per source version and prunes removed sources")
	{
		BasaltTest::TempDirectory project("ThumbnailCache");
		ThumbnailCache cache(project.GetPath());
		CHECK(cache.GetDirectory() == project.GetPath() / "Cache" / "Thumbnails");
		std::string error;
		CHECK_FALSE(cache.MakeKey("Assets/Missing.png", error));
		CHECK(error == "the file is missing");

		REQUIRE(FileSystem::WriteTextFile(project.GetPath() / "Assets/Wall.png", "v1"));
		REQUIRE(FileSystem::WriteTextFile(project.GetPath() / "Assets/Other.png", "v1"));
		const std::string key = cache.MakeKey("Assets/Wall.png", error).value_or("");
		REQUIRE_FALSE(key.empty());
		CHECK_FALSE(cache.Load("Assets/Wall.png", key));
		const ThumbnailImage image{ 2, 1, { 10, 20, 30, 255, 40, 50, 60, 128 } };
		REQUIRE_MESSAGE(cache.Store("Assets/Wall.png", key, { image, { "2 x 1", "multi\nline" } }, error), error);
		const std::string otherKey = cache.MakeKey("Assets/Other.png", error).value_or("");
		REQUIRE(cache.Store("Assets/Other.png", otherKey, { image, {} }, error));
		const auto loaded = cache.Load("Assets/Wall.png", key);
		REQUIRE(loaded);
		CHECK(loaded->Image.Width == 2);
		CHECK(loaded->Image.Pixels == image.Pixels);
		// Details keep one line each.
		CHECK(loaded->Details == std::vector<std::string>{ "2 x 1", "multi line" });
		const auto other = cache.Load("Assets/Other.png", otherKey);
		REQUIRE(other);
		CHECK(other->Details.empty());
		// Only the exact key matches.
		CHECK_FALSE(cache.Load("Assets/Wall.png", key.substr(0, key.size() - 1)));
		CHECK_FALSE(cache.Load("Assets/Wall.png", key + "0"));
		CHECK(cache.GetImagePath("Assets/Wall.png").parent_path() == cache.GetDirectory());
		CHECK(cache.GetImagePath("Assets/Wall.png") != cache.GetImagePath("Assets/Other.png"));
		// An empty image is never stored.
		CHECK_FALSE(cache.Store("Assets/Wall.png", key, { ThumbnailImage{}, {} }, error));
		CHECK(cache.Load("Assets/Wall.png", key));

		// Rewriting the source (new size or modification time) makes the entry stale.
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		REQUIRE(FileSystem::WriteTextFile(project.GetPath() / "Assets/Wall.png", "version 2"));
		const std::string changed = cache.MakeKey("Assets/Wall.png", error).value_or("");
		CHECK(changed != key);
		CHECK_FALSE(cache.Load("Assets/Wall.png", changed));
		CHECK_FALSE(cache.Load("Assets/Wall.png", ""));

		// Pruning removes entries of deleted sources, and images an interrupted store left without a key.
		std::filesystem::remove(project.GetPath() / "Assets/Other.png");
		REQUIRE(FileSystem::WriteTextFile(cache.GetDirectory() / "0123456789abcdef.png", "orphan"));
		// Counts are files: Other.png's image and key, and the orphan image.
		CHECK(cache.PruneMissing() == 3);
		CHECK_FALSE(std::filesystem::exists(cache.GetImagePath("Assets/Other.png")));
		CHECK_FALSE(std::filesystem::exists(cache.GetDirectory() / "0123456789abcdef.png"));
		CHECK(std::filesystem::exists(cache.GetImagePath("Assets/Wall.png")));
		// Wall.png's image and key.
		CHECK(cache.Clear() == 2);
		CHECK_FALSE(std::filesystem::exists(cache.GetImagePath("Assets/Wall.png")));
		CHECK(cache.Clear() == 0);
	}

	TEST_CASE("Thumbnail keys cover the files a preview also reads")
	{
		BasaltTest::TempDirectory project("ThumbnailDependencies");
		ThumbnailCache cache(project.GetPath());
		const std::filesystem::path& root = project.GetPath();
		REQUIRE(FileSystem::WriteTextFile(root / "Assets/Models/Ship.gltf", R"({"asset": {"version": "2.0"},
			"buffers": [{"uri": "Ship%20Data.bin", "byteLength": 4}],
			"images": [{"uri": "../Textures/Hull.png"}, {"uri": "data:image/png;base64,AAAA"}]})"));
		REQUIRE(FileSystem::WriteTextFile(root / "Assets/Models/Ship Data.bin", "abcd"));
		REQUIRE(FileSystem::WriteTextFile(root / "Assets/Textures/Hull.png", "v1"));
		REQUIRE(FileSystem::WriteTextFile(root / "Assets/Prefabs/Fleet.bprefab", R"({"Entities": [{"ID": 1, "Components": {
			"Mesh": {"Mesh": "Assets/Models/Ship.gltf"}, "Material": {"AlbedoMap": "Assets/Textures/Paint.png"}, "Tag": {"Tag": "not a path"}}}]})"));
		REQUIRE(FileSystem::WriteTextFile(root / "Assets/Textures/Paint.png", "v1"));

		CHECK(cache.GetDependencies("Assets/Models/Ship.gltf") == std::vector<std::string>{ "Assets/Models/Ship Data.bin", "Assets/Textures/Hull.png" });
		CHECK(cache.GetDependencies("Assets/Prefabs/Fleet.bprefab") == std::vector<std::string>{ "Assets/Models/Ship Data.bin", "Assets/Models/Ship.gltf", "Assets/Textures/Hull.png", "Assets/Textures/Paint.png" });
		CHECK(cache.GetDependencies("Assets/Textures/Paint.png").empty());

		std::string error;
		const std::string model = cache.MakeKey("Assets/Models/Ship.gltf", error).value_or("");
		const std::string prefab = cache.MakeKey("Assets/Prefabs/Fleet.bprefab", error).value_or("");
		REQUIRE_FALSE(model.empty());
		// A texture the model uses changes: both previews are stale, though their own files did not change.
		REQUIRE(FileSystem::WriteTextFile(root / "Assets/Textures/Hull.png", "version 2"));
		CHECK(cache.MakeKey("Assets/Models/Ship.gltf", error).value_or("") != model);
		CHECK(cache.MakeKey("Assets/Prefabs/Fleet.bprefab", error).value_or("") != prefab);
		// A dependency that goes missing changes the key too, and the key still exists.
		std::filesystem::remove(root / "Assets/Textures/Paint.png");
		const auto missing = cache.MakeKey("Assets/Prefabs/Fleet.bprefab", error);
		REQUIRE(missing);
		CHECK(missing->find("Assets/Textures/Paint.png missing") != std::string::npos);
	}

	TEST_CASE("Thumbnail cache entries of non-ASCII files survive pruning")
	{
		BasaltTest::TempDirectory project("ThumbnailUnicode");
		ThumbnailCache cache(project.GetPath());
		const std::string name = "Assets/\xC3\x84rger.png";
		REQUIRE(FileSystem::WriteTextFile(project.GetPath() / FileSystem::PathFromUtf8(name), "x"));
		std::string error;
		const auto key = cache.MakeKey(name, error);
		REQUIRE_MESSAGE(key, error);
		REQUIRE(cache.Store(name, *key, { ThumbnailImage{ 1, 1, { 1, 2, 3, 4 } }, {} }, error));
		CHECK(cache.PruneMissing() == 0);
		CHECK(cache.Load(name, *key));
	}

	TEST_CASE("New projects keep editor caches out of version control")
	{
		BasaltTest::TempProject temp("ProjectGitignore");
		REQUIRE(temp.IsValid());
		CHECK(FileSystem::ReadTextFile(temp.GetDirectory() / ".gitignore").value_or("") == "Cache/\n");

		// An existing ignore file (a fresh repository's) keeps its rules and gains the cache.
		BasaltTest::TempDirectory directory("ProjectGitignoreExisting");
		REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / ".gitignore", "*.tmp"));
		std::string error;
		REQUIRE_MESSAGE(Project::Create(directory.GetPath(), "Existing", error), error);
		CHECK(FileSystem::ReadTextFile(directory.GetPath() / ".gitignore").value_or("") == "*.tmp\nCache/\n");
		// Already ignored: nothing is added.
		BasaltTest::TempDirectory ignored("ProjectGitignoreIgnored");
		REQUIRE(FileSystem::WriteTextFile(ignored.GetPath() / ".gitignore", "Cache/\nSaved/\n"));
		REQUIRE(Project::Create(ignored.GetPath(), "Ignored", error));
		CHECK(FileSystem::ReadTextFile(ignored.GetPath() / ".gitignore").value_or("") == "Cache/\nSaved/\n");
		// Only a rule for the folder itself counts: "Cache/Thumbnails" leaves the rest of Cache/ tracked.
		BasaltTest::TempDirectory partial("ProjectGitignorePartial");
		REQUIRE(FileSystem::WriteTextFile(partial.GetPath() / ".gitignore", "Cache/Thumbnails\r\n"));
		REQUIRE(Project::Create(partial.GetPath(), "Partial", error));
		CHECK(FileSystem::ReadTextFile(partial.GetPath() / ".gitignore").value_or("") == "Cache/Thumbnails\r\nCache/\n");
	}
}
