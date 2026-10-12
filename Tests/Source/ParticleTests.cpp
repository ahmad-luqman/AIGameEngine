#include <doctest/doctest.h>

#include <Basalt/Particles/ParticleSystem.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
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
		for (const ParticleVertex& particle : RenderData(scene).Particles)
		{
			for (int i = 0; i < 4; i++)
				values.push_back(particle.PositionSize[i]);
			for (int i = 0; i < 4; i++)
				values.push_back(particle.Color[i]);
		}
		return values;
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
			{ { { "ConeAngle", 181 } }, "ConeAngle" },
			{ { { "ConeAngle", -1 } }, "ConeAngle" },
			{ { { "LifetimeMin", 0 } }, "LifetimeMin" },
			{ { { "LifetimeMin", 3 }, { "LifetimeMax", 2 } }, "LifetimeMax" },
			{ { { "SpeedMin", 3 }, { "SpeedMax", 2 } }, "SpeedMax" },
			{ { { "Shape", "Torus" } }, "Shape" },
			{ { { "Blend", "Multiply" } }, "Blend" },
			{ { { "SimulationSpace", "Screen" } }, "SimulationSpace" },
		};
		for (const auto& [patch, field] : bad)
		{
			INFO(patch.dump());
			error.clear();
			CHECK_FALSE(Deserialize(entity, patch, error));
			CHECK(error.find(field) != std::string::npos);
			// A rejected patch leaves the component untouched.
			CHECK(entity.GetComponent<ParticleEmitterComponent>() == ParticleEmitterComponent{});
		}

		REQUIRE(Deserialize(entity, { { "Shape", "Cone" }, { "ConeAngle", 180 }, { "Blend", "Additive" }, { "SimulationSpace", "Local" }, { "LifetimeMin", 2 }, { "LifetimeMax", 2 }, { "SpeedMin", -1 } }, error));
		const auto& emitter = entity.GetComponent<ParticleEmitterComponent>();
		CHECK(emitter.Shape == ParticleEmitterShape::Cone);
		CHECK(emitter.ConeAngle == 180.0f);
		CHECK(emitter.Blend == ParticleBlendMode::Additive);
		CHECK(emitter.SimulationSpace == ParticleSimulationSpace::Local);
		// Negative speeds are allowed: they emit inwards (towards the cone's apex, the sphere's center).
		CHECK(emitter.SpeedMin == -1.0f);
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

		// Entities without an emitter: nothing happens.
		Entity plain = scene.CreateEntity("Plain");
		CHECK_FALSE(particles.Play(plain));
		CHECK(particles.Emit(plain, 3) == 0);
		particles.Stop(plain, true);
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
		StepFrames(scene, 60);

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
		CHECK(glm::vec3(data.Particles[data.Batches[0].FirstParticle].PositionSize) == glm::vec3(0.0f, 0.0f, 0.0f));
		CHECK(glm::vec3(data.Particles[data.Batches[1].FirstParticle].PositionSize) == glm::vec3(5.0f, 1.0f, 0.0f));
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
		StepFrames(scene, 60);
		const glm::vec3 position(RenderData(scene).Particles.at(0).PositionSize);
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
		StepFrames(scene, 60);
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
		StepFrames(scene, 30);

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
		// Half-way through their life (30 of 60 frames).
		const ParticleVertex& sample = data.Particles[0];
		CHECK(sample.PositionSize.w == doctest::Approx(2.0f).epsilon(1e-3));
		CHECK(sample.Color.r == doctest::Approx(1.0f).epsilon(1e-3));
		CHECK(sample.Color.b == doctest::Approx(1.0f).epsilon(1e-3));
		CHECK(sample.Color.a == doctest::Approx(0.5f).epsilon(1e-3));
		// Bounds hold every billboard, grown by half the largest size.
		for (uint32_t i = 0; i < lineBatch.Count; i++)
		{
			const glm::vec3 p(data.Particles[i].PositionSize);
			CHECK(glm::all(glm::greaterThanEqual(p - glm::vec3(1.0f), lineBatch.Bounds.Min - glm::vec3(1e-3f))));
			CHECK(glm::all(glm::lessThanEqual(p + glm::vec3(1.0f), lineBatch.Bounds.Max + glm::vec3(1e-3f))));
		}
		scene.OnRuntimeStop();
	}

	TEST_CASE("Values written past the registry cannot break the simulation")
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
		emitter.SpeedMin = 5.0f;
		emitter.SpeedMax = 1.0f;
		emitter.Drag = -10.0f;
		emitter.ShapeRadius = -1.0f;
		emitter.Shape = ParticleEmitterShape::Sphere;
		Entity entity = AddEmitter(scene, "Broken", emitter);
		scene.OnRuntimeStart();
		ParticleSystem& particles = *scene.GetParticleSystem();
		StepFrames(scene, 3);
		// MaxParticles 0 acts as 1; every value the particle carries stays finite.
		CHECK(particles.GetParticleCount(entity) == 1);
		for (const ParticleVertex& particle : RenderData(scene).Particles)
		{
			CHECK(std::isfinite(particle.PositionSize.x));
			CHECK(std::isfinite(particle.PositionSize.y));
			CHECK(std::isfinite(particle.PositionSize.w));
			CHECK(std::isfinite(particle.Color.a));
		}
		// Zero and negative time steps do nothing.
		scene.GetParticleSystem()->Update(0.0f);
		scene.GetParticleSystem()->Update(-1.0f);
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
			CHECK(result.find("needs a ParticleEmitter component on 'Plain'") != std::string::npos);
		}
		REQUIRE(lua.ExecuteString("return tostring(plain:IsEmittingParticles()) .. ',' .. plain:GetParticleCount()", result));
		CHECK(result == "false,0");
		CHECK_FALSE(lua.ExecuteString("fx:EmitParticles(-1)", result));
		CHECK(result.find("between 0 and") != std::string::npos);
		CHECK_FALSE(lua.ExecuteString("fx:EmitParticles(20000)", result));
		CHECK(result.find("between 0 and") != std::string::npos);
		scene.OnRuntimeStop();
	}
}
