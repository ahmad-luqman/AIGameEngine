#include "Basalt/Physics/PhysicsWorldImpl.h"

#include "Basalt/Scene/SceneEventListener.h"
#include "Basalt/Scripting/ScriptEngine.h"

#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>

#include <algorithm>
#include <mutex>
#include <map>
#include <tuple>

namespace Basalt {

	using namespace PhysicsInternal;

	// Tracks body/sub-shape contacts while exposing one begin/end per entity pair. Sleeping bodies retain
	// suspended contacts until both bodies wake and Jolt can confirm whether they still touch.
	struct PhysicsWorld::Impl::ContactState
	{
		// Lower entity UUID, its body/sub-shape token, higher entity UUID, its token.
		// A ragdoll has several bodies for one entity: sub-shape IDs alone would alias its limbs.
		using ContactKey = std::tuple<uint64_t, uint64_t, uint64_t, uint64_t>;
		std::set<ContactKey> ActiveContacts;
		// Number of active body/sub-shape tokens for each entity pair.
		std::map<EntityPair, int> PairCounts;
		// A subset of ActiveContacts: Jolt removes sleeping contacts without physical separation.
		std::set<ContactKey> SuspendedContacts;

		void Acquire(const ContactKey& key)
		{
			SuspendedContacts.erase(key);
			if (ActiveContacts.insert(key).second)
				PairCounts[EntityPair(std::get<0>(key), std::get<2>(key))]++;
		}

		// Returns true only when the last body/sub-shape contact of a pair was released.
		bool Release(const ContactKey& key)
		{
			SuspendedContacts.erase(key);
			if (ActiveContacts.erase(key) == 0)
				return false;
			const EntityPair pair(std::get<0>(key), std::get<2>(key));
			auto count = PairCounts.find(pair);
			BS_CORE_ASSERT(count != PairCounts.end() && count->second > 0, "Uncounted active contact");
			if (count == PairCounts.end() || count->second <= 0)
			{
				BS_CORE_ERROR("Physics: uncounted active contact between {} and {}", pair.first, pair.second);
				PairCounts.erase(pair);
				return true;
			}
			if (--count->second != 0)
				return false;
			PairCounts.erase(count);
			return true;
		}
	};

	PhysicsWorld::Impl::Impl(Scene* scene)
		: ScenePtr(scene)
		, Contacts(CreateScope<ContactState>())
	{
		BS_CORE_ASSERT(ScenePtr, "PhysicsWorld requires an owning scene");
		BS_CORE_ASSERT(ScenePtr->GetRegistry().view<BodyRecord>().empty(), "Only one PhysicsWorld may own a scene registry");
	}

	PhysicsWorld::Impl::~Impl() = default;

	bool PhysicsWorld::Impl::IsTrigger(UUID uuid) const
	{
		auto* body = FindBody(uuid);
		return body != nullptr && body->IsTrigger;
	}

	void PhysicsWorld::Impl::EndContacts(UUID uuid, bool trigger)
	{
		const uint64_t id = static_cast<uint64_t>(uuid);
		std::set<EntityPair> ended;
		for (auto key = Contacts->ActiveContacts.begin(); key != Contacts->ActiveContacts.end();)
		{
			if (std::get<0>(*key) != id && std::get<2>(*key) != id)
			{
				++key;
				continue;
			}
			const EntityPair pair(std::get<0>(*key), std::get<2>(*key));
			const auto removed = *key++;
			if (Contacts->Release(removed))
				ended.insert(pair);
		}

		for (const EntityPair& pair : ended)
		{
			const UUID other = pair.first == id ? UUID(pair.second) : UUID(pair.first);
			const bool isTrigger = trigger || IsTrigger(other);
			Entity a = ScenePtr->GetEntityByUUID(pair.first);
			Entity b = ScenePtr->GetEntityByUUID(pair.second);
			if (a && b)
				DispatchContact(isTrigger ? ContactEventType::TriggerExit : ContactEventType::CollisionEnd, a, b, std::nullopt);
		}
	}

