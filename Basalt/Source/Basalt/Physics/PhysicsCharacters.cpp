// PhysicsWorld: character controllers (CharacterControllerComponent on Jolt's CharacterVirtual).
#include "Basalt/Physics/PhysicsWorld.h"

#include "Basalt/Physics/ColliderShapes.h"
#include "Basalt/Physics/PhysicsWorldImpl.h"

#include <algorithm>
#include <cfloat>
#include <optional>

namespace Basalt {

	using namespace PhysicsInternal;

	namespace {

		// The inner body that other bodies collide with is a little smaller than the character's shape (as in
		// Jolt's samples): the character stops just short of what it walks into, so a full-size inner body
		// would touch it and shove dynamic bodies regardless of MaxStrength.
		constexpr float InnerShapeFraction = 0.9f;

	}

	bool PhysicsWorld::Impl::IsCharacterBody(const JPH::Body& body) const
	{
		auto it = Characters.find(UUID(body.GetUserData()));
		return it != Characters.end() && it->second.Character->GetInnerBodyID() == body.GetID();
	}

	bool PhysicsWorld::Impl::CharacterLayerFilter::OnCharacterContactValidate(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact)
	{
		// Contacts with bodies were already filtered by layer when they were collected.
		if (!contact.mCharacterB)
			return true;
		const auto& characters = m_Owner.Characters;
		auto self = characters.find(UUID(character->GetUserData()));
		auto other = characters.find(UUID(contact.mCharacterB->GetUserData()));
		BS_CORE_ASSERT(self != characters.end() && other != characters.end(), "a character in CharacterCollision has no record");
		if (self == characters.end() || other == characters.end())
			return true;
		return m_Owner.ObjectLayerPairFilter.ShouldCollide(MakeObjectLayer(true, self->second.Layer), MakeObjectLayer(true, other->second.Layer));
	}

	void PhysicsWorld::Impl::ConfigureCharacter(CharacterRecord& record, std::vector<std::string>& warnings)
	{
		const CharacterControllerComponent& c = record.Settings;
		// Below about 0.8 degrees Jolt switches the slope check off and every slope becomes walkable.
		const float slope = std::isfinite(c.SlopeLimit) ? std::clamp(c.SlopeLimit, 1.0f, 90.0f) : 45.0f;
		if (slope != c.SlopeLimit)
			warnings.emplace_back(fmt::format("SlopeLimit {} must be within [1, 90] degrees; using {}", c.SlopeLimit, slope));
		record.Character->SetMaxSlopeAngle(glm::radians(slope));
		const float strength = std::isfinite(c.MaxStrength) && c.MaxStrength >= 0.0f ? c.MaxStrength : 0.0f;
		if (strength != c.MaxStrength)
			warnings.emplace_back(fmt::format("MaxStrength {} must be a finite, non-negative force; using 0", c.MaxStrength));
		record.Character->SetMaxStrength(strength);
		const float mass = std::isfinite(c.Mass) && c.Mass >= 0.0f ? c.Mass : 0.0f;
		if (mass != c.Mass)
			warnings.emplace_back(fmt::format("Mass {} must be a finite, non-negative mass; using 0", c.Mass));
		record.Character->SetMass(mass);
		record.StepHeight = std::isfinite(c.StepHeight) && c.StepHeight >= 0.0f ? c.StepHeight : 0.0f;
		if (record.StepHeight != c.StepHeight)
			warnings.emplace_back(fmt::format("StepHeight {} must be a finite, non-negative distance; using 0", c.StepHeight));
		record.GravityFactor = std::isfinite(c.GravityFactor) ? c.GravityFactor : 1.0f;
		if (record.GravityFactor != c.GravityFactor)
			warnings.emplace_back(fmt::format("GravityFactor {} is not finite; using 1", c.GravityFactor));
	}

	void PhysicsWorld::Impl::UpdateSupportingVolume(CharacterRecord& record)
	{
		const JPH::CharacterVirtual& character = *record.Character;
		const JPH::Vec3 localUp = character.GetRotation().Conjugated() * character.GetUp();
		const JPH::Vec3 extent = record.Bounds.GetExtent();
		const float bottom = localUp.Dot(record.Bounds.GetCenter()) - localUp.Abs().Dot(extent);
		const float halfWidth = extent.ReduceMin();
		record.Character->SetSupportingVolume(JPH::Plane(localUp, -(bottom + halfWidth)));
	}

	bool PhysicsWorld::Impl::WarnIfCharacter(Entity entity, const char* call)
	{
		if (!Owns(entity) || !Characters.contains(entity.GetUUID()))
			return false;
		if (WarnedCharacterBodyCalls.insert(entity.GetUUID()).second)
			BS_CORE_WARN("Physics: {} does nothing on character '{}' (it has a CharacterController); use Move", call, entity.GetName());
		return true;
	}

