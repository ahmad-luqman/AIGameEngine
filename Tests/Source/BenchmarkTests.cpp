#include "../Benchmarks/BenchmarkScene.h"

#include "Breccia/Animation/AnimationSystem.h"
#include "Breccia/Physics/PhysicsWorld.h"

#include <doctest/doctest.h>

#include <limits>

using namespace Breccia;

TEST_CASE("The diagnostics benchmark keeps its fixed workload throughout warmup and measurement")
{
	Scene scene;
	BrecciaBenchmark::BuildScene(scene);
	CHECK(scene.GetEntityCount() == BrecciaBenchmark::Entities);
	scene.OnRuntimeStart();
	REQUIRE(scene.GetPhysicsWorld());
	CHECK(scene.GetPhysicsWorld()->GetBodyCount() == BrecciaBenchmark::Bodies);
	CHECK(BrecciaBenchmark::CountWorkload(scene).IsComplete());
	for (uint32_t frame = 0; frame < BrecciaBenchmark::WarmupFrames + BrecciaBenchmark::SampleFrames; ++frame)
		scene.OnUpdate(BrecciaBenchmark::FixedStep);
	CHECK(scene.GetEntityCount() == BrecciaBenchmark::Entities);
	CHECK(scene.GetPhysicsWorld()->GetBodyCount() == BrecciaBenchmark::Bodies);
	CHECK(BrecciaBenchmark::CountWorkload(scene).IsComplete());
	CHECK(BrecciaBenchmark::CountPlayingAnimators(scene) == BrecciaBenchmark::Animators);

	// Retaining entities while dropping a component must fail the same guard the timing executable uses.
	auto light = scene.GetEntityByUUID(UUID(BrecciaBenchmark::FirstLightId));
	REQUIRE(light.HasComponent<PointLightComponent>());
	light.RemoveComponent<PointLightComponent>();
	CHECK_FALSE(BrecciaBenchmark::CountWorkload(scene).IsComplete());
	CHECK(BrecciaBenchmark::CountWorkload(scene).Lights == BrecciaBenchmark::Lights - 1);
	scene.OnRuntimeStop();
}

TEST_CASE("The benchmark rejects a reduced rendered workload even when mannequins mask missing cubes")
{
	using namespace BrecciaBenchmark;
	SceneRendererStatistics full;
	full.DrawCalls = 2;
	full.Instances = Meshes + Animators;
	full.Triangles = static_cast<uint64_t>(Meshes) * CubeTriangles + static_cast<uint64_t>(Animators) * MannequinTriangles;
	full.SkinMatrices = Animators * MannequinJoints;
	full.Lights = Lights + 1;
	CHECK(ValidateRenderedWorkload(full).empty());
	auto missingCubes = full;
	missingCubes.Instances = Animators;
	missingCubes.Triangles = static_cast<uint64_t>(Animators) * MannequinTriangles;
	CHECK_FALSE(ValidateRenderedWorkload(missingCubes).empty());
	for (auto member : { &SceneRendererStatistics::Instances, &SceneRendererStatistics::DrawCalls, &SceneRendererStatistics::SkinMatrices, &SceneRendererStatistics::Lights })
	{
		auto reduced = full;
		--(reduced.*member);
		CHECK_FALSE(ValidateRenderedWorkload(reduced).empty());
		reduced = full;
		++(reduced.*member);
		CHECK_FALSE(ValidateRenderedWorkload(reduced).empty());
	}
	auto reduced = full;
	--reduced.Triangles;
	CHECK_FALSE(ValidateRenderedWorkload(reduced).empty());
	reduced = full;
	++reduced.Triangles;
	CHECK_FALSE(ValidateRenderedWorkload(reduced).empty());
}

TEST_CASE("The benchmark rejects stale, pending, disabled and invalid GPU measurements")
{
	GpuStatistics statistics{ .DisabledReason = std::nullopt };
	statistics.Timings.SampleCount = 30;
	statistics.Timings.FrameMilliseconds = 2.0f;
	CHECK(BrecciaBenchmark::HasValidGpuTiming(statistics));
	statistics.ConsecutiveDrops = 1;
	CHECK_FALSE(BrecciaBenchmark::HasValidGpuTiming(statistics));
	statistics.ConsecutiveDrops = GpuProfiler::DropWarningThreshold;
	CHECK_FALSE(BrecciaBenchmark::HasValidGpuTiming(statistics));
	statistics.ConsecutiveDrops = 0;
	for (float invalid : { 0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
	{
		statistics.Timings.FrameMilliseconds = invalid;
		CHECK_FALSE(BrecciaBenchmark::HasValidGpuTiming(statistics));
	}
	statistics.Timings.FrameMilliseconds = 2.0f;
	statistics.Timings.SampleCount = 0;
	CHECK_FALSE(BrecciaBenchmark::HasValidGpuTiming(statistics));
	statistics.Timings.SampleCount = 30;
	statistics.DisabledReason = "query creation failed";
	CHECK_FALSE(BrecciaBenchmark::HasValidGpuTiming(statistics));
}
