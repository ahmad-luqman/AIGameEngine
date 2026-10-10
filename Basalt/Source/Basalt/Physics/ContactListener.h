#pragma once

// Internal to the physics module.

#include "Basalt/Physics/ColliderShapes.h"
#include "Basalt/Physics/JoltUtils.h"
#include "Basalt/Physics/PhysicsMaterial.h"
#include "Basalt/Physics/PhysicsWorld.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>

#include <cstdint>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

namespace Basalt::PhysicsInternal {

	struct RawContactEvent
	{
		bool Added = false;
		uint32_t Body1 = 0;
		uint32_t SubShape1 = 0;
		uint32_t Body2 = 0;
		uint32_t SubShape2 = 0;
		// Added events only (Impulse stays 0 for sensors); Normal points from body 1 toward body 2.
		ContactInfo Contact;

		auto Tie() const { return std::tie(Added, Body1, SubShape1, Body2, SubShape2); }
	};

	struct BodyMaterial
	{
		PhysicsCombineMode FrictionCombine = PhysicsCombineMode::Default;
		PhysicsCombineMode RestitutionCombine = PhysicsCombineMode::Default;
		// Colliders with their own friction and restitution; AnyOverride skips the lookup for the usual body
		// without any.
		ColliderMaterials Colliders{};
		bool AnyOverride = false;
	};

	// Called from Jolt worker threads: only records events, which the main thread dispatches, and
	// combines the bodies' materials.
	class ContactListenerImpl final : public JPH::ContactListener
	{
	public:
		void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override
		{
			CombineMaterials(body1, body2, manifold, settings);

			RawContactEvent event{ true, body1.GetID().GetIndexAndSequenceNumber(), manifold.mSubShapeID1.GetValue(), body2.GetID().GetIndexAndSequenceNumber(), manifold.mSubShapeID2.GetValue(), {} };
			if (!manifold.mRelativeContactPointsOn1.empty())
			{
				// Midway between the two surfaces, averaged over the manifold's points.
				JPH::Vec3 sum = JPH::Vec3::sZero();
				for (JPH::uint i = 0; i < manifold.mRelativeContactPointsOn1.size(); i++)
					sum += manifold.mRelativeContactPointsOn1[i] + manifold.mRelativeContactPointsOn2[i];
				const JPH::RVec3 point = manifold.mBaseOffset + sum / (2.0f * static_cast<float>(manifold.mRelativeContactPointsOn1.size()));
				event.Contact.Point = FromJolt(JPH::Vec3(point));
				event.Contact.Normal = FromJolt(manifold.mWorldSpaceNormal);

				// The solver has not run yet; Jolt's estimate uses the velocities before the impact. A sensor
				// pushes nothing.
				if (!body1.IsSensor() && !body2.IsSensor())
				{
					JPH::CollisionEstimationResult estimate;
					JPH::EstimateCollisionResponse(body1, body2, manifold, estimate, settings.mCombinedFriction, settings.mCombinedRestitution, MinVelocityForRestitution);
					for (const float impulse : estimate.mContactImpulse)
						event.Contact.Impulse += impulse;
				}
			}

			std::scoped_lock lock(Mutex);
			Events.push_back(event);
		}

		void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override
		{
			// Jolt recomputes the combined values for every contact each step.
			CombineMaterials(body1, body2, manifold, settings);
		}

		void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
		{
			std::scoped_lock lock(Mutex);
			Events.push_back({ false, pair.GetBody1ID().GetIndexAndSequenceNumber(), pair.GetSubShapeID1().GetValue(), pair.GetBody2ID().GetIndexAndSequenceNumber(), pair.GetSubShapeID2().GetValue(), {} });
		}

		std::mutex Mutex;
		std::vector<RawContactEvent> Events;
		// Indexed by BodyID::GetIndex(). Only written between steps, so workers read it without locking.
		std::vector<BodyMaterial> Materials;
		float MinVelocityForRestitution = 1.0f;

	private:
		// The friction and restitution of the part of the body that touches: its collider's own, else the body's.
		static std::pair<float, float> SurfaceOf(const BodyMaterial& material, const JPH::Body& body, const JPH::SubShapeID& subShape)
		{
			if (material.AnyOverride)
			{
				uint32_t index = 0;
				if (body.GetShape()->GetType() == JPH::EShapeType::Compound)
				{
					JPH::SubShapeID remainder;
					index = static_cast<const JPH::CompoundShape*>(body.GetShape())->GetSubShapeIndexFromID(subShape, remainder);
				}
				if (index < material.Colliders.size() && material.Colliders[index].Override)
					return { material.Colliders[index].Friction, material.Colliders[index].Restitution };
			}
			return { body.GetFriction(), body.GetRestitution() };
		}

		void CombineMaterials(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) const
		{
			const BodyMaterial& material1 = Materials[body1.GetID().GetIndex()];
			const BodyMaterial& material2 = Materials[body2.GetID().GetIndex()];
			const auto [friction1, restitution1] = SurfaceOf(material1, body1, manifold.mSubShapeID1);
			const auto [friction2, restitution2] = SurfaceOf(material2, body2, manifold.mSubShapeID2);
			settings.mCombinedFriction = CombineFriction(material1.FrictionCombine, friction1, material2.FrictionCombine, friction2);
			settings.mCombinedRestitution = CombineRestitution(material1.RestitutionCombine, restitution1, material2.RestitutionCombine, restitution2);
		}
	};

}
