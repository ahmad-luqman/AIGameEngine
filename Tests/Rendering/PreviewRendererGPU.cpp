#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "GPUFixture.h"

#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/TextureCompression.h>
#include "../Source/TestUtils.h"
#include <Basalt/Asset/ImageUtils.h>
#include <Basalt/Core/FileSystem.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Renderer/DebugDraw.h>
#include <Basalt/Renderer/PreviewRenderer.h>

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
	source.Pixels.resize(16 * 16 * 4);
	for (size_t i = 0; i < source.Pixels.size(); i += 4)
	{
		source.Pixels[i] = 190;
		source.Pixels[i + 1] = 30;
		source.Pixels[i + 2] = 50;
		source.Pixels[i + 3] = 255;
	}
	const auto png = temp.GetPath() / "color.png", ktx = temp.GetPath() / "color.ktx2";
	REQUIRE(WritePng(png, source.Pixels, source.Width, source.Height, error));
	bool hit = false;
	REQUIRE(TextureCompression::Import(png, ktx, temp.GetPath() / "Cache", true, hit, error));
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
	AssetManager::Clear();
	CHECK(graphics->GetValidationErrorCount() == 0);
}