	void PhysicsWorld::Impl::RemoveCharacter(UUID uuid)
	{
		auto it = Characters.find(uuid);
		if (it == Characters.end())
			return;
		CharacterCollision.Remove(it->second.Character.GetPtr());
		const JPH::BodyID inner = it->second.Character->GetInnerBodyID();
		if (!inner.IsInvalid())
		{
			BodyToEntity.erase(inner.GetIndexAndSequenceNumber());
			ContactListener.Materials[inner.GetIndex()] = {};
		}
		// The character destroys its inner body.
		Characters.erase(it);
		EndContacts(uuid, false);
	}

	void PhysicsWorld::Impl::OnCharacterChanged(entt::registry& registry, entt::entity entity)
	{
		if (const auto* id = registry.try_get<IDComponent>(entity))
			DirtyCharacters.Insert(id->ID);
	}

	void PhysicsWorld::Impl::RecreateCharacter(Entity entity)
	{
		JPH::Vec3 previousVelocity = JPH::Vec3::sZero();
		glm::vec3 moveVelocity(0.0f);
		if (entity)
		{
			auto existing = Characters.find(entity.GetUUID());
			if (existing != Characters.end())
			{
				// Rebuilding (e.g. after a collider change) keeps the character moving.
				previousVelocity = existing->second.Character->GetLinearVelocity();
				moveVelocity = existing->second.MoveVelocity;
				RemoveCharacter(entity.GetUUID());
			}
		}
		if (!entity || !entity.HasComponent<CharacterControllerComponent>())
			return;

		const auto& controller = entity.GetComponent<CharacterControllerComponent>();
		// Problems with the settings, joined with "; " and logged when they differ from the last ones logged.
		std::vector<std::string> warnings;
		auto report = [&]() { CharacterWarnings.Report(entity, warnings); };
		if (entity.HasComponent<RigidBodyComponent>())
			warnings.emplace_back("its RigidBody is ignored (the character controller replaces it)");

		glm::vec3 position;
		glm::quat rotation;
		glm::vec3 scale;
		if (!Math::DecomposeTransform(ScenePtr->GetWorldTransform(entity), position, rotation, scale))
		{
			warnings.emplace_back("degenerate transform; no character created");
			report();
			Unbuilt.insert(entity.GetUUID());
			return;
		}
		Unbuilt.erase(entity.GetUUID());
		// The collider shapes, scaled as for a rigid body, and the smaller inner body's (InnerShapeFraction).
		// Characters move by shape casts, which need a convex mesh collider.
		const ColliderShape colliders = BuildColliderShape(entity, scale, 1.0f, "a character", false, false, warnings);
		if (!colliders.Shape)
		{
			warnings.emplace_back("no character created");
			report();
			return;
		}
		// Same colliders, so its warnings would only repeat the outer shape's.
		std::vector<std::string> ignoredWarnings;
		const ColliderShape inner = BuildColliderShape(entity, scale, InnerShapeFraction, "a character", false, false, ignoredWarnings);
		if (!inner.Shape)
		{
			warnings.emplace_back("failed to build its inner shape; no character created");
			report();
			return;
		}
		const JPH::RefConst<JPH::Shape> shape = colliders.Shape;

		uint32_t layer = 0;
		if (const auto index = Layers.Find(controller.Layer))
			layer = *index;
		else
			warnings.emplace_back("unknown physics layer '" + controller.Layer + "', using Default");

		JPH::CharacterVirtualSettings settings;
		settings.mShape = shape;
		settings.mInnerBodyShape = inner.Shape;
		settings.mInnerBodyLayer = MakeObjectLayer(true, layer);
		const glm::vec3 gravity = FromJolt(System->GetGravity());
		settings.mUp = glm::length(gravity) > 1e-6f ? ToJolt(-glm::normalize(gravity)) : JPH::Vec3::sAxisY();
		auto* character = new JPH::CharacterVirtual(&settings, ToJolt(position), ToJolt(rotation), static_cast<uint64_t>(entity.GetUUID()), System.get());
		character->SetLinearVelocity(previousVelocity);
		character->SetCharacterVsCharacterCollision(&CharacterCollision);
		character->SetListener(&CharacterLayers);

		Impl::CharacterRecord& record = Characters[entity.GetUUID()];
		record.Character = character;
		CharacterCollision.Add(character);
		record.Settings = controller;
		record.Layer = layer;
		record.BorrowedMesh = colliders.BorrowedMesh;
		// A copy: configuration and reporting keep using warnings below.
		record.BuildWarnings = warnings; // NOLINT(performance-use-std-move)
		record.MoveVelocity = moveVelocity;
		record.LastPosition = position;
		record.LastRotation = rotation;
		record.BuiltScale = scale;
		// The shape's bounds relative to the character's position (Jolt's local bounds are around the centre of mass).
		record.Bounds = shape->GetLocalBounds();
		record.Bounds.Translate(shape->GetCenterOfMass());
		Impl::UpdateSupportingVolume(record);
		// Jolt leaves the inner body out when the body limit is reached; the character still walks, but
		// queries, triggers and other bodies cannot see it.
		if (!character->GetInnerBodyID().IsInvalid())
		{
			BodyToEntity[character->GetInnerBodyID().GetIndexAndSequenceNumber()] = entity.GetUUID();
			// Jolt created the inner body, not RecreateBody: its slot may still hold an earlier body's modes.
			ContactListener.Materials[character->GetInnerBodyID().GetIndex()] = {};
		}
		else
			BS_CORE_ERROR("Physics: body limit reached; character '{}' has no body other bodies can touch", entity.GetName());
		Impl::ConfigureCharacter(record, warnings);
		report();

		// A new character starts in the air; find its ground now so a rebuild (e.g. a crouch changing the
		// collider) does not lose a step of IsGrounded or a jump.
		const JPH::ObjectLayer objectLayer = MakeObjectLayer(true, layer);
		character->RefreshContacts(JPH::DefaultBroadPhaseLayerFilter(ObjectVsBroadPhaseLayerFilter, objectLayer), JPH::DefaultObjectLayerFilter(ObjectLayerPairFilter, objectLayer), CharacterBodyFilter, JPH::ShapeFilter(),
								   *TempAllocator);
	}

