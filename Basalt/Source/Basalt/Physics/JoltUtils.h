#pragma once

// Internal to the physics module: conversions and object layers shared by its source files.

#include "Basalt/Physics/PhysicsLayers.h"

// Jolt.h must precede every other Jolt header.
#include <Jolt/Jolt.h>

#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cmath>
#include <cstdint>

namespace Basalt::PhysicsInternal {

	// Jolt's global state (allocator, factory, type registry) is shared by every PhysicsWorld; the first
	// user sets it up and the last one tears it down.
	void AcquireJolt();
	void ReleaseJolt();

	// ---------------------------------------------------------------------------------------
	// Object layers: bit 8 = moving, bits 0-7 = physics layer index (see PhysicsLayers).
	// ---------------------------------------------------------------------------------------
	constexpr uint32_t MovingBit = 1u << 8;

	inline JPH::ObjectLayer MakeObjectLayer(bool moving, uint32_t layer)
	{
		return static_cast<JPH::ObjectLayer>((moving ? MovingBit : 0u) | (layer & 0xFFu));
	}

	inline bool IsMovingLayer(JPH::ObjectLayer layer)
	{
		return (layer & MovingBit) != 0;
	}
	inline uint32_t LayerIndex(JPH::ObjectLayer layer)
	{
		return layer & 0xFFu;
	}

	namespace BroadPhaseLayers {
		inline constexpr JPH::BroadPhaseLayer NonMoving(0);
		inline constexpr JPH::BroadPhaseLayer Moving(1);
		constexpr uint32_t Count = 2;
	}

	class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
	{
	public:
		JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::Count; }
		JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
		{
			return IsMovingLayer(layer) ? BroadPhaseLayers::Moving : BroadPhaseLayers::NonMoving;
		}
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
		const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
		{
			return layer == BroadPhaseLayers::Moving ? "Moving" : "NonMoving";
		}
#endif
	};

	class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
	{
	public:
		bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhaseLayer) const override
		{
			// Static bodies never need to test against other static bodies.
			return IsMovingLayer(layer) || broadPhaseLayer == BroadPhaseLayers::Moving;
		}
	};

	// Applies the project's collision matrix. Read by Jolt worker threads, so the masks are only set
	// before the first step.
	class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
	{
	public:
		void SetLayers(const PhysicsLayers& layers)
		{
			for (uint32_t i = 0; i < PhysicsLayers::MaxLayers; i++)
				m_Masks[i] = layers.GetCollisionMask(i);
		}

		bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
		{
			if (!IsMovingLayer(a) && !IsMovingLayer(b))
				return false;
			const uint32_t indexA = LayerIndex(a);
			const uint32_t indexB = LayerIndex(b);
			// PhysicsLayers keeps the matrix symmetric, so one direction decides.
			return indexA < PhysicsLayers::MaxLayers && indexB < PhysicsLayers::MaxLayers && (m_Masks[indexA] & (1u << indexB)) != 0;
		}

	private:
		std::array<uint32_t, PhysicsLayers::MaxLayers> m_Masks{};
	};

	inline JPH::Vec3 ToJolt(const glm::vec3& v)
	{
		return JPH::Vec3(v.x, v.y, v.z);
	}
	inline JPH::Quat ToJolt(const glm::quat& q)
	{
		return JPH::Quat(q.x, q.y, q.z, q.w);
	}
	inline glm::vec3 FromJolt(const JPH::Vec3& v)
	{
		return { v.GetX(), v.GetY(), v.GetZ() };
	}
	inline glm::quat FromJolt(const JPH::Quat& q)
	{
		return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
	}

	inline bool IsFiniteVec(const glm::vec3& v)
	{
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
	}

}
