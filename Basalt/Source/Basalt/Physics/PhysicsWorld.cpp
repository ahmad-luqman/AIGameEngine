#include "Basalt/Physics/PhysicsWorld.h"

#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Physics/JoltUtils.h"
#include "Basalt/Physics/PhysicsLayers.h"
#include "Basalt/Physics/PhysicsMaterial.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/JointFields.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scripting/ScriptEngine.h"

// Jolt.h must precede every other Jolt header.
#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Basalt {

	using namespace PhysicsInternal;

	namespace {

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

		// Whether a joint change needs a new constraint. Only the fields a live Jolt constraint can update
		// are exempt, so a field added later rebuilds the joint by default instead of being ignored in play.
		// (Toggling UseLimits rebuilds too: it changes how a distance joint's rest length is chosen.)
		bool NeedsRebuild(const JointComponent& built, const JointComponent& current)
		{
			JointComponent structural = current;
			structural.LimitMin = built.LimitMin;
			structural.LimitMax = built.LimitMax;
			structural.LimitSpringFrequency = built.LimitSpringFrequency;
			structural.LimitSpringDamping = built.LimitSpringDamping;
			structural.LinearLimitMin = built.LinearLimitMin;
			structural.LinearLimitMax = built.LinearLimitMax;
			structural.AngularLimitMin = built.AngularLimitMin;
			structural.AngularLimitMax = built.AngularLimitMax;
			structural.FreeLinearAxes = built.FreeLinearAxes;
			structural.MotorMode = built.MotorMode;
			structural.MotorTarget = built.MotorTarget;
			structural.LinearMotorMode = built.LinearMotorMode;
			structural.AngularMotorMode = built.AngularMotorMode;
			structural.LinearMotorTarget = built.LinearMotorTarget;
			structural.AngularMotorTarget = built.AngularMotorTarget;
			structural.MotorMaxForce = built.MotorMaxForce;
			structural.MotorSpringFrequency = built.MotorSpringFrequency;
			structural.MotorSpringDamping = built.MotorSpringDamping;
			structural.BreakForce = built.BreakForce;
			structural.BreakTorque = built.BreakTorque;
			structural.EnableCollision = built.EnableCollision;
			return structural != built;
		}

		// The entity whose body a joint moves: BodyEntity, or the entity holding the component when it is 0.
		UUID JointBody(UUID holder, const JointComponent& joint)
		{
			return joint.BodyEntity != 0 ? joint.BodyEntity : holder;
		}

		const char* JointTypeName(JointType type)
		{
			switch (type)
			{
				case JointType::Fixed:
					return "fixed";
				case JointType::Point:
					return "point";
				case JointType::Hinge:
					return "hinge";
				case JointType::Slider:
					return "slider";
				case JointType::Distance:
					return "distance";
				case JointType::Cone:
					return "cone";
				case JointType::SixDOF:
					return "six-DOF";
			}
			return "unknown";
		}

		// Limits Jolt accepts for the joint (it asserts on others). Hinge and slider limits must contain
		// the rest pose (0); distance limits are lengths; a cone has only its half angle. Changed values are
		// reported in warnings; fields a joint ignores are reported by ReportIgnoredFields.
		std::pair<float, float> SanitizeLimits(const JointComponent& joint, std::vector<std::string>& warnings)
		{
			if (!joint.UseLimits)
				return { 0.0f, 0.0f };
			switch (joint.Type)
			{
				case JointType::Hinge:
				case JointType::Slider:
				{
					const bool hinge = joint.Type == JointType::Hinge;
					const float range = hinge ? 180.0f : FLT_MAX;
					const float min = std::clamp(joint.LimitMin, -range, 0.0f);
					const float max = std::clamp(joint.LimitMax, 0.0f, range);
					if (min != joint.LimitMin || max != joint.LimitMax)
						warnings.emplace_back(fmt::format("{} limits [{}, {}] must satisfy {} (the rest pose is 0); clamped to [{}, {}]", JointTypeName(joint.Type), joint.LimitMin,
														  joint.LimitMax, hinge ? "-180 <= LimitMin <= 0 <= LimitMax <= 180 degrees" : "LimitMin <= 0 <= LimitMax", min, max));
					return { min, max };
				}
				case JointType::Distance:
				{
					const float min = std::max(joint.LimitMin, 0.0f);
					const float max = std::max(joint.LimitMax, min);
					if (min != joint.LimitMin || max != joint.LimitMax)
						warnings.emplace_back(fmt::format("distance limits [{}, {}] must satisfy 0 <= LimitMin <= LimitMax; clamped to [{}, {}]", joint.LimitMin, joint.LimitMax, min, max));
					if (max <= 0.0f)
						warnings.emplace_back("distance limits with LimitMax <= 0 pull the anchors together");
					return { min, max };
				}
				case JointType::Cone:
				{
					// Only the half angle; the cone is centered on the rest direction.
					const float max = std::clamp(joint.LimitMax, 0.0f, 180.0f);
					if (max != joint.LimitMax)
						warnings.emplace_back(fmt::format("cone half angle LimitMax {} must be within [0, 180] degrees; clamped to {}", joint.LimitMax, max));
					return { 0.0f, max };
				}
				case JointType::SixDOF:
				case JointType::Fixed:
				case JointType::Point:
					break;
			}
			return { 0.0f, 0.0f };
		}

		// Fields set away from their defaults that do nothing for this joint (see JointFieldApplies), split by
		// whether turning on UseLimits would make them count.
		void ReportIgnoredFields(const JointComponent& joint, std::vector<std::string>& warnings)
		{
			JointComponent limited = joint;
			limited.UseLimits = true;
			std::string byType;
			std::string byLimits;
			int byTypeCount = 0;
			int byLimitsCount = 0;
			for (const std::string& field : GetIgnoredJointFields(joint))
			{
				const bool needsLimits = JointFieldApplies(limited, field);
				std::string& list = needsLimits ? byLimits : byType;
				list += (list.empty() ? "" : ", ") + field;
				(needsLimits ? byLimitsCount : byTypeCount)++;
			}
			if (byTypeCount > 0)
				warnings.emplace_back(fmt::format("{} {} no effect on {} joints", byType, byTypeCount == 1 ? "has" : "have", JointTypeName(joint.Type)));
			if (byLimitsCount > 0)
				warnings.emplace_back(fmt::format("{} {} no effect without UseLimits", byLimits, byLimitsCount == 1 ? "has" : "have"));
		}

		// Six-DOF limits in Jolt's units (meters, radians) along/around the joint frame's axes.
		struct SixDOFLimits
		{
			glm::vec3 LinearMin = { 0.0f, 0.0f, 0.0f };
			glm::vec3 LinearMax = { 0.0f, 0.0f, 0.0f };
			glm::vec3 AngularMin = { -glm::pi<float>(), -glm::pi<float>(), -glm::pi<float>() };
			glm::vec3 AngularMax = { glm::pi<float>(), glm::pi<float>(), glm::pi<float>() };
		};

		constexpr const char* s_AxisNames[] = { "X", "Y", "Z" };

		// Without UseLimits translation is locked and rotation free (a point joint). With limits every range
		// must contain the rest pose (0), twist stays within +-180 degrees, and the swing cone is symmetric
		// (Jolt's cone swing ignores the minimum), so a swing minimum other than 0 or -max is mirrored.
		// Angles are capped at pi: Jolt asserts on more, and glm::radians(180) can round past it.
		// FreeLinearAxes free their translation axes either way (Jolt treats +-FLT_MAX as free).
		SixDOFLimits SanitizeSixDOFLimits(const JointComponent& joint, std::vector<std::string>& warnings)
		{
			SixDOFLimits limits;
			for (int i = 0; i < 3; i++)
			{
				if (!joint.FreeLinearAxes[i])
					continue;
				limits.LinearMin[i] = -FLT_MAX;
				limits.LinearMax[i] = FLT_MAX;
				if (joint.UseLimits && (joint.LinearLimitMin[i] != 0.0f || joint.LinearLimitMax[i] != 0.0f))
					warnings.emplace_back(fmt::format("six-DOF linear {} limits are ignored: the axis is free (FreeLinearAxes)", s_AxisNames[i]));
			}
			if (!joint.UseLimits)
				return limits;
			for (int i = 0; i < 3; i++)
			{
				if (!joint.FreeLinearAxes[i])
				{
					const float linearMin = std::min(joint.LinearLimitMin[i], 0.0f);
					const float linearMax = std::max(joint.LinearLimitMax[i], 0.0f);
					if (linearMin != joint.LinearLimitMin[i] || linearMax != joint.LinearLimitMax[i])
						warnings.emplace_back(fmt::format("six-DOF linear {} limits [{}, {}] must satisfy min <= 0 <= max (the rest pose is 0); clamped to [{}, {}]", s_AxisNames[i],
														  joint.LinearLimitMin[i], joint.LinearLimitMax[i], linearMin, linearMax));
					limits.LinearMin[i] = linearMin;
					limits.LinearMax[i] = linearMax;
				}

				float angularMin = 0.0f;
				const float angularMax = std::clamp(joint.AngularLimitMax[i], 0.0f, 180.0f);
				if (i == 0)
				{
					angularMin = std::clamp(joint.AngularLimitMin[i], -180.0f, 0.0f);
					if (angularMin != joint.AngularLimitMin[i] || angularMax != joint.AngularLimitMax[i])
						warnings.emplace_back(fmt::format("six-DOF twist (angular X) limits [{}, {}] must satisfy -180 <= min <= 0 <= max <= 180 degrees; clamped to [{}, {}]", joint.AngularLimitMin[i],
														  joint.AngularLimitMax[i], angularMin, angularMax));
				}
				else
				{
					// A minimum of 0 (the default) stands for -max, so setting only the maximum is enough.
					angularMin = -angularMax;
					const bool mirrored = joint.AngularLimitMin[i] == 0.0f || joint.AngularLimitMin[i] == angularMin;
					if (!mirrored || angularMax != joint.AngularLimitMax[i])
						warnings.emplace_back(fmt::format("six-DOF swing (angular {}) limits [{}, {}] must be a symmetric half angle [-max, max] with 0 <= max <= 180 degrees; using [{}, {}]", s_AxisNames[i],
														  joint.AngularLimitMin[i], joint.AngularLimitMax[i], angularMin, angularMax));
				}
				limits.AngularMin[i] = std::max(glm::radians(angularMin), -JPH::JPH_PI);
				limits.AngularMax[i] = std::min(glm::radians(angularMax), JPH::JPH_PI);
			}
			return limits;
		}

		// The spring that softens a joint's limits (a frequency of 0 keeps them rigid). Whether the joint has
		// limits to soften is reported by ReportIgnoredFields.
		JPH::SpringSettings SanitizeLimitSpring(const JointComponent& joint, std::vector<std::string>& warnings)
		{
			if (joint.LimitSpringFrequency < 0.0f)
				warnings.emplace_back(fmt::format("LimitSpringFrequency {} is negative; using 0 (rigid limits)", joint.LimitSpringFrequency));
			if (joint.LimitSpringDamping < 0.0f)
				warnings.emplace_back(fmt::format("LimitSpringDamping {} is negative; using 0", joint.LimitSpringDamping));
			const float frequency = std::max(joint.LimitSpringFrequency, 0.0f);
			const float damping = std::max(joint.LimitSpringDamping, 0.0f);
			return JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, frequency, damping);
		}

		// The spring a Position motor pulls toward its target with. Jolt switches a position motor off when its
		// spring has no stiffness, so the frequency must be positive.
		JPH::SpringSettings SanitizeMotorSpring(const JointComponent& joint, std::vector<std::string>& warnings)
		{
			const float defaultFrequency = JointComponent().MotorSpringFrequency;
			const bool validFrequency = joint.MotorSpringFrequency > 0.0f;
			if (!validFrequency)
				warnings.emplace_back(fmt::format("MotorSpringFrequency {} must be greater than 0; using {}", joint.MotorSpringFrequency, defaultFrequency));
			if (joint.MotorSpringDamping < 0.0f)
				warnings.emplace_back(fmt::format("MotorSpringDamping {} is negative; using 0", joint.MotorSpringDamping));
			return JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, validFrequency ? joint.MotorSpringFrequency : defaultFrequency, std::max(joint.MotorSpringDamping, 0.0f));
		}

		JPH::EMotorState ToJoltMotorState(JointMotorMode mode)
		{
			switch (mode)
			{
				case JointMotorMode::Velocity:
					return JPH::EMotorState::Velocity;
				case JointMotorMode::Position:
					return JPH::EMotorState::Position;
				case JointMotorMode::Off:
					break;
			}
			return JPH::EMotorState::Off;
		}

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

		// ---------------------------------------------------------------------------------------
		// Cooked mesh collider shapes, shared by every entity (and every PhysicsWorld) that uses the same
		// mesh. Cooking a level's triangle tree is the slow part of starting play, so shapes outlive the
		// world; an entry is re-cooked when its mesh asset was reloaded (the cached source expired).
		// ---------------------------------------------------------------------------------------
		struct CookedMeshShape
		{
			std::weak_ptr<MeshSource> Source;
			// Unscaled shape in the mesh's local space; null when cooking failed.
			JPH::Ref<JPH::Shape> Shape;
			std::string Error;
		};

		using MeshShapeKey = std::tuple<std::string, uint32_t, bool>;
		std::mutex s_MeshShapeMutex;
		std::map<MeshShapeKey, CookedMeshShape> s_MeshShapes;
		uint64_t s_MeshShapeCookCount = 0;

		CookedMeshShape CookMeshShape(const Ref<MeshSource>& source, uint32_t meshIndex, bool convex)
		{
			CookedMeshShape cooked;
			cooked.Source = source;
			const MeshData& mesh = source->Meshes[meshIndex];
			if (convex)
			{
				// Only vertices the index buffer draws: a glTF accessor may hold unused ones outside the visible
				// geometry, which would otherwise inflate the hull.
				JPH::Array<JPH::Vec3> points;
				for (uint32_t submeshIndex : mesh.Submeshes)
				{
					const Submesh& submesh = source->Submeshes[submeshIndex];
					std::vector<bool> used(submesh.VertexCount, false);
					for (uint32_t i = 0; i < submesh.IndexCount; i++)
					{
						const uint32_t index = source->Indices[submesh.BaseIndex + i];
						if (index < submesh.VertexCount && !used[index])
						{
							used[index] = true;
							const glm::vec3& position = source->Vertices[submesh.BaseVertex + index].Position;
							if (!IsFiniteVec(position))
							{
								cooked.Error = "the mesh has a non-finite vertex position";
								return cooked;
							}
							points.push_back(ToJolt(position));
						}
					}
				}
				JPH::ShapeSettings::ShapeResult result = JPH::ConvexHullShapeSettings(points).Create();
				if (result.HasError())
					cooked.Error = std::string("cannot build a convex hull: ") + result.GetError().c_str();
				else
					cooked.Shape = result.Get();
				return cooked;
			}

			JPH::VertexList vertices;
			JPH::IndexedTriangleList triangles;
			for (uint32_t submeshIndex : mesh.Submeshes)
			{
				const Submesh& submesh = source->Submeshes[submeshIndex];
				const uint32_t base = static_cast<uint32_t>(vertices.size());
				for (uint32_t i = 0; i < submesh.VertexCount; i++)
				{
					const glm::vec3& position = source->Vertices[submesh.BaseVertex + i].Position;
					if (!IsFiniteVec(position))
					{
						cooked.Error = "the mesh has a non-finite vertex position";
						return cooked;
					}
					vertices.push_back(JPH::Float3(position.x, position.y, position.z));
				}
				for (uint32_t i = 0; i + 2 < submesh.IndexCount; i += 3)
				{
					const uint32_t* index = &source->Indices[submesh.BaseIndex + i];
					if (index[0] >= submesh.VertexCount || index[1] >= submesh.VertexCount || index[2] >= submesh.VertexCount)
						continue;
					triangles.push_back(JPH::IndexedTriangle(base + index[0], base + index[1], base + index[2]));
				}
			}
			if (triangles.empty())
			{
				cooked.Error = "the mesh has no triangles";
				return cooked;
			}
			JPH::ShapeSettings::ShapeResult result = JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create();
			if (result.HasError())
				cooked.Error = std::string("cannot build a triangle mesh: ") + result.GetError().c_str();
			else
				cooked.Shape = result.Get();
			return cooked;
		}

		// Returns the unscaled shape for one mesh of a mesh asset, cooking it on first use. On failure returns
		// null and sets outError.
		JPH::Ref<JPH::Shape> GetMeshShape(const std::string& key, uint32_t meshIndex, bool convex, std::string& outError)
		{
			const Ref<MeshSource> source = AssetManager::GetMesh(key);
			if (!source)
			{
				const std::string error = AssetManager::GetError(key);
				outError = "cannot load mesh '" + key + "'" + (error.empty() ? "" : ": " + error);
				return nullptr;
			}
			if (meshIndex >= source->Meshes.size())
			{
				outError = "mesh '" + key + "' has no mesh " + std::to_string(meshIndex) + " (it has " + std::to_string(source->Meshes.size()) + ")";
				return nullptr;
			}

			std::scoped_lock lock(s_MeshShapeMutex);
			auto [it, inserted] = s_MeshShapes.try_emplace(MeshShapeKey(key, meshIndex, convex));
			if (inserted || it->second.Source.lock() != source)
			{
				it->second = CookMeshShape(source, meshIndex, convex);
				s_MeshShapeCookCount++;
				// Entries of unloaded or reloaded meshes would otherwise keep their shapes alive until the
				// cache is cleared; new cooks are rare, so pruning here costs little. Only after cooking: a
				// just-inserted entry has no Source yet and would count as expired.
				std::erase_if(s_MeshShapes, [](const auto& entry) { return entry.second.Source.expired(); });
			}
			outError = it->second.Error;
			return it->second.Shape;
		}

		// Smallest collider extent, so a zero scale or size still gives Jolt a valid shape.
		constexpr float MinExtent = 0.001f;

		// An entity's colliders combined into one shape, shared by rigid bodies and characters.
		struct ColliderShape
		{
			// Null when no collider is valid (the reasons went to warn).
			JPH::Ref<JPH::Shape> Shape;
			// A MeshCollider contributed an exact triangle mesh (which has no volume).
			bool HasTriangleMesh = false;
			// The MeshComponent mesh a MeshCollider without its own Mesh used.
			std::optional<std::pair<std::string, uint32_t>> BorrowedMesh;
		};

		// Builds the entity's box, sphere, capsule and mesh colliders, scaled by its world scale (signed: mesh
		// colliders keep a mirroring) and by sizeFraction (a character's inner body is a little smaller).
		// convexReason, when set, forces mesh colliders onto their convex hull and names who needs it.
		ColliderShape BuildColliderShape(Entity entity, const glm::vec3& signedScale, float sizeFraction, const char* convexReason, bool isTrigger, const std::function<void(const std::string&)>& warn)
		{
			const glm::vec3 scale = glm::abs(signedScale) * sizeFraction;
			JPH::StaticCompoundShapeSettings compound;
			uint32_t shapeCount = 0;
			auto addShape = [&](const JPH::ShapeSettings::ShapeResult& result, const glm::vec3& offset) {
				if (result.HasError())
				{
					warn(std::string("invalid collider: ") + result.GetError().c_str());
					return;
				}
				// Offsets follow the entity's scale only: a smaller inner shape stays centred on each collider.
				compound.AddShape(ToJolt(offset * glm::abs(signedScale)), JPH::Quat::sIdentity(), result.Get());
				shapeCount++;
			};

			if (const auto* box = entity.TryGetComponent<BoxColliderComponent>())
			{
				const glm::vec3 halfExtents = glm::max(box->HalfExtents * scale, glm::vec3(MinExtent));
				const float convexRadius = std::min(JPH::cDefaultConvexRadius, glm::min(halfExtents.x, glm::min(halfExtents.y, halfExtents.z)) * 0.5f);
				addShape(JPH::BoxShapeSettings(ToJolt(halfExtents), convexRadius).Create(), box->Offset);
			}
			if (const auto* sphere = entity.TryGetComponent<SphereColliderComponent>())
			{
				const float radius = std::max(sphere->Radius * glm::max(scale.x, glm::max(scale.y, scale.z)), MinExtent);
				addShape(JPH::SphereShapeSettings(radius).Create(), sphere->Offset);
			}
			if (const auto* capsule = entity.TryGetComponent<CapsuleColliderComponent>())
			{
				const float radius = std::max(capsule->Radius * glm::max(scale.x, scale.z), MinExtent);
				const float halfHeight = std::max(capsule->HalfHeight * scale.y, MinExtent);
				addShape(JPH::CapsuleShapeSettings(halfHeight, radius).Create(), capsule->Offset);
			}

			ColliderShape result;
			if (const auto* meshCollider = entity.TryGetComponent<MeshColliderComponent>())
			{
				std::string key = meshCollider->Mesh;
				uint32_t meshIndex = meshCollider->MeshIndex;
				const auto* meshComponent = entity.TryGetComponent<MeshComponent>();
				if (key.empty() && meshComponent)
				{
					if (meshIndex != 0)
						warn("MeshCollider MeshIndex is ignored without its own Mesh (the MeshComponent's is used)");
					key = meshComponent->Mesh;
					meshIndex = meshComponent->MeshIndex;
					result.BorrowedMesh.emplace(key, meshIndex);
				}
				bool convex = meshCollider->Convex;
				if (!convex && convexReason)
				{
					convex = true;
					warn(std::string(convexReason) + " collides by the convex hull of its MeshCollider; set Convex to make that explicit");
				}
				// A triangle mesh has no inside, so a trigger built from one would only report crossing its surface.
				convex |= isTrigger;
				std::string error;
				JPH::Ref<JPH::Shape> shape;
				if (key.empty())
					error = "MeshCollider has no Mesh and the entity has no MeshComponent to use";
				else
					shape = GetMeshShape(key, meshIndex, convex, error);
				if (shape)
				{
					// The cooked shape is shared, so the entity's scale wraps it instead of being baked in.
					const glm::vec3 meshScale = glm::sign(signedScale) * glm::max(scale, glm::vec3(MinExtent));
					if (meshScale != glm::vec3(1.0f))
						shape = new JPH::ScaledShape(shape, ToJolt(meshScale));
					compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), shape);
					shapeCount++;
					result.HasTriangleMesh = !convex;
				}
				else
				{
					warn("MeshCollider ignored: " + error);
				}
			}

			if (shapeCount == 0)
			{
				warn("no valid collider");
				return result;
			}
			JPH::ShapeSettings::ShapeResult shapeResult = compound.Create();
			if (shapeResult.HasError())
			{
				warn(std::string("failed to build the shape: ") + shapeResult.GetError().c_str());
				return result;
			}
			result.Shape = shapeResult.Get();
			return result;
		}

	}

	struct PhysicsWorld::Impl
	{
		struct BodyRecord
		{
			JPH::BodyID ID;
			RigidBodyType Type = RigidBodyType::Static;
			bool IsTrigger = false;
			// The MeshComponent mesh a MeshCollider without its own Mesh was built from, so edits to the
			// MeshComponent's other fields (e.g. CastShadows) do not rebuild the body.
			std::optional<std::pair<std::string, uint32_t>> BorrowedMesh;
			// Pose last written to / read from the entity, used to detect transforms changed by scripts.
			glm::vec3 LastPosition = { 0.0f, 0.0f, 0.0f };
			glm::quat LastRotation = { 1.0f, 0.0f, 0.0f, 0.0f };
		};

		struct JointRecord
		{
			JPH::Ref<JPH::TwoBodyConstraint> Constraint;
			// The component the constraint was built from (plus later in-place updates). Its Type always
			// matches the constraint's subtype; ConnectedEntity is 0 when attached to the world.
			JointComponent Settings;
			// The entity whose body the joint moves (Settings.BodyEntity resolved; the holder when that is 0).
			// Only set when the constraint is built; that stays correct because changing BodyEntity rebuilds
			// the joint (see NeedsRebuild).
			UUID Body = 0;
			// Warnings found while building the constraint (e.g. a zero axis). In-place updates only re-check
			// the other settings, so these are added back to keep the logged set the same.
			std::vector<std::string> BuildWarnings;
			// The second body's joint frame seen from the first's when the joint was built. Six-DOF frames
			// coincide there (identity), but Jolt picks each cone frame's Y and Z per body, so they can differ
			// by a twist; GetJointRotation measures from this.
			JPH::Quat RestRotation = JPH::Quat::sIdentity();
		};

		// The second body's joint frame seen from the first's (see JointRecord::RestRotation).
		static JPH::Quat RelativeJointRotation(const JPH::TwoBodyConstraint& constraint)
		{
			const JPH::Quat frame1 = constraint.GetBody1()->GetRotation() * constraint.GetConstraintToBody1Matrix().GetQuaternion();
			const JPH::Quat frame2 = constraint.GetBody2()->GetRotation() * constraint.GetConstraintToBody2Matrix().GetQuaternion();
			return (frame1.Conjugated() * frame2).Normalized();
		}

		struct CharacterRecord
		{
			JPH::Ref<JPH::CharacterVirtual> Character;
			// The settings applied to the character (from its CharacterControllerComponent).
			CharacterControllerComponent Settings;
			uint32_t Layer = 0;
			// Settings in Jolt's terms, checked by ConfigureCharacter.
			float StepHeight = 0.0f;
			float GravityFactor = 1.0f;
			// Warnings found while building the character, repeated with the settings' own when they change.
			std::string BuildWarnings;
			// The last MoveCharacter velocity, used every step until the next call.
			glm::vec3 MoveVelocity = { 0.0f, 0.0f, 0.0f };
			// Displacement over the last step divided by its duration.
			glm::vec3 ActualVelocity = { 0.0f, 0.0f, 0.0f };
			// Pose last written to / read from the entity, used to detect transforms changed by scripts.
			glm::vec3 LastPosition = { 0.0f, 0.0f, 0.0f };
			glm::quat LastRotation = { 1.0f, 0.0f, 0.0f, 0.0f };
			// The shape's bounding box in the character's local space, relative to its position.
			JPH::AABox Bounds;
			// Set while the entity's transform cannot be decomposed (e.g. a zero scale); the character waits.
			bool Degenerate = false;
			// As BodyRecord::BorrowedMesh.
			std::optional<std::pair<std::string, uint32_t>> BorrowedMesh;
		};

		Scope<JPH::TempAllocatorImpl> TempAllocator;
		Scope<JPH::JobSystemThreadPool> JobSystem;
		BroadPhaseLayerInterfaceImpl BroadPhaseLayerInterface;
		ObjectVsBroadPhaseLayerFilterImpl ObjectVsBroadPhaseLayerFilter;
		ObjectLayerPairFilterImpl ObjectLayerPairFilter;
		ContactListenerImpl ContactListener;
		Scope<JPH::PhysicsSystem> System;

		std::unordered_map<UUID, BodyRecord> Bodies;
		std::unordered_map<UUID, CharacterRecord> Characters;
		// Characters whose CharacterControllerComponent changed (collider changes go to DirtyEntities).
		std::unordered_set<UUID> DirtyCharacters;
		// The character warnings last logged for each entity.
		std::unordered_map<UUID, std::string> LoggedCharacterWarnings;
		// Characters already warned that rigid-body velocity and force calls do nothing on them.
		std::unordered_set<UUID> WarnedCharacterBodyCalls;
		std::unordered_map<uint32_t, UUID> BodyToEntity;
		std::unordered_set<UUID> DirtyEntities;
		// Keyed by the entity that holds the JointComponent (not necessarily the body it moves).
		std::unordered_map<UUID, JointRecord> Joints;
		std::unordered_set<UUID> DirtyJoints;
		// The warnings last logged for each joint, so repeats are not logged again.
		std::unordered_map<UUID, std::vector<std::string>> LoggedJointWarnings;
		// The collision layers this world was built with (scene override, else the active project's).
		PhysicsLayers Layers;
		// The body warnings (unknown layer, ignored Continuous) last logged for each entity.
		std::unordered_map<UUID, std::string> LoggedBodyWarnings;
		// Set while Start() creates every body; it marks all joints dirty itself afterwards.
		bool Starting = false;
		// Jolt reports contacts per sub-shape pair; entities see one begin/end per entity pair. Keys are
		// (entity A, sub-shape A, entity B, sub-shape B) with A < B; PairCounts counts keys per entity pair.
		using ContactKey = std::tuple<uint64_t, uint32_t, uint64_t, uint32_t>;
		using EntityPair = std::pair<uint64_t, uint64_t>;
		std::set<ContactKey> ActiveContacts;
		std::map<EntityPair, int> PairCounts;
		// Entity pairs (lower UUID first) joined by a joint with EnableCollision off. Read by Jolt worker
		// threads during Update, so it only changes between steps.
		std::set<EntityPair> IgnoredPairs;
		// Contacts whose removal was reported because a body fell asleep. They stay active; once both bodies
		// are awake again, a contact that Jolt does not re-report has really ended.
		std::set<ContactKey> SuspendedContacts;

		// Applies the record's Settings that a live character can change, sanitizing out-of-range values
		// (reported through warn).
		static void ConfigureCharacter(CharacterRecord& record, const std::function<void(const std::string&)>& warn)
		{
			const CharacterControllerComponent& c = record.Settings;
			// Below about 0.8 degrees Jolt switches the slope check off and every slope becomes walkable.
			const float slope = std::isfinite(c.SlopeLimit) ? std::clamp(c.SlopeLimit, 1.0f, 90.0f) : 45.0f;
			if (slope != c.SlopeLimit)
				warn(fmt::format("SlopeLimit {} must be within [1, 90] degrees; using {}", c.SlopeLimit, slope));
			record.Character->SetMaxSlopeAngle(glm::radians(slope));
			const float strength = std::isfinite(c.MaxStrength) && c.MaxStrength >= 0.0f ? c.MaxStrength : 0.0f;
			if (strength != c.MaxStrength)
				warn(fmt::format("MaxStrength {} must be a finite, non-negative force; using 0", c.MaxStrength));
			record.Character->SetMaxStrength(strength);
			const float mass = std::isfinite(c.Mass) && c.Mass >= 0.0f ? c.Mass : 0.0f;
			if (mass != c.Mass)
				warn(fmt::format("Mass {} must be a finite, non-negative mass; using 0", c.Mass));
			record.Character->SetMass(mass);
			record.StepHeight = std::isfinite(c.StepHeight) && c.StepHeight >= 0.0f ? c.StepHeight : 0.0f;
			if (record.StepHeight != c.StepHeight)
				warn(fmt::format("StepHeight {} must be a finite, non-negative distance; using 0", c.StepHeight));
			record.GravityFactor = std::isfinite(c.GravityFactor) ? c.GravityFactor : 1.0f;
			if (record.GravityFactor != c.GravityFactor)
				warn(fmt::format("GravityFactor {} is not finite; using 1", c.GravityFactor));
		}

		// Only contacts on the lower part of the shape can be ground; others are walls or ceilings. "Lower" is
		// along the character's up axis (against gravity) in its local space, so a turned gravity or a tilted
		// entity keeps the bottom of the shape as its feet: below the lowest point of its bounds plus their
		// narrowest half extent (the hemisphere of an upright capsule).
		static void UpdateSupportingVolume(CharacterRecord& record)
		{
			const JPH::CharacterVirtual& character = *record.Character;
			const JPH::Vec3 localUp = character.GetRotation().Conjugated() * character.GetUp();
			const JPH::Vec3 extent = record.Bounds.GetExtent();
			const float bottom = localUp.Dot(record.Bounds.GetCenter()) - localUp.Abs().Dot(extent);
			const float halfWidth = extent.ReduceMin();
			record.Character->SetSupportingVolume(JPH::Plane(localUp, -(bottom + halfWidth)));
		}

		// Velocity and force calls address rigid bodies; a character moves only through Move. Warns once per
		// character so knockback code that silently does nothing is noticed. Returns whether it is a character.
		bool WarnIfCharacter(Entity entity, const char* call)
		{
			if (!Characters.contains(entity.GetUUID()))
				return false;
			if (WarnedCharacterBodyCalls.insert(entity.GetUUID()).second)
				BS_CORE_WARN("Physics: {} does nothing on character '{}' (it has a CharacterController); use Move", call, entity.GetName());
			return true;
		}

		// Logs a character's warnings ("; "-joined) unless they are the ones last logged for it.
		void ReportCharacterWarnings(Entity entity, const std::string& warnings)
		{
			if (warnings.empty())
			{
				LoggedCharacterWarnings.erase(entity.GetUUID());
			}
			else if (std::string& logged = LoggedCharacterWarnings[entity.GetUUID()]; logged != warnings)
			{
				logged = warnings;
				BS_CORE_WARN("Physics: character '{}': {}", entity.GetName(), warnings);
			}
		}

		bool IsTrigger(UUID uuid) const
		{
			auto it = Bodies.find(uuid);
			return it != Bodies.end() && it->second.IsTrigger;
		}

		void UpdateIgnoredPairs()
		{
			std::set<EntityPair> pairs;
			for (const auto& [holder, joint] : Joints)
			{
				if (joint.Settings.ConnectedEntity == 0 || joint.Settings.EnableCollision)
					continue;
				const uint64_t a = static_cast<uint64_t>(joint.Body);
				const uint64_t b = static_cast<uint64_t>(joint.Settings.ConnectedEntity);
				pairs.emplace(std::min(a, b), std::max(a, b));
			}
			// Jolt replays cached contacts for bodies that have not moved (and skips sleeping ones), bypassing
			// the pair filter. Bodies whose pair starts or stops being ignored lose that cache and wake, so two
			// resting bodies stop (or start) colliding at once.
			std::vector<EntityPair> changed;
			std::ranges::set_symmetric_difference(pairs, IgnoredPairs, std::back_inserter(changed));
			IgnoredPairs = std::move(pairs);
			JPH::BodyInterface& bodies = System->GetBodyInterface();
			for (const auto& [a, b] : changed)
			{
				for (uint64_t uuid : { a, b })
				{
					auto body = Bodies.find(uuid);
					if (body == Bodies.end())
						continue;
					bodies.InvalidateContactCache(body->second.ID);
					if (body->second.Type == RigidBodyType::Dynamic)
						bodies.ActivateBody(body->second.ID);
				}
			}
		}

		void RemoveJoint(UUID holder)
		{
			auto it = Joints.find(holder);
			if (it == Joints.end())
				return;
			System->RemoveConstraint(it->second.Constraint);
			Joints.erase(it);
			UpdateIgnoredPairs();
		}

		// Jolt constraints point at their bodies, so they must go before either body is destroyed. They are
		// rebuilt before the next step if both bodies still exist then (e.g. a body that was recreated).
		void RemoveJointsOfBody(UUID uuid)
		{
			// Jolt removes a constraint by moving its last one into the gap, which changes the solve order of
			// the rest; removing in UUID order (not hash order) keeps that order reproducible.
			std::vector<UUID> holders;
			for (const auto& [holder, joint] : Joints)
			{
				if (joint.Body == uuid || joint.Settings.ConnectedEntity == uuid)
					holders.push_back(holder);
			}
			std::ranges::sort(holders);
			for (UUID holder : holders)
			{
				auto it = Joints.find(holder);
				System->RemoveConstraint(it->second.Constraint);
				DirtyJoints.insert(holder);
				Joints.erase(it);
			}
			UpdateIgnoredPairs();
		}

		// Removes a body. Contacts it had are ended now (Jolt's removal callbacks for a destroyed body can
		// no longer be mapped to an entity), and surviving entities are notified.
		void RemoveBody(Scene* scene, UUID uuid)
		{
			auto it = Bodies.find(uuid);
			if (it == Bodies.end())
				return;
			const bool trigger = it->second.IsTrigger;
			RemoveJointsOfBody(uuid);
			JPH::BodyInterface& bodies = System->GetBodyInterface();
			BodyToEntity.erase(it->second.ID.GetIndexAndSequenceNumber());
			// Jolt reuses the index for the next body; whatever creates it must not inherit these modes.
			ContactListener.Materials[it->second.ID.GetIndex()] = {};
			bodies.RemoveBody(it->second.ID);
			bodies.DestroyBody(it->second.ID);
			Bodies.erase(it);
			EndContacts(scene, uuid, trigger);
		}

		// Removes a character and its inner body, ending its contacts like RemoveBody.
		void RemoveCharacter(Scene* scene, UUID uuid)
		{
			auto it = Characters.find(uuid);
			if (it == Characters.end())
				return;
			const JPH::BodyID inner = it->second.Character->GetInnerBodyID();
			if (!inner.IsInvalid())
			{
				BodyToEntity.erase(inner.GetIndexAndSequenceNumber());
				ContactListener.Materials[inner.GetIndex()] = {};
			}
			// The character destroys its inner body.
			Characters.erase(it);
			EndContacts(scene, uuid, false);
		}

		// Ends every contact of an entity whose body is gone and notifies the surviving entities.
		void EndContacts(Scene* scene, UUID uuid, bool trigger)
		{
			const uint64_t id = static_cast<uint64_t>(uuid);
			std::set<EntityPair> ended;
			for (auto key = ActiveContacts.begin(); key != ActiveContacts.end();)
			{
				if (std::get<0>(*key) != id && std::get<2>(*key) != id)
				{
					++key;
					continue;
				}
				const EntityPair pair(std::get<0>(*key), std::get<2>(*key));
				if (--PairCounts[pair] <= 0)
				{
					PairCounts.erase(pair);
					ended.insert(pair);
				}
				SuspendedContacts.erase(*key);
				key = ActiveContacts.erase(key);
			}

			ScriptEngine* scriptEngine = scene->GetScriptEngine();
			for (const EntityPair& pair : ended)
			{
				const UUID other = pair.first == id ? UUID(pair.second) : UUID(pair.first);
				const bool isTrigger = trigger || IsTrigger(other);
				Entity a = scene->GetEntityByUUID(pair.first);
				Entity b = scene->GetEntityByUUID(pair.second);
				if (scriptEngine && a && b)
					scriptEngine->OnContactEvent(isTrigger ? ContactEventType::TriggerExit : ContactEventType::CollisionEnd, a, b);
			}
		}

		void OnPhysicsComponentChanged(entt::registry& registry, entt::entity entity)
		{
			if (const auto* id = registry.try_get<IDComponent>(entity))
				DirtyEntities.insert(id->ID);
		}

		// A MeshCollider without its own Mesh collides by the entity's MeshComponent.
		void OnMeshChanged(entt::registry& registry, entt::entity entity)
		{
			if (const auto* collider = registry.try_get<MeshColliderComponent>(entity); collider && collider->Mesh.empty())
				OnPhysicsComponentChanged(registry, entity);
		}

		// Only a different mesh changes the collider.
		void OnMeshUpdated(entt::registry& registry, entt::entity entity)
		{
			const auto* id = registry.try_get<IDComponent>(entity);
			const auto& mesh = registry.get<MeshComponent>(entity);
			const std::pair borrowed(mesh.Mesh, mesh.MeshIndex);
			if (auto it = id ? Bodies.find(id->ID) : Bodies.end(); it != Bodies.end() && it->second.BorrowedMesh == borrowed)
				return;
			if (auto it = id ? Characters.find(id->ID) : Characters.end(); it != Characters.end() && it->second.BorrowedMesh == borrowed)
				return;
			OnMeshChanged(registry, entity);
		}

		void OnCharacterChanged(entt::registry& registry, entt::entity entity)
		{
			if (const auto* id = registry.try_get<IDComponent>(entity))
				DirtyCharacters.insert(id->ID);
		}

		void OnJointChanged(entt::registry& registry, entt::entity entity)
		{
			if (const auto* id = registry.try_get<IDComponent>(entity))
				DirtyJoints.insert(id->ID);
		}

		void OnJointDestroyed(entt::registry& registry, entt::entity entity)
		{
			if (const auto* id = registry.try_get<IDComponent>(entity))
			{
				RemoveJoint(id->ID);
				DirtyJoints.erase(id->ID);
				LoggedJointWarnings.erase(id->ID);
			}
		}
	};

	PhysicsWorld::PhysicsWorld(Scene* scene)
		: m_Impl(CreateScope<Impl>())
		, m_Scene(scene)
	{
		AcquireJolt();

		constexpr uint32_t MaxBodies = 65536;
		constexpr uint32_t NumBodyMutexes = 0; // Jolt default
		constexpr uint32_t MaxBodyPairs = 65536;
		constexpr uint32_t MaxContactConstraints = 16384;

		m_Impl->TempAllocator = CreateScope<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
		const int threadCount = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
		m_Impl->JobSystem = CreateScope<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threadCount);

		if (const auto& layers = scene->GetPhysicsLayers())
			m_Impl->Layers = *layers;
		else if (const Ref<Project>& project = Project::GetActive())
			m_Impl->Layers = project->GetConfig().Physics;
		m_Impl->ObjectLayerPairFilter.SetLayers(m_Impl->Layers);

		m_Impl->System = CreateScope<JPH::PhysicsSystem>();
		m_Impl->System->Init(MaxBodies, NumBodyMutexes, MaxBodyPairs, MaxContactConstraints,
							 m_Impl->BroadPhaseLayerInterface, m_Impl->ObjectVsBroadPhaseLayerFilter, m_Impl->ObjectLayerPairFilter);
		m_Impl->ContactListener.Materials.resize(MaxBodies);
		m_Impl->ContactListener.MinVelocityForRestitution = m_Impl->System->GetPhysicsSettings().mMinVelocityForRestitution;
		m_Impl->System->SetContactListener(&m_Impl->ContactListener);
		// Bodies joined by a joint do not collide with each other unless the joint asks for it.
		m_Impl->System->SetSimCollideBodyVsBody([impl = m_Impl.get()](const JPH::Body& body1, const JPH::Body& body2, JPH::Mat44Arg transform1, JPH::Mat44Arg transform2,
																	  JPH::CollideShapeSettings& settings, JPH::CollideShapeCollector& collector, const JPH::ShapeFilter& filter) {
			if (!impl->IgnoredPairs.empty())
			{
				const uint64_t a = body1.GetUserData();
				const uint64_t b = body2.GetUserData();
				if (impl->IgnoredPairs.contains({ std::min(a, b), std::max(a, b) }))
					return;
			}
			JPH::PhysicsSystem::sDefaultSimCollideBodyVsBody(body1, body2, transform1, transform2, settings, collector, filter);
		});
		SetGravity(scene->GetPhysicsSettings().Gravity);

		entt::registry& registry = scene->GetRegistry();
		Impl& impl = *m_Impl;
		registry.on_construct<RigidBodyComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<RigidBodyComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<RigidBodyComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<BoxColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<BoxColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<BoxColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<SphereColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<SphereColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<SphereColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<CapsuleColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<CapsuleColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<CapsuleColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<MeshColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<MeshColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<MeshColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<MeshComponent>().connect<&Impl::OnMeshChanged>(impl);
		registry.on_destroy<MeshComponent>().connect<&Impl::OnMeshChanged>(impl);
		registry.on_update<MeshComponent>().connect<&Impl::OnMeshUpdated>(impl);
		// Adding or removing a controller swaps a body for a character, so it rebuilds like a collider change.
		registry.on_construct<CharacterControllerComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<CharacterControllerComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<CharacterControllerComponent>().connect<&Impl::OnCharacterChanged>(impl);
		registry.on_construct<JointComponent>().connect<&Impl::OnJointChanged>(impl);
		registry.on_update<JointComponent>().connect<&Impl::OnJointChanged>(impl);
		registry.on_destroy<JointComponent>().connect<&Impl::OnJointDestroyed>(impl);
	}

	PhysicsWorld::~PhysicsWorld()
	{
		entt::registry& registry = m_Scene->GetRegistry();
		Impl& impl = *m_Impl;
		registry.on_construct<RigidBodyComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<RigidBodyComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<RigidBodyComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<BoxColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<BoxColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<BoxColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<SphereColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<SphereColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<SphereColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<CapsuleColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<CapsuleColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<CapsuleColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<MeshColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<MeshColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<MeshColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<MeshComponent>().disconnect<&Impl::OnMeshChanged>(impl);
		registry.on_destroy<MeshComponent>().disconnect<&Impl::OnMeshChanged>(impl);
		registry.on_update<MeshComponent>().disconnect<&Impl::OnMeshUpdated>(impl);
		registry.on_construct<CharacterControllerComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<CharacterControllerComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<CharacterControllerComponent>().disconnect<&Impl::OnCharacterChanged>(impl);
		registry.on_construct<JointComponent>().disconnect<&Impl::OnJointChanged>(impl);
		registry.on_update<JointComponent>().disconnect<&Impl::OnJointChanged>(impl);
		registry.on_destroy<JointComponent>().disconnect<&Impl::OnJointDestroyed>(impl);

		for (auto& [uuid, joint] : m_Impl->Joints)
			m_Impl->System->RemoveConstraint(joint.Constraint);
		m_Impl->Joints.clear();
		// Characters remove their inner bodies, so they go while the system exists.
		m_Impl->Characters.clear();
		JPH::BodyInterface& bodies = m_Impl->System->GetBodyInterface();
		for (auto& [uuid, record] : m_Impl->Bodies)
		{
			bodies.RemoveBody(record.ID);
			bodies.DestroyBody(record.ID);
		}
		m_Impl->Bodies.clear();
		m_Impl->System.reset();
		m_Impl->JobSystem.reset();
		m_Impl->TempAllocator.reset();
		m_Impl.reset();

		ReleaseJolt();
	}

	void PhysicsWorld::Start()
	{
		auto view = m_Scene->GetAllEntitiesWith<RigidBodyComponent>();
		m_Impl->Starting = true;
		for (entt::entity handle : view)
			RecreateBody({ handle, m_Scene });
		for (entt::entity handle : m_Scene->GetAllEntitiesWith<CharacterControllerComponent>())
			RecreateCharacter({ handle, m_Scene });
		m_Impl->Starting = false;
		m_Impl->DirtyEntities.clear();
		m_Impl->DirtyCharacters.clear();
		// Joints need both of their bodies, so they are built after every body exists.
		for (entt::entity handle : m_Scene->GetAllEntitiesWith<JointComponent>())
			m_Impl->DirtyJoints.insert(Entity(handle, m_Scene).GetUUID());
		RebuildDirtyJoints();
		m_Impl->System->OptimizeBroadPhase();
	}

	void PhysicsWorld::RecreateBody(Entity entity)
	{
		Impl& impl = *m_Impl;
		JPH::BodyInterface& bodies = impl.System->GetBodyInterface();
		JPH::Vec3 previousLinearVelocity = JPH::Vec3::sZero();
		JPH::Vec3 previousAngularVelocity = JPH::Vec3::sZero();

		if (entity)
		{
			auto existing = impl.Bodies.find(entity.GetUUID());
			if (existing != impl.Bodies.end())
			{
				// Rebuilding (e.g. after a collider or rigid body change) keeps the body moving.
				previousLinearVelocity = bodies.GetLinearVelocity(existing->second.ID);
				previousAngularVelocity = bodies.GetAngularVelocity(existing->second.ID);
				impl.RemoveBody(m_Scene, entity.GetUUID());
			}
		}

		// A character controller replaces the body (RecreateCharacter reports the ignored RigidBody).
		if (!entity || !entity.HasComponent<RigidBodyComponent>() || entity.HasComponent<CharacterControllerComponent>())
			return;

		const auto& rigidBody = entity.GetComponent<RigidBodyComponent>();
		glm::vec3 position;
		glm::quat rotation;
		glm::vec3 scale;
		if (!Math::DecomposeTransform(m_Scene->GetWorldTransform(entity), position, rotation, scale))
		{
			BS_CORE_WARN("Physics: entity '{}' has a degenerate transform; no body created", entity.GetName());
			return;
		}

		JPH::EMotionType motionType = JPH::EMotionType::Static;
		if (rigidBody.Type == RigidBodyType::Dynamic)
			motionType = JPH::EMotionType::Dynamic;
		else if (rigidBody.Type == RigidBodyType::Kinematic)
			motionType = JPH::EMotionType::Kinematic;

		// Problems with the body's settings, joined with "; ". Logged when they differ from the last ones
		// logged for this entity, so a script that sets the component every frame does not flood the log.
		std::string warnings;
		auto warn = [&warnings](const std::string& warning) {
			warnings += (warnings.empty() ? "" : "; ") + warning;
		};
		auto reportWarnings = [&]() {
			if (warnings.empty())
			{
				impl.LoggedBodyWarnings.erase(entity.GetUUID());
			}
			else if (std::string& logged = impl.LoggedBodyWarnings[entity.GetUUID()]; logged != warnings)
			{
				logged = warnings;
				BS_CORE_WARN("Physics: entity '{}': {}", entity.GetName(), warnings);
			}
		};

		// Jolt cannot simulate a triangle mesh on a dynamic body, so those always use the convex hull.
		const ColliderShape colliders = BuildColliderShape(entity, scale, 1.0f, motionType == JPH::EMotionType::Dynamic ? "a dynamic body" : nullptr, rigidBody.IsTrigger, warn);
		if (!colliders.Shape)
		{
			warn("no body created");
			reportWarnings();
			return;
		}
		const bool hasTriangleMesh = colliders.HasTriangleMesh;

		const bool moving = motionType != JPH::EMotionType::Static;
		uint32_t layer = 0;
		if (const auto index = impl.Layers.Find(rigidBody.Layer))
			layer = *index;
		else
			warn("unknown physics layer '" + rigidBody.Layer + "', using Default");
		JPH::BodyCreationSettings settings(colliders.Shape, ToJolt(position), ToJolt(rotation), motionType, MakeObjectLayer(moving, layer));
		settings.mUserData = static_cast<uint64_t>(entity.GetUUID());
		// The combine modes pass these on unchecked: negative friction inverts Jolt's friction clamp and
		// restitution above 1 adds energy on every bounce.
		settings.mFriction = std::max(rigidBody.Friction, 0.0f);
		if (rigidBody.Friction < 0.0f)
			warn("Friction must not be negative, using 0");
		settings.mRestitution = std::clamp(rigidBody.Restitution, 0.0f, 1.0f);
		if (rigidBody.Restitution < 0.0f || rigidBody.Restitution > 1.0f)
			warn("Restitution must be between 0 and 1, clamped");
		settings.mLinearDamping = rigidBody.LinearDamping;
		settings.mAngularDamping = rigidBody.AngularDamping;
		settings.mGravityFactor = rigidBody.GravityFactor;
		settings.mIsSensor = rigidBody.IsTrigger;
		// Kinematic triggers must also see static bodies; kinematic bodies are otherwise skipped vs static.
		settings.mCollideKinematicVsNonDynamic = rigidBody.IsTrigger;
		if (rigidBody.FixedRotation && motionType == JPH::EMotionType::Dynamic)
			settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
		// Jolt only sweeps dynamic, non-sensor bodies.
		if (rigidBody.Continuous)
		{
			if (motionType == JPH::EMotionType::Dynamic && !rigidBody.IsTrigger)
				settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
			else
				warn("Continuous only affects dynamic bodies that are not triggers");
		}
		reportWarnings();
		// A triangle mesh has no volume, so Jolt cannot derive the mass a kinematic body still needs; a solid
		// box of the shape's bounds stands in (kinematic bodies are not moved by forces, only its presence counts).
		if (motionType == JPH::EMotionType::Kinematic && hasTriangleMesh)
		{
			const JPH::AABox bounds = colliders.Shape->GetLocalBounds();
			settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
			settings.mMassPropertiesOverride.SetMassAndInertiaOfSolidBox(JPH::Vec3::sMax(bounds.GetSize(), JPH::Vec3::sReplicate(MinExtent)), 1.0f);
			settings.mMassPropertiesOverride.ScaleToMass(std::max(rigidBody.Mass, 0.001f));
		}
		if (motionType == JPH::EMotionType::Dynamic)
		{
			settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
			settings.mMassPropertiesOverride.mMass = std::max(rigidBody.Mass, 0.001f);
		}

		const JPH::EActivation activation = moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
		const JPH::BodyID bodyID = bodies.CreateAndAddBody(settings, activation);
		if (bodyID.IsInvalid())
		{
			BS_CORE_ERROR("Physics: body limit reached; no body created for '{}'", entity.GetName());
			return;
		}

		impl.ContactListener.Materials[bodyID.GetIndex()] = { rigidBody.FrictionCombine, rigidBody.RestitutionCombine };

		Impl::BodyRecord record;
		record.ID = bodyID;
		record.Type = rigidBody.Type;
		record.IsTrigger = rigidBody.IsTrigger;
		record.BorrowedMesh = colliders.BorrowedMesh;
		record.LastPosition = position;
		record.LastRotation = rotation;
		impl.Bodies[entity.GetUUID()] = record;
		impl.BodyToEntity[bodyID.GetIndexAndSequenceNumber()] = entity.GetUUID();
		if (motionType == JPH::EMotionType::Dynamic)
		{
			bodies.SetLinearVelocity(bodyID, previousLinearVelocity);
			bodies.SetAngularVelocity(bodyID, previousAngularVelocity);
		}
		// Joints that could not be built without this body (or were removed with its old one) can be now.
		if (!impl.Starting)
			MarkJointsDirty(entity.GetUUID());
	}

	void PhysicsWorld::RecreateCharacter(Entity entity)
	{
		Impl& impl = *m_Impl;
		JPH::Vec3 previousVelocity = JPH::Vec3::sZero();
		glm::vec3 moveVelocity(0.0f);
		if (entity)
		{
			auto existing = impl.Characters.find(entity.GetUUID());
			if (existing != impl.Characters.end())
			{
				// Rebuilding (e.g. after a collider change) keeps the character moving.
				previousVelocity = existing->second.Character->GetLinearVelocity();
				moveVelocity = existing->second.MoveVelocity;
				impl.RemoveCharacter(m_Scene, entity.GetUUID());
			}
		}
		if (!entity || !entity.HasComponent<CharacterControllerComponent>())
			return;

		const auto& controller = entity.GetComponent<CharacterControllerComponent>();
		// Problems with the settings, joined with "; " and logged when they differ from the last ones logged.
		std::string warnings;
		auto warn = [&warnings](const std::string& warning) {
			warnings += (warnings.empty() ? "" : "; ") + warning;
		};
		auto report = [&]() { impl.ReportCharacterWarnings(entity, warnings); };
		if (entity.HasComponent<RigidBodyComponent>())
			warn("its RigidBody is ignored (the character controller replaces it)");

		glm::vec3 position;
		glm::quat rotation;
		glm::vec3 scale;
		if (!Math::DecomposeTransform(m_Scene->GetWorldTransform(entity), position, rotation, scale))
		{
			warn("degenerate transform; no character created");
			report();
			return;
		}
		// The collider shapes, scaled as for a rigid body. The inner body that other bodies collide with is a
		// little smaller (as in Jolt's samples): the character stops just short of what it walks into, so a
		// full-size inner body would touch it and shove dynamic bodies regardless of MaxStrength. Characters
		// move by shape casts, which need a convex mesh collider.
		constexpr float InnerShapeFraction = 0.9f;
		const ColliderShape colliders = BuildColliderShape(entity, scale, 1.0f, "a character", false, warn);
		if (!colliders.Shape)
		{
			warn("no character created");
			report();
			return;
		}
		// Same colliders, so its warnings would only repeat the outer shape's.
		const ColliderShape inner = BuildColliderShape(entity, scale, InnerShapeFraction, "a character", false, [](const std::string&) {});
		if (!inner.Shape)
		{
			warn("failed to build its inner shape; no character created");
			report();
			return;
		}
		const JPH::RefConst<JPH::Shape> shape = colliders.Shape;

		uint32_t layer = 0;
		if (const auto index = impl.Layers.Find(controller.Layer))
			layer = *index;
		else
			warn("unknown physics layer '" + controller.Layer + "', using Default");

		JPH::CharacterVirtualSettings settings;
		settings.mShape = shape;
		settings.mInnerBodyShape = inner.Shape;
		settings.mInnerBodyLayer = MakeObjectLayer(true, layer);
		const glm::vec3 gravity = FromJolt(impl.System->GetGravity());
		settings.mUp = glm::length(gravity) > 1e-6f ? ToJolt(-glm::normalize(gravity)) : JPH::Vec3::sAxisY();
		auto* character = new JPH::CharacterVirtual(&settings, ToJolt(position), ToJolt(rotation), static_cast<uint64_t>(entity.GetUUID()), impl.System.get());
		character->SetLinearVelocity(previousVelocity);

		Impl::CharacterRecord& record = impl.Characters[entity.GetUUID()];
		record.Character = character;
		record.Settings = controller;
		record.Layer = layer;
		record.BorrowedMesh = colliders.BorrowedMesh;
		record.BuildWarnings = warnings;
		record.MoveVelocity = moveVelocity;
		record.LastPosition = position;
		record.LastRotation = rotation;
		// The shape's bounds relative to the character's position (Jolt's local bounds are around the centre of mass).
		record.Bounds = shape->GetLocalBounds();
		record.Bounds.Translate(shape->GetCenterOfMass());
		Impl::UpdateSupportingVolume(record);
		// Jolt leaves the inner body out when the body limit is reached; the character still walks, but
		// queries, triggers and other bodies cannot see it.
		if (!character->GetInnerBodyID().IsInvalid())
		{
			impl.BodyToEntity[character->GetInnerBodyID().GetIndexAndSequenceNumber()] = entity.GetUUID();
			// Jolt created the inner body, not RecreateBody: its slot may still hold an earlier body's modes.
			impl.ContactListener.Materials[character->GetInnerBodyID().GetIndex()] = {};
		}
		else
			BS_CORE_ERROR("Physics: body limit reached; character '{}' has no body other bodies can touch", entity.GetName());
		Impl::ConfigureCharacter(record, warn);
		report();

		// A new character starts in the air; find its ground now so a rebuild (e.g. a crouch changing the
		// collider) does not lose a step of IsGrounded or a jump.
		const JPH::ObjectLayer objectLayer = MakeObjectLayer(true, layer);
		character->RefreshContacts(JPH::DefaultBroadPhaseLayerFilter(impl.ObjectVsBroadPhaseLayerFilter, objectLayer), JPH::DefaultObjectLayerFilter(impl.ObjectLayerPairFilter, objectLayer), JPH::BodyFilter(), JPH::ShapeFilter(),
								   *impl.TempAllocator);
	}

	void PhysicsWorld::ApplyCharacterSettings(Entity entity)
	{
		Impl& impl = *m_Impl;
		auto it = impl.Characters.find(entity.GetUUID());
		if (!entity.HasComponent<CharacterControllerComponent>())
			return;
		const auto& controller = entity.GetComponent<CharacterControllerComponent>();
		// A character that could not be built may be buildable now; the layer is baked into the inner body.
		if (it == impl.Characters.end() || controller.Layer != it->second.Settings.Layer)
		{
			RecreateCharacter(entity);
			return;
		}
		Impl::CharacterRecord& record = it->second;
		// Scripts may set the component every frame; the same values change nothing.
		if (controller == record.Settings)
			return;
		record.Settings = controller;
		std::string warnings = record.BuildWarnings;
		Impl::ConfigureCharacter(record, [&warnings](const std::string& warning) {
			warnings += (warnings.empty() ? "" : "; ") + warning;
		});
		impl.ReportCharacterWarnings(entity, warnings);
	}

	void PhysicsWorld::MarkJointsDirty(UUID uuid)
	{
		for (entt::entity handle : m_Scene->GetAllEntitiesWith<JointComponent>())
		{
			Entity entity(handle, m_Scene);
			const JointComponent& joint = entity.GetComponent<JointComponent>();
			if (JointBody(entity.GetUUID(), joint) == uuid || joint.ConnectedEntity == uuid)
				m_Impl->DirtyJoints.insert(entity.GetUUID());
		}
	}

	void PhysicsWorld::OnEntityDestroyed(Entity entity)
	{
		m_Impl->RemoveBody(m_Scene, entity.GetUUID());
		m_Impl->RemoveCharacter(m_Scene, entity.GetUUID());
		m_Impl->RemoveJoint(entity.GetUUID());
		m_Impl->DirtyEntities.erase(entity.GetUUID());
		m_Impl->DirtyCharacters.erase(entity.GetUUID());
		m_Impl->LoggedCharacterWarnings.erase(entity.GetUUID());
		m_Impl->DirtyJoints.erase(entity.GetUUID());
		m_Impl->LoggedJointWarnings.erase(entity.GetUUID());
		m_Impl->LoggedBodyWarnings.erase(entity.GetUUID());
	}

	void PhysicsWorld::RebuildDirtyJoints()
	{
		Impl& impl = *m_Impl;
		if (impl.DirtyJoints.empty())
			return;
		// Walk the registry rather than the (unordered) dirty set so constraints are added in a stable
		// order: Jolt solves them in that order, and replays must not depend on hash-set iteration.
		for (entt::entity handle : m_Scene->GetAllEntitiesWith<JointComponent>())
		{
			Entity entity(handle, m_Scene);
			// An entity destroyed during this frame's scripts is still here until the end of the frame.
			if (!impl.DirtyJoints.contains(entity.GetUUID()) || m_Scene->IsEntityPendingDestruction(entity))
				continue;
			// Scripts drive motors by setting the component every frame. Rebuilding would reset the
			// constraint's rest pose and warm start, so only structural changes rebuild it.
			auto existing = impl.Joints.find(entity.GetUUID());
			const JointComponent& joint = entity.GetComponent<JointComponent>();
			std::vector<std::string> warnings;
			if (existing != impl.Joints.end() && !NeedsRebuild(existing->second.Settings, joint))
			{
				// Rewriting the same values must not wake the bodies.
				if (joint == existing->second.Settings)
					continue;
				warnings = existing->second.BuildWarnings;
				ApplyJointSettings(entity, warnings);
			}
			else
			{
				CreateJoint(entity, warnings);
			}
			ReportJointWarnings(entity, std::move(warnings));
		}
		impl.DirtyJoints.clear();
		impl.UpdateIgnoredPairs();
	}

	void PhysicsWorld::ReportJointWarnings(Entity entity, std::vector<std::string> warnings)
	{
		// Messages name the offending values, so the same messages mean the same problem.
		auto& logged = m_Impl->LoggedJointWarnings;
		auto it = logged.find(entity.GetUUID());
		if (warnings.empty())
		{
			if (it != logged.end())
				logged.erase(it);
			return;
		}
		if (it != logged.end() && it->second == warnings)
			return;
		for (const std::string& warning : warnings)
			BS_CORE_WARN("Physics: joint on '{}': {}", entity.GetName(), warning);
		logged[entity.GetUUID()] = std::move(warnings);
	}

	void PhysicsWorld::ApplyJointSettings(Entity entity, std::vector<std::string>& warnings)
	{
		Impl::JointRecord& record = m_Impl->Joints.at(entity.GetUUID());
		const JointComponent& joint = entity.GetComponent<JointComponent>();
		record.Settings = joint;

		ReportIgnoredFields(joint, warnings);
		const auto [limitMin, limitMax] = SanitizeLimits(joint, warnings);
		const JPH::SpringSettings limitSpring = SanitizeLimitSpring(joint, warnings);
		if (joint.MotorMaxForce < 0.0f)
			warnings.emplace_back(fmt::format("MotorMaxForce {} is negative; using 0", joint.MotorMaxForce));
		const JPH::EMotorState motorState = ToJoltMotorState(joint.MotorMode);
		const float motorLimit = std::max(joint.MotorMaxForce, 0.0f);
		const JPH::SpringSettings motorSpring = SanitizeMotorSpring(joint, warnings);

		switch (joint.Type)
		{
			case JointType::Hinge:
			{
				BS_CORE_ASSERT(record.Constraint->GetSubType() == JPH::EConstraintSubType::Hinge, "joint record out of sync with its constraint");
				auto* hinge = static_cast<JPH::HingeConstraint*>(record.Constraint.GetPtr());
				if (joint.UseLimits)
					hinge->SetLimits(glm::radians(limitMin), glm::radians(limitMax));
				hinge->SetLimitsSpringSettings(limitSpring);
				hinge->GetMotorSettings().SetTorqueLimit(motorLimit);
				hinge->GetMotorSettings().mSpringSettings = motorSpring;
				hinge->SetMotorState(motorState);
				if (motorState == JPH::EMotorState::Velocity)
				{
					hinge->SetTargetAngularVelocity(glm::radians(joint.MotorTarget));
				}
				else if (motorState == JPH::EMotorState::Position)
				{
					if (joint.MotorTarget < -180.0f || joint.MotorTarget > 180.0f)
						warnings.emplace_back("hinge position motor targets must be within [-180, 180] degrees; clamped");
					hinge->SetTargetAngle(glm::radians(std::clamp(joint.MotorTarget, -180.0f, 180.0f)));
				}
				break;
			}
			case JointType::Slider:
			{
				BS_CORE_ASSERT(record.Constraint->GetSubType() == JPH::EConstraintSubType::Slider, "joint record out of sync with its constraint");
				auto* slider = static_cast<JPH::SliderConstraint*>(record.Constraint.GetPtr());
				if (joint.UseLimits)
					slider->SetLimits(limitMin, limitMax);
				slider->SetLimitsSpringSettings(limitSpring);
				slider->GetMotorSettings().SetForceLimit(motorLimit);
				slider->GetMotorSettings().mSpringSettings = motorSpring;
				slider->SetMotorState(motorState);
				if (motorState == JPH::EMotorState::Velocity)
					slider->SetTargetVelocity(joint.MotorTarget);
				else if (motorState == JPH::EMotorState::Position)
					slider->SetTargetPosition(joint.MotorTarget);
				break;
			}
			case JointType::Distance:
			{
				BS_CORE_ASSERT(record.Constraint->GetSubType() == JPH::EConstraintSubType::Distance, "joint record out of sync with its constraint");
				auto* distance = static_cast<JPH::DistanceConstraint*>(record.Constraint.GetPtr());
				if (joint.UseLimits)
					distance->SetDistance(limitMin, limitMax);
				distance->SetLimitsSpringSettings(limitSpring);
				break;
			}
			case JointType::Cone:
				BS_CORE_ASSERT(record.Constraint->GetSubType() == JPH::EConstraintSubType::Cone, "joint record out of sync with its constraint");
				// Jolt requires at most pi, which glm::radians(180) can round past.
				static_cast<JPH::ConeConstraint*>(record.Constraint.GetPtr())->SetHalfConeAngle(joint.UseLimits ? std::min(glm::radians(limitMax), JPH::JPH_PI) : JPH::JPH_PI);
				break;
			case JointType::SixDOF:
			{
				BS_CORE_ASSERT(record.Constraint->GetSubType() == JPH::EConstraintSubType::SixDOF, "joint record out of sync with its constraint");
				auto* sixDOF = static_cast<JPH::SixDOFConstraint*>(record.Constraint.GetPtr());
				const SixDOFLimits limits = SanitizeSixDOFLimits(joint, warnings);
				sixDOF->SetTranslationLimits(ToJolt(limits.LinearMin), ToJolt(limits.LinearMax));
				sixDOF->SetRotationLimits(ToJolt(limits.AngularMin), ToJolt(limits.AngularMax));
				// Jolt softens translation limits only; rotation limits stay rigid. A spring on a locked axis would
				// make the lock soft too, so only limited axes get it.
				using EAxis = JPH::SixDOFConstraint::EAxis;
				bool softened = false;
				for (int i = 0; i < 3; i++)
				{
					const auto axis = static_cast<EAxis>(EAxis::TranslationX + i);
					const bool limited = !sixDOF->IsFixedAxis(axis) && !sixDOF->IsFreeAxis(axis);
					sixDOF->SetLimitsSpringSettings(axis, limited ? limitSpring : JPH::SpringSettings());
					softened |= limited;
				}
				if (limitSpring.HasStiffness() && joint.UseLimits && !softened)
					warnings.emplace_back("LimitSpringFrequency has no effect on a six-DOF joint without a limited translation axis (rotation limits and locked or free axes stay rigid)");

				// Per-axis motors in the joint frame. Jolt keeps one target vector per kind, so each axis takes
				// its own component and the rest are 0; the Position rotation axes share one target orientation.
				glm::vec3 linearVelocity(0.0f);
				glm::vec3 linearPosition(0.0f);
				glm::vec3 angularVelocity(0.0f);
				glm::vec3 angularPosition(0.0f);
				for (int i = 0; i < 3; i++)
				{
					const auto linearAxis = static_cast<EAxis>(EAxis::TranslationX + i);
					const auto angularAxis = static_cast<EAxis>(EAxis::RotationX + i);
					// Jolt skips motors only when a whole group (translation or rotation) is locked; a motor on one
					// locked axis would fight the lock, so it stays off.
					JointMotorMode linearMode = joint.LinearMotorMode[i];
					JointMotorMode angularMode = joint.AngularMotorMode[i];
					if (linearMode != JointMotorMode::Off && sixDOF->IsFixedAxis(linearAxis))
					{
						warnings.emplace_back(fmt::format("six-DOF linear {} motor has no effect: the axis is locked{}", s_AxisNames[i], joint.UseLimits ? "" : " (translation is locked without UseLimits)"));
						linearMode = JointMotorMode::Off;
					}
					if (angularMode != JointMotorMode::Off && sixDOF->IsFixedAxis(angularAxis))
					{
						warnings.emplace_back(fmt::format("six-DOF angular {} motor has no effect: the axis is locked", s_AxisNames[i]));
						angularMode = JointMotorMode::Off;
					}
					if (linearMode == JointMotorMode::Velocity)
						linearVelocity[i] = joint.LinearMotorTarget[i];
					else if (linearMode == JointMotorMode::Position)
					{
						// Jolt clamps only the orientation target; past a limit the motor would push against it at
						// MotorMaxForce indefinitely.
						linearPosition[i] = joint.LinearMotorTarget[i];
						if (!sixDOF->IsFreeAxis(linearAxis))
							linearPosition[i] = std::clamp(linearPosition[i], limits.LinearMin[i], limits.LinearMax[i]);
					}
					if (angularMode == JointMotorMode::Velocity)
						angularVelocity[i] = joint.AngularMotorTarget[i];
					else if (angularMode == JointMotorMode::Position)
						angularPosition[i] = joint.AngularMotorTarget[i];

					for (const auto axis : { linearAxis, angularAxis })
					{
						JPH::MotorSettings& motor = sixDOF->GetMotorSettings(axis);
						motor.SetForceLimit(motorLimit);
						motor.SetTorqueLimit(motorLimit);
						motor.mSpringSettings = motorSpring;
					}
					sixDOF->SetMotorState(linearAxis, ToJoltMotorState(linearMode));
					sixDOF->SetMotorState(angularAxis, ToJoltMotorState(angularMode));
				}
				sixDOF->SetTargetVelocityCS(ToJolt(linearVelocity));
				sixDOF->SetTargetPositionCS(ToJolt(linearPosition));
				sixDOF->SetTargetAngularVelocityCS(ToJolt(glm::radians(angularVelocity)));
				// Jolt clamps the orientation to the rotation limits.
				sixDOF->SetTargetOrientationCS(ToJolt(Math::QuatFromEuler(glm::radians(angularPosition))));
				break;
			}
			case JointType::Fixed:
			case JointType::Point:
				break;
		}

		// Sleeping bodies would ignore a new motor target or limit.
		JPH::BodyInterface& bodies = m_Impl->System->GetBodyInterface();
		for (const JPH::Body* body : { record.Constraint->GetBody1(), record.Constraint->GetBody2() })
		{
			if (body && !body->IsStatic())
				bodies.ActivateBody(body->GetID());
		}
	}

	void PhysicsWorld::CheckBrokenJoints(float fixedStep)
	{
		Impl& impl = *m_Impl;
		if (impl.Joints.empty())
			return;

		// Constraint lambdas are the impulses applied during the last step; divided by the step they are
		// the force (N) and torque (N·m) the joint needed to hold. Hinge and slider limits and motors act
		// along the joint's free axis, so they add to the force or torque the joint carries.
		struct BrokenJoint
		{
			UUID Owner;
			float Force = 0.0f;
			float Torque = 0.0f;
		};
		auto exceeds = [](float value, float limit) { return limit > 0.0f && value > limit; };
		auto combine = [](float perpendicular, float axial) { return std::sqrt(perpendicular * perpendicular + axial * axial); };
		std::vector<BrokenJoint> broken;
		for (entt::entity handle : m_Scene->GetAllEntitiesWith<JointComponent>())
		{
			Entity entity(handle, m_Scene);
			auto it = impl.Joints.find(entity.GetUUID());
			if (it == impl.Joints.end())
				continue;
			const Impl::JointRecord& record = it->second;
			const float breakForce = record.Settings.BreakForce;
			const float breakTorque = record.Settings.BreakTorque;
			if (breakForce <= 0.0f && breakTorque <= 0.0f)
				continue;

			float forceImpulse = 0.0f;
			float torqueImpulse = 0.0f;
			const JPH::Constraint* constraint = record.Constraint.GetPtr();
			switch (record.Settings.Type)
			{
				case JointType::Fixed:
				{
					const auto* fixed = static_cast<const JPH::FixedConstraint*>(constraint);
					forceImpulse = fixed->GetTotalLambdaPosition().Length();
					torqueImpulse = fixed->GetTotalLambdaRotation().Length();
					break;
				}
				case JointType::Point:
					forceImpulse = static_cast<const JPH::PointConstraint*>(constraint)->GetTotalLambdaPosition().Length();
					break;
				case JointType::Hinge:
				{
					const auto* hinge = static_cast<const JPH::HingeConstraint*>(constraint);
					forceImpulse = hinge->GetTotalLambdaPosition().Length();
					torqueImpulse = combine(hinge->GetTotalLambdaRotation().Length(), hinge->GetTotalLambdaRotationLimits() + hinge->GetTotalLambdaMotor());
					break;
				}
				case JointType::Slider:
				{
					const auto* slider = static_cast<const JPH::SliderConstraint*>(constraint);
					forceImpulse = combine(slider->GetTotalLambdaPosition().Length(), slider->GetTotalLambdaPositionLimits() + slider->GetTotalLambdaMotor());
					torqueImpulse = slider->GetTotalLambdaRotation().Length();
					break;
				}
				case JointType::Distance:
					forceImpulse = std::abs(static_cast<const JPH::DistanceConstraint*>(constraint)->GetTotalLambdaPosition());
					break;
				case JointType::Cone:
				{
					const auto* cone = static_cast<const JPH::ConeConstraint*>(constraint);
					forceImpulse = cone->GetTotalLambdaPosition().Length();
					torqueImpulse = std::abs(cone->GetTotalLambdaRotation());
					break;
				}
				case JointType::SixDOF:
				{
					// Motors only run while a group is partly free, and then both lambdas are per joint-frame axis, so
					// they add component-wise (like a hinge's limit and motor lambdas).
					const auto* sixDOF = static_cast<const JPH::SixDOFConstraint*>(constraint);
					forceImpulse = (sixDOF->GetTotalLambdaPosition() + sixDOF->GetTotalLambdaMotorTranslation()).Length();
					torqueImpulse = (sixDOF->GetTotalLambdaRotation() + sixDOF->GetTotalLambdaMotorRotation()).Length();
					break;
				}
			}
			const float force = forceImpulse / fixedStep;
			const float torque = torqueImpulse / fixedStep;
			if (exceeds(force, breakForce) || exceeds(torque, breakTorque))
				broken.push_back({ entity.GetUUID(), force, torque });
		}

		// A broken joint is gone for good: its component is removed, so the scene state (and scene.hash)
		// records the break and replays stay consistent.
		ScriptEngine* scriptEngine = m_Scene->GetScriptEngine();
		for (const BrokenJoint& joint : broken)
		{
			// An earlier OnJointBreak callback may have removed this joint or its entity.
			Entity entity = m_Scene->GetEntityByUUID(joint.Owner);
			auto it = impl.Joints.find(joint.Owner);
			if (!entity || it == impl.Joints.end() || !entity.HasComponent<JointComponent>())
				continue;
			const JointComponent& settings = it->second.Settings;
			// The measured values help tune thresholds.
			BS_CORE_INFO("Physics: joint on '{}' broke (force {:.1f} N, BreakForce {}; torque {:.1f} N·m, BreakTorque {})", entity.GetName(), joint.Force, settings.BreakForce, joint.Torque, settings.BreakTorque);
			const UUID connectedID = settings.ConnectedEntity;
			const UUID bodyID = it->second.Body;
			entity.RemoveComponent<JointComponent>();
			if (scriptEngine)
				scriptEngine->OnJointBroken(entity, m_Scene->GetEntityByUUID(bodyID), m_Scene->GetEntityByUUID(connectedID));
		}
	}

	void PhysicsWorld::CreateJoint(Entity entity, std::vector<std::string>& warnings)
	{
		Impl& impl = *m_Impl;
		impl.RemoveJoint(entity.GetUUID());
		const JointComponent& joint = entity.GetComponent<JointComponent>();

		// A RigidBody without a valid collider has no body either; say which part is missing.
		auto describeMissingBody = [](Entity e) {
			if (e.HasComponent<CharacterControllerComponent>())
				return "is a character (a CharacterController replaces its body; joints need a RigidBody)";
			return e.HasComponent<RigidBodyComponent>() ? "has a RigidBody but no valid collider" : "has no RigidBody";
		};
		// The joint moves BodyEntity's body; its anchor and axes are in that entity's local space. The holder
		// itself always exists, so only a BodyEntity can be missing.
		Entity bodyEntity = m_Scene->GetEntityByUUID(JointBody(entity.GetUUID(), joint));
		if (!bodyEntity)
		{
			warnings.emplace_back(fmt::format("not built: its BodyEntity {} is missing", static_cast<uint64_t>(joint.BodyEntity)));
			return;
		}
		auto self = impl.Bodies.find(bodyEntity.GetUUID());
		if (self == impl.Bodies.end())
		{
			if (bodyEntity == entity)
				warnings.emplace_back(fmt::format("not built: the entity {}", describeMissingBody(entity)));
			else
				warnings.emplace_back(fmt::format("not built: its BodyEntity '{}' {}", bodyEntity.GetName(), describeMissingBody(bodyEntity)));
			return;
		}

		JPH::BodyID bodyIDs[2] = { JPH::BodyID(), self->second.ID };
		RigidBodyType connectedType = RigidBodyType::Static;
		Entity connected;
		if (joint.ConnectedEntity != 0)
		{
			connected = m_Scene->GetEntityByUUID(joint.ConnectedEntity);
			if (!connected || connected == bodyEntity)
			{
				warnings.emplace_back(fmt::format("not built: it connects to {} entity {}", connected ? "its own body's" : "a missing", static_cast<uint64_t>(joint.ConnectedEntity)));
				return;
			}
			auto other = impl.Bodies.find(joint.ConnectedEntity);
			if (other == impl.Bodies.end())
			{
				warnings.emplace_back(fmt::format("not built: the connected entity '{}' {}", connected.GetName(), describeMissingBody(connected)));
				return;
			}
			bodyIDs[0] = other->second.ID;
			connectedType = other->second.Type;
		}
		// Jolt only moves dynamic bodies; a constraint between static and kinematic bodies does nothing.
		if (self->second.Type != RigidBodyType::Dynamic && connectedType != RigidBodyType::Dynamic)
		{
			warnings.emplace_back("not built: it connects no dynamic body (only static, kinematic or the world) and would have no effect");
			return;
		}

		// The joint frame in world space. Both bodies share it at creation, so their current relative pose
		// becomes the joint's rest pose. The axis turns with the body's rotation only: Jolt bodies are
		// unscaled, so a scaled world matrix would skew it under non-uniform scale.
		const glm::mat4 world = m_Scene->GetWorldTransform(bodyEntity);
		glm::vec3 position;
		glm::quat rotation;
		glm::vec3 scale;
		if (!Math::DecomposeTransform(world, position, rotation, scale))
		{
			warnings.emplace_back(fmt::format("not built: '{}' has a degenerate transform", bodyEntity.GetName()));
			return;
		}
		const glm::vec3 anchor = glm::vec3(world * glm::vec4(joint.Anchor, 1.0f));
		glm::vec3 axis = joint.Axis;
		if (glm::length(axis) < 1e-6f)
		{
			// Fixed, point and distance joints have no axis.
			if (joint.Type != JointType::Fixed && joint.Type != JointType::Point && joint.Type != JointType::Distance)
				warnings.emplace_back("Axis is zero; using local Y");
			axis = glm::vec3(0.0f, 1.0f, 0.0f);
		}
		const glm::vec3 worldAxis = glm::normalize(rotation * axis);
		const JPH::Vec3 joltAxis = ToJolt(worldAxis);
		const JPH::Vec3 joltNormal = joltAxis.GetNormalizedPerpendicular();

		// Limits, motors and break thresholds are applied by ApplyJointSettings below.
		JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
		switch (joint.Type)
		{
			case JointType::Fixed:
			{
				auto* fixed = new JPH::FixedConstraintSettings();
				fixed->mPoint1 = fixed->mPoint2 = ToJolt(anchor);
				settings = fixed;
				break;
			}
			case JointType::Point:
			{
				auto* point = new JPH::PointConstraintSettings();
				point->mPoint1 = point->mPoint2 = ToJolt(anchor);
				settings = point;
				break;
			}
			case JointType::Hinge:
			{
				auto* hinge = new JPH::HingeConstraintSettings();
				hinge->mPoint1 = hinge->mPoint2 = ToJolt(anchor);
				hinge->mHingeAxis1 = hinge->mHingeAxis2 = joltAxis;
				hinge->mNormalAxis1 = hinge->mNormalAxis2 = joltNormal;
				settings = hinge;
				break;
			}
			case JointType::Slider:
			{
				auto* slider = new JPH::SliderConstraintSettings();
				slider->mPoint1 = slider->mPoint2 = ToJolt(anchor);
				slider->mSliderAxis1 = slider->mSliderAxis2 = joltAxis;
				slider->mNormalAxis1 = slider->mNormalAxis2 = joltNormal;
				settings = slider;
				break;
			}
			case JointType::Distance:
			{
				auto* distance = new JPH::DistanceConstraintSettings();
				const glm::vec3 connectedAnchor = connected ? glm::vec3(m_Scene->GetWorldTransform(connected) * glm::vec4(joint.ConnectedAnchor, 1.0f)) : joint.ConnectedAnchor;
				distance->mPoint1 = ToJolt(connectedAnchor);
				distance->mPoint2 = ToJolt(anchor);
				// Jolt keeps the starting distance (its default min/max of -1 mean "current"); limits replace it.
				settings = distance;
				break;
			}
			case JointType::Cone:
			{
				auto* cone = new JPH::ConeConstraintSettings();
				cone->mPoint1 = cone->mPoint2 = ToJolt(anchor);
				cone->mTwistAxis1 = cone->mTwistAxis2 = joltAxis;
				cone->mHalfConeAngle = JPH::JPH_PI;
				settings = cone;
				break;
			}
			case JointType::SixDOF:
			{
				// The frame's X is Axis; Y is SecondaryAxis with its Axis component removed.
				glm::vec3 secondary = rotation * joint.SecondaryAxis;
				secondary -= glm::dot(secondary, worldAxis) * worldAxis;
				JPH::Vec3 joltSecondary = joltNormal;
				if (glm::length(secondary) < 1e-4f)
					warnings.emplace_back("SecondaryAxis is zero or parallel to Axis; using an arbitrary perpendicular");
				else
					joltSecondary = ToJolt(glm::normalize(secondary));
				auto* sixDOF = new JPH::SixDOFConstraintSettings();
				sixDOF->mPosition1 = sixDOF->mPosition2 = ToJolt(anchor);
				sixDOF->mAxisX1 = sixDOF->mAxisX2 = joltAxis;
				sixDOF->mAxisY1 = sixDOF->mAxisY2 = joltSecondary;
				sixDOF->mSwingType = JPH::ESwingType::Cone;
				settings = sixDOF;
				break;
			}
		}

		// Body 1 is the connected body (or the world), body 2 the joint's body (BodyEntity's, or the holder's).
		const int bodyCount = connected ? 2 : 1;
		JPH::BodyLockMultiWrite lock(impl.System->GetBodyLockInterface(), connected ? bodyIDs : bodyIDs + 1, bodyCount);
		JPH::Body* body1 = connected ? lock.GetBody(0) : &JPH::Body::sFixedToWorld;
		JPH::Body* body2 = lock.GetBody(bodyCount - 1);
		if (!body1 || !body2)
		{
			// Both bodies are in Bodies, so Jolt should always find them.
			BS_CORE_ERROR("Physics: joint on '{}' not built: its bodies could not be locked", entity.GetName());
			return;
		}

		Impl::JointRecord& record = impl.Joints[entity.GetUUID()];
		record.Constraint = settings->Create(*body1, *body2);
		record.Settings = joint;
		record.Body = bodyEntity.GetUUID();
		record.BuildWarnings = warnings;
		record.RestRotation = Impl::RelativeJointRotation(*record.Constraint);
		impl.System->AddConstraint(record.Constraint);
		lock.ReleaseLocks();
		ApplyJointSettings(entity, warnings);
	}

	bool PhysicsWorld::HasJoint(Entity entity) const
	{
		return entity && m_Impl->Joints.contains(entity.GetUUID());
	}

	std::optional<float> PhysicsWorld::GetJointPosition(Entity entity) const
	{
		if (!entity)
			return std::nullopt;
		auto it = m_Impl->Joints.find(entity.GetUUID());
		if (it == m_Impl->Joints.end())
			return std::nullopt;
		const JPH::Constraint* constraint = it->second.Constraint.GetPtr();
		switch (it->second.Settings.Type)
		{
			case JointType::Hinge:
				return glm::degrees(static_cast<const JPH::HingeConstraint*>(constraint)->GetCurrentAngle());
			case JointType::Slider:
				return static_cast<const JPH::SliderConstraint*>(constraint)->GetCurrentPosition();
			case JointType::Fixed:
			case JointType::Point:
			case JointType::Distance:
			case JointType::Cone:
			case JointType::SixDOF:
				break;
		}
		return std::nullopt;
	}

	std::optional<glm::vec3> PhysicsWorld::GetJointRotation(Entity entity) const
	{
		if (!entity)
			return std::nullopt;
		auto it = m_Impl->Joints.find(entity.GetUUID());
		if (it == m_Impl->Joints.end())
			return std::nullopt;
		const JointType type = it->second.Settings.Type;
		if (type != JointType::Cone && type != JointType::SixDOF)
			return std::nullopt;
		// Removing the rest offset on the right keeps the result in the first body's joint frame.
		const Impl::JointRecord& record = it->second;
		const JPH::Quat rotation = Impl::RelativeJointRotation(*record.Constraint) * record.RestRotation.Conjugated();
		return glm::degrees(Math::EulerFromQuat(FromJolt(rotation.Normalized())));
	}

	bool PhysicsWorld::HasBody(Entity entity) const
	{
		return entity && m_Impl->Bodies.contains(entity.GetUUID());
	}

	void PhysicsWorld::UpdateCharacters(float fixedStep)
	{
		Impl& impl = *m_Impl;
		if (impl.Characters.empty())
			return;
		const JPH::Vec3 sceneGravity = impl.System->GetGravity();
		// Registry order, not hash order: characters push bodies, so the order shows in replays.
		for (entt::entity handle : m_Scene->GetAllEntitiesWith<CharacterControllerComponent>())
		{
			Entity entity(handle, m_Scene);
			auto it = impl.Characters.find(entity.GetUUID());
			if (it == impl.Characters.end())
				continue;
			Impl::CharacterRecord& record = it->second;
			JPH::CharacterVirtual& character = *record.Character;

			glm::vec3 position;
			glm::quat rotation;
			glm::vec3 scale;
			if (!Math::DecomposeTransform(m_Scene->GetWorldTransform(entity), position, rotation, scale))
			{
				if (!record.Degenerate)
					impl.ReportCharacterWarnings(entity, record.BuildWarnings + (record.BuildWarnings.empty() ? "" : "; ") + "degenerate transform (e.g. a zero scale); the character does not move");
				record.Degenerate = true;
				continue;
			}
			if (record.Degenerate)
			{
				record.Degenerate = false;
				impl.ReportCharacterWarnings(entity, record.BuildWarnings);
			}
			const JPH::ObjectLayer layer = MakeObjectLayer(true, record.Layer);
			const JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(impl.ObjectVsBroadPhaseLayerFilter, layer);
			const JPH::DefaultObjectLayerFilter objectLayerFilter(impl.ObjectLayerPairFilter, layer);
			const JPH::BodyFilter bodyFilter;
			const JPH::ShapeFilter shapeFilter;

			// A script that moved the transform teleports the character (whose old ground no longer holds it);
			// its rotation always follows the entity.
			if (glm::any(glm::greaterThan(glm::abs(position - record.LastPosition), glm::vec3(1e-5f))))
			{
				character.SetPosition(ToJolt(position));
				character.RefreshContacts(broadPhaseFilter, objectLayerFilter, bodyFilter, shapeFilter, *impl.TempAllocator);
			}
			if (glm::abs(glm::dot(rotation, record.LastRotation)) < 1.0f - 1e-6f)
				character.SetRotation(ToJolt(rotation));

			// Standing characters move with their ground and may jump off it; airborne ones keep their vertical
			// speed under gravity and steer sideways (Jolt's CharacterVirtual sample). Unlike the sample, gravity
			// is left out on walkable ground so an idle character does not creep down slopes; walking down them
			// relies on the StepHeight stick-to-floor.
			// Up is against the scene gravity, which scripts may turn.
			if (!sceneGravity.IsNearZero())
				character.SetUp(-sceneGravity.Normalized());
			Impl::UpdateSupportingVolume(record);
			const JPH::Vec3 up = character.GetUp();
			const JPH::Vec3 gravity = sceneGravity * record.GravityFactor;
			const JPH::Vec3 move = ToJolt(record.MoveVelocity);
			const JPH::Vec3 moveVertical = up * move.Dot(up);
			const JPH::Vec3 current = character.GetLinearVelocity();
			const JPH::Vec3 groundVelocity = character.GetGroundVelocity();
			const bool onGround = character.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
			JPH::Vec3 velocity;
			if (gravity.IsNearZero())
			{
				velocity = move + (onGround ? groundVelocity : JPH::Vec3::sZero());
			}
			else
			{
				// Moving away from the ground (just jumped) counts as airborne, or the jump would be undone.
				if (onGround && (current - groundVelocity).Dot(up) < 0.1f)
					velocity = groundVelocity + moveVertical;
				else
					velocity = up * current.Dot(up) + gravity * fixedStep;
				velocity += move - moveVertical;
			}
			character.SetLinearVelocity(velocity);

			JPH::CharacterVirtual::ExtendedUpdateSettings update;
			update.mStickToFloorStepDown = -up * record.StepHeight;
			update.mWalkStairsStepUp = up * record.StepHeight;
			const JPH::RVec3 before = character.GetPosition();
			character.ExtendedUpdate(fixedStep, gravity, update, broadPhaseFilter, objectLayerFilter, bodyFilter, shapeFilter, *impl.TempAllocator);
			const JPH::Vec3 moved = JPH::Vec3(character.GetPosition() - before);
			record.ActualVelocity = FromJolt(moved / fixedStep);

			// Jolt does not stop a rising character at a ceiling; without this it would stick there until
			// gravity ate the jump speed.
			const float wantedRise = character.GetLinearVelocity().Dot(up);
			const float actualRise = moved.Dot(up) / fixedStep;
			if (wantedRise > 0.0f && actualRise < wantedRise)
			{
				const JPH::Vec3 corrected = character.GetLinearVelocity() + up * (std::max(actualRise, 0.0f) - wantedRise);
				character.SetLinearVelocity(corrected);
			}

			record.LastPosition = FromJolt(JPH::Vec3(character.GetPosition()));
			record.LastRotation = rotation;
			m_Scene->SetWorldTransform(entity, Math::ComposeTransform(record.LastPosition, rotation, scale));
		}
	}

	bool PhysicsWorld::HasCharacter(Entity entity) const
	{
		return entity && m_Impl->Characters.contains(entity.GetUUID());
	}

	void PhysicsWorld::MoveCharacter(Entity entity, const glm::vec3& velocity)
	{
		auto it = m_Impl->Characters.find(entity.GetUUID());
		if (it != m_Impl->Characters.end() && IsFiniteVec(velocity))
			it->second.MoveVelocity = velocity;
	}

	bool PhysicsWorld::IsCharacterGrounded(Entity entity) const
	{
		auto it = m_Impl->Characters.find(entity.GetUUID());
		return it != m_Impl->Characters.end() && it->second.Character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
	}

	std::optional<glm::vec3> PhysicsWorld::GetCharacterGroundNormal(Entity entity) const
	{
		auto it = m_Impl->Characters.find(entity.GetUUID());
		if (it == m_Impl->Characters.end() || !it->second.Character->IsSupported())
			return std::nullopt;
		return FromJolt(it->second.Character->GetGroundNormal());
	}

	uint32_t PhysicsWorld::GetCharacterCount() const
	{
		return static_cast<uint32_t>(m_Impl->Characters.size());
	}

	void PhysicsWorld::Step(Timestep ts)
	{
		Impl& impl = *m_Impl;
		JPH::BodyInterface& bodies = impl.System->GetBodyInterface();
		const PhysicsSettings& settings = m_Scene->GetPhysicsSettings();
		const float fixedStep = settings.FixedTimestep > 0.0f ? settings.FixedTimestep : 1.0f / 60.0f;

		// Rebuild bodies whose physics components changed since the last step.
		if (!impl.DirtyEntities.empty())
		{
			const std::unordered_set<UUID> dirty = std::move(impl.DirtyEntities);
			impl.DirtyEntities.clear();
			for (UUID uuid : dirty)
			{
				Entity entity = m_Scene->GetEntityByUUID(uuid);
				if (entity)
				{
					RecreateBody(entity);
					RecreateCharacter(entity);
				}
				else
				{
					impl.RemoveBody(m_Scene, uuid);
					impl.RemoveCharacter(m_Scene, uuid);
				}
			}
		}
		if (!impl.DirtyCharacters.empty())
		{
			const std::unordered_set<UUID> dirty = std::move(impl.DirtyCharacters);
			impl.DirtyCharacters.clear();
			for (UUID uuid : dirty)
			{
				if (Entity entity = m_Scene->GetEntityByUUID(uuid))
					ApplyCharacterSettings(entity);
			}
		}
		RebuildDirtyJoints();

		m_Accumulator += ts.GetSeconds();
		uint32_t steps = 0;
		while (m_Accumulator >= fixedStep && steps < settings.MaxStepsPerFrame)
		{
			// Push entity-side transform changes into the simulation.
			for (auto& [uuid, record] : impl.Bodies)
			{
				Entity entity = m_Scene->GetEntityByUUID(uuid);
				if (!entity)
					continue;

				glm::vec3 position;
				glm::quat rotation;
				glm::vec3 scale;
				if (!Math::DecomposeTransform(m_Scene->GetWorldTransform(entity), position, rotation, scale))
					continue;

				const bool moved = glm::any(glm::greaterThan(glm::abs(position - record.LastPosition), glm::vec3(1e-5f))) || glm::abs(glm::dot(rotation, record.LastRotation)) < 1.0f - 1e-6f;
				if (record.Type == RigidBodyType::Kinematic)
				{
					bodies.MoveKinematic(record.ID, ToJolt(position), ToJolt(rotation), fixedStep);
				}
				else if (moved)
				{
					const JPH::EActivation activation = record.Type == RigidBodyType::Dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
					bodies.SetPositionAndRotation(record.ID, ToJolt(position), ToJolt(rotation), activation);
				}
				record.LastPosition = position;
				record.LastRotation = rotation;
			}

			// Characters move before the bodies step, as Jolt expects.
			UpdateCharacters(fixedStep);

			impl.System->Update(fixedStep, 1, impl.TempAllocator.get(), impl.JobSystem.get());
			m_Accumulator -= fixedStep;
			m_StepCount++;
			steps++;

			// Write simulated poses of dynamic bodies back to their entities.
			for (auto& [uuid, record] : impl.Bodies)
			{
				if (record.Type != RigidBodyType::Dynamic)
					continue;
				Entity entity = m_Scene->GetEntityByUUID(uuid);
				if (!entity)
					continue;

				JPH::RVec3 joltPosition;
				JPH::Quat joltRotation;
				bodies.GetPositionAndRotation(record.ID, joltPosition, joltRotation);
				const glm::vec3 position = FromJolt(joltPosition);
				const glm::quat rotation = glm::normalize(FromJolt(joltRotation));

				glm::vec3 oldPosition;
				glm::quat oldRotation;
				glm::vec3 scale;
				Math::DecomposeTransform(m_Scene->GetWorldTransform(entity), oldPosition, oldRotation, scale);
				m_Scene->SetWorldTransform(entity, Math::ComposeTransform(position, rotation, scale));
				record.LastPosition = position;
				record.LastRotation = rotation;
			}

			CheckBrokenJoints(fixedStep);
			DispatchContacts();
		}

		// Never let a long stall build an unbounded backlog of steps.
		if (steps == settings.MaxStepsPerFrame)
			m_Accumulator = std::min(m_Accumulator, fixedStep);
	}

	void PhysicsWorld::DispatchContacts()
	{
		Impl& impl = *m_Impl;
		JPH::BodyInterface& bodies = impl.System->GetBodyInterface();

		std::vector<RawContactEvent> events;
		{
			std::scoped_lock lock(impl.ContactListener.Mutex);
			events.swap(impl.ContactListener.Events);
		}
		// Worker threads record events in scheduling order; sort so callbacks are deterministic.
		std::sort(events.begin(), events.end(), [](const RawContactEvent& a, const RawContactEvent& b) { return a.Tie() < b.Tie(); });

		// A body that falls asleep reports its contacts as removed; it is still touching, so keep them.
		auto isSleeping = [&](uint32_t body) {
			auto entity = impl.BodyToEntity.find(body);
			if (entity == impl.BodyToEntity.end())
				return false;
			auto record = impl.Bodies.find(entity->second);
			return record != impl.Bodies.end() && record->second.Type != RigidBodyType::Static && !bodies.IsActive(record->second.ID);
		};

		std::map<Impl::EntityPair, bool> touched; // pair -> was active before this step
		// The strongest new contact of each pair, relative to the pair's first (lower UUID) entity.
		std::map<Impl::EntityPair, ContactInfo> newContacts;
		for (const RawContactEvent& event : events)
		{
			auto first = impl.BodyToEntity.find(event.Body1);
			auto second = impl.BodyToEntity.find(event.Body2);
			if (first == impl.BodyToEntity.end() || second == impl.BodyToEntity.end())
				continue;

			uint64_t a = static_cast<uint64_t>(first->second);
			uint64_t b = static_cast<uint64_t>(second->second);
			uint32_t subA = event.SubShape1;
			uint32_t subB = event.SubShape2;
			// The event's normal points from body 1 toward body 2; ContactInfo's from the second entity
			// toward the first.
			ContactInfo contact = event.Contact.Flipped();
			if (b < a)
			{
				std::swap(a, b);
				std::swap(subA, subB);
				contact = event.Contact;
			}
			const Impl::ContactKey key(a, subA, b, subB);
			const Impl::EntityPair pair(a, b);

			if (!event.Added && (isSleeping(event.Body1) || isSleeping(event.Body2)))
			{
				if (impl.ActiveContacts.contains(key))
					impl.SuspendedContacts.insert(key);
				continue;
			}

			touched.emplace(pair, impl.PairCounts.contains(pair));
			if (event.Added)
			{
				auto [best, inserted] = newContacts.try_emplace(pair, contact);
				if (!inserted && contact.Impulse > best->second.Impulse)
					best->second = contact;
				impl.SuspendedContacts.erase(key);
				if (impl.ActiveContacts.insert(key).second)
					impl.PairCounts[pair]++;
			}
			else if (impl.ActiveContacts.erase(key) > 0)
			{
				if (--impl.PairCounts[pair] <= 0)
					impl.PairCounts.erase(pair);
			}
		}

		// Suspended contacts whose bodies are awake again and were not re-reported this step have ended.
		auto isEntitySleeping = [&](uint64_t uuid) {
			auto record = impl.Bodies.find(uuid);
			return record != impl.Bodies.end() && record->second.Type != RigidBodyType::Static && !bodies.IsActive(record->second.ID);
		};
		for (auto key = impl.SuspendedContacts.begin(); key != impl.SuspendedContacts.end();)
		{
			if (isEntitySleeping(std::get<0>(*key)) || isEntitySleeping(std::get<2>(*key)))
			{
				++key;
				continue;
			}
			const Impl::EntityPair pair(std::get<0>(*key), std::get<2>(*key));
			touched.emplace(pair, impl.PairCounts.contains(pair));
			if (impl.ActiveContacts.erase(*key) > 0 && --impl.PairCounts[pair] <= 0)
				impl.PairCounts.erase(pair);
			key = impl.SuspendedContacts.erase(key);
		}

		ScriptEngine* scriptEngine = m_Scene->GetScriptEngine();
		if (!scriptEngine)
			return;
		for (const auto& [pair, wasActive] : touched)
		{
			const bool isActive = impl.PairCounts.contains(pair);
			if (wasActive == isActive)
				continue;
			Entity entityA = m_Scene->GetEntityByUUID(pair.first);
			Entity entityB = m_Scene->GetEntityByUUID(pair.second);
			if (!entityA || !entityB)
				continue;
			const bool trigger = impl.IsTrigger(pair.first) || impl.IsTrigger(pair.second);
			ContactEventType type;
			if (trigger)
				type = isActive ? ContactEventType::TriggerEnter : ContactEventType::TriggerExit;
			else
				type = isActive ? ContactEventType::CollisionBegin : ContactEventType::CollisionEnd;
			auto contact = newContacts.find(pair);
			scriptEngine->OnContactEvent(type, entityA, entityB, contact != newContacts.end() ? contact->second : ContactInfo{});
		}
	}

	void PhysicsWorld::AddForce(Entity entity, const glm::vec3& force)
	{
		if (m_Impl->WarnIfCharacter(entity, "AddForce"))
			return;
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it != m_Impl->Bodies.end())
			m_Impl->System->GetBodyInterface().AddForce(it->second.ID, ToJolt(force));
	}

	void PhysicsWorld::AddImpulse(Entity entity, const glm::vec3& impulse)
	{
		if (m_Impl->WarnIfCharacter(entity, "AddImpulse"))
			return;
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it != m_Impl->Bodies.end())
			m_Impl->System->GetBodyInterface().AddImpulse(it->second.ID, ToJolt(impulse));
	}

	void PhysicsWorld::AddTorque(Entity entity, const glm::vec3& torque)
	{
		if (m_Impl->WarnIfCharacter(entity, "AddTorque"))
			return;
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it != m_Impl->Bodies.end())
			m_Impl->System->GetBodyInterface().AddTorque(it->second.ID, ToJolt(torque));
	}

	void PhysicsWorld::SetLinearVelocity(Entity entity, const glm::vec3& velocity)
	{
		if (m_Impl->WarnIfCharacter(entity, "SetLinearVelocity"))
			return;
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it != m_Impl->Bodies.end())
			m_Impl->System->GetBodyInterface().SetLinearVelocity(it->second.ID, ToJolt(velocity));
	}

	glm::vec3 PhysicsWorld::GetLinearVelocity(Entity entity) const
	{
		if (auto character = m_Impl->Characters.find(entity.GetUUID()); character != m_Impl->Characters.end())
			return character->second.ActualVelocity;
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it == m_Impl->Bodies.end())
			return glm::vec3(0.0f);
		return FromJolt(m_Impl->System->GetBodyInterface().GetLinearVelocity(it->second.ID));
	}

	void PhysicsWorld::SetAngularVelocity(Entity entity, const glm::vec3& velocity)
	{
		if (m_Impl->WarnIfCharacter(entity, "SetAngularVelocity"))
			return;
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it != m_Impl->Bodies.end())
			m_Impl->System->GetBodyInterface().SetAngularVelocity(it->second.ID, ToJolt(velocity));
	}

	glm::vec3 PhysicsWorld::GetAngularVelocity(Entity entity) const
	{
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it == m_Impl->Bodies.end())
			return glm::vec3(0.0f);
		return FromJolt(m_Impl->System->GetBodyInterface().GetAngularVelocity(it->second.ID));
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

	const PhysicsLayers& PhysicsWorld::GetLayers() const
	{
		return m_Impl->Layers;
	}

	glm::vec3 PhysicsWorld::GetGravity() const
	{
		return FromJolt(m_Impl->System->GetGravity());
	}

	void PhysicsWorld::SetGravity(const glm::vec3& gravity)
	{
		m_Impl->System->SetGravity(ToJolt(gravity));
	}

	void PhysicsWorld::ClearMeshShapeCache()
	{
		std::scoped_lock lock(s_MeshShapeMutex);
		s_MeshShapes.clear();
	}

	uint64_t PhysicsWorld::GetMeshShapeCookCount()
	{
		std::scoped_lock lock(s_MeshShapeMutex);
		return s_MeshShapeCookCount;
	}

	uint32_t PhysicsWorld::GetBodyCount() const
	{
		return static_cast<uint32_t>(m_Impl->Bodies.size());
	}

}
