#include "Basalt/ImGui/StatisticsOverlay.h"

#include "Basalt/Core/Application.h"
#include "Basalt/Core/MemoryTracker.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Renderer/RenderUtils.h"
#include "Basalt/Renderer/SceneRenderer.h"
#include "Basalt/Scene/Scene.h"

#include <imgui.h>
#include <spdlog/fmt/fmt.h>

namespace Basalt {

	void DrawStatisticsOverlay(ImDrawList* drawList, const glm::vec2& origin, const glm::vec2& size, const Scene& scene, const SceneRenderer* renderer, const GpuProfiler* host, const GpuProfiler* blit)
	{
		const float frameMs = Application::Get().GetFrameMilliseconds();
		const float fps = frameMs > 0 ? 1000.0f / frameMs : 0.0f;
		const auto* physics = scene.GetPhysicsWorld();
		std::string text = fmt::format("Frame {:.2f} ms ({:.0f} FPS)\nPhysics bodies {}  |  Lua {:.2f} MiB (process)\n",
									   frameMs, fps, physics ? physics->GetBodyCount() : 0,
									   static_cast<double>(MemoryTracker::Get(MemoryCategory::Lua).Bytes) / 1048576.0);
		if (renderer)
		{
			const auto& stats = renderer->GetStatistics();
			text += fmt::format("Draw calls {} (+{} shadow)  |  Triangles {}\n", stats.DrawCalls, stats.ShadowDrawCalls, stats.Triangles);
			const auto& gpu = renderer->GetGpuTimings();
			text += fmt::format("Game GPU: {}\n", gpu.SampleCount ? fmt::format("{:.2f} ms ({} samples, {} dropped)", gpu.FrameMilliseconds, gpu.SampleCount, gpu.DroppedFrames) : FormatGpuTimings(gpu, renderer->HasGpuTimers()));
			if (gpu.SampleCount)
				for (const auto& pass : gpu.Passes)
					text += fmt::format("  {}: {:.2f} ms\n", pass.Name, pass.Milliseconds);
			if (gpu.SampleCount && host && host->GetTimings().SampleCount && (!blit || blit->GetTimings().SampleCount))
				text += fmt::format("Game + host GPU: {:.2f} ms\n", gpu.FrameMilliseconds + host->GetTimings().FrameMilliseconds + (blit ? blit->GetTimings().FrameMilliseconds : 0.0f));
		}
		else
			text += "Game rendering: unavailable\n";
		auto hostLine = [&](const char* name, const GpuProfiler* profiler) {
			if (!profiler)
				return;
			const auto& timings = profiler->GetTimings();
			text += fmt::format("{}: {}\n", name, timings.SampleCount ? fmt::format("{:.2f} ms ({} samples, {} dropped)", timings.FrameMilliseconds, timings.SampleCount, timings.DroppedFrames) : FormatGpuTimings(timings, profiler->IsEnabled()));
		};
		hostLine("ImGui GPU", host);
		hostLine("Runtime blit GPU", blit);
		// Draw-list primitives never take focus or pointer input. Clip to the letterboxed image, including small views.
		const ImVec2 min(origin.x, origin.y);
		const ImVec2 max(origin.x + size.x, origin.y + size.y);
		drawList->PushClipRect(min, max, true);
		const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
		drawList->AddRectFilled(ImVec2(min.x + 6, min.y + 6), ImVec2(min.x + textSize.x + 22, min.y + textSize.y + 22), IM_COL32(12, 14, 18, 220), 4);
		drawList->AddText(ImVec2(min.x + 14, min.y + 14), IM_COL32(235, 240, 245, 255), text.c_str());
		drawList->PopClipRect();
	}

}
