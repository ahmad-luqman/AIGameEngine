#pragma once

#include "Basalt/Core/Timestep.h"
#include "Basalt/Core/UUID.h"
#include "Basalt/Math/AABB.h"
#include "Basalt/Math/Random.h"
#include "Basalt/Scene/Components.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace Basalt {

	class Entity;
	class Scene;

	// One billboard as the renderer uploads it (matches ParticleData in Particle.vert).
	struct ParticleVertex
	{
		glm::vec4 PositionSize; // xyz: world position, w: billboard size (m)
		glm::vec4 Color;        // linear HDR color, a: opacity
	};
	static_assert(sizeof(ParticleVertex) == 32);

	// The live particles of one emitter, sorted back to front, as a range of ParticleRenderData::Particles.
	struct ParticleBatch
	{
		uint32_t FirstParticle = 0;
		uint32_t Count = 0;
		// View depth of the particles' bounds center: sorts the batch among transparent meshes.
		float ViewDepth = 0.0f;
		// World bounds of the billboards (centers grown by the largest half-size), for frustum culling.
		AABB Bounds;
		ParticleBlendMode Blend = ParticleBlendMode::Alpha;
		std::string Texture;
	};

	struct ParticleRenderData
	{
		std::vector<ParticleVertex> Particles;
		std::vector<ParticleBatch> Batches;

		void Clear()
		{
			Particles.clear();
			Batches.clear();
		}
	};

	// Simulates the ParticleEmitterComponents of a running scene on the CPU. Exists while the scene plays or
	// simulates; particles are runtime state, keyed by entity UUID, and never written back to the scene.
	//
	// Determinism: each emitter draws from its own Random, seeded from the project's RandomSeed and the
	// emitter's registration order (scene emitters by UUID at Start, then emitters added during play in the
	// order they appear), never from a runtime spawn's random UUID. A play session with the same inputs and
	// time steps therefore produces the same particles, and emitting never shifts Lua's random sequence.
	class ParticleSystem
	{
	public:
		explicit ParticleSystem(Scene* scene);
		~ParticleSystem();

		ParticleSystem(const ParticleSystem&) = delete;
		ParticleSystem& operator=(const ParticleSystem&) = delete;

		// Registers the scene's emitters and starts the ones with PlayOnStart.
		void Start();
		// Ages and moves particles, then emits (rate and bursts) from playing emitters.
		void Update(Timestep ts);
		void OnEntityDestroyed(Entity entity);

		// Starts a new emission cycle (with its burst) unless the emitter is already playing. Returns false
		// when the entity has no ParticleEmitterComponent.
		bool Play(Entity entity);
		// Stops emitting; live particles finish their lives unless `clear` removes them now.
		void Stop(Entity entity, bool clear = false);
		// Emits up to `count` particles right away, playing or not. Returns how many fit under MaxParticles.
		uint32_t Emit(Entity entity, uint32_t count);
		bool IsPlaying(Entity entity) const;
		uint32_t GetParticleCount(Entity entity) const;
		uint32_t GetTotalParticleCount() const;

		// Fills `out` with every live particle's billboard (color and size interpolated over its life), one
		// batch per emitter with particles, sorted back to front within the batch. Batches follow emitter
		// registration order; the renderer sorts them by ViewDepth.
		void BuildRenderData(const glm::vec3& cameraPosition, const glm::vec3& cameraForward, ParticleRenderData& out) const;

	private:
		struct Particle
		{
			glm::vec3 Position; // world space, or emitter-local for SimulationSpace::Local
			float Age = 0.0f;
			glm::vec3 Velocity;
			float Lifetime = 1.0f;
		};

		struct Emitter
		{
			entt::entity Handle = entt::null;
			Random Rng;
			std::vector<Particle> Particles;
			// Seconds into the current emission cycle.
			float CycleTime = 0.0f;
			// Fractional particles owed by the rate, carried between frames.
			float RateAccumulator = 0.0f;
			bool Playing = false;
			bool BurstPending = false;
		};

		void OnConstruct(entt::registry& registry, entt::entity handle);
		void OnDestroy(entt::registry& registry, entt::entity handle);
		void Register(UUID id, entt::entity handle);
		void Unregister(UUID id);
		Emitter* Find(Entity entity);
		const Emitter* Find(Entity entity) const;
		void StartCycle(Emitter& emitter) const;
		uint32_t Spawn(Emitter& emitter, const ParticleEmitterComponent& component, const glm::mat4& emitterTransform, uint32_t count) const;
		void Simulate(Emitter& emitter, const ParticleEmitterComponent& component, const glm::mat4& emitterTransform, float deltaTime) const;

	private:
		Scene* m_Scene = nullptr;
		uint32_t m_Seed = Random::DefaultSeed;
		uint32_t m_NextOrdinal = 0;
		std::unordered_map<UUID, Emitter> m_Emitters;
		// Registration order: updates and render batches follow it, so they never depend on hash order.
		std::vector<UUID> m_Order;
	};

}
