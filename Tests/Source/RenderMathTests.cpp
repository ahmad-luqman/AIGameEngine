#include <doctest/doctest.h>

#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/MeshSource.h>
#include <Basalt/Renderer/DebugDraw.h>
#include <Basalt/Renderer/RenderMath.h>
#include <Basalt/Scene/SceneCamera.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using namespace Basalt;

TEST_SUITE("RenderMath")
{
	TEST_CASE("Cascade splits are increasing, finite and end at the shadow distance")
	{
		const auto perspective = ComputeCascadeSplits<4>(0.1f, 60.0f, 0.85f, false);
		for (size_t i = 0; i < 4; i++)
		{
			CHECK(std::isfinite(perspective[i]));
			if (i > 0)
				CHECK(perspective[i] > perspective[i - 1]);
		}
		CHECK(perspective[3] == doctest::Approx(60.0f));
		// The logarithmic part concentrates resolution near the camera.
		CHECK(perspective[0] < 60.0f / 4.0f);

		// Orthographic cameras commonly have a negative near plane: the log scheme would produce NaN.
		const auto orthographic = ComputeCascadeSplits<4>(-100.0f, 60.0f, 0.85f, true);
		for (float split : orthographic)
			CHECK(std::isfinite(split));
		CHECK(orthographic[3] == doctest::Approx(60.0f));
		const auto zeroNear = ComputeCascadeSplits<4>(0.0f, 50.0f, 1.0f, false);
		for (float split : zeroNear)
			CHECK(std::isfinite(split));
	}

	TEST_CASE("Frustum culling with a 0..1 depth projection")
	{
		const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
		const Frustum frustum(projection * view);
		const auto box = [](glm::vec3 center) { return AABB{ center - glm::vec3(0.5f), center + glm::vec3(0.5f) }; };
		CHECK(frustum.Intersects(box({ 0.0f, 0.0f, 0.0f })));
		CHECK_FALSE(frustum.Intersects(box({ 0.0f, 0.0f, 20.0f })));   // behind the camera
		CHECK_FALSE(frustum.Intersects(box({ 0.0f, 0.0f, -200.0f }))); // beyond the far plane
		CHECK_FALSE(frustum.Intersects(box({ 50.0f, 0.0f, 0.0f })));   // off to the side
		CHECK_FALSE(frustum.Intersects(AABB{}));
		CHECK(IsOrthographic(glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, -10.0f, 10.0f)));
		CHECK_FALSE(IsOrthographic(projection));

		SceneCamera camera;
		CHECK_FALSE(IsOrthographic(camera.GetProjection()));
		// +Y up clip space: a point above the camera's view axis projects to positive NDC y.
		const glm::vec4 clip = camera.GetProjection() * glm::vec4(0.0f, 1.0f, -5.0f, 1.0f);
		CHECK(clip.y / clip.w > 0.0f);
	}

	TEST_CASE("Generated tangents follow the glTF handedness convention")
	{
		// Plane: N = +Y, U along +X, V along +Z (image down). glTF bitangent must point to image-up (-Z).
		Ref<MeshSource> plane = Primitives::CreatePlane();
		for (const Vertex& vertex : plane->Vertices)
		{
			const glm::vec3 bitangent = glm::cross(vertex.Normal, glm::vec3(vertex.Tangent)) * vertex.Tangent.w;
			CHECK(vertex.Tangent.x == doctest::Approx(1.0f));
			CHECK(bitangent.z == doctest::Approx(-1.0f));
		}
	}

	TEST_CASE("Debug lines accumulate per frame and are capped")
	{
		DebugDraw::Clear();
		DebugDraw::Line({ 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 1, 1 });
		DebugDraw::Box(AABB{ glm::vec3(-1.0f), glm::vec3(1.0f) }, { 1, 0, 0, 1 });
		DebugDraw::Sphere({ 0, 0, 0 }, 1.0f, { 0, 1, 0, 1 }, 8);
		DebugDraw::Arrow({ 0, 0, 0 }, { 0, 2, 0 }, { 0, 0, 1, 1 });
		CHECK(DebugDraw::GetLineCount() == 1 + 12 + 3 * 8 + 5);
		CHECK(DebugDraw::TakeLines().size() == 2 * (1 + 12 + 3 * 8 + 5));
		CHECK(DebugDraw::GetLineCount() == 0);
	}

	TEST_CASE("Embedded image keys with out-of-range indices are rejected without throwing")
	{
		AssetManager::Clear();
		CHECK_FALSE(AssetManager::GetTexture("Assets/x.glb#image/99999999999999999999"));
		CHECK(AssetManager::GetError("Assets/x.glb#image/99999999999999999999").find("invalid embedded image key") != std::string::npos);
		CHECK_FALSE(AssetManager::GetTexture("Assets/x.glb#image/-1"));
		AssetManager::Clear();
	}
}
