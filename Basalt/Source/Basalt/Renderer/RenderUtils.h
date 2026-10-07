#pragma once

#include "Basalt/Renderer/SceneRenderer.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	class Scene;

	// Camera for the scene's primary CameraComponent (aspect ratio from the viewport), if any.
	std::optional<RenderCamera> GetPrimaryRenderCamera(Scene& scene, uint32_t viewportWidth, uint32_t viewportHeight);

	// Encodes RGBA8 pixels (top row first) as a PNG file.
	bool WritePng(const std::filesystem::path& path, const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, std::string& outError);

	// Writes the renderer's last output image as a PNG.
	bool SaveScreenshot(SceneRenderer& renderer, const std::filesystem::path& path, std::string& outError);

}
