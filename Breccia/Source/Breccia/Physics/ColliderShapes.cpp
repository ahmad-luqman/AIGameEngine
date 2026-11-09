#include "Breccia/Physics/ColliderShapes.h"

#include "Breccia/Core/Assert.h"
#include "Breccia/Physics/JoltUtils.h"
#include "Breccia/Physics/MeshShapeCache.h"
#include "Breccia/Scene/Components.h"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include <algorithm>

namespace Breccia::PhysicsInternal {

	ColliderMaterial SanitizeSurface(float friction, float restitution, const std::string& owner, std::vector<std::string>& warnings)
	{
		if (friction < 0.0f)
			warnings.emplace_back(owner + "Friction must not be negative, using 0");
		if (restitution < 0.0f || restitution > 1.0f)
			warnings.emplace_back(owner + "Restitution must be between 0 and 1, clamped");
		return { .Override = true, .Friction = std::max(friction, 0.0f), .Restitution = std::clamp(restitution, 0.0f, 1.0f) };
	}

	ColliderSettings ColliderSettings::Capture(Entity entity)
	{
		ColliderSettings colliders;
		const auto capture = [&]<typename Component>(std::optional<Component>& slot) {
			if (const auto* collider = entity.TryGetComponent<Component>())
				slot = *collider;
		};
		capture(colliders.Box);
		capture(colliders.Sphere);
		capture(colliders.Capsule);
		capture(colliders.Mesh);
		if (colliders.Mesh && colliders.Mesh->Mesh.empty())
			if (const auto* mesh = entity.TryGetComponent<MeshComponent>())
				colliders.BorrowedMesh.emplace(mesh->Mesh, mesh->MeshIndex);
		return colliders;
	}

	bool ColliderSettings::SameChildren(const ColliderSettings& other) const
	{
		if (Box.has_value() != other.Box.has_value() || Sphere.has_value() != other.Sphere.has_value() || Capsule.has_value() != other.Capsule.has_value() || Mesh.has_value() != other.Mesh.has_value())
			return false;
		return !Mesh || (Mesh->Mesh == other.Mesh->Mesh && Mesh->MeshIndex == other.Mesh->MeshIndex && Mesh->Convex == other.Mesh->Convex && BorrowedMesh == other.BorrowedMesh);
	}

	ColliderShape BuildColliderShape(Entity entity, const glm::vec3& signedScale, float sizeFraction, const char* convexReason, bool isTrigger, bool useMaterials, std::vector<std::string>& warnings)
	{
		const glm::vec3 scale = glm::abs(signedScale) * sizeFraction;
		JPH::StaticCompoundShapeSettings compound;
		uint32_t shapeCount = 0;
		ColliderShape result;
		bool ignoredMaterial = false;
		auto materialOf = [&](const auto& collider, const char* name) -> ColliderMaterial {
			if (!collider.OverrideMaterial)
				return {};
			if (!useMaterials)
			{
				ignoredMaterial = true;
				return {};
			}
			return SanitizeSurface(collider.Friction, collider.Restitution, std::string(name) + " ", warnings);
		};
		// Every compound child goes through here, so Materials stays indexed like the children.
		auto addChild = [&](const JPH::Shape* shape, const glm::vec3& offset, const ColliderMaterial& material) {
			BR_CORE_ASSERT(shapeCount < MaxColliders, "more collider kinds than MaxColliders");
			// Offsets follow the entity's scale only: a smaller inner shape stays centred on each collider.
			compound.AddShape(ToJolt(offset * glm::abs(signedScale)), JPH::Quat::sIdentity(), shape);
			result.Materials[shapeCount++] = material;
		};
		auto addShape = [&](const JPH::ShapeSettings::ShapeResult& shape, const glm::vec3& offset, const ColliderMaterial& material) {
			if (shape.HasError())
			{
				warnings.emplace_back(std::string("invalid collider: ") + shape.GetError().c_str());
				return;
			}
			addChild(shape.Get(), offset, material);
		};

		if (const auto* box = entity.TryGetComponent<BoxColliderComponent>())
		{
			if (glm::any(glm::lessThanEqual(box->HalfExtents, glm::vec3(0.0f))))
				warnings.emplace_back("BoxCollider HalfExtents must be positive, using the smallest size");
			const glm::vec3 halfExtents = glm::max(box->HalfExtents * scale, glm::vec3(MinExtent));
			const float convexRadius = std::min(JPH::cDefaultConvexRadius, glm::min(halfExtents.x, glm::min(halfExtents.y, halfExtents.z)) * 0.5f);
			addShape(JPH::BoxShapeSettings(ToJolt(halfExtents), convexRadius).Create(), box->Offset, materialOf(*box, "BoxCollider"));
		}
		if (const auto* sphere = entity.TryGetComponent<SphereColliderComponent>())
		{
			if (sphere->Radius <= 0.0f)
				warnings.emplace_back("SphereCollider Radius must be positive, using the smallest size");
			const float radius = std::max(sphere->Radius * glm::max(scale.x, glm::max(scale.y, scale.z)), MinExtent);
			addShape(JPH::SphereShapeSettings(radius).Create(), sphere->Offset, materialOf(*sphere, "SphereCollider"));
		}
		if (const auto* capsule = entity.TryGetComponent<CapsuleColliderComponent>())
		{
			if (capsule->Radius <= 0.0f || capsule->HalfHeight <= 0.0f)
				warnings.emplace_back("CapsuleCollider Radius and HalfHeight must be positive, using the smallest size");
			const float radius = std::max(capsule->Radius * glm::max(scale.x, scale.z), MinExtent);
			const float halfHeight = std::max(capsule->HalfHeight * scale.y, MinExtent);
			addShape(JPH::CapsuleShapeSettings(halfHeight, radius).Create(), capsule->Offset, materialOf(*capsule, "CapsuleCollider"));
		}

		if (const auto* meshCollider = entity.TryGetComponent<MeshColliderComponent>())
		{
			std::string key = meshCollider->Mesh;
			uint32_t meshIndex = meshCollider->MeshIndex;
			const auto* meshComponent = entity.TryGetComponent<MeshComponent>();
			if (key.empty() && meshComponent)
			{
				if (meshIndex != 0)
					warnings.emplace_back("MeshCollider MeshIndex is ignored without its own Mesh (the MeshComponent's is used)");
				key = meshComponent->Mesh;
				meshIndex = meshComponent->MeshIndex;
			}
			bool convex = meshCollider->Convex;
			if (!convex && convexReason)
			{
				convex = true;
				warnings.emplace_back(std::string(convexReason) + " collides by the convex hull of its MeshCollider; set Convex to make that explicit");
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
				addChild(shape, glm::vec3(0.0f), materialOf(*meshCollider, "MeshCollider"));
				result.HasTriangleMesh = !convex;
			}
			else
			{
				warnings.emplace_back("MeshCollider ignored: " + error);
			}
		}

		if (ignoredMaterial)
			warnings.emplace_back("OverrideMaterial has no effect here (characters do not use collider materials)");
		if (shapeCount == 0)
		{
			warnings.emplace_back("no valid collider");
			return result;
		}
		JPH::ShapeSettings::ShapeResult shapeResult = compound.Create();
		if (shapeResult.HasError())
		{
			warnings.emplace_back(std::string("failed to build the shape: ") + shapeResult.GetError().c_str());
			return result;
		}
		result.Shape = shapeResult.Get();
		return result;
	}

}
