#pragma once

#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

namespace BasaltBenchmark {

	inline constexpr uint32_t Meshes = 2048;
	inline constexpr uint32_t Bodies = 256;
	inline constexpr uint32_t Lights = 64;
	inline constexpr uint32_t Animators = 128;
	inline constexpr uint32_t Entities = Meshes + Lights + Animators + 2;
	inline constexpr uint32_t WarmupFrames = 60;
	inline constexpr uint32_t SampleFrames = 180;
	inline constexpr float FixedStep = 1.0f / 60.0f;

	// Measured from live components, so accidentally dropping part of the workload cannot improve the baseline.
	struct WorkloadCounts
	{
		size_t Entities = 0;
		uint32_t Meshes = 0;
		uint32_t Animators = 0;
		uint32_t Lights = 0;
		uint32_t Bodies = 0;

		bool IsComplete() const
		{
			return Entities == BasaltBenchmark::Entities && Meshes == BasaltBenchmark::Meshes && Animators == BasaltBenchmark::Animators && Lights == BasaltBenchmark::Lights && Bodies == BasaltBenchmark::Bodies;
		}
	};

	inline WorkloadCounts CountWorkload(Basalt::Scene& scene)
	{
		WorkloadCounts counts;
		counts.Entities = scene.GetEntityCount();
		counts.Bodies = scene.GetPhysicsWorld() ? scene.GetPhysicsWorld()->GetBodyCount() : 0;
		for (auto entity : scene.GetAllEntitiesOrdered())
		{
			if (entity.HasComponent<Basalt::MeshComponent>())
				++counts.Meshes;
			if (entity.HasComponent<Basalt::AnimatorComponent>())
				++counts.Animators;
			if (entity.HasComponent<Basalt::PointLightComponent>())
				++counts.Lights;
		}
		return counts;
	}

	// Fixed workload for the pre-instancing baseline: individual cube draws, overlapping lights and awake bodies.
	inline void BuildScene(Basalt::Scene& scene)
	{
		using namespace Basalt;
		uint64_t id = 1;
		auto create = [&](const char* name) { return scene.CreateEntityWithUUID(UUID(id++), name); };
		for (uint32_t i = 0; i < Meshes; ++i)
		{
			auto entity = create("Benchmark cube");
			const uint32_t row = i / 64;
			entity.GetTransform().Translation = { static_cast<float>(i % 64) - 31.5f, 0.5f, static_cast<float>(row) - 15.5f };
			entity.GetTransform().Scale = glm::vec3(0.6f);
			entity.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
			entity.AddComponent<MaterialComponent>();
			if (i < Bodies)
			{
				auto& body = entity.AddComponent<RigidBodyComponent>();
				body.Type = RigidBodyType::Dynamic;
				body.AllowSleeping = false;
				body.GravityFactor = 0.0f;
				entity.AddComponent<BoxColliderComponent>();
			}
		}
		for (uint32_t i = 0; i < Animators; ++i)
		{
			auto entity = create("Benchmark mannequin");
			const uint32_t row = i / 32;
			entity.GetTransform().Translation = { static_cast<float>(i % 32) * 2.0f - 31.0f, 0, static_cast<float>(row) * 2.0f - 3.0f };
			auto& animator = entity.AddComponent<AnimatorComponent>();
			animator.Mesh = BASALT_SOURCE_DIR "/Samples/MovementDemo/Assets/Models/Mannequin.gltf";
			animator.Clip = "Walk";
		}
		for (uint32_t i = 0; i < Lights; ++i)
		{
			auto entity = create("Benchmark light");
			const uint32_t row = i / 8;
			entity.GetTransform().Translation = { static_cast<float>(i % 8) * 8.0f - 28.0f, 3.0f, static_cast<float>(row) * 4.0f - 14.0f };
			auto& light = entity.AddComponent<PointLightComponent>();
			light.Radius = 8.0f;
			light.Intensity = 5.0f;
		}
		auto sun = create("Benchmark sun");
		sun.GetTransform().SetRotationEuler(glm::radians(glm::vec3(-55, 20, 0)));
		sun.AddComponent<DirectionalLightComponent>();
		auto camera = create("Benchmark camera");
		camera.AddComponent<CameraComponent>();
		camera.GetTransform().Translation = { 0, 38, 40 };
		camera.GetTransform().SetRotationEuler(glm::radians(glm::vec3(-43, 0, 0)));
	}

}
