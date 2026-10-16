#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "GPUFixture.h"

#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/ImageUtils.h>
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

	DebugDraw::Clear();
	AssetManager::Clear();
	Project::SetActive(nullptr);
	CHECK(graphics->GetValidationErrorCount() == 0);
}
