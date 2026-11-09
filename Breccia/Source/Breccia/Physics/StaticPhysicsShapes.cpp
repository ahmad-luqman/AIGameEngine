#include "Breccia/Physics/StaticPhysicsShapes.h"

#include "Breccia/Asset/AssetManager.h"
#include "Breccia/Math/Math.h"
#include "Breccia/Physics/ColliderShapes.h"
#include "Breccia/Physics/JoltUtils.h"
#include "Breccia/Physics/PhysicsShapeSnapshotImpl.h"
#include "Breccia/Physics/WarningLog.h"
#include "Breccia/Scene/Components.h"
#include "Breccia/Scene/Entity.h"
#include "Breccia/Scene/Scene.h"

#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>

#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Breccia {

	using namespace PhysicsInternal;

	namespace {
		// What a body's shape was built from: everything BuildColliderShape reads, and its layer. Default: nothing yet.
		struct BuiltColliders
		{
			std::optional<BoxColliderComponent> Box;
			std::optional<SphereColliderComponent> Sphere;
			std::optional<CapsuleColliderComponent> Capsule;
			std::optional<MeshColliderComponent> MeshCollider;
			std::optional<std::pair<std::string, uint32_t>> BorrowedMesh;
			// The mesh's asset: a reloaded one cooks a new shape, and one that went away fails to.
			std::weak_ptr<MeshSource> MeshAsset;
			bool HasMeshAsset = false;
			// The layer's name, and what the project's layers make of it.
			std::string Layer;
			std::optional<uint32_t> LayerIndex;
			glm::vec3 Scale = glm::vec3(1);
		};

		template<typename Component>
		bool Same(const std::optional<Component>& built, const Component* current)
		{
			return current ? built == *current : !built;
		}

		template<typename Component>
		std::optional<Component> Copy(const Component* current)
		{
			return current ? std::optional<Component>(*current) : std::nullopt;
		}
	}

	struct StaticPhysicsShapes::Impl
	{
		explicit Impl(Scene& scene)
			: Owner(scene)
		{
		}

		struct Body
		{
			BuiltColliders Built;
			// Null when no collider could be built, or the transform cannot place one (reported through Warnings).
			JPH::Ref<JPH::Shape> Shape;
			uint32_t Layer = 0;
			// Valid while Shape is set. Its body ID is made up, unique among the bodies: PhysicsShapeSnapshot::PushOut finds the
			// shape it hit by it.
			JPH::TransformedShape Placed;
			JPH::AABox Bounds;
		};

		Scene& Owner;
		PhysicsLayers Layers;
		// By UUID: the order of the registry changes with edits.
		std::map<UUID, Body> Bodies;
		WarningLog Warnings{ "entity", true };
	};

	StaticPhysicsShapes::StaticPhysicsShapes(Scene& scene)
		: m_Impl(CreateScope<Impl>(scene))
	{
		AcquireJolt();
		Refresh();
	}

	StaticPhysicsShapes::~StaticPhysicsShapes()
	{
		// Shapes go first: Jolt's allocator callbacks stay, but its types do not outlive the last user.
		m_Impl.reset();
		ReleaseJolt();
	}

	const PhysicsLayers& StaticPhysicsShapes::GetLayers() const
	{
		return m_Impl->Layers;
	}

	void StaticPhysicsShapes::Refresh()
	{
		Impl& impl = *m_Impl;
		Scene& scene = impl.Owner;
		impl.Layers = scene.ResolvePhysicsLayers();
		std::map<UUID, Impl::Body> bodies;
		for (entt::entity handle : scene.GetAllEntitiesWith<RigidBodyComponent>())
		{
			Entity entity(handle, &scene);
			const auto& rigidBody = entity.GetComponent<RigidBodyComponent>();
			if (rigidBody.Type != RigidBodyType::Static || rigidBody.IsTrigger || entity.HasComponent<CharacterControllerComponent>())
				continue;
			const auto* box = entity.TryGetComponent<BoxColliderComponent>();
			const auto* sphere = entity.TryGetComponent<SphereColliderComponent>();
			const auto* capsule = entity.TryGetComponent<CapsuleColliderComponent>();
			const auto* mesh = entity.TryGetComponent<MeshColliderComponent>();
			if (!box && !sphere && !capsule && !mesh)
				continue;
			const glm::mat4 world = scene.GetWorldTransform(entity);
			glm::vec3 position, scale;
			glm::quat rotation;
			if (!Math::IsFinite(world) || !Math::DecomposeTransform(world, position, rotation, scale))
			{
				// Kept, so the warning is not repeated, and rebuilt once it can be placed.
				impl.Warnings.Report(entity, { "degenerate transform; the preview leaves its body out" });
				bodies.emplace(entity.GetUUID(), Impl::Body{});
				continue;
			}
			Impl::Body body;
			if (auto found = impl.Bodies.find(entity.GetUUID()); found != impl.Bodies.end())
				body = std::move(found->second);

			std::optional<std::pair<std::string, uint32_t>> borrowed;
			if (const auto* shown = entity.TryGetComponent<MeshComponent>(); mesh && mesh->Mesh.empty() && shown)
				borrowed.emplace(shown->Mesh, shown->MeshIndex);
			const std::string meshKey = borrowed ? borrowed->first : (mesh ? mesh->Mesh : std::string());
			const Ref<MeshSource> asset = meshKey.empty() ? nullptr : AssetManager::GetMesh(meshKey);
			const std::optional<uint32_t> layer = impl.Layers.Find(rigidBody.Layer);
			const BuiltColliders& built = body.Built;
			const bool current = built.Scale == scale && built.Layer == rigidBody.Layer && built.LayerIndex == layer && Same(built.Box, box) && Same(built.Sphere, sphere) && Same(built.Capsule, capsule) && Same(built.MeshCollider, mesh) && built.BorrowedMesh == borrowed && built.HasMeshAsset == (asset != nullptr) && built.MeshAsset.lock() == asset;
			if (!current)
			{
				body.Built = { .Box = Copy(box), .Sphere = Copy(sphere), .Capsule = Copy(capsule), .MeshCollider = Copy(mesh), .BorrowedMesh = borrowed, .MeshAsset = asset, .HasMeshAsset = asset != nullptr, .Layer = rigidBody.Layer, .LayerIndex = layer, .Scale = scale };
				std::vector<std::string> warnings;
				body.Shape = BuildColliderShape(entity, scale, 1.0f, nullptr, false, true, warnings).Shape;
				// An unknown layer plays on Default, as a body's does.
				if (!layer)
					warnings.push_back("unknown physics layer '" + rigidBody.Layer + "', using Default");
				body.Layer = layer.value_or(0);
				impl.Warnings.Report(entity, warnings);
			}
			if (body.Shape)
			{
				const JPH::Quat joltRotation = ToJolt(glm::normalize(rotation));
				// A shape is placed by its center of mass, as a body is.
				body.Placed = JPH::TransformedShape(ToJolt(position) + joltRotation * body.Shape->GetCenterOfMass(), joltRotation, body.Shape, JPH::BodyID(static_cast<JPH::uint32>(bodies.size())));
				body.Bounds = body.Placed.GetWorldSpaceBounds();
			}
			bodies.emplace(entity.GetUUID(), std::move(body));
		}
		for (const auto& [uuid, body] : impl.Bodies)
			if (!bodies.contains(uuid))
				impl.Warnings.Forget(uuid);
		impl.Bodies = std::move(bodies);
	}

	PhysicsShapeSnapshot StaticPhysicsShapes::CollectShapes(const glm::vec3& min, const glm::vec3& max, float radius, const PhysicsQueryFilter& filter, const std::function<bool(UUID)>& accept) const
	{
		if (!IsValidShapeRegion(min, max, radius))
			return {};
		const JPH::AABox region(ToJolt(min), ToJolt(max));
		const glm::vec3 center = (min + max) * 0.5f;
		std::vector<PhysicsShapeSnapshot::Candidate> candidates;
		for (const auto& [uuid, body] : m_Impl->Bodies)
		{
			if (!body.Shape || (filter.LayerMask & (1u << body.Layer)) == 0 || uuid == filter.IgnoreEntity || !body.Bounds.Overlaps(region) || (accept && !accept(uuid)))
				continue;
			candidates.push_back(PhysicsShapeSnapshot::Candidate::At(uuid, body.Placed, center));
		}
		return PhysicsShapeSnapshot::FromCandidates(std::move(candidates), radius);
	}

	std::vector<RaycastHit> StaticPhysicsShapes::RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter) const
	{
		const auto unitDirection = CastDirection(direction, maxDistance);
		if (!unitDirection || !IsFiniteVec(origin))
			return {};
		const JPH::RRayCast ray(ToJolt(origin), ToJolt(*unitDirection * maxDistance));
		const JPH::AABox swept(ToJolt(glm::min(origin, origin + *unitDirection * maxDistance)), ToJolt(glm::max(origin, origin + *unitDirection * maxDistance)));
		std::vector<RaycastHit> hits;
		for (const auto& [uuid, body] : m_Impl->Bodies)
		{
			if (!body.Shape || (filter.LayerMask & (1u << body.Layer)) == 0 || uuid == filter.IgnoreEntity || !body.Bounds.Overlaps(swept))
				continue;
			JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
			body.Placed.CastRay(ray, JPH::RayCastSettings(), collector);
			for (const JPH::RayCastResult& result : collector.mHits)
			{
				RaycastHit& hit = hits.emplace_back();
				hit.EntityID = uuid;
				hit.Distance = result.mFraction * maxDistance;
				hit.Point = origin + *unitDirection * hit.Distance;
				hit.Normal = FromJolt(body.Placed.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, ToJolt(hit.Point)));
			}
		}
		return SortHits(std::move(hits));
	}

}
