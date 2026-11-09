// PhysicsWorld: raycasts, shape casts and overlap queries.
#include "Breccia/Physics/PhysicsWorld.h"

#include "Breccia/Physics/PhysicsShapeSnapshotImpl.h"
#include "Breccia/Physics/PhysicsWorldImpl.h"

#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/TransformedShape.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <tuple>
#include <vector>

namespace Breccia {

	using namespace PhysicsInternal;

	std::optional<glm::vec3> CastDirection(const glm::vec3& direction, float maxDistance)
	{
		const float length = glm::length(direction);
		if (!IsFiniteVec(direction) || !std::isfinite(length) || length <= 0.0f || !std::isfinite(maxDistance) || maxDistance <= 0.0f)
			return std::nullopt;
		return direction / length;
	}

	std::vector<RaycastHit> SortHits(std::vector<RaycastHit> hits)
	{
		std::ranges::sort(hits, [](const RaycastHit& a, const RaycastHit& b) {
			return a.EntityID != b.EntityID ? a.EntityID < b.EntityID : a.Distance < b.Distance;
		});
		const auto duplicates = std::ranges::unique(hits, [](const RaycastHit& a, const RaycastHit& b) { return a.EntityID == b.EntityID; });
		hits.erase(duplicates.begin(), duplicates.end());
		std::ranges::sort(hits, [](const RaycastHit& a, const RaycastHit& b) {
			return a.Distance != b.Distance ? a.Distance < b.Distance : a.EntityID < b.EntityID;
		});
		return hits;
	}

	namespace {

		// Query filters: the layer mask, then triggers and the ignored entity (see PhysicsQueryFilter).
		class QueryObjectLayerFilter final : public JPH::ObjectLayerFilter
		{
		public:
			explicit QueryObjectLayerFilter(uint32_t mask)
				: m_Mask(mask)
			{
			}

			bool ShouldCollide(JPH::ObjectLayer layer) const override
			{
				const uint32_t index = LayerIndex(layer);
				return index < PhysicsLayers::MaxLayers && (m_Mask & (1u << index)) != 0;
			}

		private:
			uint32_t m_Mask = 0;
		};

		class QueryBodyFilter final : public JPH::BodyFilter
		{
		public:
			explicit QueryBodyFilter(const PhysicsQueryFilter& filter)
				: m_Ignore(static_cast<uint64_t>(filter.IgnoreEntity))
				, m_IncludeTriggers(filter.IncludeTriggers)
			{
			}

			bool ShouldCollideLocked(const JPH::Body& body) const override
			{
				return (m_IncludeTriggers || !body.IsSensor()) && (m_Ignore == 0 || body.GetUserData() != m_Ignore);
			}

		private:
			uint64_t m_Ignore = 0;
			bool m_IncludeTriggers = false;
		};

