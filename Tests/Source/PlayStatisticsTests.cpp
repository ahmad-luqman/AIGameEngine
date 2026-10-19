#include "Basalt/Renderer/PlayStatistics.h"

#include "Basalt/Scene/Scene.h"

#include <doctest/doctest.h>

using namespace Basalt;

namespace {
	GpuStatistics Ready(float ms)
	{
		GpuStatistics result;
		result.Timers = true;
		result.Timings.SampleCount = 3;
		result.Timings.FrameMilliseconds = ms;
		result.Timings.Passes = { { "Lighting", ms } };
		return result;
	}
}

TEST_CASE("Play statistics preserve absent data separately from measured zero values")
{
	Scene scene;
	auto statistics = CollectPlayStatistics(scene, {});
	CHECK_FALSE(statistics.FrameMilliseconds);
	CHECK_FALSE(statistics.PhysicsBodies);
	auto json = PlayStatisticsToJson(statistics);
	for (const char* field : { "frameMs", "physicsBodies", "renderStats", "imguiGpu", "blitGpu" })
		CHECK(json[field].is_null());
	auto text = FormatStatisticsText(statistics);
	CHECK(text.find("Frame n/a") != std::string::npos);
	CHECK(text.find("Physics bodies n/a") != std::string::npos);
	CHECK(text.find("Game rendering: unavailable") != std::string::npos);
	CHECK(text.find("Game + host GPU: n/a") != std::string::npos);
	scene.OnRuntimeStart();
	statistics = CollectPlayStatistics(scene, { .FrameMilliseconds = 20 });
	REQUIRE(statistics.PhysicsBodies);
	CHECK(*statistics.PhysicsBodies == 0);
	json = PlayStatisticsToJson(statistics);
	CHECK(json["physicsBodies"] == 0);
	CHECK(json["frameMs"] == 20);
	text = FormatStatisticsText(statistics);
	CHECK(text.find("Frame 20.00 ms (50 FPS)") != std::string::npos);
	CHECK(text.find("Physics bodies 0") != std::string::npos);
	scene.OnRuntimeStop();
}

TEST_CASE("The play statistics formatter sums only current named scene and host timings")
{
	PlayStatistics statistics;
	statistics.Renderer.emplace();
	statistics.Renderer->DrawCalls = 2;
	statistics.Renderer->Triangles = 24;
	statistics.RenderDetails = { { "triangles", 24 } };
	statistics.SceneGpu = Ready(5);
	statistics.ImGuiGpu = Ready(2);
	auto text = FormatStatisticsText(statistics);
	CHECK(text.find("Triangles 24") != std::string::npos);
	CHECK(text.find("Lighting: 5.00 ms") != std::string::npos);
	CHECK(text.find("Game + host GPU: 7.00 ms") != std::string::npos);
	CHECK(text.find("Runtime blit") == std::string::npos);
	statistics.BlitGpu = Ready(1);
	text = FormatStatisticsText(statistics);
	CHECK(text.find("Game + host GPU: 8.00 ms") != std::string::npos);
	CHECK(text.find("Runtime blit GPU: 1.00 ms") != std::string::npos);
	const auto json = PlayStatisticsToJson(statistics);
	CHECK(json["renderStats"]["triangles"] == 24);
	CHECK(json["renderStats"]["gpu"]["timers"] == true);
	CHECK(json["imguiGpu"]["frameMs"] == 2);
	CHECK(json["blitGpu"]["frameMs"] == 1);
	statistics.BlitGpu->Timings.SampleCount = 0;
	text = FormatStatisticsText(statistics);
	CHECK(text.find("Game + host GPU: n/a") != std::string::npos);
	CHECK(text.find("Runtime blit GPU: n/a (waiting") != std::string::npos);
	statistics.BlitGpu = Ready(1);
	statistics.SceneGpu->ConsecutiveDrops = GpuProfiler::DropWarningThreshold;
	text = FormatStatisticsText(statistics);
	CHECK(text.find("Game GPU: stalled") != std::string::npos);
	CHECK(text.find("Game + host GPU: n/a (GPU timings stalled)") != std::string::npos);
	CHECK(text.find("Lighting: 5.00 ms") == std::string::npos);
	CHECK(text.find("Game + host GPU: n/a") != std::string::npos);
	CHECK(PlayStatisticsToJson(statistics)["renderStats"]["gpu"]["stalled"] == true);
	statistics.ImGuiGpu->Timers = false;
	CHECK(FormatStatisticsText(statistics).find("Game + host GPU: n/a (GPU timings unavailable)") != std::string::npos);
}

TEST_CASE("GPU statistics distinguish a missing profiler, disabled timer queries and pending results")
{
	CHECK_FALSE(CollectGpuStatistics(nullptr));
	GpuProfiler disabled(nullptr, { "ImGui" }, false);
	const auto statistics = CollectGpuStatistics(&disabled);
	REQUIRE(statistics);
	CHECK_FALSE(statistics->Timers);
	CHECK(FormatGpuStatistics(statistics).find("disabled or unsupported") != std::string::npos);
	CHECK(GpuStatisticsToJson(statistics)["timers"] == false);
	GpuStatistics pending;
	pending.Timers = true;
	CHECK(FormatGpuStatistics(pending).find("waiting") != std::string::npos);
	CHECK(GpuStatisticsToJson(pending)["timers"] == true);
}
