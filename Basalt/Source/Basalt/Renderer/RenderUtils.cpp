#include "Basalt/Renderer/RenderUtils.h"

#include "Basalt/Core/Log.h" // fmt::format (bundled with spdlog)
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <nlohmann/json.hpp>

namespace Basalt {

	std::optional<GpuStatistics> CollectGpuStatistics(const GpuProfiler* profiler)
	{
		if (!profiler)
			return std::nullopt;
		return GpuStatistics{ .Timings = profiler->GetTimings(),
							  .DisabledReason = profiler->IsEnabled() ? std::nullopt : std::optional<std::string>(profiler->GetDisabledReason()),
							  .ConsecutiveDrops = profiler->GetConsecutiveDrops() };
	}

	nlohmann::json GpuStatisticsToJson(const std::optional<GpuStatistics>& statistics)
	{
		if (!statistics)
			return nullptr;
		auto result = GpuTimingsToJson(statistics->Timings);
		result["timers"] = statistics->HasTimers();
		result["consecutiveDrops"] = statistics->ConsecutiveDrops;
		result["stalled"] = statistics->IsStalled();
		result["disabledReason"] = statistics->DisabledReason ? nlohmann::json(*statistics->DisabledReason) : nlohmann::json();
		if (!statistics->IsReady())
		{
			result["frameMs"] = nullptr;
			for (auto& pass : result["passes"])
				pass["ms"] = nullptr;
		}
		return result;
	}

	std::string FormatGpuStatistics(const std::optional<GpuStatistics>& statistics, bool includePasses)
	{
		if (!statistics)
			return "unavailable";
		if (statistics->DisabledReason)
			return "unavailable (" + *statistics->DisabledReason + ")";
		if (statistics->IsStalled())
			return fmt::format("stalled ({} consecutive frames without results)", statistics->ConsecutiveDrops);
		if (!statistics->Timings.SampleCount)
			return "not available yet (waiting for GPU timings)";
		if (includePasses)
			return FormatGpuTimings(statistics->Timings, true);
		return fmt::format("{:.2f} ms ({} samples, {} dropped in session)", statistics->Timings.FrameMilliseconds, statistics->Timings.SampleCount, statistics->Timings.DroppedFrames);
	}

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
		camera.Identity = static_cast<uint64_t>(cameraEntity.GetUUID());
		const bool perspective = component.Camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective;
		camera.Near = perspective ? component.Camera.GetPerspectiveNearClip() : component.Camera.GetOrthographicNearClip();
		camera.Far = perspective ? component.Camera.GetPerspectiveFarClip() : component.Camera.GetOrthographicFarClip();
		return camera;
	}

	bool SaveScreenshot(SceneRenderer& renderer, const std::filesystem::path& path, std::string& outError)
	{
		if (renderer.GetStatistics().Failed)
		{
			outError = "cannot save screenshot: the last render failed";
			return false;
		}

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
			text += fmt::format("; {} frames dropped in session", timings.DroppedFrames);
		return text;
	}

	nlohmann::json GetRenderStats(const SceneRenderer& renderer)
	{
		return GetRenderStats(renderer, CollectGpuStatistics(&renderer.GetGpuProfiler()));
	}

	nlohmann::json GetRenderStats(const SceneRenderer& renderer, const std::optional<GpuStatistics>& gpuStatistics)
	{
		const SceneRendererStatistics& statistics = renderer.GetStatistics();
		const DynamicResolutionSettings& dynamic = renderer.GetDynamicResolution();
		const auto gpu = GpuStatisticsToJson(gpuStatistics);
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
			{ "instances", statistics.Instances },
			{ "triangles", statistics.Triangles },
			{ "shadowDrawCalls", statistics.ShadowDrawCalls },
			{ "lightsSkipped", statistics.LightsSkipped },
			{ "localShadowMaps", statistics.LocalShadowMaps },
			{ "localShadowsSkipped", statistics.LocalShadowsSkipped },
			{ "localShadowAtlasSize", statistics.LocalShadowAtlasSize },
			{ "failed", statistics.Failed },
			{ "fsr1Active", statistics.FSR1Active },
			{ "taaActive", statistics.TAAActive },
			{ "taaHistoryValid", statistics.TAAHistoryValid },
			{ "culledSubmeshes", statistics.CulledSubmeshes },
			{ "skinMatrices", statistics.SkinMatrices },
			{ "morphWeights", statistics.MorphWeights },
			{ "morphWeightsDropped", statistics.MorphWeightsDropped },
			{ "particles", statistics.Particles },
			{ "particlesSkipped", statistics.ParticlesSkipped },
			{ "lights", statistics.Lights },
			{ "materials", statistics.Materials },
			{ "gpu", gpu },
		};
	}

}
