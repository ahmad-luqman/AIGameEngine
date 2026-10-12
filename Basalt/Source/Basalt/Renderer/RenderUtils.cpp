#include "Basalt/Renderer/RenderUtils.h"

#include "Basalt/Core/Log.h" // fmt::format (bundled with spdlog)
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <nlohmann/json.hpp>

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

	nlohmann::json GpuTimingsToJson(const GpuTimings& timings)
	{
		nlohmann::json passes = nlohmann::json::array();
		for (const GpuPassTiming& pass : timings.Passes)
			passes.push_back({ { "name", pass.Name }, { "ms", pass.Milliseconds } });
		return { { "frames", timings.SampleCount }, { "droppedFrames", timings.DroppedFrames }, { "frameMs", timings.FrameMilliseconds }, { "passes", std::move(passes) } };
	}

	std::string FormatGpuTimings(const GpuTimings& timings, bool timersAvailable)
	{
		if (!timersAvailable)
			return "unavailable (no GPU timer queries)";
		if (timings.SampleCount == 0)
			return fmt::format("not available yet: no GPU timer results arrived ({} frames dropped)", timings.DroppedFrames);
		std::string text = fmt::format("{:.2f} ms over {} frames (", timings.FrameMilliseconds, timings.SampleCount);
		for (size_t i = 0; i < timings.Passes.size(); i++)
			text += fmt::format("{}{} {:.2f}", i == 0 ? "" : ", ", timings.Passes[i].Name, timings.Passes[i].Milliseconds);
		text += ')';
		if (timings.DroppedFrames > 0)
			text += fmt::format("; {} frames dropped", timings.DroppedFrames);
		return text;
	}

	nlohmann::json GetRenderStats(const SceneRenderer& renderer)
	{
		const SceneRendererStatistics& statistics = renderer.GetStatistics();
		const DynamicResolutionSettings& dynamic = renderer.GetDynamicResolution();
		nlohmann::json gpu = GpuTimingsToJson(renderer.GetGpuTimings());
		gpu["timers"] = renderer.HasGpuTimers();
		return {
			{ "viewport", { renderer.GetViewportWidth(), renderer.GetViewportHeight() } },
			{ "renderSize", { renderer.GetRenderWidth(), renderer.GetRenderHeight() } },
			{ "renderScale", renderer.GetRenderScale() },
			{ "currentRenderScale", renderer.GetCurrentRenderScale() },
			{ "dynamicResolution", dynamic.Enabled },
			{ "dynamicResolutionActive", renderer.IsDynamicResolutionActive() },
			{ "dynamicResolutionState", DescribeDynamicResolutionState(renderer.GetDynamicResolutionState(), dynamic, renderer.GetRenderScale()) },
			{ "dynamicResolutionSamples", { { "accepted", renderer.GetDynamicResolutionSamples().Accepted }, { "rejected", renderer.GetDynamicResolutionSamples().Rejected } } },
			{ "targetFrameRate", dynamic.TargetFrameRate },
			{ "minRenderScale", dynamic.MinScale },
			{ "drawCalls", statistics.DrawCalls },
			{ "shadowDrawCalls", statistics.ShadowDrawCalls },
			{ "culledSubmeshes", statistics.CulledSubmeshes },
			{ "lights", statistics.Lights },
			{ "materials", statistics.Materials },
			{ "gpu", std::move(gpu) },
		};
	}

}
