#pragma once

// Internal to the physics module.

#include "Basalt/Physics/JoltUtils.h"
#include "Basalt/Physics/PhysicsMaterial.h"
#include "Basalt/Physics/PhysicsWorld.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>

#include <cstdint>
#include <mutex>
#include <tuple>
#include <vector>

namespace Basalt::PhysicsInternal {

	struct RawContactEvent
	{
		bool Added = false;
		uint32_t Body1 = 0;
		uint32_t SubShape1 = 0;
		uint32_t Body2 = 0;
		uint32_t SubShape2 = 0;
		// Added events only; Normal points from body 1 toward body 2.
		ContactInfo Contact;

		auto Tie() const { return std::tie(Added, Body1, SubShape1, Body2, SubShape2); }
	};

	struct BodyMaterial
	{
		PhysicsCombineMode FrictionCombine = PhysicsCombineMode::Default;
		PhysicsCombineMode RestitutionCombine = PhysicsCombineMode::Default;
	};

	// Called from Jolt worker threads: only records events, which the main thread dispatches, and
	// combines the bodies' materials.
	class ContactListenerImpl final : public JPH::ContactListener
	{
	public:
		void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override
		{
			CombineMaterials(body1, body2, settings);

			RawContactEvent event{ true, body1.GetID().GetIndexAndSequenceNumber(), manifold.mSubShapeID1.GetValue(), body2.GetID().GetIndexAndSequenceNumber(), manifold.mSubShapeID2.GetValue(), {} };
			if (!body1.IsSensor() && !body2.IsSensor() && !manifold.mRelativeContactPointsOn1.empty())
			{
				// Midway between the two surfaces, averaged over the manifold's points.
				JPH::Vec3 sum = JPH::Vec3::sZero();
				for (JPH::uint i = 0; i < manifold.mRelativeContactPointsOn1.size(); i++)
					sum += manifold.mRelativeContactPointsOn1[i] + manifold.mRelativeContactPointsOn2[i];
				const JPH::RVec3 point = manifold.mBaseOffset + sum / (2.0f * static_cast<float>(manifold.mRelativeContactPointsOn1.size()));
				event.Contact.Point = FromJolt(JPH::Vec3(point));
				event.Contact.Normal = FromJolt(manifold.mWorldSpaceNormal);

				// The solver has not run yet; Jolt's estimate uses the velocities before the impact.
				JPH::CollisionEstimationResult estimate;
				JPH::EstimateCollisionResponse(body1, body2, manifold, estimate, settings.mCombinedFriction, settings.mCombinedRestitution, MinVelocityForRestitution);
				for (const float impulse : estimate.mContactImpulse)
					event.Contact.Impulse += impulse;
			}

			std::scoped_lock lock(Mutex);
			Events.push_back(event);
		}

		void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold&, JPH::ContactSettings& settings) override
		{
			// Jolt recomputes the combined values for every contact each step.
			CombineMaterials(body1, body2, settings);
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
		void CombineMaterials(const JPH::Body& body1, const JPH::Body& body2, JPH::ContactSettings& settings) const
		{
			const BodyMaterial& material1 = Materials[body1.GetID().GetIndex()];
			const BodyMaterial& material2 = Materials[body2.GetID().GetIndex()];
			settings.mCombinedFriction = CombineFriction(material1.FrictionCombine, body1.GetFriction(), material2.FrictionCombine, body2.GetFriction());
			settings.mCombinedRestitution = CombineRestitution(material1.RestitutionCombine, body1.GetRestitution(), material2.RestitutionCombine, body2.GetRestitution());
		}
	};

}
