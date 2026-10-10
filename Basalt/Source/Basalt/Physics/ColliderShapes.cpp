#include "Basalt/Physics/ColliderShapes.h"

#include "Basalt/Physics/JoltUtils.h"
#include "Basalt/Physics/MeshShapeCache.h"
#include "Basalt/Scene/Components.h"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include <algorithm>

namespace Basalt::PhysicsInternal {

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
