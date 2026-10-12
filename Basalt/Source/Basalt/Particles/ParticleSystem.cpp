#include "Basalt/Particles/ParticleSystem.h"

#include "Basalt/Project/Project.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace Basalt {

	namespace {

		// Spreads consecutive ordinals of one project seed over unrelated mt19937 seeds (murmur3's finalizer).
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

		float FiniteOr(float value, float fallback)
		{
			return std::isfinite(value) ? value : fallback;
		}

		// The registry rejects values the simulation cannot use, but C++ code can write the component
		// directly; this keeps such values from producing NaNs, endless loops or unbounded allocations.
		ParticleEmitterComponent Sanitize(const ParticleEmitterComponent& c)
		{
			ParticleEmitterComponent s = c;
			s.Duration = std::max(FiniteOr(c.Duration, 1.0f), 1e-3f);
			s.MaxParticles = std::clamp<uint32_t>(c.MaxParticles, 1, ParticleEmitterComponent::MaxParticlesLimit);
			s.BurstCount = std::min(c.BurstCount, ParticleEmitterComponent::MaxParticlesLimit);
			s.Rate = std::max(FiniteOr(c.Rate, 0.0f), 0.0f);
			s.ShapeRadius = std::max(FiniteOr(c.ShapeRadius, 0.0f), 0.0f);
			s.BoxExtents = glm::max(glm::vec3(FiniteOr(c.BoxExtents.x, 0.0f), FiniteOr(c.BoxExtents.y, 0.0f), FiniteOr(c.BoxExtents.z, 0.0f)), glm::vec3(0.0f));
			s.ConeAngle = std::clamp(FiniteOr(c.ConeAngle, 0.0f), 0.0f, 180.0f);
			s.LifetimeMin = std::max(FiniteOr(c.LifetimeMin, 1.0f), 1e-3f);
			s.LifetimeMax = std::max(FiniteOr(c.LifetimeMax, s.LifetimeMin), s.LifetimeMin);
			s.SpeedMin = FiniteOr(c.SpeedMin, 0.0f);
			s.SpeedMax = std::max(FiniteOr(c.SpeedMax, s.SpeedMin), s.SpeedMin);
			s.Velocity = glm::vec3(FiniteOr(c.Velocity.x, 0.0f), FiniteOr(c.Velocity.y, 0.0f), FiniteOr(c.Velocity.z, 0.0f));
			s.GravityScale = FiniteOr(c.GravityScale, 0.0f);
			s.Drag = std::max(FiniteOr(c.Drag, 0.0f), 0.0f);
			s.StartSize = std::max(FiniteOr(c.StartSize, 0.0f), 0.0f);
			s.EndSize = std::max(FiniteOr(c.EndSize, 0.0f), 0.0f);
			return s;
		}

		// The transform's rotation: its basis with the scale divided out (a zero-scale axis keeps its unit axis).
		glm::mat3 RotationOf(const glm::mat4& transform)
		{
			glm::mat3 rotation(1.0f);
			for (int axis = 0; axis < 3; axis++)
			{
				const glm::vec3 column(transform[axis]);
				const float length = glm::length(column);
				if (length > 0.0f)
					rotation[axis] = column / length;
			}
			return rotation;
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
		m_Seed = Project::GetActiveRandomSeed();
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
			Register(id->ID, handle);
	}

	void ParticleSystem::OnDestroy(entt::registry& registry, entt::entity handle)
	{
		// While an entity is destroyed its IDComponent may already be gone; find the emitter by handle then.
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
		if (m_Emitters.contains(id))
			return;
		Emitter& emitter = m_Emitters[id];
		emitter.Handle = handle;
		emitter.Rng.Seed(MixSeed(m_Seed, m_NextOrdinal++));
		m_Order.push_back(id);
		const auto& component = m_Scene->GetRegistry().get<ParticleEmitterComponent>(handle);
		if (component.PlayOnStart)
			StartCycle(emitter);
	}

	void ParticleSystem::Unregister(UUID id)
	{
		if (m_Emitters.erase(id) > 0)
			std::erase(m_Order, id);
	}

	void ParticleSystem::OnEntityDestroyed(Entity entity)
	{
		Unregister(entity.GetUUID());
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

	void ParticleSystem::StartCycle(Emitter& emitter) const
	{
		emitter.Playing = true;
		emitter.BurstPending = true;
		emitter.CycleTime = 0.0f;
		emitter.RateAccumulator = 0.0f;
	}

	bool ParticleSystem::Play(Entity entity)
	{
		Emitter* emitter = Find(entity);
		if (!emitter)
			return false;
		if (!emitter->Playing)
			StartCycle(*emitter);
		return true;
	}

	void ParticleSystem::Stop(Entity entity, bool clear)
	{
		Emitter* emitter = Find(entity);
		if (!emitter)
			return;
		emitter->Playing = false;
		emitter->BurstPending = false;
		if (clear)
			emitter->Particles.clear();
	}

	uint32_t ParticleSystem::Emit(Entity entity, uint32_t count)
	{
		Emitter* emitter = Find(entity);
		if (!emitter)
			return 0;
		const ParticleEmitterComponent component = Sanitize(entity.GetComponent<ParticleEmitterComponent>());
		return Spawn(*emitter, component, m_Scene->GetWorldTransform(entity), count);
	}

	bool ParticleSystem::IsPlaying(Entity entity) const
	{
		const Emitter* emitter = Find(entity);
		return emitter && emitter->Playing;
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

	uint32_t ParticleSystem::Spawn(Emitter& emitter, const ParticleEmitterComponent& component, const glm::mat4& emitterTransform, uint32_t count) const
	{
		const size_t capacity = component.MaxParticles > emitter.Particles.size() ? component.MaxParticles - emitter.Particles.size() : 0;
		const uint32_t spawned = static_cast<uint32_t>(std::min<size_t>(count, capacity));
		const bool world = component.SimulationSpace == ParticleSimulationSpace::World;
		// The shape scales with the emitter; velocities only turn with it.
		const glm::mat3 rotation = RotationOf(emitterTransform);
		for (uint32_t i = 0; i < spawned; i++)
		{
			glm::vec3 position;
			glm::vec3 direction;
			SampleShape(emitter.Rng, component, position, direction);
			const float speed = emitter.Rng.Range(component.SpeedMin, component.SpeedMax);
			const float lifetime = emitter.Rng.Range(component.LifetimeMin, component.LifetimeMax);

			Particle particle;
			particle.Position = position;
			particle.Velocity = direction * speed + component.Velocity;
			particle.Lifetime = lifetime;
			if (world)
			{
				particle.Position = glm::vec3(emitterTransform * glm::vec4(position, 1.0f));
				particle.Velocity = rotation * particle.Velocity;
			}
			emitter.Particles.push_back(particle);
		}
		return spawned;
	}

	void ParticleSystem::Simulate(Emitter& emitter, const ParticleEmitterComponent& component, const glm::mat4& emitterTransform, float deltaTime) const
	{
		glm::vec3 gravity = m_Scene->GetPhysicsSettings().Gravity * component.GravityScale;
		if (component.SimulationSpace == ParticleSimulationSpace::Local)
		{
			// Local particles are drawn through the emitter's transform, so world gravity is expressed in its frame.
			const glm::mat3 basis(emitterTransform);
			gravity = std::abs(glm::determinant(basis)) > 1e-12f ? glm::inverse(basis) * gravity : glm::vec3(0.0f);
		}
		// Implicit linear drag: stable for any step, and needs no exp() (which differs between C libraries).
		const float damping = 1.0f / (1.0f + component.Drag * deltaTime);

		std::vector<Particle>& particles = emitter.Particles;
		for (size_t i = 0; i < particles.size();)
		{
			Particle& particle = particles[i];
			particle.Age += deltaTime;
			if (particle.Age >= particle.Lifetime)
			{
				// Swap-remove: order within an emitter does not matter (rendering sorts by depth).
				particle = particles.back();
				particles.pop_back();
				continue;
			}
			particle.Velocity = (particle.Velocity + gravity * deltaTime) * damping;
			particle.Position += particle.Velocity * deltaTime;
			i++;
		}
	}

	void ParticleSystem::Update(Timestep ts)
	{
		const float deltaTime = ts.GetSeconds();
		if (!(deltaTime > 0.0f) || !std::isfinite(deltaTime))
			return;

		for (UUID id : m_Order)
		{
			Emitter& emitter = m_Emitters.at(id);
			Entity entity(emitter.Handle, m_Scene);
			const ParticleEmitterComponent component = Sanitize(entity.GetComponent<ParticleEmitterComponent>());
			const glm::mat4 transform = m_Scene->GetWorldTransform(entity);

			// A smaller MaxParticles (set during play) takes effect at once.
			if (emitter.Particles.size() > component.MaxParticles)
				emitter.Particles.resize(component.MaxParticles);

			Simulate(emitter, component, transform, deltaTime);
			if (!emitter.Playing)
				continue;

			if (emitter.BurstPending)
			{
				emitter.BurstPending = false;
				Spawn(emitter, component, transform, component.BurstCount);
			}
			// Particles the emitter has no room for are dropped, not owed: a full emitter does not burst
			// once space frees up.
			emitter.RateAccumulator += component.Rate * deltaTime;
			const float whole = std::floor(emitter.RateAccumulator);
			emitter.RateAccumulator -= whole;
			Spawn(emitter, component, transform, static_cast<uint32_t>(std::min(whole, static_cast<float>(ParticleEmitterComponent::MaxParticlesLimit))));

			emitter.CycleTime += deltaTime;
			if (emitter.CycleTime >= component.Duration)
			{
				if (component.Looping)
				{
					// One burst per frame even when a long frame spans several cycles.
					emitter.CycleTime = std::fmod(emitter.CycleTime, component.Duration);
					emitter.BurstPending = true;
				}
				else
				{
					emitter.Playing = false;
				}
			}
		}
	}

	void ParticleSystem::BuildRenderData(const glm::vec3& cameraPosition, const glm::vec3& cameraForward, ParticleRenderData& out) const
	{
		out.Clear();
		for (UUID id : m_Order)
		{
			const Emitter& emitter = m_Emitters.at(id);
			if (emitter.Particles.empty())
				continue;
			Entity entity(emitter.Handle, m_Scene);
			const ParticleEmitterComponent component = Sanitize(entity.GetComponent<ParticleEmitterComponent>());
			const bool local = component.SimulationSpace == ParticleSimulationSpace::Local;
			const glm::mat4 transform = local ? m_Scene->GetWorldTransform(entity) : glm::mat4(1.0f);

			ParticleBatch batch;
			batch.FirstParticle = static_cast<uint32_t>(out.Particles.size());
			batch.Count = static_cast<uint32_t>(emitter.Particles.size());
			batch.Blend = component.Blend;
			batch.Texture = component.Texture;
			float largestSize = 0.0f;
			for (const Particle& particle : emitter.Particles)
			{
				const float t = std::clamp(particle.Age / particle.Lifetime, 0.0f, 1.0f);
				const glm::vec3 position = local ? glm::vec3(transform * glm::vec4(particle.Position, 1.0f)) : particle.Position;
				const float size = glm::mix(component.StartSize, component.EndSize, t);
				largestSize = std::max(largestSize, size);
				out.Particles.push_back({ glm::vec4(position, size), glm::mix(component.StartColor, component.EndColor, t) });
				batch.Bounds.Expand(position);
			}
			batch.Bounds.Min -= glm::vec3(largestSize * 0.5f);
			batch.Bounds.Max += glm::vec3(largestSize * 0.5f);
			batch.ViewDepth = glm::dot(batch.Bounds.GetCenter() - cameraPosition, cameraForward);

			// Back to front, so alpha blending composites correctly within the emitter. Stable: equal depths
			// keep simulation order, so a frame always renders the same image.
			auto first = out.Particles.begin() + batch.FirstParticle;
			std::stable_sort(first, out.Particles.end(), [&](const ParticleVertex& a, const ParticleVertex& b) {
				return glm::dot(glm::vec3(a.PositionSize) - cameraPosition, cameraForward) > glm::dot(glm::vec3(b.PositionSize) - cameraPosition, cameraForward);
			});
			out.Batches.push_back(std::move(batch));
		}
	}

}
