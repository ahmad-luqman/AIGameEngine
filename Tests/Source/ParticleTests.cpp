#include <doctest/doctest.h>

#include <Basalt/Core/Log.h>
#include <Basalt/Particles/ParticleEmitterRules.h>
#include <Basalt/Particles/ParticleSystem.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/Scene/SceneSerializer.h>
#include <Basalt/Scripting/ScriptEngine.h>

#include "TestUtils.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace Basalt;
using json = nlohmann::json;

namespace {

	constexpr float Step = 1.0f / 60.0f;

	// An emitter that only emits what a test asks for, with particles that outlive the test.
	ParticleEmitterComponent QuietEmitter()
	{
		ParticleEmitterComponent emitter;
		emitter.PlayOnStart = false;
		emitter.Rate = 0.0f;
		emitter.LifetimeMin = 100.0f;
		emitter.LifetimeMax = 100.0f;
		emitter.SpeedMin = 0.0f;
		emitter.SpeedMax = 0.0f;
		return emitter;
	}

	// Increasing UUIDs: scene emitters register in UUID order, so batches follow creation order.
	Entity AddEmitter(Scene& scene, const std::string& name, const ParticleEmitterComponent& emitter, const glm::vec3& position = glm::vec3(0.0f))
	{
		static uint64_t s_NextID = 1000;
		Entity entity = scene.CreateEntityWithUUID(UUID(s_NextID++), name);
		entity.GetTransform().Translation = position;
		entity.AddComponent<ParticleEmitterComponent>(emitter);
		return entity;
	}

	void StepFrames(Scene& scene, int frames)
	{
		for (int i = 0; i < frames; i++)
			scene.OnUpdate(Step);
	}

	ParticleRenderData RenderData(const Scene& scene, const glm::vec3& cameraPosition = { 0.0f, 0.0f, 10.0f }, const glm::vec3& cameraForward = { 0.0f, 0.0f, -1.0f })
	{
		ParticleRenderData data;
		scene.GetParticleSystem()->BuildRenderData(cameraPosition, cameraForward, data);
		return data;
	}

	// Every particle's position, size and color, in render order.
	std::vector<float> Snapshot(const Scene& scene)
	{
		std::vector<float> values;
		for (const GpuParticle& particle : RenderData(scene).Particles)
		{
			for (int i = 0; i < 4; i++)
				values.push_back(particle.PositionSize[i]);
			for (int i = 0; i < 4; i++)
				values.push_back(particle.Color[i]);
		}
		return values;
	}

	// Position of particle `index` of batch `batch`, in render order.
	glm::vec3 ParticlePosition(const ParticleRenderData& data, size_t batch, uint32_t index = 0)
	{
		return glm::vec3(data.Particles[data.Batches[batch].FirstParticle + index].PositionSize);
	}

	bool Deserialize(Entity entity, const json& patch, std::string& error)
	{
		return ComponentRegistry::Find("ParticleEmitter")->Deserialize(entity, patch, error);
	}

}

