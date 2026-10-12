#pragma once

#include "Basalt/Core/Timestep.h"
#include "Basalt/Core/UUID.h"
#include "Basalt/Math/AABB.h"
#include "Basalt/Math/Random.h"
#include "Basalt/Scene/Components.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace Basalt {

	class Entity;
	class Scene;

	// One billboard in the renderer's particle storage buffer. The Particles module produces this GPU layout
	// itself (BuildRenderData writes it directly), so it must match ParticleData in Particle.vert.
	struct GpuParticle
	{
		glm::vec4 PositionSize; // xyz: world position, w: billboard size (m)
		glm::vec4 Color;        // linear HDR color, a: opacity
	};
	static_assert(sizeof(GpuParticle) == 32 && offsetof(GpuParticle, PositionSize) == 0 && offsetof(GpuParticle, Color) == 16, "GpuParticle must match the std430 ParticleData struct");

	// The live particles of one emitter, sorted back to front, as a range of ParticleRenderData::Particles.
	struct ParticleBatch
	{
		uint32_t FirstParticle = 0;
		uint32_t Count = 0;
		// View depth of the particles' bounds center: sorts the batch among transparent meshes.
		float ViewDepth = 0.0f;
		// World bounds of every billboard whatever the camera's orientation (centers grown by the largest
		// half-diagonal), for frustum culling.
		AABB Bounds;
		ParticleBlendMode Blend = ParticleBlendMode::Alpha;
		std::string Texture;
	};

	// One frame's particles for the renderer: every emitter's billboards in one array, a batch per emitter.
	// Sorted for the camera given to BuildRenderData; rebuild it for another view.
	struct ParticleRenderData
	{
		std::vector<GpuParticle> Particles;
		std::vector<ParticleBatch> Batches;

		void Clear()
		{
			Particles.clear();
			Batches.clear();
		}

		// Keeps at most `maxParticles` particles, dropping whole batches from the end (a partial batch would
		// change an emitter's look more than a missing one). Returns whether anything was dropped.
		bool LimitTo(size_t maxParticles)
		{
			if (Particles.size() <= maxParticles)
				return false;
			while (!Batches.empty() && Batches.back().FirstParticle + Batches.back().Count > maxParticles)
				Batches.pop_back();
			Particles.resize(Batches.empty() ? 0 : Batches.back().FirstParticle + Batches.back().Count);
			return true;
		}
	};

	// Simulates the ParticleEmitterComponents of a running scene on the CPU. Exists while the scene plays or
	// simulates; particles are runtime state, keyed by entity UUID, and never written back to the scene.
	//
	// Determinism: each emitter draws from its own Random, seeded from the session's seed
	// (Scene::GetRandomSeed, the project's RandomSeed) and the emitter's registration order: scene emitters
	// by UUID at Start, then emitters added during play in the order they appear, never by a runtime spawn's
	// random UUID. A session with the same inputs and time steps therefore produces the same particles on
	// the same platform (shape sampling uses sin/cos/cbrt, which differ between C libraries), and emitting
	// never shifts Lua's random sequence.
	class ParticleSystem
	{
	public:
		explicit ParticleSystem(Scene* scene);
		~ParticleSystem();

		ParticleSystem(const ParticleSystem&) = delete;
		ParticleSystem& operator=(const ParticleSystem&) = delete;

		// Registers the scene's emitters and starts the ones with PlayOnStart. Emitters added later register
		// themselves (EnTT signals), as removed or destroyed ones unregister.
		void Start();
		// Ages and moves particles, then emits (bursts and rate) from playing emitters.
		void Update(Timestep ts);

		// Starts a new emission cycle (with its burst) unless the emitter is already playing. Returns false
		// when the entity has no registered emitter.
		bool Play(Entity entity);
		// Stops emitting; live particles finish their lives unless `clear` removes them now.
		void Stop(Entity entity, bool clear = false);
		// Emits up to `count` particles right away, playing or not; they are first drawn at age 0, like
		// burst and rate particles. Returns how many fit under MaxParticles.
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
			glm::vec3 Position{ 0.0f }; // world space, or emitter-local for SimulationSpace::Local
			float Age = 0.0f;
			glm::vec3 Velocity{ 0.0f }; // m/s; for Local, in the emitter's frame with its scale divided out
			float Lifetime = 1.0f;
		};

		enum class EmitterState
		{
			Stopped,
			Playing,
			// Playing, and the cycle's burst fires at the next update.
			BurstPending
		};

		struct Emitter
		{
			entt::entity Handle = entt::null;
			Random Rng;
			std::vector<Particle> Particles;
			EmitterState State = EmitterState::Stopped;
			// Seconds into the current emission cycle.
			float CycleTime = 0.0f;
			// Fractional particles owed by the rate, carried between frames.
			float RateAccumulator = 0.0f;
			// The last particles, emitted by Emit since the last update: that update does not age them.
			size_t FreshCount = 0;
			// The component broke its rules (written from C++) and was reported once.
			bool ReportedInvalid = false;
		};

		void OnConstruct(entt::registry& registry, entt::entity handle);
		void OnDestroy(entt::registry& registry, entt::entity handle);
		void Register(UUID id, entt::entity handle);
		void Unregister(UUID id);
		Emitter* Find(Entity entity);
		const Emitter* Find(Entity entity) const;
		// The emitter's component with its values in range (reported once when they were not).
		ParticleEmitterComponent ReadComponent(Emitter& emitter) const;

		static void StartCycle(Emitter& emitter);
		static uint32_t Spawn(Emitter& emitter, const ParticleEmitterComponent& component, const glm::mat4& emitterTransform, uint32_t count);
		static void Simulate(std::vector<Particle>& particles, const glm::vec3& gravity, float damping, float deltaTime);

	private:
		Scene* m_Scene = nullptr;
		uint32_t m_Seed = Random::DefaultSeed;
		// Never reused, so removing an emitter does not change the seeds of emitters registered later.
		uint32_t m_NextOrdinal = 0;
		std::unordered_map<UUID, Emitter> m_Emitters;
		// Registration order: updates and render batches follow it, so they never depend on hash order.
		std::vector<UUID> m_Order;
		bool m_ReportedTimestep = false;
	};

}