	void PhysicsWorld::Impl::ResizeCharacter(Entity entity, const glm::vec3& scale)
	{
		auto found = Characters.find(entity.GetUUID());
		if (found == Characters.end())
			return;
		Impl::CharacterRecord& record = found->second;
		std::vector<std::string> warnings;
		const ColliderShape outer = BuildColliderShape(entity, scale, 1.0f, "a character", false, false, warnings);
		const ColliderShape inner = BuildColliderShape(entity, scale, InnerShapeFraction, "a character", false, false, warnings);
		std::ranges::sort(warnings);
		warnings.erase(std::unique(warnings.begin(), warnings.end()), warnings.end());
		ResizeWarnings.Report(entity, warnings);
		if (!outer.Shape || !inner.Shape)
		{
			RecreateCharacter(entity);
			return;
		}

		JPH::CharacterVirtual& character = *record.Character;
		const JPH::ObjectLayer layer = MakeObjectLayer(true, record.Layer);
		// Switches whatever the new shape overlaps (FLT_MAX); the next update pushes a grown character out. The
		// character keeps its contacts and ground.
		character.SetShape(outer.Shape, FLT_MAX, JPH::DefaultBroadPhaseLayerFilter(ObjectVsBroadPhaseLayerFilter, layer), JPH::DefaultObjectLayerFilter(ObjectLayerPairFilter, layer), CharacterBodyFilter, JPH::ShapeFilter(),
						   *TempAllocator);
		character.SetInnerBodyShape(inner.Shape);
		record.Bounds = outer.Shape->GetLocalBounds();
		record.Bounds.Translate(outer.Shape->GetCenterOfMass());
		Impl::UpdateSupportingVolume(record);
		record.BuiltScale = scale;
	}

	void PhysicsWorld::Impl::ApplyCharacterSettings(Entity entity)
	{
		auto it = Characters.find(entity.GetUUID());
		if (!entity.HasComponent<CharacterControllerComponent>())
			return;
		const auto& controller = entity.GetComponent<CharacterControllerComponent>();
		// A character that could not be built may be buildable now; the layer is baked into the inner body.
		if (it == Characters.end() || controller.Layer != it->second.Settings.Layer)
		{
			RecreateCharacter(entity);
			return;
		}
		Impl::CharacterRecord& record = it->second;
		// Scripts may set the component every frame; the same values change nothing.
		if (controller == record.Settings)
			return;
		record.Settings = controller;
		std::vector<std::string> warnings = record.BuildWarnings;
		Impl::ConfigureCharacter(record, warnings);
		CharacterWarnings.Report(entity, warnings);
	}

