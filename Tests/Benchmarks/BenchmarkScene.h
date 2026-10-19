#pragma once

#include "Basalt/Animation/AnimationSystem.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Renderer/RenderUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <cmath>

namespace BasaltBenchmark {

	// Run.py pins the same scene counts/timestep and adds report protocol/build requirements.
	inline constexpr uint32_t Meshes = 2048;
	inline constexpr uint32_t Bodies = 256;
	inline constexpr uint32_t Lights = 64;
	inline constexpr uint32_t Animators = 128;
	inline constexpr uint32_t SunAndCamera = 2;
	inline constexpr uint32_t Entities = Meshes + Lights + Animators + SunAndCamera;
	inline constexpr uint32_t Width = 320, Height = 180;
	inline constexpr uint32_t CubeTriangles = 12;
	// Samples/MovementDemo/Assets/Models/Mannequin.gltf (GenerateMannequin.py): 216 triangles, 17 joints.
	inline constexpr uint32_t MannequinTriangles = 216;
	inline constexpr uint32_t MannequinJoints = 17;
	inline constexpr uint64_t FirstLightId = Meshes + Animators + 1;
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

	inline uint32_t CountPlayingAnimators(Basalt::Scene& scene)
	{
		uint32_t playing = 0;
		if (auto* animation = scene.GetAnimationSystem())
			for (auto entity : scene.GetAllEntitiesOrdered())
				if (entity.HasComponent<Basalt::AnimatorComponent>() && animation->IsPlaying(entity))
					++playing;
		return playing;
	}

	// WaitIdle makes every result due by the final frame; even a short unresolved drop invalidates the gate.
	inline bool HasValidGpuTiming(const Basalt::GpuStatistics& statistics)
	{
		return statistics.IsReady() && statistics.ConsecutiveDrops == 0 && std::isfinite(statistics.Timings.FrameMilliseconds) && statistics.Timings.FrameMilliseconds > 0;
	}

	// This fixed workload fits entirely in the camera. Reduced rendering must fail, not look faster.
	inline std::string ValidateRenderedWorkload(const Basalt::SceneRendererStatistics& stats)
	{
		if (stats.Instances != Meshes + Animators)
			return fmt::format("mesh instances: expected {}, got {}", Meshes + Animators, stats.Instances);
		if (stats.DrawCalls != 2)
			return fmt::format("batched draw calls: expected 2, got {}", stats.DrawCalls);
		constexpr uint64_t triangles = static_cast<uint64_t>(Meshes) * CubeTriangles + static_cast<uint64_t>(Animators) * MannequinTriangles;
		if (stats.Triangles != triangles)
			return fmt::format("color triangles: expected {}, got {}", triangles, stats.Triangles);
		if (stats.SkinMatrices != Animators * MannequinJoints)
			return fmt::format("skin matrices: expected {}, got {}", Animators * MannequinJoints, stats.SkinMatrices);
		// The directional sun joins the point lights in renderer statistics.
		if (stats.Lights != Lights + 1)
			return fmt::format("rendered lights: expected {}, got {}", Lights + 1, stats.Lights);
		return {};
	}

	// The same workload as the pre-instancing baseline: cube instances, overlapping lights and awake bodies.
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
