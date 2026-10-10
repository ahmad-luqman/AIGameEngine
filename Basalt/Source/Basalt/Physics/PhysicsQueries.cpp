// PhysicsWorld: raycasts, shape casts and overlap queries.
#include "Basalt/Physics/PhysicsWorld.h"

#include "Basalt/Physics/PhysicsWorldImpl.h"

#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace Basalt {

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

		// Normalized cast direction, or nullopt when the direction or distance cannot describe a cast.
		std::optional<glm::vec3> CastDirection(const glm::vec3& direction, float maxDistance)
		{
			const float length = glm::length(direction);
			if (!IsFiniteVec(direction) || !std::isfinite(length) || length <= 0.0f || !std::isfinite(maxDistance) || maxDistance <= 0.0f)
				return std::nullopt;
			return direction / length;
		}

		// Keeps each entity's closest hit, ordered by distance and then UUID so results are deterministic.
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

	std::vector<UUID> PhysicsWorld::OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, const PhysicsQueryFilter& filter) const
	{
		if (!IsValidBox(halfExtents) || !IsValidRotation(rotation))
			return {};
		return Overlap(*m_Impl->System, *MakeQueryBox(halfExtents), center, rotation, filter);
	}

}
