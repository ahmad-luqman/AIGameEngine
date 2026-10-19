#include "../Benchmarks/BenchmarkScene.h"

#include "Basalt/Animation/AnimationSystem.h"
#include "Basalt/Physics/PhysicsWorld.h"

#include <doctest/doctest.h>

using namespace Basalt;

TEST_CASE("The diagnostics benchmark keeps its fixed workload throughout warmup and measurement")
{
	Scene scene;
	BasaltBenchmark::BuildScene(scene);
	CHECK(scene.GetEntityCount() == BasaltBenchmark::Entities);
	scene.OnRuntimeStart();
	REQUIRE(scene.GetPhysicsWorld());
	CHECK(scene.GetPhysicsWorld()->GetBodyCount() == BasaltBenchmark::Bodies);
	CHECK(BasaltBenchmark::CountWorkload(scene).IsComplete());
	for (uint32_t frame = 0; frame < BasaltBenchmark::WarmupFrames + BasaltBenchmark::SampleFrames; ++frame)
		scene.OnUpdate(BasaltBenchmark::FixedStep);
	CHECK(scene.GetEntityCount() == BasaltBenchmark::Entities);
	CHECK(scene.GetPhysicsWorld()->GetBodyCount() == BasaltBenchmark::Bodies);
	CHECK(BasaltBenchmark::CountWorkload(scene).IsComplete());
	uint32_t playing = 0;
	for (auto entity : scene.GetAllEntitiesOrdered())
		if (entity.HasComponent<AnimatorComponent>() && scene.GetAnimationSystem()->IsPlaying(entity))
			++playing;
	CHECK(playing == BasaltBenchmark::Animators);
	// Retaining entities while dropping a component must fail the same guard the timing executable uses.
	auto light = scene.GetEntityByUUID(UUID(BasaltBenchmark::Meshes + BasaltBenchmark::Animators + 1));
	REQUIRE(light.HasComponent<PointLightComponent>());
	light.RemoveComponent<PointLightComponent>();
	CHECK_FALSE(BasaltBenchmark::CountWorkload(scene).IsComplete());
	CHECK(BasaltBenchmark::CountWorkload(scene).Lights == BasaltBenchmark::Lights - 1);
	scene.OnRuntimeStop();
}
