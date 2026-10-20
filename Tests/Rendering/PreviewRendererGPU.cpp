#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "GPUFixture.h"

#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/TextureCompression.h>
#include <Basalt/Asset/ImageUtils.h>
#include <Basalt/Core/FileSystem.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Renderer/DebugDraw.h>
#include <Basalt/Renderer/PreviewRenderer.h>

#include "../Source/TestUtils.h"
#include "../Source/MeshTestUtils.h"

#include <glm/gtc/constants.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

using namespace Basalt;

namespace {

	struct Rgb
	{
		int R, G, B;
	};

	Rgb At(const ThumbnailImage& image, uint32_t x, uint32_t y)
	{
		const uint8_t* pixel = image.Pixels.data() + (static_cast<size_t>(y) * image.Width + x) * 4;
		return { pixel[0], pixel[1], pixel[2] };
	}

	int Difference(const Rgb& a, const Rgb& b)
	{
		return std::abs(a.R - b.R) + std::abs(a.G - b.G) + std::abs(a.B - b.B);
	}

	// The preview, or a failed test that reports why it was not made.
	ThumbnailImage Made(std::optional<ThumbnailImage> image, const std::string& error)
	{
		if (!image)
		{
			FAIL("no preview: ", error);
			return {};
		}
		return std::move(*image);
	}

	void Save(const ThumbnailImage& image, const char* name)
	{
		std::string error;
		CHECK_MESSAGE(WritePng(std::filesystem::path(BASALT_PREVIEW_OUTPUT_DIR) / name, image.Pixels, image.Width, image.Height, error), error);
	}

	// The object fills the middle of the preview and the sky shows in the corners.
	void CheckFramed(const ThumbnailImage& image, const char* what)
	{
		INFO(what);
		REQUIRE(image.Width == PreviewRenderer::OutputSize);
		REQUIRE(image.Height == PreviewRenderer::OutputSize);
		const Rgb centre = At(image, image.Width / 2, image.Height / 2);
		const Rgb corner = At(image, 2, 2);
		CHECK(Difference(centre, corner) > 40);
		// Corners are sky: the same colour on both top corners, bluish rather than black.
		CHECK(Difference(corner, At(image, image.Width - 3, 2)) < 12);
		CHECK(corner.B > 60);
		// Framed, not cropped: the edges are still sky.
		CHECK(Difference(At(image, image.Width / 2, 1), At(image, image.Width / 2 + 10, 1)) < 12);
	}

}

