#include "Basalt/Renderer/RenderUtils.h"

#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

namespace Basalt {

	std::optional<RenderCamera> GetPrimaryRenderCamera(Scene& scene, uint32_t viewportWidth, uint32_t viewportHeight)
	{
		Entity cameraEntity = scene.GetPrimaryCameraEntity();
		if (!cameraEntity)
			return std::nullopt;

		auto& component = cameraEntity.GetComponent<CameraComponent>();
		if (!component.FixedAspectRatio)
			component.Camera.SetViewportSize(viewportWidth, viewportHeight);

		glm::vec3 position;
		glm::quat rotation;
		glm::vec3 scale;
		Math::DecomposeTransform(scene.GetWorldTransform(cameraEntity), position, rotation, scale);

		RenderCamera camera;
		// The view ignores scale so scaled camera entities still produce an orthonormal view.
		camera.View = glm::inverse(Math::ComposeTransform(position, rotation, glm::vec3(1.0f)));
		camera.Projection = component.Camera.GetProjection();
		camera.Position = position;
		const bool perspective = component.Camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective;
		camera.Near = perspective ? component.Camera.GetPerspectiveNearClip() : component.Camera.GetOrthographicNearClip();
		camera.Far = perspective ? component.Camera.GetPerspectiveFarClip() : component.Camera.GetOrthographicFarClip();
		return camera;
	}

	bool SaveScreenshot(SceneRenderer& renderer, const std::filesystem::path& path, std::string& outError)
	{
		std::vector<uint8_t> pixels;
		uint32_t width = 0;
		uint32_t height = 0;
		if (!renderer.ReadOutputPixels(pixels, width, height))
		{
			outError = "could not read back the rendered image";
			return false;
		}

		return WritePng(path, pixels, width, height, outError);
	}

}