TEST_SUITE("Particles")
{
	TEST_CASE("The registry rejects emitter values the simulation cannot use")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Emitter");
		entity.AddComponent<ParticleEmitterComponent>();
		std::string error;
		REQUIRE_MESSAGE(Deserialize(entity, json::object(), error), "the defaults are valid: " << error);

		const std::vector<std::pair<json, const char*>> bad = {
			{ { { "Duration", 0 } }, "Duration" },
			{ { { "MaxParticles", 0 } }, "MaxParticles" },
			{ { { "MaxParticles", ParticleEmitterComponent::MaxParticlesLimit + 1 } }, "MaxParticles" },
			{ { { "BurstCount", ParticleEmitterComponent::MaxParticlesLimit + 1 } }, "BurstCount" },
			{ { { "Rate", -1 } }, "Rate" },
			{ { { "ShapeRadius", -0.5 } }, "ShapeRadius" },
			{ { { "Drag", -1 } }, "Drag" },
			{ { { "StartSize", -1 } }, "StartSize" },
			{ { { "EndSize", -1 } }, "EndSize" },
			{ { { "BoxExtents", { 1, -1, 1 } } }, "BoxExtents" },
			{ { { "ConeAngle", 181 } }, "ConeAngle must be between 0 and 180 (got 181)" },
			{ { { "ConeAngle", -1 } }, "ConeAngle" },
			{ { { "Duration", 0.0001 } }, "Duration" },
			{ { { "Duration", 2e5 } }, "Duration" },
			{ { { "LifetimeMax", 2e5 } }, "LifetimeMax" },
			{ { { "LifetimeMin", 0 } }, "LifetimeMin" },
			{ { { "LifetimeMax", 0 } }, "LifetimeMax" },
			// Values beyond MinTime/MaxTime, MaxRate and MaxMagnitude, and colors outside their ranges.
			{ { { "Rate", 2e6 } }, "Rate" },
			{ { { "SpeedMax", 2e4 } }, "SpeedMax" },
			{ { { "SpeedMin", -2e4 } }, "SpeedMin" },
			{ { { "Velocity", { 0, 3e38, 0 } } }, "Velocity" },
			{ { { "GravityScale", 1e5 } }, "GravityScale" },
			{ { { "Drag", 1e5 } }, "Drag" },
			{ { { "ShapeRadius", 1e5 } }, "ShapeRadius" },
			{ { { "BoxExtents", { 1, 1, 1e5 } } }, "BoxExtents.z must be between 0 and 10000 (got 100000)" },
			{ { { "StartSize", 1e5 } }, "StartSize" },
			{ { { "StartColor", { 1, 1, 1, 1.5 } } }, "StartColor" },
			{ { { "EndColor", { -1, 0, 0, 0 } } }, "EndColor" },
			{ { { "Shape", "Torus" } }, "Shape" },
			{ { { "Blend", "Multiply" } }, "Blend" },
			{ { { "SimulationSpace", "Screen" } }, "SimulationSpace" },
		};
		for (const auto& [patch, field] : bad)
		{
			INFO(patch.dump());
			error.clear();
			CHECK_FALSE(Deserialize(entity, patch, error));
			// The message names the field and the value it got.
			CHECK(error.find(field) != std::string::npos);
			CHECK((error.find("got") != std::string::npos || error.find("invalid value") != std::string::npos));
			// A rejected patch leaves the component untouched.
			CHECK(entity.GetComponent<ParticleEmitterComponent>() == ParticleEmitterComponent{});
		}

		// Min and max fields are independent (either order picks between them), so the inspector can edit one
		// past the other.
		REQUIRE(Deserialize(entity, { { "Shape", "Cone" }, { "ConeAngle", 180 }, { "Blend", "Additive" }, { "SimulationSpace", "Local" }, { "LifetimeMin", 3 }, { "LifetimeMax", 2 }, { "SpeedMin", -1 }, { "SpeedMax", -3 } }, error));
		const auto& emitter = entity.GetComponent<ParticleEmitterComponent>();
		CHECK(emitter.Shape == ParticleEmitterShape::Cone);
		CHECK(emitter.ConeAngle == 180.0f);
		CHECK(emitter.Blend == ParticleBlendMode::Additive);
		CHECK(emitter.SimulationSpace == ParticleSimulationSpace::Local);
		// Negative speeds are allowed: they reverse the emission direction (towards the sphere's center, down
		// through the cone's base disc).
		CHECK(emitter.SpeedMin == -1.0f);
		CHECK(emitter.LifetimeMin == 3.0f);
	}

	TEST_CASE("A burst fires when the emitter starts and the rate emits continuously")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.PlayOnStart = true;
		emitter.BurstCount = 10;
		emitter.Rate = 30.0f; // half a particle per frame
		Entity entity = AddEmitter(scene, "Emitter", emitter);
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		CHECK(particles.IsPlaying(entity));
		CHECK(particles.GetParticleCount(entity) == 0);

		scene.OnUpdate(Step);
		CHECK(particles.GetParticleCount(entity) == 10);
		scene.OnUpdate(Step);
		CHECK(particles.GetParticleCount(entity) == 11);
		StepFrames(scene, 58);
		// One second: the burst and 30 more (the last may still be owed to float rounding).
		CHECK(particles.GetParticleCount(entity) >= 39);
		CHECK(particles.GetParticleCount(entity) <= 40);
		CHECK(particles.GetTotalParticleCount() == particles.GetParticleCount(entity));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Particles expire after their lifetime")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.LifetimeMin = 0.5f;
		emitter.LifetimeMax = 0.5f;
		Entity entity = AddEmitter(scene, "Emitter", emitter);
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		CHECK(particles.Emit(entity, 5) == 5);
		StepFrames(scene, 29);
		CHECK(particles.GetParticleCount(entity) == 5);
		StepFrames(scene, 2);
		CHECK(particles.GetParticleCount(entity) == 0);
		scene.OnRuntimeStop();
	}

	TEST_CASE("MaxParticles caps every way of emitting, also when lowered during play")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.PlayOnStart = true;
		emitter.BurstCount = 20;
		emitter.Rate = 600.0f;
		emitter.MaxParticles = 8;
		Entity entity = AddEmitter(scene, "Emitter", emitter);
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		scene.OnUpdate(Step);
		CHECK(particles.GetParticleCount(entity) == 8);
		CHECK(particles.Emit(entity, 5) == 0);
		StepFrames(scene, 10);
		CHECK(particles.GetParticleCount(entity) == 8);

		entity.GetComponent<ParticleEmitterComponent>().MaxParticles = 3;
		scene.OnUpdate(Step);
		CHECK(particles.GetParticleCount(entity) == 3);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Stop keeps or clears live particles; Play starts a new cycle with its burst")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.BurstCount = 4;
		Entity entity = AddEmitter(scene, "Emitter", emitter);
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		CHECK_FALSE(particles.IsPlaying(entity));
		StepFrames(scene, 5);
		CHECK(particles.GetParticleCount(entity) == 0);

		CHECK(particles.Play(entity));
		scene.OnUpdate(Step);
		CHECK(particles.GetParticleCount(entity) == 4);
		// Already playing: no second burst.
		CHECK(particles.Play(entity));
		scene.OnUpdate(Step);
		CHECK(particles.GetParticleCount(entity) == 4);

		particles.Stop(entity);
		CHECK_FALSE(particles.IsPlaying(entity));
		scene.OnUpdate(Step);
		CHECK(particles.GetParticleCount(entity) == 4);
		particles.Stop(entity, true);
		CHECK(particles.GetParticleCount(entity) == 0);

		CHECK(particles.Play(entity));
		scene.OnUpdate(Step);
		CHECK(particles.GetParticleCount(entity) == 4);

		// Entities without an emitter: nothing happens, and the calls say so.
		Entity plain = scene.CreateEntity("Plain");
		CHECK(particles.HasEmitter(entity));
		CHECK_FALSE(particles.HasEmitter(plain));
		CHECK_FALSE(particles.Play(plain));
		CHECK(particles.Emit(plain, 3) == 0);
		CHECK_FALSE(particles.Stop(plain, true));
		CHECK(particles.Stop(entity));
		CHECK_FALSE(particles.IsPlaying(plain));
		CHECK(particles.GetParticleCount(plain) == 0);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A looping emitter bursts every cycle; a one-shot emitter stops after its duration")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.PlayOnStart = true;
		emitter.BurstCount = 3;
		emitter.Duration = 0.5f;
		Entity looping = AddEmitter(scene, "Looping", emitter);
		emitter.Looping = false;
		Entity once = AddEmitter(scene, "Once", emitter);
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();

		StepFrames(scene, 29);
		CHECK(particles.GetParticleCount(looping) == 3);
		CHECK(particles.IsPlaying(once));
		StepFrames(scene, 3);
		CHECK(particles.GetParticleCount(looping) == 6);
		CHECK(particles.GetParticleCount(once) == 3);
		CHECK_FALSE(particles.IsPlaying(once));
		StepFrames(scene, 30);
		CHECK(particles.GetParticleCount(looping) == 9);
		CHECK(particles.GetParticleCount(once) == 3);
		CHECK(particles.IsPlaying(looping));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Gravity and drag move particles")
	{
		Scene scene;
		scene.GetPhysicsSettings().Gravity = { 0.0f, -10.0f, 0.0f };
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.GravityScale = 0.5f;
		Entity falling = AddEmitter(scene, "Falling", emitter);
		emitter.GravityScale = 0.0f;
		emitter.Drag = 1.0f;
		emitter.Velocity = { 1.0f, 0.0f, 0.0f };
		Entity dragged = AddEmitter(scene, "Dragged", emitter);
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		particles.Emit(falling, 1);
		particles.Emit(dragged, 1);
		// The emitting frame draws them where they were born; 60 more updates move them.
		StepFrames(scene, 61);

		// The same integration in double precision: velocity first, then position (semi-implicit Euler).
		double fallVelocity = 0.0, fall = 0.0, dragVelocity = 1.0, drift = 0.0;
		for (int i = 0; i < 60; i++)
		{
			fallVelocity += -5.0 * Step;
			fall += fallVelocity * Step;
			dragVelocity /= 1.0 + Step;
			drift += dragVelocity * Step;
		}
		const ParticleRenderData data = RenderData(scene);
		REQUIRE(data.Batches.size() == 2);
		const glm::vec4 fallen = data.Particles[data.Batches[0].FirstParticle].PositionSize;
		const glm::vec4 drifted = data.Particles[data.Batches[1].FirstParticle].PositionSize;
		CHECK(fallen.y == doctest::Approx(fall).epsilon(1e-4));
		CHECK(fallen.x == doctest::Approx(0.0));
		CHECK(drifted.x == doctest::Approx(drift).epsilon(1e-4));
		// Drag slowed it: without, it would have travelled 1 m.
		CHECK(drifted.x < 0.7f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("World-space particles stay behind a moving emitter; local-space particles move with it")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		Entity world = AddEmitter(scene, "World", emitter, { 0.0f, 0.0f, 0.0f });
		emitter.SimulationSpace = ParticleSimulationSpace::Local;
		Entity local = AddEmitter(scene, "Local", emitter, { 0.0f, 1.0f, 0.0f });
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		particles.Emit(world, 1);
		particles.Emit(local, 1);
		world.GetTransform().Translation = { 5.0f, 0.0f, 0.0f };
		local.GetTransform().Translation = { 5.0f, 1.0f, 0.0f };
		scene.OnUpdate(Step);

		const ParticleRenderData data = RenderData(scene);
		REQUIRE(data.Batches.size() == 2);
		CHECK(ParticlePosition(data, 0) == glm::vec3(0.0f, 0.0f, 0.0f));
		CHECK(ParticlePosition(data, 1) == glm::vec3(5.0f, 1.0f, 0.0f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Emitter-local velocities turn with the emitter in world space")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.Velocity = { 0.0f, 1.0f, 0.0f };
		Entity entity = AddEmitter(scene, "Turned", emitter);
		// +Y turned 90 degrees about Z points along -X; the scale must not speed it up.
		entity.GetTransform().Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		entity.GetTransform().Scale = glm::vec3(3.0f);
		scene.OnRuntimeStart();
		scene.GetParticleSystem()->Emit(entity, 1);
		StepFrames(scene, 61); // the emitting frame, then one second of motion
		const glm::vec3 position = ParticlePosition(RenderData(scene), 0);
		CHECK(position.x == doctest::Approx(-1.0).epsilon(1e-3));
		CHECK(position.y == doctest::Approx(0.0).epsilon(1e-3));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Every shape emits from inside its volume")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.ShapeRadius = 2.0f;
		emitter.BoxExtents = { 1.0f, 2.0f, 3.0f };
		emitter.ConeAngle = 30.0f;
		emitter.Shape = ParticleEmitterShape::Point;
		Entity point = AddEmitter(scene, "Point", emitter);
		emitter.Shape = ParticleEmitterShape::Sphere;
		Entity sphere = AddEmitter(scene, "Sphere", emitter);
		emitter.Shape = ParticleEmitterShape::Box;
		Entity box = AddEmitter(scene, "Box", emitter);
		emitter.Shape = ParticleEmitterShape::Cone;
		emitter.SpeedMin = 1.0f;
		emitter.SpeedMax = 1.0f;
		Entity cone = AddEmitter(scene, "Cone", emitter);
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		for (Entity entity : { point, sphere, box })
			particles.Emit(entity, 200);
		const ParticleRenderData atBirth = RenderData(scene);
		REQUIRE(atBirth.Batches.size() == 3);
		auto positions = [&atBirth](size_t batch) {
			std::vector<glm::vec3> result;
			result.reserve(atBirth.Batches[batch].Count);
			for (uint32_t i = 0; i < atBirth.Batches[batch].Count; i++)
				result.emplace_back(atBirth.Particles[atBirth.Batches[batch].FirstParticle + i].PositionSize);
			return result;
		};
		for (const glm::vec3& p : positions(0))
			CHECK(p == glm::vec3(0.0f));
		float farthest = 0.0f;
		for (const glm::vec3& p : positions(1))
			farthest = std::max(farthest, glm::length(p));
		CHECK(farthest <= 2.0f + 1e-5f);
		CHECK(farthest > 1.5f); // spread through the volume, not bunched at the center
		glm::vec3 extent(0.0f);
		for (const glm::vec3& p : positions(2))
			extent = glm::max(extent, glm::abs(p));
		CHECK(glm::all(glm::lessThanEqual(extent, glm::vec3(1.0f, 2.0f, 3.0f))));
		CHECK(glm::all(glm::greaterThan(extent, glm::vec3(0.8f, 1.6f, 2.4f))));

		// Cone particles leave the base disc within 30 degrees of +Y: after one second at 1 m/s each is
		// 1 m from its start, so at least cos(30) m up.
		particles.Emit(cone, 200);
		StepFrames(scene, 61); // the emitting frame, then one second of motion
		const ParticleRenderData moved = RenderData(scene);
		const ParticleBatch& coneBatch = moved.Batches.back();
		for (uint32_t i = 0; i < coneBatch.Count; i++)
		{
			const glm::vec3 p(moved.Particles[coneBatch.FirstParticle + i].PositionSize);
			CHECK(p.y >= std::cos(glm::radians(30.0f)) - 1e-3f);
			CHECK(p.y <= 1.0f + 1e-3f);
		}
		scene.OnRuntimeStop();
	}

	TEST_CASE("Emitters added, removed or destroyed during play")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Late");
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.PlayOnStart = true;
		emitter.BurstCount = 5;
		entity.AddComponent<ParticleEmitterComponent>(emitter);
		CHECK(particles.IsPlaying(entity));
		scene.OnUpdate(Step);
		CHECK(particles.GetParticleCount(entity) == 5);

		entity.RemoveComponent<ParticleEmitterComponent>();
		CHECK_FALSE(particles.IsPlaying(entity));
		CHECK(particles.GetParticleCount(entity) == 0);
		scene.OnUpdate(Step);

		Entity doomed = AddEmitter(scene, "Doomed", emitter);
		scene.OnUpdate(Step);
		CHECK(particles.GetTotalParticleCount() == 5);
		scene.DestroyEntity(doomed);
		scene.OnUpdate(Step);
		CHECK(particles.GetTotalParticleCount() == 0);
		CHECK(RenderData(scene).Batches.empty());
		scene.OnRuntimeStop();
		CHECK(scene.GetParticleSystem() == nullptr);
	}

	TEST_CASE("Particles simulate in simulation mode too")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.PlayOnStart = true;
		emitter.BurstCount = 2;
		Entity entity = AddEmitter(scene, "Emitter", emitter);
		scene.OnSimulationStart();
		REQUIRE(scene.GetParticleSystem());
		scene.OnUpdate(Step);
		CHECK(scene.GetParticleSystem()->GetParticleCount(entity) == 2);
		scene.OnSimulationStop();
		CHECK(scene.GetParticleSystem() == nullptr);
	}

	TEST_CASE("Particles repeat in every play session, follow the project seed and ignore registry order")
	{
		BasaltTest::TempProject project("ParticleSeed");
		REQUIRE(project.IsValid());
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.PlayOnStart = true;
		emitter.Rate = 20.0f;
		emitter.BurstCount = 5;
		emitter.Shape = ParticleEmitterShape::Sphere;
		emitter.SpeedMin = 0.5f;
		emitter.SpeedMax = 2.0f;
		emitter.LifetimeMin = 0.5f;
		emitter.LifetimeMax = 1.5f;
		emitter.EndSize = 1.0f;

		// Two emitters, created in either order with the same UUIDs (and positions).
		auto run = [&emitter](bool reversed) {
			Scene scene;
			const UUID ids[] = { UUID(1001), UUID(1002) };
			for (int i = 0; i < 2; i++)
			{
				const int index = reversed ? 1 - i : i;
				Entity entity = scene.CreateEntityWithUUID(ids[index], "Emitter");
				entity.GetTransform().Translation = { 5.0f * static_cast<float>(index), 0.0f, 0.0f };
				entity.AddComponent<ParticleEmitterComponent>(emitter);
			}
			scene.OnRuntimeStart();
			StepFrames(scene, 45);
			std::vector<float> snapshot = Snapshot(scene);
			const ParticleRenderData data = RenderData(scene);
			REQUIRE(data.Batches.size() == 2);
			// Identical settings, but each emitter has its own random stream.
			const glm::vec4 offset(5.0f, 0.0f, 0.0f, 0.0f);
			CHECK(glm::distance(data.Particles[data.Batches[0].FirstParticle].PositionSize + offset, data.Particles[data.Batches[1].FirstParticle].PositionSize) > 1e-3f);
			scene.OnRuntimeStop();
			return snapshot;
		};
		const std::vector<float> first = run(false);
		CHECK_FALSE(first.empty());
		CHECK(run(false) == first);
		CHECK(run(true) == first);

		Project::GetActive()->GetConfig().RandomSeed = 12345;
		CHECK(run(false) != first);
	}

	TEST_CASE("Emitting particles does not change Lua's random numbers")
	{
		auto sequence = [](bool withEmitter) {
			Scene scene;
			if (withEmitter)
			{
				ParticleEmitterComponent emitter = QuietEmitter();
				emitter.PlayOnStart = true;
				emitter.Rate = 100.0f;
				AddEmitter(scene, "Emitter", emitter);
			}
			scene.OnRuntimeStart();
			StepFrames(scene, 10);
			std::string result;
			REQUIRE(scene.GetScriptEngine()->ExecuteString("return math.random(1000000) .. ',' .. Math.RandomInt(1, 1000000)", result));
			scene.OnRuntimeStop();
			return result;
		};
		CHECK(sequence(true) == sequence(false));
	}

	TEST_CASE("Render data: billboards sorted back to front, color and size over life, one batch per emitter")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.Shape = ParticleEmitterShape::Box;
		emitter.BoxExtents = { 0.0f, 0.0f, 4.0f };
		emitter.LifetimeMin = 1.0f;
		emitter.LifetimeMax = 1.0f;
		emitter.StartColor = { 2.0f, 0.0f, 0.0f, 1.0f };
		emitter.EndColor = { 0.0f, 0.0f, 2.0f, 0.0f };
		emitter.StartSize = 1.0f;
		emitter.EndSize = 3.0f;
		emitter.Blend = ParticleBlendMode::Additive;
		emitter.Texture = "Assets/Textures/Spark.png";
		Entity line = AddEmitter(scene, "Line", emitter);
		emitter.Blend = ParticleBlendMode::Alpha;
		emitter.Texture.clear();
		emitter.BoxExtents = glm::vec3(0.0f);
		Entity far = AddEmitter(scene, "Far", emitter, { 0.0f, 0.0f, -20.0f });
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		particles.Emit(line, 50);
		particles.Emit(far, 2);
		StepFrames(scene, 31); // the emitting frame, then half a second of aging

		// Camera on +Z looking down -Z: farther means smaller z.
		const ParticleRenderData data = RenderData(scene, { 0.0f, 0.0f, 10.0f }, { 0.0f, 0.0f, -1.0f });
		REQUIRE(data.Batches.size() == 2);
		const ParticleBatch& lineBatch = data.Batches[0];
		const ParticleBatch& farBatch = data.Batches[1];
		CHECK(lineBatch.FirstParticle == 0);
		CHECK(lineBatch.Count == 50);
		CHECK(farBatch.FirstParticle == 50);
		CHECK(farBatch.Count == 2);
		CHECK(data.Particles.size() == 52);
		CHECK(lineBatch.Blend == ParticleBlendMode::Additive);
		CHECK(lineBatch.Texture == "Assets/Textures/Spark.png");
		CHECK(farBatch.Blend == ParticleBlendMode::Alpha);
		CHECK(farBatch.Texture.empty());
		CHECK(farBatch.ViewDepth == doctest::Approx(30.0f));
		CHECK(lineBatch.ViewDepth == doctest::Approx(10.0f - lineBatch.Bounds.GetCenter().z));

		for (uint32_t i = 1; i < lineBatch.Count; i++)
			CHECK(data.Particles[i - 1].PositionSize.z <= data.Particles[i].PositionSize.z);
		// Half-way through their life (aged 30 of 60 frames).
		const GpuParticle& sample = data.Particles[0];
		CHECK(sample.PositionSize.w == doctest::Approx(2.0f).epsilon(1e-3));
		CHECK(sample.Color.r == doctest::Approx(1.0f).epsilon(1e-3));
		CHECK(sample.Color.b == doctest::Approx(1.0f).epsilon(1e-3));
		CHECK(sample.Color.a == doctest::Approx(0.5f).epsilon(1e-3));
		// Bounds hold every billboard in any camera orientation: a corner of a 2 m square lies up to half its
		// diagonal (sqrt(2) m) from the center along a world axis.
		const glm::vec3 reach(std::sqrt(2.0f));
		for (uint32_t i = 0; i < lineBatch.Count; i++)
		{
			const glm::vec3 p(data.Particles[i].PositionSize);
			CHECK(glm::all(glm::greaterThanEqual(p - reach, lineBatch.Bounds.Min - glm::vec3(1e-3f))));
			CHECK(glm::all(glm::lessThanEqual(p + reach, lineBatch.Bounds.Max + glm::vec3(1e-3f))));
		}
		scene.OnRuntimeStop();
	}

	TEST_CASE("Values written past the registry cannot break the simulation, and are reported once")
	{
		Scene scene;
		ParticleEmitterComponent emitter;
		emitter.PlayOnStart = true;
		emitter.Duration = 0.0f;
		emitter.MaxParticles = 0;
		emitter.Rate = std::numeric_limits<float>::infinity();
		emitter.BurstCount = 1000000;
		emitter.LifetimeMin = 0.0f;
		emitter.LifetimeMax = std::numeric_limits<float>::quiet_NaN();
		emitter.SpeedMin = -3e38f;
		emitter.SpeedMax = 3e38f;
		emitter.Velocity = { 3e38f, 0.0f, 0.0f };
		emitter.Drag = -10.0f;
		emitter.ShapeRadius = -1.0f;
		emitter.StartColor = { std::numeric_limits<float>::quiet_NaN(), -1.0f, 2.0f, 5.0f };
		emitter.Shape = ParticleEmitterShape::Sphere;
		emitter.Blend = static_cast<ParticleBlendMode>(7);
		Entity entity = AddEmitter(scene, "Broken", emitter);
		ParticleEmitterComponent dragged = QuietEmitter();
		dragged.Drag = -1.0f;
		AddEmitter(scene, "BrokenB", dragged);
		const uint64_t logStart = Log::GetHistory().GetTotalCount();
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		StepFrames(scene, 3);
		// Once per emitter, naming the emitter and the broken field.
		CHECK(BasaltTest::CountLogMessages(logStart, "has invalid values") == 2);
		CHECK(BasaltTest::CountLogMessages(logStart, "'BrokenB' has invalid values (Drag must be between") == 1);
		// MaxParticles 0 acts as 1; every value the particle carries stays finite, and the blend mode valid.
		CHECK(particles.GetParticleCount(entity) == 1);
		const ParticleRenderData data = RenderData(scene);
		REQUIRE(data.Batches.size() == 1); // BrokenB has no particles
		CHECK(data.Batches[0].Blend == ParticleBlendMode::Alpha);
		CHECK(std::isfinite(data.Batches[0].ViewDepth));
		for (const GpuParticle& particle : data.Particles)
		{
			for (int i = 0; i < 4; i++)
			{
				CHECK(std::isfinite(particle.PositionSize[i]));
				CHECK(std::isfinite(particle.Color[i]));
			}
			CHECK(particle.Color.a <= 1.0f);
		}

		// Zero, negative and non-finite time steps change nothing (the bad ones are reported once).
		const std::vector<float> before = Snapshot(scene);
		const uint64_t stepLog = Log::GetHistory().GetTotalCount();
		particles.Update(0.0f); // a paused frame: legitimate, silent
		CHECK(BasaltTest::CountLogMessages(stepLog, "ignoring a time step") == 0);
		for (float step : { 0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
			particles.Update(step);
		CHECK(Snapshot(scene) == before);
		CHECK(BasaltTest::CountLogMessages(stepLog, "ignoring a time step") == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("The emitter rules: a component is valid exactly when sanitizing leaves it unchanged")
	{
		CHECK(ValidateParticleEmitter(ParticleEmitterComponent{}).empty());
		// Boundary and out-of-range values for every field, combined at random.
		const float nan = std::numeric_limits<float>::quiet_NaN();
		const float values[] = { 0.0f, -0.0f, 1e-4f, 1e-3f, 0.5f, 1.0f, 2.0f, 180.0f, 181.0f, 1e4f, 1.5e4f, 1e6f, 2e6f, -1.0f, -1e4f, -2e4f, 3e38f, std::numeric_limits<float>::infinity(), nan };
		const uint32_t counts[] = { 0, 1, 50, ParticleEmitterComponent::MaxParticlesLimit, ParticleEmitterComponent::MaxParticlesLimit + 1 };
		Random random(99);
		auto pick = [&]() { return values[random.Int(0, std::size(values) - 1)]; };
		int valid = 0;
		for (int i = 0; i < 6000; i++)
		{
			ParticleEmitterComponent c;
			// Most fields keep their defaults, so valid components come up too.
			auto maybe = [&](float& field) {
				if (random.Float() < 0.06f)
					field = pick();
			};
			for (float* field : { &c.Duration, &c.Rate, &c.ShapeRadius, &c.ConeAngle, &c.LifetimeMin, &c.LifetimeMax, &c.SpeedMin, &c.SpeedMax, &c.GravityScale, &c.Drag, &c.StartSize, &c.EndSize })
				maybe(*field);
			for (int axis = 0; axis < 3; axis++)
			{
				maybe(c.BoxExtents[axis]);
				maybe(c.Velocity[axis]);
			}
			for (int channel = 0; channel < 4; channel++)
			{
				maybe(c.StartColor[channel]);
				maybe(c.EndColor[channel]);
			}
			if (random.Float() < 0.1f)
				c.MaxParticles = counts[random.Int(0, std::size(counts) - 1)];
			if (random.Float() < 0.1f)
				c.BurstCount = counts[random.Int(0, std::size(counts) - 1)];
			if (random.Float() < 0.05f)
				c.Shape = static_cast<ParticleEmitterShape>(random.Int(0, 5));
			if (random.Float() < 0.05f)
				c.SimulationSpace = static_cast<ParticleSimulationSpace>(random.Int(0, 3));
			if (random.Float() < 0.05f)
				c.Blend = static_cast<ParticleBlendMode>(random.Int(0, 3));

			INFO("iteration " << i);
			const bool isValid = ValidateParticleEmitter(c).empty();
			valid += isValid ? 1 : 0;
			const ParticleEmitterComponent sanitized = SanitizeParticleEmitter(c);
			CHECK((sanitized == c) == isValid);
			CHECK(ValidateParticleEmitter(sanitized).empty());
		}
		// Both outcomes were exercised.
		CHECK(valid > 100);
		CHECK(valid < 5900);
	}

	TEST_CASE("Emitters added through the registry during play honour PlayOnStart; failed adds change nothing")
	{
		BasaltTest::TempProject project("ParticleRegistryAdd");
		REQUIRE(project.IsValid());
		Scene scene;
		Entity manual = scene.CreateEntity("Manual");
		Entity rejected = scene.CreateEntity("Rejected");
		Entity automatic = scene.CreateEntity("Automatic");
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		std::string error;
		// Lua AddComponent, component.set and prefab instantiation all add through AddOrPatch.
		REQUIRE(ComponentRegistry::AddOrPatch(manual, "ParticleEmitter", { { "PlayOnStart", false }, { "BurstCount", 7 }, { "Rate", 0 } }, error));
		CHECK_FALSE(particles.IsPlaying(manual));
		CHECK_FALSE(ComponentRegistry::AddOrPatch(rejected, "ParticleEmitter", { { "BurstCount", 7 }, { "LifetimeMin", 0 } }, error));
		CHECK_FALSE(rejected.HasComponent<ParticleEmitterComponent>());
		CHECK_FALSE(particles.IsPlaying(rejected));
		REQUIRE(ComponentRegistry::AddOrPatch(automatic, "ParticleEmitter", { { "BurstCount", 7 }, { "Rate", 0 } }, error));
		CHECK(particles.IsPlaying(automatic));
		scene.OnUpdate(Step);
		CHECK(particles.GetParticleCount(manual) == 0);
		CHECK(particles.GetParticleCount(automatic) == 7);

		// A prefab whose emitter waits for a script to fire it.
		project.WriteFile("Assets/Prefabs/Blast.bprefab", R"({ "Format": "BasaltPrefab", "Version": 1, "Entities": [
			{ "ID": 1, "Name": "Blast", "Parent": 0, "Components": { "ParticleEmitter": { "PlayOnStart": false, "BurstCount": 9 } } } ] })");
		Entity blast = SceneSerializer::InstantiatePrefab(scene, project.GetDirectory() / "Assets/Prefabs/Blast.bprefab", "Assets/Prefabs/Blast.bprefab", {}, error);
		REQUIRE_MESSAGE(blast, error);
		CHECK_FALSE(particles.IsPlaying(blast));
		scene.OnUpdate(Step);
		CHECK(particles.GetParticleCount(blast) == 0);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Emitters spawned during play with random UUIDs repeat in every session")
	{
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.PlayOnStart = true;
		emitter.Rate = 30.0f;
		emitter.Shape = ParticleEmitterShape::Sphere;
		emitter.SpeedMin = 0.5f;
		emitter.SpeedMax = 1.5f;
		auto run = [&emitter]() {
			Scene scene;
			AddEmitter(scene, "SceneEmitter", emitter);
			scene.OnRuntimeStart();
			StepFrames(scene, 5);
			for (int i = 0; i < 3; i++)
			{
				// CreateEntity draws a fresh random UUID every session.
				Entity spawned = scene.CreateEntity("Spawned");
				spawned.GetTransform().Translation = { 2.0f * static_cast<float>(i), 0.0f, 0.0f };
				spawned.AddComponent<ParticleEmitterComponent>(emitter);
			}
			StepFrames(scene, 30);
			std::vector<float> snapshot = Snapshot(scene);
			scene.OnRuntimeStop();
			return snapshot;
		};
		const std::vector<float> first = run();
		CHECK_FALSE(first.empty());
		CHECK(run() == first);
	}

	TEST_CASE("Particles repeat across sessions of one scene, its play copy, and after simulating")
	{
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.PlayOnStart = true;
		emitter.Rate = 30.0f;
		emitter.Shape = ParticleEmitterShape::Cone;
		emitter.SpeedMin = 1.0f;
		Ref<Scene> scene = CreateRef<Scene>();
		AddEmitter(*scene, "A", emitter);
		AddEmitter(*scene, "B", emitter, { 3.0f, 0.0f, 0.0f });
		auto play = [](Scene& target) {
			target.OnRuntimeStart();
			StepFrames(target, 20);
			std::vector<float> snapshot = Snapshot(target);
			target.OnRuntimeStop();
			return snapshot;
		};
		const std::vector<float> first = play(*scene);
		CHECK_FALSE(first.empty());
		CHECK(play(*scene) == first);
		Ref<Scene> copy = Scene::Copy(scene);
		CHECK(play(*copy) == first);
		scene->OnSimulationStart();
		StepFrames(*scene, 7);
		scene->OnSimulationStop();
		CHECK(play(*scene) == first);
	}

	TEST_CASE("Local-space particles move in world units whatever the emitter's scale and rotation")
	{
		Scene scene;
		scene.GetPhysicsSettings().Gravity = { 0.0f, -10.0f, 0.0f };
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.SimulationSpace = ParticleSimulationSpace::Local;
		emitter.Velocity = { 0.0f, 1.0f, 0.0f };
		Entity scaled = AddEmitter(scene, "Scaled", emitter);
		scaled.GetTransform().Scale = glm::vec3(3.0f);
		emitter.Velocity = glm::vec3(0.0f);
		emitter.GravityScale = 1.0f;
		Entity turned = AddEmitter(scene, "Turned", emitter, { 10.0f, 0.0f, 0.0f });
		turned.GetTransform().Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		turned.GetTransform().Scale = { 2.0f, 0.5f, 1.0f };
		emitter.SimulationSpace = ParticleSimulationSpace::World;
		Entity reference = AddEmitter(scene, "Reference", emitter, { 20.0f, 0.0f, 0.0f });
		Entity flat = AddEmitter(scene, "Flat", emitter, { 30.0f, 0.0f, 0.0f });
		flat.GetComponent<ParticleEmitterComponent>().SimulationSpace = ParticleSimulationSpace::Local;
		flat.GetTransform().Scale = { 1.0f, 0.0f, 1.0f };
		scene.OnRuntimeStart();
		for (Entity entity : { scaled, turned, reference, flat })
			scene.GetParticleSystem()->Emit(entity, 1);
		StepFrames(scene, 61); // the emitting frame, then one second of motion

		const ParticleRenderData data = RenderData(scene);
		REQUIRE(data.Batches.size() == 4);
		// 1 m/s for a second: 1 m, not 3, although the emitter is three times larger.
		CHECK(ParticlePosition(data, 0).y == doctest::Approx(1.0).epsilon(1e-3));
		// Gravity pulls a turned, unevenly scaled local particle straight down, as far as a world one.
		const glm::vec3 fallen = ParticlePosition(data, 1);
		const glm::vec3 expected = ParticlePosition(data, 2) - glm::vec3(10.0f, 0.0f, 0.0f);
		CHECK(fallen.x == doctest::Approx(expected.x).epsilon(1e-3));
		CHECK(fallen.y == doctest::Approx(expected.y).epsilon(1e-3));
		CHECK(expected.y < -4.0f);
		// A flattened emitter has no frame to express gravity in; its particle stays put and finite.
		const glm::vec3 flattened = ParticlePosition(data, 3);
		CHECK(flattened == glm::vec3(30.0f, 0.0f, 0.0f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Emission directions: Point and Sphere in every direction, Box along +Y, Cone within its angle")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.SpeedMin = 1.0f;
		emitter.SpeedMax = 1.0f;
		emitter.ShapeRadius = 0.0f;
		emitter.BoxExtents = glm::vec3(0.0f);
		emitter.Shape = ParticleEmitterShape::Point;
		Entity point = AddEmitter(scene, "Point", emitter);
		emitter.Shape = ParticleEmitterShape::Box;
		Entity box = AddEmitter(scene, "Box", emitter);
		emitter.Shape = ParticleEmitterShape::Cone;
		emitter.ConeAngle = 30.0f;
		Entity cone = AddEmitter(scene, "Cone", emitter);
		scene.OnRuntimeStart();
		constexpr uint32_t Count = 500;
		for (Entity entity : { point, box, cone })
			scene.GetParticleSystem()->Emit(entity, Count);
		StepFrames(scene, 61); // the emitting frame, then one second of motion
		const ParticleRenderData data = RenderData(scene);
		REQUIRE(data.Batches.size() == 3);

		glm::vec3 mean(0.0f);
		glm::bvec3 negative(false);
		for (uint32_t i = 0; i < Count; i++)
		{
			const glm::vec3 p = ParticlePosition(data, 0, i);
			CHECK(glm::length(p) == doctest::Approx(1.0).epsilon(1e-3));
			mean += p / static_cast<float>(Count);
			negative = glm::bvec3(negative.x || p.x < 0.0f, negative.y || p.y < 0.0f, negative.z || p.z < 0.0f);
		}
		CHECK(glm::length(mean) < 0.15f);
		CHECK(glm::all(negative));
		for (uint32_t i = 0; i < Count; i++)
			CHECK(glm::distance(ParticlePosition(data, 1, i), glm::vec3(0.0f, 1.0f, 0.0f)) < 1e-3f);
		float lowest = 1.0f;
		for (uint32_t i = 0; i < Count; i++)
			lowest = std::min(lowest, ParticlePosition(data, 2, i).y);
		// Within 30 degrees of +Y, and actually spread over that angle.
		CHECK(lowest >= std::cos(glm::radians(30.0f)) - 1e-3f);
		CHECK(lowest < std::cos(glm::radians(25.0f)));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Long frames: one burst however many cycles they span; a one-shot emitter emits only for its duration")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.PlayOnStart = true;
		emitter.BurstCount = 3;
		emitter.Duration = 0.5f;
		Entity looping = AddEmitter(scene, "Looping", emitter);
		emitter.BurstCount = 0;
		emitter.Looping = false;
		emitter.Duration = 0.05f;
		emitter.Rate = 100.0f;
		Entity once = AddEmitter(scene, "Once", emitter);
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		particles.Update(Step);
		CHECK(particles.GetParticleCount(looping) == 3);
		particles.Update(10.0f);
		particles.Update(Step);
		CHECK(particles.GetParticleCount(looping) == 6);
		// The long frame wrapped into the current cycle: no burst on the next frames.
		particles.Update(Step);
		CHECK(particles.GetParticleCount(looping) == 6);
		CHECK(particles.IsPlaying(looping));
		// 100/s for what is left of 0.05 s, not for the whole 10 s step.
		CHECK(particles.GetParticleCount(once) >= 4);
		CHECK(particles.GetParticleCount(once) <= 5);
		CHECK_FALSE(particles.IsPlaying(once));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Particles emitted by scripts are drawn at age 0 before they move")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.LifetimeMin = 0.01f; // shorter than a frame
		emitter.LifetimeMax = 0.01f;
		emitter.Velocity = { 1.0f, 0.0f, 0.0f };
		emitter.StartColor = { 1.0f, 0.0f, 0.0f, 1.0f };
		Entity entity = AddEmitter(scene, "Emitter", emitter);
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		// As from OnUpdate: emitted before the particle update of the same frame.
		particles.Emit(entity, 2);
		scene.OnUpdate(Step);
		CHECK(particles.GetParticleCount(entity) == 2);
		const ParticleRenderData data = RenderData(scene);
		CHECK(ParticlePosition(data, 0) == glm::vec3(0.0f));
		CHECK(data.Particles[0].Color == emitter.StartColor);
		scene.OnUpdate(Step);
		CHECK(particles.GetParticleCount(entity) == 0);
		// Stop(clear) right after Emit forgets the fresh particles too (defensive: Emit adds fresh ones anyway).
		particles.Emit(entity, 1);
		particles.Stop(entity, true);
		particles.Emit(entity, 1);
		scene.OnUpdate(Step);
		CHECK(particles.GetParticleCount(entity) == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("An emitter whose entity loses its ID before the emitter is still unregistered")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		Entity entity = AddEmitter(scene, "Emitter", emitter);
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		particles.Emit(entity, 4);
		entt::registry& registry = scene.GetRegistry();
		registry.erase<IDComponent>(entity);
		registry.erase<ParticleEmitterComponent>(entity);
		CHECK(particles.GetTotalParticleCount() == 0);
		scene.OnUpdate(Step);
		CHECK(RenderData(scene).Batches.empty());
		registry.emplace<IDComponent>(entity, UUID(77));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Lowering MaxParticles right after Emit trims the newest particles; the survivors keep aging")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.LifetimeMin = 1.0f;
		emitter.LifetimeMax = 1.0f;
		emitter.StartSize = 1.0f;
		emitter.EndSize = 3.0f;
		Entity entity = AddEmitter(scene, "Emitter", emitter);
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();

		// Only fresh particles, more than the new cap.
		CHECK(particles.Emit(entity, 5) == 5);
		entity.GetComponent<ParticleEmitterComponent>().MaxParticles = 3;
		scene.OnUpdate(Step);
		CHECK(particles.GetParticleCount(entity) == 3);
		for (const GpuParticle& particle : RenderData(scene).Particles)
			CHECK(particle.PositionSize.w == doctest::Approx(1.0f)); // still age 0

		// Old particles plus fresh ones: the trim drops the fresh ones, and the old survivors age this update.
		particles.Stop(entity, true);
		entity.GetComponent<ParticleEmitterComponent>().MaxParticles = 10;
		particles.Emit(entity, 4);
		StepFrames(scene, 31); // the emitting frame, then 30 frames: age 30/60
		particles.Emit(entity, 5);
		entity.GetComponent<ParticleEmitterComponent>().MaxParticles = 3;
		scene.OnUpdate(Step);
		REQUIRE(particles.GetParticleCount(entity) == 3);
		for (const GpuParticle& particle : RenderData(scene).Particles)
			CHECK(particle.PositionSize.w == doctest::Approx(1.0f + 2.0f * 31.0f / 60.0f).epsilon(1e-4));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Simulate mode seeds particles from the project, like play mode")
	{
		BasaltTest::TempProject project("ParticleSimulateSeed");
		REQUIRE(project.IsValid());
		Project::GetActive()->GetConfig().RandomSeed = 12345;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.PlayOnStart = true;
		emitter.Rate = 30.0f;
		emitter.Shape = ParticleEmitterShape::Sphere;
		emitter.SpeedMin = 0.5f;
		emitter.SpeedMax = 2.0f;
		auto run = [&emitter](bool simulate) {
			Scene scene; // fresh: its seed starts at the default
			AddEmitter(scene, "Emitter", emitter);
			if (simulate)
				scene.OnSimulationStart();
			else
				scene.OnRuntimeStart();
			CHECK(scene.GetRandomSeed() == 12345u);
			StepFrames(scene, 20);
			std::vector<float> snapshot = Snapshot(scene);
			if (simulate)
				scene.OnSimulationStop();
			else
				scene.OnRuntimeStop();
			return snapshot;
		};
		CHECK(run(true) == run(false));
	}

	TEST_CASE("Huge rates and steps emit at most MaxParticles")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.PlayOnStart = true;
		emitter.Rate = ParticleEmitterComponent::MaxRate;
		emitter.MaxParticles = 50;
		Entity entity = AddEmitter(scene, "Firehose", emitter);
		scene.OnRuntimeStart();
		// 1e10 particles owed: more than a uint32 holds.
		scene.GetParticleSystem()->Update(1e4f);
		CHECK(scene.GetParticleSystem()->GetParticleCount(entity) == 50);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A removed emitter's ordinal is not reused, so later emitters keep their own streams")
	{
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.Shape = ParticleEmitterShape::Sphere;
		emitter.ShapeRadius = 1.0f;
		Scene scene;
		Entity first = AddEmitter(scene, "A", emitter);
		Entity second = AddEmitter(scene, "B", emitter, { 5.0f, 0.0f, 0.0f });
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		first.RemoveComponent<ParticleEmitterComponent>();
		Entity third = scene.CreateEntity("C");
		third.GetTransform().Translation = { 10.0f, 0.0f, 0.0f };
		third.AddComponent<ParticleEmitterComponent>(emitter);
		particles.Emit(second, 1);
		particles.Emit(third, 1);
		const ParticleRenderData data = RenderData(scene);
		REQUIRE(data.Batches.size() == 2);
		// With a reused ordinal C would draw B's stream: the same offset from its emitter.
		const glm::vec3 fromB = ParticlePosition(data, 0) - glm::vec3(5.0f, 0.0f, 0.0f);
		const glm::vec3 fromC = ParticlePosition(data, 1) - glm::vec3(10.0f, 0.0f, 0.0f);
		CHECK(glm::distance(fromB, fromC) > 1e-3f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Velocities ignore uneven scale in local space and mirror with a mirrored world emitter")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.SimulationSpace = ParticleSimulationSpace::Local;
		emitter.Velocity = { 0.0f, 1.0f, 0.0f };
		Entity squashed = AddEmitter(scene, "Squashed", emitter);
		squashed.GetTransform().Scale = { 1.0f, 0.5f, 1.0f };
		emitter.SimulationSpace = ParticleSimulationSpace::World;
		emitter.Velocity = { 1.0f, 0.0f, 0.0f };
		Entity mirrored = AddEmitter(scene, "Mirrored", emitter, { 10.0f, 0.0f, 0.0f });
		mirrored.GetTransform().Scale = { -1.0f, 1.0f, 1.0f };
		scene.OnRuntimeStart();
		scene.GetParticleSystem()->Emit(squashed, 1);
		scene.GetParticleSystem()->Emit(mirrored, 1);
		StepFrames(scene, 61); // the emitting frame, then one second of motion
		const ParticleRenderData data = RenderData(scene);
		REQUIRE(data.Batches.size() == 2);
		CHECK(ParticlePosition(data, 0).y == doctest::Approx(1.0).epsilon(1e-3));
		CHECK(ParticlePosition(data, 1).x == doctest::Approx(9.0).epsilon(1e-3));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Reversed min/max pairs pick between the two values")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.LifetimeMin = 0.5f;
		emitter.LifetimeMax = 0.25f;
		Entity entity = AddEmitter(scene, "Reversed", emitter);
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		particles.Emit(entity, 100);
		StepFrames(scene, 14); // the emitting frame, then 13/60 s: younger than 0.25 s
		CHECK(particles.GetParticleCount(entity) == 100);
		StepFrames(scene, 18); // 31/60 s: older than 0.5 s
		CHECK(particles.GetParticleCount(entity) == 0);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Render data beyond a particle budget drops whole batches from the end")
	{
		ParticleRenderData data;
		for (uint32_t i = 0; i < 3; i++)
		{
			ParticleBatch batch;
			batch.FirstParticle = static_cast<uint32_t>(data.Particles.size());
			batch.Count = 4;
			data.Batches.push_back(batch);
			data.Particles.resize(data.Particles.size() + 4);
		}
		CHECK_FALSE(data.LimitTo(12));
		CHECK(data.Batches.size() == 3);
		CHECK(data.LimitTo(10));
		CHECK(data.Batches.size() == 2);
		CHECK(data.Particles.size() == 8);
		CHECK(data.LimitTo(3));
		CHECK(data.Batches.empty());
		CHECK(data.Particles.empty());
	}

	TEST_CASE("Lowering a one-shot emitter's Duration below the time it has run stops it without a burst")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.PlayOnStart = true;
		emitter.Looping = false;
		emitter.Rate = 50.0f;
		Entity entity = AddEmitter(scene, "Fountain", emitter);
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		StepFrames(scene, 180);
		const uint32_t before = particles.GetParticleCount(entity);
		CHECK(before >= 149);
		entity.GetComponent<ParticleEmitterComponent>().Duration = 1.0f; // valid, but 3 s have passed
		scene.OnUpdate(Step);
		CHECK(particles.GetParticleCount(entity) == before);
		CHECK_FALSE(particles.IsPlaying(entity));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Invalid emitter values are reported again after they were fixed, listing every problem")
	{
		Scene scene;
		Entity entity = AddEmitter(scene, "Emitter", QuietEmitter());
		scene.OnRuntimeStart();
		auto& component = entity.GetComponent<ParticleEmitterComponent>();
		const uint64_t logStart = Log::GetHistory().GetTotalCount();
		component.Drag = -1.0f;
		component.Blend = static_cast<ParticleBlendMode>(7);
		StepFrames(scene, 3);
		CHECK(BasaltTest::CountLogMessages(logStart, "has invalid values (Drag must be between 0 and 10000 (got -1); Blend must be one of its 2 values (got 7))") == 1);
		component = QuietEmitter();
		StepFrames(scene, 2);
		component.Rate = -5.0f;
		StepFrames(scene, 2);
		CHECK(BasaltTest::CountLogMessages(logStart, "has invalid values") == 2);
		CHECK(BasaltTest::CountLogMessages(logStart, "Rate must be between") == 1);
		scene.OnRuntimeStop();

		ParticleEmitterComponent broken;
		broken.Duration = std::numeric_limits<float>::quiet_NaN();
		broken.Shape = static_cast<ParticleEmitterShape>(9);
		const std::vector<std::string> problems = FindParticleEmitterProblems(broken);
		REQUIRE(problems.size() == 2);
		CHECK(problems[0].find("Duration") != std::string::npos);
		CHECK(problems[1] == "Shape must be one of its 4 values (got 9)");
		CHECK(ValidateParticleEmitter(broken) == problems[0]);
	}

	TEST_CASE("Changing SimulationSpace during play keeps live particles where they are and moving as they were")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.Velocity = { 0.0f, 1.0f, 0.0f };
		Entity entity = AddEmitter(scene, "Emitter", emitter, { 5.0f, 0.0f, 0.0f });
		entity.GetTransform().Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		entity.GetTransform().Scale = { 2.0f, 3.0f, 0.5f };
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		particles.Emit(entity, 1);
		StepFrames(scene, 31); // half a second along world -X (local +Y turned 90 degrees about Z)
		const glm::vec3 start = ParticlePosition(RenderData(scene), 0);
		CHECK(start.x == doctest::Approx(4.5).epsilon(1e-3));

		auto& component = entity.GetComponent<ParticleEmitterComponent>();
		component.SimulationSpace = ParticleSimulationSpace::Local;
		// Drawn before the next update: still where it was.
		CHECK(glm::distance(ParticlePosition(RenderData(scene), 0), start) < 1e-4f);
		StepFrames(scene, 30);
		const glm::vec3 local = ParticlePosition(RenderData(scene), 0);
		CHECK(local.x == doctest::Approx(4.0).epsilon(1e-3));
		CHECK(local.y == doctest::Approx(0.0).epsilon(1e-3));

		component.SimulationSpace = ParticleSimulationSpace::World;
		StepFrames(scene, 30);
		CHECK(ParticlePosition(RenderData(scene), 0).x == doctest::Approx(3.5).epsilon(1e-3));
		// New particles follow the new space too.
		particles.Emit(entity, 1);
		scene.OnUpdate(Step);
		CHECK(particles.GetParticleCount(entity) == 2);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Lua: EmitParticles, PlayParticles, StopParticles, IsEmittingParticles, GetParticleCount")
	{
		Scene scene;
		ParticleEmitterComponent emitter = QuietEmitter();
		emitter.BurstCount = 3;
		emitter.MaxParticles = 50;
		AddEmitter(scene, "Fx", emitter);
		scene.CreateEntity("Plain");
		scene.OnRuntimeStart();
		ScriptEngine& lua = *scene.GetScriptEngine();
		std::string result;
		REQUIRE(lua.ExecuteString("fx = Scene.FindEntityByName('Fx') plain = Scene.FindEntityByName('Plain')", result));
		REQUIRE(lua.ExecuteString("return tostring(fx:IsEmittingParticles()) .. ',' .. fx:GetParticleCount()", result));
		CHECK(result == "false,0");
		REQUIRE(lua.ExecuteString("return fx:EmitParticles(10)", result));
		CHECK(result == "10");
		REQUIRE(lua.ExecuteString("return fx:EmitParticles(100)", result));
		CHECK(result == "40");
		REQUIRE(lua.ExecuteString("fx:StopParticles(true) fx:PlayParticles() return tostring(fx:IsEmittingParticles()) .. ',' .. fx:GetParticleCount()", result));
		CHECK(result == "true,0");
		scene.OnUpdate(Step);
		REQUIRE(lua.ExecuteString("return fx:GetParticleCount()", result));
		CHECK(result == "3");
		REQUIRE(lua.ExecuteString("fx:StopParticles() return tostring(fx:IsEmittingParticles()) .. ',' .. fx:GetParticleCount()", result));
		CHECK(result == "false,3");

		for (const char* call : { "plain:EmitParticles(1)", "plain:PlayParticles()", "plain:StopParticles()" })
		{
			INFO(call);
			CHECK_FALSE(lua.ExecuteString(call, result));
			CHECK(result.find("needs the ParticleEmitter component on 'Plain'") != std::string::npos);
		}
		REQUIRE(lua.ExecuteString("return tostring(plain:IsEmittingParticles()) .. ',' .. plain:GetParticleCount()", result));
		CHECK(result == "false,0");
		CHECK_FALSE(lua.ExecuteString("fx:EmitParticles(-1)", result));
		CHECK(result.find("between 0 and") != std::string::npos);
		CHECK_FALSE(lua.ExecuteString("fx:EmitParticles(20000)", result));
		CHECK(result.find("between 0 and") != std::string::npos);
		// clear must be a boolean: a truthy 1 or "yes" would otherwise silently mean false.
		for (const char* call : { "fx:StopParticles(1)", "fx:StopParticles('yes')" })
		{
			INFO(call);
			CHECK_FALSE(lua.ExecuteString(call, result));
			CHECK(result.find("clear must be true, false or nil") != std::string::npos);
		}
		scene.OnRuntimeStop();
	}
}