TEST_CASE("Previews frame models, prefabs and materials under the studio lighting")
{
	auto graphics = CreateTestGraphics();
	std::string error;
	const Ref<Project> project = Project::Load(std::filesystem::path(BASALT_TEST_DATA_DIR) / "FeatureTest", error);
	REQUIRE_MESSAGE(project, error);
	Project::SetActive(project);

	PreviewRenderer previews(graphics->GetDevice());
	REQUIRE(previews.Initialize());
	// Pending viewport lines survive a preview and do not appear in it.
	DebugDraw::Clear();
	DebugDraw::Line(glm::vec3(-100.0f), glm::vec3(100.0f), glm::vec4(1.0f, 0.0f, 1.0f, 1.0f));

	const ThumbnailImage model = Made(previews.RenderModel("Assets/Models/TexturedCube.glb", error), error);
	CheckFramed(model, "model");
	Save(model, "Model.png");
	CHECK(DebugDraw::GetLineCount() == 1);

	// Rendering again gives the same image (no exposure or resource history between previews).
	CHECK(Made(previews.RenderModel("Assets/Models/TexturedCube.glb", error), error).Pixels == model.Pixels);

	// A built-in mesh is framed the same way as a model file.
	CheckFramed(Made(previews.RenderModel("builtin://Sphere", error), error), "builtin sphere");

	const ThumbnailImage prefab = Made(previews.RenderPrefab("Assets/Prefabs/Projectile.bprefab", error), error);
	CheckFramed(prefab, "prefab");
	Save(prefab, "Prefab.png");
	// The projectile is orange (and so bright it nearly saturates).
	const Rgb projectile = At(prefab, prefab.Width / 2, prefab.Height / 2);
	CHECK(projectile.R > projectile.B + 30);

	MaterialComponent red;
	red.AlbedoColor = { 1.0f, 0.05f, 0.05f, 1.0f };
	const ThumbnailImage redSphere = Made(previews.RenderMaterial(red, error), error);
	CheckFramed(redSphere, "red material");
	Save(redSphere, "MaterialRed.png");
	const Rgb redCentre = At(redSphere, redSphere.Width / 2, redSphere.Height / 2);
	CHECK(redCentre.R > 2 * redCentre.G);
	CHECK(redCentre.R > 2 * redCentre.B);

	// Emission shows on top of the lit surface.
	MaterialComponent dark;
	dark.AlbedoColor = { 0.1f, 0.1f, 0.1f, 1.0f };
	const ThumbnailImage plain = Made(previews.RenderMaterial(dark, error), error);
	MaterialComponent glowing = dark;
	glowing.EmissiveColor = { 0.1f, 0.8f, 0.1f };
	const ThumbnailImage glow = Made(previews.RenderMaterial(glowing, error), error);
	Save(glow, "MaterialEmissive.png");
	const Rgb plainCentre = At(plain, plain.Width / 2, plain.Height / 2);
	const Rgb glowCentre = At(glow, glow.Width / 2, glow.Height / 2);
	CHECK(glowCentre.G > plainCentre.G + 80);
	CHECK(glowCentre.G > glowCentre.R + 40);

	// Failures are reported, not rendered.
	CHECK_FALSE(previews.RenderModel("Assets/Models/Missing.glb", error));
	CHECK_FALSE(error.empty());
	CHECK_FALSE(previews.RenderPrefab("Assets/Prefabs/Missing.bprefab", error));
	// A prefab with nothing to draw says so (an absolute path: the test data stays untouched).
	const std::filesystem::path empty = std::filesystem::path(BASALT_PREVIEW_OUTPUT_DIR) / "Empty.bprefab";
	REQUIRE(FileSystem::WriteTextFile(empty, R"({"Format": "BasaltPrefab", "Version": 1, "Entities": [{"ID": 1, "Name": "Root", "Parent": 0, "Components": {}}]})", error));
	CHECK_FALSE(previews.RenderPrefab(FileSystem::PathToUtf8(empty), error));
	CHECK(error == "nothing to show (no meshes)");

	DebugDraw::Clear();
	AssetManager::Clear();
	Project::SetActive(nullptr);
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("BC7 mip upload and material extensions render with validation")
{
	auto graphics = CreateTestGraphics();
	BasaltTest::TempDirectory temp("MaterialExtensionsGPU");
	std::string error;
	TextureSource source;
	source.Width = 16;
	source.Height = 16;
	source.Pixels.resize(size_t{ 16 } * 16 * 4);
	for (size_t i = 0; i < source.Pixels.size(); i += 4)
	{
		source.Pixels[i] = 190;
		source.Pixels[i + 1] = 30;
		source.Pixels[i + 2] = 50;
		source.Pixels[i + 3] = 255;
	}
	const auto png = temp.GetPath() / "color.png", ktx = temp.GetPath() / "color.ktx2";
	REQUIRE(WritePng(png, source.Pixels, source.Width, source.Height, error));
	TextureCompression::ImportResult imported;
	REQUIRE(TextureCompression::Import(png, ktx, temp.GetPath() / "Cache", true, imported, error));
	PreviewRenderer previews(graphics->GetDevice());
	REQUIRE(previews.Initialize());
	MaterialComponent material;
	material.Roughness = 0.45f;
	material.AlbedoMap = FileSystem::PathToUtf8(png);
	const auto original = Made(previews.RenderMaterial(material, error), error);
	material.AlbedoMap = FileSystem::PathToUtf8(ktx);
	const auto compressed = Made(previews.RenderMaterial(material, error), error);
	CHECK(Difference(At(original, 64, 64), At(compressed, 64, 64)) < 12);
	material.Clearcoat = 1;
	material.ClearcoatRoughness = 0.05f;
	const auto coat = Made(previews.RenderMaterial(material, error), error);
	Save(coat, "Clearcoat.png");
	size_t different = 0;
	for (size_t i = 0; i < coat.Pixels.size(); i++)
		if (std::abs(int(coat.Pixels[i]) - int(compressed.Pixels[i])) > 3)
			different++;
	CHECK(different > 100);
	material.Transmission = 1;
	material.Metallic = 0;
	const auto glass = Made(previews.RenderMaterial(material, error), error);
	Save(glass, "Transmission.png");
	CHECK(Difference(At(coat, 64, 64), At(glass, 64, 64)) > 20);
	// A red-channel-zero transmission map restores the opaque coated result.
	material.TransmissionMap = FileSystem::PathToUtf8(temp.GetPath() / "zero.png");
	std::fill(source.Pixels.begin(), source.Pixels.end(), uint8_t(0));
	REQUIRE(WritePng(temp.GetPath() / "zero.png", source.Pixels, source.Width, source.Height, error));
	const auto zeroTransmission = Made(previews.RenderMaterial(material, error), error);
	material.Transmission = 0.01f;
	CHECK(bool(Made(previews.RenderMaterial(material, error), error).Pixels == zeroTransmission.Pixels));
	CHECK(Difference(At(zeroTransmission, 64, 64), At(coat, 64, 64)) < 8);
	// Constant UV transforms select opposite halves of an emissive image.
	for (size_t i = 0; i < source.Pixels.size(); i += 4)
	{
		const bool left = (i / 4) % 16 < 8;
		source.Pixels[i] = left ? 255 : 0;
		source.Pixels[i + 1] = left ? 0 : 255;
		source.Pixels[i + 2] = 0;
		source.Pixels[i + 3] = 255;
	}
	REQUIRE(WritePng(temp.GetPath() / "split.png", source.Pixels, source.Width, source.Height, error));
	material = MaterialComponent{};
	material.AlbedoColor = { 0, 0, 0, 1 };
	material.EmissiveColor = { 1, 1, 1 };
	material.EmissiveIntensity = 0.5f;
	material.EmissiveMap = FileSystem::PathToUtf8(temp.GetPath() / "split.png");
	material.UVScale = glm::vec2(0);
	material.UVOffset = { .25f, .5f };
	const auto left = Made(previews.RenderMaterial(material, error), error);
	material.UVOffset.x = .75f;
	const auto right = Made(previews.RenderMaterial(material, error), error);
	CHECK(At(left, 64, 64).R > At(left, 64, 64).G + 80);
	CHECK(At(right, 64, 64).G > At(right, 64, 64).R + 80);
	material.UVScale = { 0, 0.5f };
	material.UVOffset = { 0.5f, 0.25f };
	material.UVRotation = glm::half_pi<float>();
	const auto rotated = Made(previews.RenderMaterial(material, error), error);
	material.UVRotation = -glm::half_pi<float>();
	const auto inverse = Made(previews.RenderMaterial(material, error), error);
	CHECK(At(rotated, 64, 64).R > At(rotated, 64, 64).G + 80);
	CHECK(At(inverse, 64, 64).G > At(inverse, 64, 64).R + 80);
	AssetManager::Clear();
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Imported UV1 and texture rotation preserve normal orientation and alpha-mode coverage")
{
	auto graphics = CreateTestGraphics();
	BasaltTest::TempDirectory temp("ImportedMaterialsGPU");
	const auto& root = temp.GetPath();
	std::string error;
	auto plane = Primitives::CreatePlane();
	for (auto& vertex : plane->Vertices)
		vertex.TexCoord1 = { 0.75f, 0.5f };
	const auto path = BasaltTest::WriteSphereGltf(root, false, plane);
	const auto text = FileSystem::ReadTextFile(path);
	REQUIRE(text);
	auto model = nlohmann::json::parse(text.value_or("{}"));
	TextureSource normal;
	normal.Width = 2;
	normal.Height = 2;
	normal.Pixels = { 128, 200, 230, 255, 128, 200, 230, 255, 128, 200, 230, 255, 128, 200, 230, 255 };
	REQUIRE(WritePng(root / "normal.png", normal.Pixels, 2, 2, error));
	model["images"] = { { { "uri", "normal.png" } } };
	model["textures"] = { { { "source", 0 } } };
	model["materials"][0]["pbrMetallicRoughness"] = { { "metallicFactor", 0 }, { "roughnessFactor", 0.65 } };
	model["materials"][0]["normalTexture"] = { { "index", 0 } };
	PreviewRenderer previews(graphics->GetDevice());
	REQUIRE(previews.Initialize());
	auto render = [&] { REQUIRE(FileSystem::WriteTextFile(path,model.dump()));AssetManager::Clear();return Made(previews.RenderModel(FileSystem::PathToUtf8(path),error),error); };
	const auto base = render();
	model["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_1"] = 3;
	model["materials"][0]["normalTexture"]["texCoord"] = 1;
	const auto uv1 = render();
	CHECK(Difference(At(base, 64, 64), At(uv1, 64, 64)) < 5);
	// The same along the tangent (red), so a flipped derived T is caught as well as a flipped B.
	normal.Pixels = { 200, 128, 230, 255, 200, 128, 230, 255, 200, 128, 230, 255, 200, 128, 230, 255 };
	REQUIRE(WritePng(root / "normal.png", normal.Pixels, 2, 2, error));
	const auto tiltedUV1 = render();
	model["materials"][0]["normalTexture"]["texCoord"] = 0;
	const auto tiltedBase = render();
	CHECK(Difference(At(tiltedBase, 64, 64), At(tiltedUV1, 64, 64)) < 5);
	CHECK(Difference(At(tiltedBase, 64, 64), At(base, 64, 64)) > 10);
	normal.Pixels = { 128, 200, 230, 255, 128, 200, 230, 255, 128, 200, 230, 255, 128, 200, 230, 255 };
	REQUIRE(WritePng(root / "normal.png", normal.Pixels, 2, 2, error));
	// Clearcoat normals: no map and a flat map agree; a tilted map changes the coat, identically through the vertex
	// frame (texCoord 0) and the derived frame (texCoord 1 holding the same UVs).
	model["materials"][0].erase("normalTexture");
	model["materials"][0]["extensions"]["KHR_materials_clearcoat"] = { { "clearcoatFactor", 1 }, { "clearcoatRoughnessFactor", 0.2 } };
	const auto smoothCoat = render();
	TextureSource flat;
	flat.Width = flat.Height = 1;
	flat.Pixels = { 128, 128, 255, 255 };
	REQUIRE(WritePng(root / "flat.png", flat.Pixels, 1, 1, error));
	model["images"].push_back({ { "uri", "flat.png" } });
	model["textures"].push_back({ { "source", 1 } });
	model["materials"][0]["extensions"]["KHR_materials_clearcoat"]["clearcoatNormalTexture"] = { { "index", 1 } };
	CHECK(Difference(At(render(), 64, 64), At(smoothCoat, 64, 64)) < 4);
	model["materials"][0]["extensions"]["KHR_materials_clearcoat"]["clearcoatNormalTexture"] = { { "index", 0 } };
	const auto coat = render();
	size_t coatChanged = 0;
	for (size_t i = 0; i < coat.Pixels.size(); i++)
		if (std::abs(int(coat.Pixels[i]) - int(smoothCoat.Pixels[i])) > 3)
			coatChanged++;
	CHECK(coatChanged > 100);
	model["materials"][0]["extensions"]["KHR_materials_clearcoat"]["clearcoatNormalTexture"]["texCoord"] = 1;
	const auto coatUV1 = render();
	CHECK(Difference(At(coat, 64, 64), At(coatUV1, 64, 64)) < 5);
	model["images"].erase(1);
	model["textures"].erase(1);
	// OPAQUE and surviving MASK coverage ignore base-color alpha even in the transmission pass.
	model["materials"][0].erase("extensions");
	model["materials"][0]["extensions"]["KHR_materials_transmission"] = { { "transmissionFactor", 0.8 } };
	model["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = { 0.5, 0.6, 0.7, 1 };
	const auto opaque = render();
	model["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"][3] = 0.5;
	CHECK(bool(render().Pixels == opaque.Pixels));
	model["materials"][0]["alphaMode"] = "MASK";
	model["materials"][0]["alphaCutoff"] = 0.2;
	CHECK(bool(render().Pixels == opaque.Pixels));
	// Below the cutoff, MASK discards the surface even though transmission sends it to the blended pass.
	model["materials"][0]["alphaCutoff"] = 0.6;
	const auto masked = render();
	CHECK(bool(masked.Pixels != opaque.Pixels));
	model["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"][3] = 0.0;
	CHECK(bool(render().Pixels == masked.Pixels));
	model["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"][3] = 0.5;
	model["materials"][0]["alphaCutoff"] = 0.2;
	model["materials"][0]["alphaMode"] = "BLEND";
	CHECK(bool(render().Pixels != opaque.Pixels));
	// A distinct second UV set reaches the seventh vertex attribute and the fragment selector.
	std::vector<uint8_t> split(size_t{ 16 } * 16 * 4, 255);
	for (size_t pixel = 0; pixel < 256; pixel++)
	{
		split[pixel * 4] = (pixel % 16 < 8) ? 255 : 0;
		split[pixel * 4 + 1] = (pixel % 16 < 8) ? 0 : 255;
		split[pixel * 4 + 2] = 0;
	}
	REQUIRE(WritePng(root / "split.png", split, 16, 16, error));
	model["images"][0]["uri"] = "split.png";
	model["accessors"].push_back({ { "bufferView", 0 }, { "byteOffset", offsetof(Vertex, TexCoord1) }, { "componentType", 5126 }, { "count", plane->Vertices.size() }, { "type", "VEC2" } });
	model["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_1"] = 5;
	model["materials"][0] = { { "pbrMetallicRoughness", { { "baseColorFactor", { 0, 0, 0, 1 } }, { "metallicFactor", 0 } } }, { "emissiveFactor", { 0.5, 0.5, 0.5 } }, { "emissiveTexture", { { "index", 0 }, { "texCoord", 1 } } } };
	const auto secondUV = render();
	CHECK(At(secondUV, 64, 64).G > At(secondUV, 64, 64).R + 60);
	model["materials"][0]["emissiveTexture"]["texCoord"] = 0;
	CHECK(bool(render().Pixels != secondUV.Pixels));
	AssetManager::Clear();
	CHECK(graphics->GetValidationErrorCount() == 0);
}