		std::vector<RaycastHit> CastRay(const JPH::PhysicsSystem& system, const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter, bool all)
		{
			const auto unitDirection = CastDirection(direction, maxDistance);
			if (!unitDirection || !IsFiniteVec(origin))
				return {};

			const JPH::RRayCast ray(ToJolt(origin), ToJolt(*unitDirection * maxDistance));
			const QueryObjectLayerFilter layerFilter(filter.LayerMask);
			const QueryBodyFilter bodyFilter(filter);
			std::vector<JPH::RayCastResult> results;
			if (all)
			{
				JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
				system.GetNarrowPhaseQuery().CastRay(ray, JPH::RayCastSettings(), collector, {}, layerFilter, bodyFilter);
				results.assign(collector.mHits.begin(), collector.mHits.end());
			}
			else
			{
				JPH::RayCastResult result;
				if (system.GetNarrowPhaseQuery().CastRay(ray, result, {}, layerFilter, bodyFilter))
					results.push_back(result);
			}

			std::vector<RaycastHit> hits;
			for (const JPH::RayCastResult& result : results)
			{
				const JPH::BodyLockRead lock(system.GetBodyLockInterface(), result.mBodyID);
				if (!lock.Succeeded())
					continue;
				RaycastHit& hit = hits.emplace_back();
				hit.Distance = result.mFraction * maxDistance;
				hit.Point = origin + *unitDirection * hit.Distance;
				hit.EntityID = lock.GetBody().GetUserData();
				hit.Normal = FromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, ToJolt(hit.Point)));
			}
			return SortHits(std::move(hits));
		}

		std::vector<RaycastHit> CastShape(const JPH::PhysicsSystem& system, const JPH::Shape& shape, const glm::vec3& origin, const glm::quat& rotation, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter, bool all)
		{
			const auto unitDirection = CastDirection(direction, maxDistance);
			if (!unitDirection || !IsFiniteVec(origin))
				return {};

			const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(&shape, JPH::Vec3::sOne(), JPH::RMat44::sRotationTranslation(ToJolt(glm::normalize(rotation)), ToJolt(origin)), ToJolt(*unitDirection * maxDistance));
			JPH::ShapeCastSettings settings;
			// For a shape that starts inside a body (fraction 0), compute the deepest point and penetration
			// axis, and keep the hit even when the cast moves out of the body (Jolt drops such "back face" hits
			// by default, so a cast would only see bodies it starts inside when moving deeper).
			settings.mReturnDeepestPoint = true;
			settings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
			const QueryObjectLayerFilter layerFilter(filter.LayerMask);
			const QueryBodyFilter bodyFilter(filter);
			std::vector<JPH::ShapeCastResult> results;
			if (all)
			{
				JPH::AllHitCollisionCollector<JPH::CastShapeCollector> collector;
				system.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector, {}, layerFilter, bodyFilter);
				results.assign(collector.mHits.begin(), collector.mHits.end());
			}
			else
			{
				JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
				system.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector, {}, layerFilter, bodyFilter);
				if (collector.HadHit())
					results.push_back(collector.mHit);
			}

			std::vector<RaycastHit> hits;
			for (const JPH::ShapeCastResult& result : results)
			{
				const JPH::BodyLockRead lock(system.GetBodyLockInterface(), result.mBodyID2);
				if (!lock.Succeeded())
					continue;
				RaycastHit& hit = hits.emplace_back();
				hit.EntityID = lock.GetBody().GetUserData();
				hit.Distance = result.mFraction * maxDistance;
				hit.Point = FromJolt(JPH::Vec3(result.mContactPointOn2));
				// The penetration axis points from the cast shape into the body; the surface normal faces back.
				const JPH::Vec3 axis = result.mPenetrationAxis;
				hit.Normal = axis.LengthSq() > 1e-12f ? FromJolt(-axis.Normalized()) : -*unitDirection;
			}
			return SortHits(std::move(hits));
		}

		std::vector<UUID> Overlap(const JPH::PhysicsSystem& system, const JPH::Shape& shape, const glm::vec3& center, const glm::quat& rotation, const PhysicsQueryFilter& filter)
		{
			if (!IsFiniteVec(center))
				return {};
			const QueryObjectLayerFilter layerFilter(filter.LayerMask);
			const QueryBodyFilter bodyFilter(filter);
			JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
			system.GetNarrowPhaseQuery().CollideShape(&shape, JPH::Vec3::sOne(), JPH::RMat44::sRotationTranslation(ToJolt(glm::normalize(rotation)), ToJolt(center)), JPH::CollideShapeSettings(), JPH::RVec3::sZero(), collector, {}, layerFilter, bodyFilter);

			std::vector<UUID> entities;
			for (const JPH::CollideShapeResult& result : collector.mHits)
			{
				const JPH::BodyLockRead lock(system.GetBodyLockInterface(), result.mBodyID2);
				if (lock.Succeeded())
					entities.emplace_back(lock.GetBody().GetUserData());
			}
			// Compound bodies report one hit per sub-shape.
			std::ranges::sort(entities);
			const auto duplicates = std::ranges::unique(entities);
			entities.erase(duplicates.begin(), duplicates.end());
			return entities;
		}

		bool IsValidRadius(float radius)
		{
			return std::isfinite(radius) && radius > 0.0f;
		}

		bool IsValidBox(const glm::vec3& halfExtents)
		{
			return IsFiniteVec(halfExtents) && halfExtents.x > 0.0f && halfExtents.y > 0.0f && halfExtents.z > 0.0f;
		}

		bool IsValidRotation(const glm::quat& rotation)
		{
			const float length = glm::length(rotation);
			return std::isfinite(length) && length > 1e-6f;
		}

		std::optional<RaycastHit> ClosestHit(const std::vector<RaycastHit>& hits)
		{
			if (hits.empty())
				return std::nullopt;
			return hits.front();
		}

		const glm::quat IdentityRotation(1.0f, 0.0f, 0.0f, 0.0f);

		JPH::Ref<JPH::Shape> MakeQueryBox(const glm::vec3& halfExtents)
		{
			const float convexRadius = std::min(JPH::cDefaultConvexRadius, 0.5f * std::min({ halfExtents.x, halfExtents.y, halfExtents.z }));
			return new JPH::BoxShape(ToJolt(halfExtents), convexRadius);
		}

	}

	std::optional<RaycastHit> PhysicsWorld::Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter) const
	{
		return ClosestHit(CastRay(*m_Impl->System, origin, direction, maxDistance, filter, false));
	}

	std::vector<RaycastHit> PhysicsWorld::RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter) const
	{
		return CastRay(*m_Impl->System, origin, direction, maxDistance, filter, true);
	}

	std::optional<RaycastHit> PhysicsWorld::SphereCast(const glm::vec3& origin, float radius, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter) const
	{
		if (!IsValidRadius(radius))
			return std::nullopt;
		const JPH::Ref<JPH::Shape> sphere = new JPH::SphereShape(radius);
		return ClosestHit(CastShape(*m_Impl->System, *sphere, origin, IdentityRotation, direction, maxDistance, filter, false));
	}

	std::vector<RaycastHit> PhysicsWorld::SphereCastAll(const glm::vec3& origin, float radius, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter) const
	{
		if (!IsValidRadius(radius))
			return {};
		const JPH::Ref<JPH::Shape> sphere = new JPH::SphereShape(radius);
		return CastShape(*m_Impl->System, *sphere, origin, IdentityRotation, direction, maxDistance, filter, true);
	}

	std::optional<RaycastHit> PhysicsWorld::BoxCast(const glm::vec3& origin, const glm::vec3& halfExtents, const glm::quat& rotation, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter) const
	{
		if (!IsValidBox(halfExtents) || !IsValidRotation(rotation))
			return std::nullopt;
		return ClosestHit(CastShape(*m_Impl->System, *MakeQueryBox(halfExtents), origin, rotation, direction, maxDistance, filter, false));
	}

	std::vector<RaycastHit> PhysicsWorld::BoxCastAll(const glm::vec3& origin, const glm::vec3& halfExtents, const glm::quat& rotation, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter) const
	{
		if (!IsValidBox(halfExtents) || !IsValidRotation(rotation))
			return {};
		return CastShape(*m_Impl->System, *MakeQueryBox(halfExtents), origin, rotation, direction, maxDistance, filter, true);
	}

	std::vector<UUID> PhysicsWorld::OverlapSphere(const glm::vec3& center, float radius, const PhysicsQueryFilter& filter) const
	{
		if (!IsValidRadius(radius))
			return {};
		const JPH::Ref<JPH::Shape> sphere = new JPH::SphereShape(radius);
		return Overlap(*m_Impl->System, *sphere, center, IdentityRotation, filter);
	}

	PhysicsShapeSnapshot::PhysicsShapeSnapshot() = default;
	PhysicsShapeSnapshot::~PhysicsShapeSnapshot() = default;
	PhysicsShapeSnapshot::PhysicsShapeSnapshot(PhysicsShapeSnapshot&&) noexcept = default;
	PhysicsShapeSnapshot& PhysicsShapeSnapshot::operator=(PhysicsShapeSnapshot&&) noexcept = default;

	bool PhysicsShapeSnapshot::Empty() const
	{
		return !m_Impl || m_Impl->Shapes.empty();
	}

	bool PhysicsShapeSnapshot::Truncated() const
	{
		return m_Impl && m_Impl->Truncated;
	}

	float PhysicsShapeSnapshot::GetRadius() const
	{
		return m_Impl ? m_Impl->Radius : 0.0f;
	}

	std::optional<glm::vec3> PhysicsShapeSnapshot::PushOut(const glm::vec3& point) const
	{
		if (Empty() || !IsFiniteVec(point))
			return std::nullopt;
		JPH::CollideShapeSettings settings;
		settings.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideOnlyWithActive;
		const JPH::RMat44 transform = JPH::RMat44::sTranslation(ToJolt(point));
		// The deepest penetration over every shape (the collector keeps the deepest hit of all the calls).
		JPH::ClosestHitCollisionCollector<JPH::CollideShapeCollector> collector;
		for (const JPH::TransformedShape& shape : m_Impl->Shapes)
			shape.CollideShape(m_Impl->Sphere, JPH::Vec3::sOne(), transform, settings, JPH::RVec3::sZero(), collector);
		if (!collector.HadHit() || !(collector.mHit.mPenetrationDepth > 0.0f))
			return std::nullopt;
		glm::vec3 center = point;
		for (const JPH::TransformedShape& shape : m_Impl->Shapes)
			if (shape.mBodyID == collector.mHit.mBodyID2)
				center = FromJolt(JPH::Vec3(shape.GetCenterOfMassTransform().GetTranslation()));
		return point + PushOutDirection(FromJolt(collector.mHit.mPenetrationAxis), point, center) * collector.mHit.mPenetrationDepth;
	}

	std::optional<PhysicsSweepHit> PhysicsShapeSnapshot::Sweep(const glm::vec3& from, const glm::vec3& to) const
	{
		if (Empty() || !IsFiniteVec(from) || !IsFiniteVec(to) || from == to)
			return std::nullopt;
		const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(m_Impl->Sphere, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(ToJolt(from)), ToJolt(to - from));
		JPH::ShapeCastSettings settings;
		// A sphere that starts inside gets the shortest way out as its normal. Back faces stay ignored, which is what
		// lets it leave: Jolt reports such a start only when the sphere moves deeper.
		settings.mReturnDeepestPoint = true;
		JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
		for (const JPH::TransformedShape& shape : m_Impl->Shapes)
			shape.CastShape(cast, settings, JPH::RVec3::sZero(), collector);
		if (!collector.HadHit())
			return std::nullopt;
		// The axis points from the sphere into the body.
		const JPH::Vec3 axis = collector.mHit.mPenetrationAxis;
		return PhysicsSweepHit{ .Fraction = collector.mHit.mFraction, .Normal = axis.LengthSq() > 1e-12f ? FromJolt(-axis.Normalized()) : glm::normalize(from - to) };
	}

	glm::vec3 PushOutDirection(const glm::vec3& penetrationAxis, const glm::vec3& point, const glm::vec3& bodyCenter)
	{
		// The axis moves the body out, so the sphere goes the other way.
		for (const glm::vec3 direction : { -penetrationAxis, point - bodyCenter })
		{
			const float length = glm::length(direction);
			if (std::isfinite(length) && length > 1e-12f)
				return direction / length;
		}
		return glm::vec3(0, 1, 0);
	}

	PhysicsShapeSnapshot PhysicsWorld::CollectShapes(const glm::vec3& min, const glm::vec3& max, float radius, const PhysicsQueryFilter& filter, const std::function<bool(UUID)>& accept) const
	{
		if (!IsValidShapeRegion(min, max, radius))
			return {};
		const JPH::PhysicsSystem& system = *m_Impl->System;
		JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> collector;
		const QueryObjectLayerFilter layerFilter(filter.LayerMask);
		system.GetBroadPhaseQuery().CollideAABox(JPH::AABox(ToJolt(min), ToJolt(max)), collector, {}, layerFilter);
		const QueryBodyFilter bodyFilter(filter);
		const glm::vec3 center = (min + max) * 0.5f;
		std::vector<PhysicsShapeSnapshot::Candidate> candidates;
		for (const JPH::BodyID& id : collector.mHits)
		{
			UUID entity = 0;
			JPH::TransformedShape shape;
			{
				const JPH::BodyLockRead lock(system.GetBodyLockInterface(), id);
				if (!lock.Succeeded() || !bodyFilter.ShouldCollideLocked(lock.GetBody()))
					continue;
				entity = lock.GetBody().GetUserData();
				shape = lock.GetBody().GetTransformedShape();
			}
			// After the lock is released: `accept` may query the world.
			if (accept && !accept(entity))
				continue;
			candidates.push_back(PhysicsShapeSnapshot::Candidate::At(entity, shape, center));
		}
		return PhysicsShapeSnapshot::FromCandidates(std::move(candidates), radius);
	}

	PhysicsShapeSnapshot PhysicsShapeSnapshot::FromCandidates(std::vector<Candidate> candidates, float radius)
	{
		PhysicsShapeSnapshot snapshot;
		// Nearest first, then by entity and body (broad-phase order is not stable), so the kept bodies do not depend on it.
		std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) { return std::tie(a.Distance, a.Entity, a.Shape.mBodyID) < std::tie(b.Distance, b.Entity, b.Shape.mBodyID); });
		snapshot.m_Impl = CreateScope<Impl>();
		const bool truncated = candidates.size() > MaxShapes;
		snapshot.m_Impl->Truncated = truncated;
		if (truncated)
			candidates.resize(MaxShapes);
		for (Candidate& candidate : candidates)
			snapshot.m_Impl->Shapes.push_back(std::move(candidate.Shape));
		snapshot.m_Impl->Radius = std::max(radius, MinRadius);
		snapshot.m_Impl->Sphere = new JPH::SphereShape(snapshot.m_Impl->Radius);
		return snapshot;
	}

	std::vector<UUID> PhysicsWorld::OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, const PhysicsQueryFilter& filter) const
	{
		if (!IsValidBox(halfExtents) || !IsValidRotation(rotation))
			return {};
		return Overlap(*m_Impl->System, *MakeQueryBox(halfExtents), center, rotation, filter);
	}

}