	void PhysicsWorld::Impl::DispatchContact(ContactEventType type, Entity first, Entity second, const std::optional<ContactInfo>& contact)
	{
		if (SceneEventListener* listener = ScenePtr->GetEventListener())
			listener->OnContact(*ScenePtr, type, first, second, contact);
		if (ScriptEngine* scriptEngine = ScenePtr->GetScriptEngine())
			scriptEngine->OnContactEvent(type, first, second, contact);
	}

	std::optional<ContactInfo> PhysicsWorld::Impl::SeparationContact(UUID first, UUID second, float fixedStep) const
	{
		const JPH::BodyInterface& bodies = System->GetBodyInterface();
		// Character inner bodies are teleported and report no velocity: use gameplay velocity. Ragdolls expose
		// every part so separation data comes
		// from the closest pair of limbs, rather than an arbitrary root body.
		using MovingBody = std::pair<JPH::BodyID, JPH::Vec3>;
		auto bodiesOf = [&](UUID uuid) -> std::vector<MovingBody> {
			if (auto* body = FindBody(uuid); body != nullptr)
				return { { body->ID, bodies.GetLinearVelocity(body->ID) } };
			if (auto character = Characters.find(uuid); character != Characters.end())
				return { { character->second.Character->GetInnerBodyID(), ToJolt(character->second.ActualVelocity) } };
			std::vector<MovingBody> parts;
			if (auto ragdoll = Ragdolls.find(uuid); ragdoll != Ragdolls.end() && ragdoll->second.Doll)
				for (auto id : ragdoll->second.Doll->GetBodyIDs())
					parts.emplace_back(id, bodies.GetLinearVelocity(id));
			return parts;
		};
		const auto firstBodies = bodiesOf(first);
		const auto secondBodies = bodiesOf(second);
		// One collector keeps the least-separated pair across every combination of limbs.
		JPH::ClosestHitCollisionCollector<JPH::CollideShapeCollector> collector;
		for (const auto& [bodyA, velocityA] : firstBodies)
		{
			const JPH::TransformedShape shapeA = bodies.GetTransformedShape(bodyA);
			for (const auto& [bodyB, velocityB] : secondBodies)
			{
				const JPH::TransformedShape shapeB = bodies.GetTransformedShape(bodyB);
				// They can separate by one step of relative motion plus Jolt's 0.02 m speculative distance.
				// Twice that distance, with a rotation margin, covers the closest remaining body pair.
				JPH::CollideShapeSettings settings;
				settings.mMaxSeparationDistance = 0.25f + 2.0f * (velocityA - velocityB).Length() * fixedStep;
				shapeB.CollideShape(shapeA.mShape, shapeA.GetShapeScale(), shapeA.GetCenterOfMassTransform(), settings, JPH::RVec3::sZero(), collector);
			}
		}
		if (!collector.HadHit())
			return std::nullopt;
		// Shape 1 is A's: the penetration axis points from A toward B, and the normal from B toward A. Shapes
		// that touch at a single point give no direction, which is no contact to report.
		const JPH::CollideShapeResult& hit = collector.mHit;
		if (hit.mPenetrationAxis.IsNearZero())
			return std::nullopt;
		ContactInfo contact;
		contact.Point = FromJolt(JPH::Vec3(0.5f * (hit.mContactPointOn1 + hit.mContactPointOn2)));
		contact.Normal = FromJolt(-hit.mPenetrationAxis.Normalized());
		return contact;
	}

