#include "Basalt/Particles/ParticleSystem.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Particles/ParticleEmitterRules.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Basalt {

	namespace {

		// A camera-facing square's corner lies within half its diagonal of the center along any world axis.
		constexpr float HalfDiagonal = 0.70710678f;

		// Spreads consecutive ordinals of one seed over unrelated mt19937 seeds (murmur3's finalizer).
		uint32_t MixSeed(uint32_t seed, uint32_t ordinal)
		{
			uint32_t hash = seed ^ (ordinal * 0x9E3779B9u);
			hash ^= hash >> 16;
			hash *= 0x85EBCA6Bu;
			hash ^= hash >> 13;
			hash *= 0xC2B2AE35u;
			hash ^= hash >> 16;
			return hash;
		}

		// The transform's basis with the scale divided out: a rotation, possibly mirrored (so velocities turn
		// like mirrored positions). A zero-scale axis keeps its unit axis.
		glm::mat3 NormalizedBasis(const glm::mat4& transform)
		{
			glm::mat3 basis(1.0f);
			for (int axis = 0; axis < 3; axis++)
			{
				const glm::vec3 column(transform[axis]);
				const float length = glm::length(column);
				if (length > 0.0f)
					basis[axis] = column / length;
			}
			return basis;
		}

		// Maps world-space vectors into a local-space emitter's frame, so that drawing through the emitter's
		// (scaled) transform gives them back. A degenerate emitter (an axis scaled to zero, or axes that
		// coincide) has no such frame: nothing. The test compares the determinant with the axis lengths, so
		// small but valid scales (0.01 under a 0.01 parent) still count. Degenerate: the zero matrix (local
		// velocities and gravity vanish).
		glm::mat3 WorldToEmitter(const glm::mat4& transform)
		{
			const glm::mat3 basis(transform);
			const float lengths = glm::length(basis[0]) * glm::length(basis[1]) * glm::length(basis[2]);
			return lengths > 0.0f && std::abs(glm::determinant(basis)) > 1e-6f * lengths ? glm::inverse(basis) : glm::mat3(0.0f);
		}

		// Uniform direction on the unit sphere. Each random number is drawn in its own statement: argument
		// evaluation order differs between compilers, and the draws must happen in the same order everywhere.
		glm::vec3 RandomDirection(Random& random)
		{
			const float y = random.Range(-1.0f, 1.0f);
			const float angle = random.Range(0.0f, glm::two_pi<float>());
			const float radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
			return { radius * std::cos(angle), y, radius * std::sin(angle) };
		}

		// Local position and direction of a new particle for the emitter's shape.
		void SampleShape(Random& random, const ParticleEmitterComponent& c, glm::vec3& outPosition, glm::vec3& outDirection)
		{
			switch (c.Shape)
			{
				case ParticleEmitterShape::Point:
					outPosition = glm::vec3(0.0f);
					outDirection = RandomDirection(random);
					return;
				case ParticleEmitterShape::Sphere:
				{
					outDirection = RandomDirection(random);
					// The cube root spreads particles evenly through the volume rather than bunching at the center.
					const float distance = c.ShapeRadius * std::cbrt(random.Float());
					outPosition = outDirection * distance;
					return;
				}
				case ParticleEmitterShape::Box:
				{
					const float x = random.Range(-c.BoxExtents.x, c.BoxExtents.x);
					const float y = random.Range(-c.BoxExtents.y, c.BoxExtents.y);
					const float z = random.Range(-c.BoxExtents.z, c.BoxExtents.z);
					outPosition = { x, y, z };
					outDirection = { 0.0f, 1.0f, 0.0f };
					return;
				}
				case ParticleEmitterShape::Cone:
				{
					// Uniform over the spherical cap around +Y (cosines uniform in [cos(angle), 1]); the start
					// position is uniform over the base disc.
					const float cosTheta = random.Range(std::cos(glm::radians(c.ConeAngle)), 1.0f);
					const float directionAngle = random.Range(0.0f, glm::two_pi<float>());
					const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
					outDirection = { sinTheta * std::cos(directionAngle), cosTheta, sinTheta * std::sin(directionAngle) };
					const float discRadius = c.ShapeRadius * std::sqrt(random.Float());
					const float discAngle = random.Range(0.0f, glm::two_pi<float>());
					outPosition = { discRadius * std::cos(discAngle), 0.0f, discRadius * std::sin(discAngle) };
					return;
				}
			}
			BS_CORE_ASSERT(false, "SampleShape needs a sanitized component (Shape in range)");
			outPosition = glm::vec3(0.0f);
			outDirection = { 0.0f, 1.0f, 0.0f };
		}

	}

	ParticleSystem::ParticleSystem(Scene* scene)
		: m_Scene(scene)
	{
		entt::registry& registry = scene->GetRegistry();
		registry.on_construct<ParticleEmitterComponent>().connect<&ParticleSystem::OnConstruct>(*this);
		registry.on_destroy<ParticleEmitterComponent>().connect<&ParticleSystem::OnDestroy>(*this);
	}

	ParticleSystem::~ParticleSystem()
	{
		entt::registry& registry = m_Scene->GetRegistry();
		registry.on_construct<ParticleEmitterComponent>().disconnect<&ParticleSystem::OnConstruct>(*this);
		registry.on_destroy<ParticleEmitterComponent>().disconnect<&ParticleSystem::OnDestroy>(*this);
	}

	void ParticleSystem::Start()
	{
		m_NextOrdinal = 0;
		m_Emitters.clear();
		m_Order.clear();

		// Scene emitters by UUID: stored in the scene file, so the order (and every emitter's seed) is the same
		// in every session, whatever order the registry holds them in.
		std::vector<std::pair<UUID, entt::entity>> emitters;
		auto view = m_Scene->GetAllEntitiesWith<ParticleEmitterComponent, IDComponent>();
		for (entt::entity handle : view)
			emitters.emplace_back(view.get<IDComponent>(handle).ID, handle);
		std::sort(emitters.begin(), emitters.end(), [](const auto& a, const auto& b) { return static_cast<uint64_t>(a.first) < static_cast<uint64_t>(b.first); });
		for (const auto& [id, handle] : emitters)
			Register(id, handle);
	}

	void ParticleSystem::OnConstruct(entt::registry& registry, entt::entity handle)
	{
		if (const auto* id = registry.try_get<IDComponent>(handle))
		{
			Register(id->ID, handle);
			return;
		}
		const auto* tag = registry.try_get<TagComponent>(handle);
		BS_CORE_ERROR("ParticleSystem: a ParticleEmitter was added to entity {} ('{}') without an ID; it will not emit", static_cast<uint32_t>(handle), tag ? tag->Tag : "");
	}

	// Covers RemoveComponent and entity destruction alike (destroying an entity removes its components).
	void ParticleSystem::OnDestroy(entt::registry& registry, entt::entity handle)
	{
		// The IDComponent normally outlives the emitter (EnTT's single-entity destroy removes components in
		// reverse pool-creation order), but the emitter must never outlive its entity: fall back to the handle.
		if (const auto* id = registry.try_get<IDComponent>(handle))
		{
			Unregister(id->ID);
			return;
		}
		for (const auto& [id, emitter] : m_Emitters)
		{
			if (emitter.Handle == handle)
			{
				Unregister(id);
				return;
			}
		}
	}

	void ParticleSystem::Register(UUID id, entt::entity handle)
	{
		// Defensive: a second registration of one UUID (duplicate UUIDs) must not create another emitter or
		// use up an ordinal.
		if (auto existing = m_Emitters.find(id); existing != m_Emitters.end())
		{
			if (existing->second.Handle != handle)
				BS_CORE_ERROR("ParticleSystem: two entities share UUID {}; only the first one's ParticleEmitter emits", static_cast<uint64_t>(id));
			return;
		}
		Emitter& emitter = m_Emitters[id];
		emitter.Handle = handle;
		emitter.Rng.Seed(MixSeed(m_Scene->GetRandomSeed(), m_NextOrdinal++));
		m_Order.push_back(id);
		const ParticleEmitterComponent component = ReadComponent(emitter);
		emitter.Space = component.SimulationSpace;
		if (component.PlayOnStart)
			StartCycle(emitter);
	}

	void ParticleSystem::Unregister(UUID id)
	{
		if (m_Emitters.erase(id) > 0)
			std::erase(m_Order, id);
	}

	ParticleSystem::Emitter* ParticleSystem::Find(Entity entity)
	{
		auto it = m_Emitters.find(entity.GetUUID());
		return it != m_Emitters.end() ? &it->second : nullptr;
	}

	const ParticleSystem::Emitter* ParticleSystem::Find(Entity entity) const
	{
		auto it = m_Emitters.find(entity.GetUUID());
		return it != m_Emitters.end() ? &it->second : nullptr;
	}

	ParticleEmitterComponent ParticleSystem::ReadComponent(const Emitter& emitter) const
	{
		const auto& component = m_Scene->GetRegistry().get<ParticleEmitterComponent>(emitter.Handle);
		ParticleEmitterComponent sanitized = SanitizeParticleEmitter(component);
		if (sanitized == component)
		{
			emitter.ReportedInvalid = false;
			return sanitized;
		}
		// The registry rejects such values, so C++ code wrote them directly; simulate repaired ones, and say so.
		if (!emitter.ReportedInvalid)
		{
			emitter.ReportedInvalid = true;
			std::string problems;
			for (const std::string& problem : FindParticleEmitterProblems(component))
				problems += (problems.empty() ? "" : "; ") + problem;
			BS_CORE_WARN("ParticleSystem: the ParticleEmitter on '{}' has invalid values ({}); simulating corrected ones", Entity(emitter.Handle, m_Scene).GetName(), problems);
		}
		return sanitized;
	}

	void ParticleSystem::ConvertSpace(Emitter& emitter, ParticleSimulationSpace space, const glm::mat4& emitterTransform)
	{
		if (emitter.Space == space)
			return;
		emitter.Space = space;
		if (space == ParticleSimulationSpace::World)
		{
			// Local units through the scaled transform give world positions and m/s.
			const glm::mat3 basis(emitterTransform);
			for (Particle& particle : emitter.Particles)
			{
				particle.Position = glm::vec3(emitterTransform * glm::vec4(particle.Position, 1.0f));
				particle.Velocity = basis * particle.Velocity;
			}
			return;
		}
		const glm::mat3 toEmitter = WorldToEmitter(emitterTransform);
		// A degenerate emitter has no local frame to hold them: its particles cannot be kept.
		if (toEmitter == glm::mat3(0.0f))
		{
			emitter.Particles.clear();
			emitter.FreshCount = 0;
			return;
		}
		const glm::vec3 origin(emitterTransform[3]);
		for (Particle& particle : emitter.Particles)
		{
			particle.Position = toEmitter * (particle.Position - origin);
			particle.Velocity = toEmitter * particle.Velocity;
		}
	}

	void ParticleSystem::StartCycle(Emitter& emitter)
	{
		emitter.State = EmitterState::BurstPending;
		emitter.CycleTime = 0.0f;
		emitter.RateAccumulator = 0.0f;
	}

	bool ParticleSystem::HasEmitter(Entity entity) const
	{
		return Find(entity) != nullptr;
	}

	bool ParticleSystem::Play(Entity entity)
	{
		Emitter* emitter = Find(entity);
		if (!emitter)
			return false;
		if (emitter->State == EmitterState::Stopped)
			StartCycle(*emitter);
		return true;
	}

	bool ParticleSystem::Stop(Entity entity, bool clear)
	{
		Emitter* emitter = Find(entity);
		if (!emitter)
			return false;
		emitter->State = EmitterState::Stopped;
		if (clear)
		{
			emitter->Particles.clear();
			emitter->FreshCount = 0;
		}
		return true;
	}

	uint32_t ParticleSystem::Emit(Entity entity, uint32_t count)
	{
		Emitter* emitter = Find(entity);
		if (!emitter)
			return 0;
		const ParticleEmitterComponent component = ReadComponent(*emitter);
		const glm::mat4 transform = m_Scene->GetWorldTransform(entity);
		// New particles are stored in the component's space: bring the live ones there first.
		ConvertSpace(*emitter, component.SimulationSpace, transform);
		const uint32_t spawned = Spawn(*emitter, component, transform, count);
		emitter->FreshCount += spawned;
		return spawned;
	}

	bool ParticleSystem::IsPlaying(Entity entity) const
	{
		const Emitter* emitter = Find(entity);
		return emitter && emitter->State != EmitterState::Stopped;
	}

	uint32_t ParticleSystem::GetParticleCount(Entity entity) const
	{
		const Emitter* emitter = Find(entity);
		return emitter ? static_cast<uint32_t>(emitter->Particles.size()) : 0;
	}

	uint32_t ParticleSystem::GetTotalParticleCount() const
	{
		size_t total = 0;
		for (const auto& [id, emitter] : m_Emitters)
			total += emitter.Particles.size();
		return static_cast<uint32_t>(total);
	}

	uint32_t ParticleSystem::Spawn(Emitter& emitter, const ParticleEmitterComponent& component, const glm::mat4& emitterTransform, uint32_t count)
	{
		const size_t capacity = component.MaxParticles > emitter.Particles.size() ? component.MaxParticles - emitter.Particles.size() : 0;
		const uint32_t spawned = static_cast<uint32_t>(std::min<size_t>(count, capacity));
		const bool world = component.SimulationSpace == ParticleSimulationSpace::World;
		// The shape scales with the emitter; velocities only turn with it. World particles store world
		// velocities; local ones store them in the emitter's frame, where drawing through the scaled
		// transform must leave their speed unchanged.
		const glm::mat3 rotation = NormalizedBasis(emitterTransform);
		const glm::mat3 velocityToStored = world ? rotation : WorldToEmitter(emitterTransform) * rotation;
		for (uint32_t i = 0; i < spawned; i++)
		{
			glm::vec3 position;
			glm::vec3 direction;
			SampleShape(emitter.Rng, component, position, direction);
			const float speed = emitter.Rng.Range(component.SpeedMin, component.SpeedMax);
			const float lifetime = emitter.Rng.Range(component.LifetimeMin, component.LifetimeMax);

			Particle particle;
			particle.Position = world ? glm::vec3(emitterTransform * glm::vec4(position, 1.0f)) : position;
			particle.Velocity = velocityToStored * (direction * speed + component.Velocity);
			particle.Lifetime = lifetime;
			emitter.Particles.push_back(particle);
		}
		return spawned;
	}

	size_t ParticleSystem::Simulate(std::vector<Particle>& particles, size_t count, const glm::vec3& gravity, float damping, float deltaTime)
	{
		for (size_t i = 0; i < count;)
		{
			Particle& particle = particles[i];
			particle.Age += deltaTime;
			if (particle.Age >= particle.Lifetime)
			{
				// Swap-remove within the range: changes the order, but deterministically; rendering re-sorts by
				// depth (ties keep this order).
				particle = particles[--count];
				continue;
			}
			particle.Velocity = (particle.Velocity + gravity * deltaTime) * damping;
			particle.Position += particle.Velocity * deltaTime;
			i++;
		}
		return count;
	}

	void ParticleSystem::Update(Timestep ts)
	{
		const float deltaTime = ts.GetSeconds();
		// A zero step is a paused frame: nothing to do, nothing wrong.
		if (deltaTime == 0.0f)
			return;
		// Negative or non-finite steps are a host bug: report it, but keep the particles as they are.
		if (!(deltaTime > 0.0f) || !std::isfinite(deltaTime))
		{
			if (!m_ReportedTimestep)
			{
				m_ReportedTimestep = true;
				BS_CORE_ERROR("ParticleSystem: ignoring a time step of {} s (must be positive and finite)", deltaTime);
			}
			return;
		}

		for (UUID id : m_Order)
		{
			auto it = m_Emitters.find(id);
			BS_CORE_ASSERT(it != m_Emitters.end(), "m_Order and m_Emitters hold the same emitters");
			if (it == m_Emitters.end())
				continue;
			Emitter& emitter = it->second;
			const ParticleEmitterComponent component = ReadComponent(emitter);
			const glm::mat4 transform = m_Scene->GetWorldTransform(Entity(emitter.Handle, m_Scene));

			// A smaller MaxParticles (set during play) takes effect at once. The trim removes the newest
			// particles, so the fresh ones (last) go first.
			if (emitter.Particles.size() > component.MaxParticles)
			{
				const size_t removed = emitter.Particles.size() - component.MaxParticles;
				emitter.FreshCount -= std::min(emitter.FreshCount, removed);
				emitter.Particles.resize(component.MaxParticles);
			}
			BS_CORE_ASSERT(emitter.FreshCount <= emitter.Particles.size(), "fresh particles are a suffix of the particles");
			ConvertSpace(emitter, component.SimulationSpace, transform);

			glm::vec3 gravity = m_Scene->GetPhysicsSettings().Gravity * component.GravityScale;
			// Local particles are drawn through the emitter's transform, so world gravity is expressed in its
			// frame (none for a zero-scale emitter, whose particles collapse to a point anyway).
			if (component.SimulationSpace == ParticleSimulationSpace::Local)
				gravity = WorldToEmitter(transform) * gravity;
			// Implicit (backward Euler) linear drag: stable for any step and Drag.
			const float damping = 1.0f / (1.0f + component.Drag * deltaTime);
			// Particles Emit added since the last update sit at the end; this update does not age them, so they
			// are drawn at age 0 like burst and rate particles.
			std::vector<Particle>& particles = emitter.Particles;
			const size_t aged = particles.size() - emitter.FreshCount;
			const size_t alive = Simulate(particles, aged, gravity, damping, deltaTime);
			particles.erase(particles.begin() + static_cast<std::ptrdiff_t>(alive), particles.begin() + static_cast<std::ptrdiff_t>(aged));
			emitter.FreshCount = 0;
			if (emitter.State == EmitterState::Stopped)
				continue;

			if (emitter.State == EmitterState::BurstPending)
			{
				emitter.State = EmitterState::Playing;
				Spawn(emitter, component, transform, component.BurstCount);
			}
			// A one-shot emitter emits only for what is left of its cycle (nothing when Duration was lowered
			// below the time already spent). Particles the emitter has no room for are dropped, not owed: a
			// full emitter does not burst once space frees up.
			const float emitTime = component.Looping ? deltaTime : std::clamp(component.Duration - emitter.CycleTime, 0.0f, deltaTime);
			// Capped before accumulating, so any finite step keeps the accumulator finite and the count in range.
			const float limit = static_cast<float>(ParticleEmitterComponent::MaxParticlesLimit);
			emitter.RateAccumulator += std::min(component.Rate * emitTime, limit);
			const float whole = std::floor(emitter.RateAccumulator);
			BS_CORE_ASSERT(whole >= 0.0f, "particles owed are never negative (converting them would be undefined)");
			emitter.RateAccumulator -= whole;
			Spawn(emitter, component, transform, static_cast<uint32_t>(std::min(whole, limit)));

			emitter.CycleTime += deltaTime;
			if (emitter.CycleTime >= component.Duration)
			{
				if (component.Looping)
				{
					// Cycles a long frame skips do not each burst: wrap into the current cycle and queue one
					// burst (spawned by the next update).
					emitter.CycleTime = std::fmod(emitter.CycleTime, component.Duration);
					emitter.State = EmitterState::BurstPending;
				}
				else
				{
					emitter.State = EmitterState::Stopped;
				}
			}
		}
	}

	void ParticleSystem::BuildRenderData(const glm::vec3& cameraPosition, const glm::vec3& cameraForward, ParticleRenderData& out) const
	{
		out.Clear();
		std::vector<std::pair<float, uint32_t>> order;
		std::vector<glm::vec3> positions;
		for (UUID id : m_Order)
		{
			auto it = m_Emitters.find(id);
			BS_CORE_ASSERT(it != m_Emitters.end(), "m_Order and m_Emitters hold the same emitters");
			if (it == m_Emitters.end())
				continue;
			const Emitter& emitter = it->second;
			if (emitter.Particles.empty())
				continue;
			// Read here too (not cached from Update): edits made while paused render, and are reported, at once.
			const ParticleEmitterComponent component = ReadComponent(emitter);
			// The space the particles are stored in (a SimulationSpace change converts them at the next update).
			const bool local = emitter.Space == ParticleSimulationSpace::Local;
			const glm::mat4 transform = local ? m_Scene->GetWorldTransform(Entity(emitter.Handle, m_Scene)) : glm::mat4(1.0f);

			ParticleBatch batch;
			batch.FirstParticle = static_cast<uint32_t>(out.Particles.size());
			batch.Count = static_cast<uint32_t>(emitter.Particles.size());
			batch.Blend = component.Blend;
			batch.Texture = component.Texture;

			// Back to front, so alpha blending composites correctly within the emitter. Stable, so equal depths
			// keep simulation order whatever the standard library's sort does.
			order.clear();
			positions.clear();
			for (uint32_t i = 0; i < batch.Count; i++)
			{
				const glm::vec3& position = emitter.Particles[i].Position;
				positions.push_back(local ? glm::vec3(transform * glm::vec4(position, 1.0f)) : position);
				order.emplace_back(glm::dot(positions.back() - cameraPosition, cameraForward), i);
			}
			std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

			float largestSize = 0.0f;
			for (const auto& [depth, index] : order)
			{
				const Particle& particle = emitter.Particles[index];
				const float t = std::clamp(particle.Age / particle.Lifetime, 0.0f, 1.0f);
				const glm::vec3& position = positions[index];
				const float size = glm::mix(component.StartSize, component.EndSize, t);
				largestSize = std::max(largestSize, size);
				out.Particles.push_back({ glm::vec4(position, size), glm::mix(component.StartColor, component.EndColor, t) });
				batch.Bounds.Expand(position);
			}
			batch.Bounds.Min -= glm::vec3(largestSize * HalfDiagonal);
			batch.Bounds.Max += glm::vec3(largestSize * HalfDiagonal);
			batch.ViewDepth = glm::dot(batch.Bounds.GetCenter() - cameraPosition, cameraForward);
			out.Batches.push_back(std::move(batch));
		}
	}

}
