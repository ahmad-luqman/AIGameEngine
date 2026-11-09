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
	};

	// A networked entity as it is this tick.
	struct ReplicatedEntity
	{
		UUID Entity = 0;
		UUID Parent = 0;
		// Ancestors in the scene: parents go out before their children when both wait.
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
		ReplicationWorld(std::vector<ReplicatedEntity> entities, SpawnBuilder spawnBuilder);

		const std::vector<ReplicatedEntity>& GetEntities() const { return m_Entities; }
		bool Contains(UUID entity) const { return m_Index.contains(entity); }
		// Null when the spawn cannot be built.
		const std::vector<uint8_t>* GetSpawnRecord(size_t index) const;

	private:
		std::vector<ReplicatedEntity> m_Entities;
		std::unordered_map<UUID, size_t> m_Index;
		SpawnBuilder m_SpawnBuilder;
		mutable std::vector<std::optional<std::optional<std::vector<uint8_t>>>> m_Spawns;
	};

	// Whether a sender includes a component's changes for an entity (false: the receiver decides them itself).
	using ReplicationFilter = std::function<bool(const ReplicatedEntity& entity, uint32_t component)>;

	struct ReplicationSenderOptions
	{
		// Host to client: spawns, despawns, component additions and removals, and parents. Off (client to host): only field
		// changes of entities the receiver already has.
		bool Spawns = true;
		// Largest message the transport carries; a record that cannot fit in one is never sent (logged once).
		size_t MaxMessageBytes = size_t{ 1024 } * 1024;
		// Messages remembered until acknowledged. An acknowledgement of a forgotten one is ignored; what it carried is
		// sent again.
		uint32_t HistoryLength = 256;
	};

	// Builds one direction's state messages for one connection, sending only what the receiver has not acknowledged:
	// every message carries each field whose value differs from the last one the receiver acknowledged, or that went out
	// in a message not yet acknowledged, so the newest message the receiver applies always completes its state (older
	// ones it receives later are dropped). Spawns repeat until acknowledged, then become field updates; despawns repeat
	// until acknowledged. When the byte budget is short, entities that wait gain their Priority each tick and the
	// largest totals go first. Pure bookkeeping over ReplicationWorld: no scene, no transport.
	class ReplicationSender
	{
	public:
		explicit ReplicationSender(ReplicationSenderOptions options = {});

		// The next message (Snapshot or ClientUpdate). acknowledged is the newest message of the other direction this
		// side applied. budget is the bytes the records may use: records beyond it wait, except that the first one goes
		// when budget is positive (a large spawn must go some time; the caller's budget runs into debt).
		std::vector<uint8_t> Build(NetMessageType type, const ReplicationWorld& world, uint32_t acknowledged, size_t budget, const ReplicationFilter& filter = {});
		// The receiver applied message `sequence` (and drops anything older it receives later).
		void Acknowledge(uint32_t sequence);

		// Entities whose spawn the receiver acknowledged and that it has not been told to despawn.
		size_t GetKnownEntityCount() const;
		// Records of the last Build that waited for budget.
		size_t GetDeferredRecordCount() const { return m_Deferred; }
		uint32_t GetLastSequence() const { return m_NextSequence - 1; }

	private:
		// A value as the receiver last acknowledged it, and the newest message that carried it.
		struct Tracked
		{
			std::vector<uint8_t> Acked;
			bool HasAcked = false;
			uint32_t AckedSequence = 0;
			uint32_t SentSequence = 0;

			bool Pending(const std::vector<uint8_t>& current) const { return !HasAcked || Acked != current || SentSequence > AckedSequence; }
			void Acknowledge(const std::vector<uint8_t>& value, uint32_t sequence);
		};

		struct ComponentTrack
		{
			Tracked Presence;
			std::vector<Tracked> Fields;
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
				// Index into the component's fields, or PresenceField for the component's presence.
				uint32_t Field = 0;
				std::vector<uint8_t> Bytes;
			};
			static constexpr uint32_t PresenceField = UINT32_MAX;

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
		NetEntityUpdate BuildUpdate(const ReplicatedEntity& entity, EntityTrack& track, const ReplicationFilter& filter, SentRecord& record) const;
		void MarkSent(EntityTrack& track, const SentRecord& record, uint32_t sequence);

		ReplicationSenderOptions m_Options;
		std::unordered_map<UUID, EntityTrack> m_Entities;
		std::deque<SentMessage> m_History;
		uint32_t m_NextSequence = 1;
		size_t m_Deferred = 0;
	};

}