	void PhysicsWorld::Impl::DispatchContacts(float fixedStep)
	{
		std::vector<RawContactEvent> events;
		{
			std::scoped_lock lock(ContactListener.Mutex);
			events.swap(ContactListener.Events);
		}
		// Worker threads record events in scheduling order; sort so callbacks are deterministic.
		std::sort(events.begin(), events.end(), [](const RawContactEvent& a, const RawContactEvent& b) { return a.Tie() < b.Tie(); });

		struct PairChange
		{
			bool WasActive = false;
			// The strongest new contact, relative to the pair's first (lower UUID) entity. A pair only becomes
			// active through an Added event, so begin and enter events always have one.
			std::optional<ContactInfo> Begin;
		};
		const auto& bodies = System->GetBodyInterface();
		auto sleeping = [&](uint64_t token) {
			const JPH::BodyID id(static_cast<uint32_t>(token >> 32));
			return BodyToEntity.contains(id.GetIndexAndSequenceNumber()) && bodies.GetMotionType(id) == JPH::EMotionType::Dynamic && !bodies.IsActive(id);
		};
		std::map<Impl::EntityPair, PairChange> touched;
		for (const RawContactEvent& event : events)
		{
			auto first = BodyToEntity.find(event.Body1);
			auto second = BodyToEntity.find(event.Body2);
			if (first == BodyToEntity.end() || second == BodyToEntity.end())
				continue;

			uint64_t a = static_cast<uint64_t>(first->second);
			uint64_t b = static_cast<uint64_t>(second->second);
			if (a == b)
				continue;
			uint64_t subA = (static_cast<uint64_t>(event.Body1) << 32) | event.SubShape1;
			uint64_t subB = (static_cast<uint64_t>(event.Body2) << 32) | event.SubShape2;
			// The event's normal points from body 1 toward body 2; ContactInfo's from the second entity
			// toward the first.
			ContactInfo contact = event.Contact.Flipped();
			if (b < a)
			{
				std::swap(a, b);
				std::swap(subA, subB);
				contact = event.Contact;
			}
			const ContactState::ContactKey key(a, subA, b, subB);
			const Impl::EntityPair pair(a, b);

			// A body that falls asleep reports its contacts as removed; it is still touching, so keep them.
			if (!event.Added && (sleeping(subA) || sleeping(subB)))
			{
				if (Contacts->ActiveContacts.contains(key))
					Contacts->SuspendedContacts.insert(key);
				continue;
			}

			PairChange& change = touched.try_emplace(pair, PairChange{ Contacts->PairCounts.contains(pair), std::nullopt }).first->second;
			if (event.Added)
			{
				if (!change.Begin || contact.Impulse > change.Begin->Impulse)
					change.Begin = contact;
				Contacts->Acquire(key);
			}
			else
			{
				Contacts->Release(key);
			}
		}

		// Suspended contacts whose bodies are awake again and were not re-reported this step have ended.
		for (auto key = Contacts->SuspendedContacts.begin(); key != Contacts->SuspendedContacts.end();)
		{
			if (sleeping(std::get<1>(*key)) || sleeping(std::get<3>(*key)))
			{
				++key;
				continue;
			}
			const Impl::EntityPair pair(std::get<0>(*key), std::get<2>(*key));
			touched.try_emplace(pair, PairChange{ Contacts->PairCounts.contains(pair), std::nullopt });
			const auto removed = *key++;
			Contacts->Release(removed);
		}

		// Simulate runs no scripts, but a listener still observes the contacts.
		if (!ScenePtr->GetScriptEngine() && !ScenePtr->GetEventListener())
			return;
		for (const auto& [pair, change] : touched)
		{
			const bool isActive = Contacts->PairCounts.contains(pair);
			if (change.WasActive == isActive)
				continue;
			Entity entityA = ScenePtr->GetEntityByUUID(pair.first);
			Entity entityB = ScenePtr->GetEntityByUUID(pair.second);
			if (!entityA || !entityB)
				continue;
			const bool trigger = IsTrigger(pair.first) || IsTrigger(pair.second);
			ContactEventType type;
			if (trigger)
				type = isActive ? ContactEventType::TriggerEnter : ContactEventType::TriggerExit;
			else
				type = isActive ? ContactEventType::CollisionBegin : ContactEventType::CollisionEnd;
			const std::optional<ContactInfo> contact = isActive ? change.Begin : SeparationContact(UUID(pair.first), UUID(pair.second), fixedStep);
			DispatchContact(type, entityA, entityB, contact);
		}
	}

}