	void PhysicsWorld::Impl::UpdateCharacters(float fixedStep)
	{
		if (Characters.empty())
			return;
		BS_CORE_ASSERT(CharacterCollision.mCharacters.size() == Characters.size(), "CharacterCollision is out of sync with the characters");
		const JPH::Vec3 sceneGravity = System->GetGravity();
		// Registry order, not hash order: characters push bodies, so the order shows in replays.
		for (entt::entity handle : ScenePtr->GetAllEntitiesWith<CharacterControllerComponent>())
		{
			Entity entity(handle, ScenePtr);
			auto it = Characters.find(entity.GetUUID());
			if (it == Characters.end())
				continue;
			Impl::CharacterRecord& record = it->second;
			JPH::CharacterVirtual& character = *record.Character;

			glm::vec3 position;
			glm::quat rotation;
			glm::vec3 scale;
			if (!Math::DecomposeTransform(ScenePtr->GetWorldTransform(entity), position, rotation, scale))
			{
				if (!record.Degenerate)
				{
					std::vector<std::string> warnings = record.BuildWarnings;
					warnings.emplace_back("degenerate transform (e.g. a zero scale); the character does not move");
					CharacterWarnings.Report(entity, warnings);
				}
				record.Degenerate = true;
				continue;
			}
			if (record.Degenerate)
			{
				record.Degenerate = false;
				CharacterWarnings.Report(entity, record.BuildWarnings);
			}
			const JPH::ObjectLayer layer = MakeObjectLayer(true, record.Layer);
			const JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(ObjectVsBroadPhaseLayerFilter, layer);
			const JPH::DefaultObjectLayerFilter objectLayerFilter(ObjectLayerPairFilter, layer);
			// Other characters are met through CharacterCollision, not their inner bodies.
			const JPH::BodyFilter& bodyFilter = CharacterBodyFilter;
			const JPH::ShapeFilter shapeFilter;

			// A script that moved the transform teleports the character (whose old ground no longer holds it);
			// its rotation always follows the entity.
			if (glm::any(glm::greaterThan(glm::abs(position - record.LastPosition), glm::vec3(1e-5f))))
			{
				character.SetPosition(ToJolt(position));
				character.RefreshContacts(broadPhaseFilter, objectLayerFilter, bodyFilter, shapeFilter, *TempAllocator);
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
			glm::vec3 requested = record.MoveVelocity;
			if (record.RootMotionVelocity)
			{
				const glm::vec3 worldUp = FromJolt(up);
				const glm::vec3 rootVelocity = *record.RootMotionVelocity;
				const glm::vec3 tangent = rootVelocity - worldUp * glm::dot(rootVelocity, worldUp);
				requested = worldUp * glm::dot(requested, worldUp) + tangent;
			}
			const JPH::Vec3 move = ToJolt(requested);
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
			character.ExtendedUpdate(fixedStep, gravity, update, broadPhaseFilter, objectLayerFilter, bodyFilter, shapeFilter, *TempAllocator);
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
			// The transform decomposed above, so this cannot fail.
			Impl::WritePose(entity, record.LastPosition, rotation);
		}
	}

	bool PhysicsWorld::HasCharacter(Entity entity) const
	{
		return m_Impl->Owns(entity) && m_Impl->Characters.contains(entity.GetUUID());
	}

	void PhysicsWorld::MoveCharacter(Entity entity, const glm::vec3& velocity)
	{
		if (!m_Impl->Owns(entity))
			return;
		auto it = m_Impl->Characters.find(entity.GetUUID());
		if (it != m_Impl->Characters.end() && IsFiniteVec(velocity))
			it->second.MoveVelocity = velocity;
	}

	void PhysicsWorld::SetRootMotionDelta(Entity entity, std::optional<glm::vec3> displacement)
	{
		if (!m_Impl->Owns(entity) || !entity.HasComponent<CharacterControllerComponent>())
			return;
		if (!displacement)
		{
			if (auto it = m_Impl->RootMotion.find(entity.GetUUID()); it != m_Impl->RootMotion.end())
				it->second.Active = false;
			return;
		}
		auto& request = m_Impl->RootMotion[entity.GetUUID()];
		if (!IsFiniteVec(*displacement))
		{
			if (!request.InvalidReported)
				BS_CORE_ERROR("Physics: character '{}': root motion displacement must be finite", entity.GetName());
			request.InvalidReported = true;
			return;
		}
		request.InvalidReported = false;
		request.Active = true;
		request.Pending += *displacement;
	}

	bool PhysicsWorld::IsCharacterGrounded(Entity entity) const
	{
		if (!m_Impl->Owns(entity))
			return false;
		auto it = m_Impl->Characters.find(entity.GetUUID());
		return it != m_Impl->Characters.end() && it->second.Character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
	}

	std::optional<glm::vec3> PhysicsWorld::GetCharacterGroundNormal(Entity entity) const
	{
		if (!m_Impl->Owns(entity))
			return std::nullopt;
		auto it = m_Impl->Characters.find(entity.GetUUID());
		if (it == m_Impl->Characters.end() || !it->second.Character->IsSupported())
			return std::nullopt;
		return FromJolt(it->second.Character->GetGroundNormal());
	}

	uint32_t PhysicsWorld::GetCharacterCount() const
	{
		return static_cast<uint32_t>(m_Impl->Characters.size());
	}

}
