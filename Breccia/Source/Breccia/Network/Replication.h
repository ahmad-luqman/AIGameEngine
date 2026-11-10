#pragma once

#include "Breccia/Core/UUID.h"
#include "Breccia/Network/NetProtocol.h"
#include "Breccia/Scene/Components.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Breccia {

	// One replicated field's value as it is now: its JSON (what ComponentRegistry writes) and that JSON's NetValue
	// encoding, which is what changes are detected on.
	struct ReplicatedFieldValue
	{
		std::vector<uint8_t> Bytes;
		nlohmann::json Value;
	};

	// A component of a networked entity that declares replicated fields, with all of them in ReplicatedFields order.
	struct ReplicatedComponent
	{
		uint32_t Component = 0;
		std::vector<ReplicatedFieldValue> Fields;
		// A field could not be encoded this tick (text that is not UTF-8): the component is present but nothing about it
		// is sent until it can be (Fields is then empty). Never mistaken for a removal.
		bool Unsendable = false;
	};

	// A networked entity as it is this tick.
	struct ReplicatedEntity
	{
		UUID Entity = 0;
		UUID Parent = 0;
		// Ancestors in the scene: among entities whose wait (priority totals) is equal, parents go first.
		uint32_t Depth = 0;
		float Priority = 1.0f;
		uint32_t Owner = 0;
		NetworkAuthority Authority = NetworkAuthority::Server;
		// Sorted by component index.
		std::vector<ReplicatedComponent> Components;
	};

	// The networked entities of one tick, captured once and shared by every connection's sender. Spawn records (an
	// entity's whole tree) are encoded on first use, at most once per tick.
	class ReplicationWorld
	{
	public:
		// The encoded NetEntitySpawn of an entity, or nullopt when it cannot be sent (logged by the builder).
		using SpawnBuilder = std::function<std::optional<std::vector<uint8_t>>(const ReplicatedEntity&)>;

		ReplicationWorld() = default;
		// Entity IDs must be unique (a message names each entity once).
		ReplicationWorld(std::vector<ReplicatedEntity> entities, SpawnBuilder spawnBuilder);

		const std::vector<ReplicatedEntity>& GetEntities() const { return m_Entities; }
		bool Contains(UUID entity) const { return m_Ids.contains(entity); }
		// Null when the spawn cannot be built (or the world has no builder: a client's).
		const std::vector<uint8_t>* GetSpawnRecord(size_t index) const;

	private:
		// One per entity: whether the builder ran, and what it returned.
		struct SpawnCache
		{
			bool Built = false;
			std::optional<std::vector<uint8_t>> Record;
		};

		std::vector<ReplicatedEntity> m_Entities;
		std::unordered_set<UUID> m_Ids;
		SpawnBuilder m_SpawnBuilder;
		mutable std::vector<SpawnCache> m_Spawns;
	};

	// Which way a sender's messages go.
	enum class ReplicationDirection : uint8_t
	{
		// Snapshots: spawns, despawns, component additions and removals, parents and field changes.
		HostToClient,
		// Client updates: field changes of entities the host already has, nothing else.
		ClientToHost
	};

	// How much of a component a sender includes for an entity.
	enum class ReplicationScope : uint8_t
	{
		Everything,
		// Whether it exists (an addition carries the values it starts with), not its later field changes: the receiver decides
		// those itself (the entity's owner). Host to client only: client updates carry no presence, so there it sends
		// nothing about the component. The caller keeps the ownership component (Networked) Everything.
		PresenceOnly
	};

	using ReplicationFilter = std::function<ReplicationScope(const ReplicatedEntity& entity, uint32_t component)>;

	struct ReplicationSenderOptions
	{
		ReplicationDirection Direction = ReplicationDirection::HostToClient;
		// Largest message the receiver accepts (NetworkSystem::MaxHostMessageBytes, or MaxClientMessageBytes for client
		// updates); a record that cannot fit in one is not sent while it stays that large (logged once per entity). More
		// than the message header.
		size_t MaxMessageBytes = size_t{ 1024 } * 1024;
		// Messages remembered until acknowledged (at least 1). An acknowledgement of a forgotten one is ignored; what it
		// carried is sent again.
		uint32_t HistoryLength = 256;
	};

	// Builds one direction's state messages for one connection, sending only what the receiver may not have: each message
	// carries every value that differs from the last one the receiver acknowledged, or that a message not yet acknowledged
	// carried differently, so once the receiver applies a message (and drops anything older) it is up to date, as of that
	// message, on everything any message up to it carried; only records that waited for the budget follow later. Spawns repeat until
	// acknowledged, then become field updates; despawns repeat until acknowledged, even if the entity is back meanwhile
	// (its new spawn follows). When the byte budget is short, entities that wait gain their Priority each tick and the
	// largest totals go first. Pure bookkeeping over ReplicationWorld: no scene, no transport.
	class ReplicationSender
	{
	public:
		explicit ReplicationSender(ReplicationSenderOptions options = {});

		// The next message (a Snapshot, or a ClientUpdate for ClientToHost). acknowledged is the newest message of the other
		// direction this side applied. budget is the bytes the message may use: records beyond it wait, except that the
		// first one goes when budget is positive (a large spawn must go some time; the caller's budget runs into debt).
		// extras go into the message as they are (the time, predicted characters' states, inputs), outside the budget.
		std::vector<uint8_t> Build(const ReplicationWorld& world, uint32_t acknowledged, size_t budget, const ReplicationFilter& filter = {}, const NetStateExtras& extras = {});
		// The receiver applied message `sequence` (and drops anything older it receives later). False, changing nothing, for
		// a sequence this sender never built: the peer is broken or lying.
		bool Acknowledge(uint32_t sequence);

		// Entities whose spawn the receiver acknowledged and that it has not been told to despawn.
		size_t GetKnownEntityCount() const;
		// Records of the last Build that waited for budget.
		size_t GetDeferredRecordCount() const { return m_Deferred; }

	private:
		// One value as the receiver last acknowledged it, and what unacknowledged messages carried.
		struct Tracked
		{
			std::optional<std::vector<uint8_t>> Acked;
			uint32_t AckedSequence = 0;
			// The newest message that carried the value, and what it carried.
			uint32_t SentSequence = 0;
			std::vector<uint8_t> Sent;
			// The newest message that carried a value other than Sent (0: none): until one at least as new is acknowledged,
			// the receiver may hold that other value.
			uint32_t StaleSequence = 0;

			bool Pending(const std::vector<uint8_t>& current) const
			{
				if (!Acked || *Acked != current)
					return true;
				// Acknowledged as it is now: only an unacknowledged message carrying something else can undo that.
				return (SentSequence > AckedSequence && Sent != current) || StaleSequence > AckedSequence;
			}
			void MarkSent(const std::vector<uint8_t>& value, uint32_t sequence);
			void Acknowledge(const std::vector<uint8_t>& value, uint32_t sequence);
		};

		struct ComponentTrack
		{
			Tracked Presence;
			std::vector<Tracked> Fields;
			// The receiver decided the fields last time (PresenceOnly): what was acknowledged before says nothing about what
			// it holds now.
			bool ReceiverDecided = false;
		};

		struct EntityTrack
		{
			bool Known = false;
			// The newest message that carried the entity's spawn, and its despawn (0: none).
			uint32_t SpawnSent = 0;
			uint32_t DespawnSent = 0;
			Tracked Parent;
			std::map<uint32_t, ComponentTrack> Components;
			float Accumulator = 0.0f;
			bool ReportedOversized = false;
		};

		// What one record of a sent message carried, applied to the tracks when the message is acknowledged.
		struct SentRecord
		{
			enum class Kind : uint8_t
			{
				Spawn,
				Update,
				Despawn
			};
			struct Value
			{
				uint32_t Component = 0;
				// Index into the component's fields; nullopt for the component's presence.
				std::optional<uint32_t> Field;
				std::vector<uint8_t> Bytes;
			};

			Kind Type = Kind::Update;
			UUID Entity = 0;
			std::optional<std::vector<uint8_t>> Parent;
			std::vector<Value> Values;
		};

		struct SentMessage
		{
			uint32_t Sequence = 0;
			std::vector<SentRecord> Records;
		};

		// The update record for a known entity (empty Components and no Parent: nothing to send).
		NetEntityUpdate BuildUpdate(const ReplicatedEntity& entity, EntityTrack& track, const ReplicationFilter& filter, uint32_t sequence, SentRecord& record) const;
		static Tracked& TrackFor(EntityTrack& track, const SentRecord::Value& value);
		void MarkSent(EntityTrack& track, const SentRecord& record, uint32_t sequence);

		ReplicationSenderOptions m_Options;
		std::unordered_map<UUID, EntityTrack> m_Entities;
		std::deque<SentMessage> m_History;
		uint32_t m_NextSequence = 1;
		size_t m_Deferred = 0;
	};

}
