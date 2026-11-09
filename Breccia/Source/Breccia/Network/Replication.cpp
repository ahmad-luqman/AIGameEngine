#include "Breccia/Network/Replication.h"

#include "Breccia/Core/Log.h"
#include "Breccia/Network/ReplicationSchema.h"

#include <algorithm>

namespace Breccia {

	namespace {

		// The most a message's type, sequence numbers and three record counts take.
		constexpr size_t HeaderBytes = 1 + 4 + 4 + 3 * 5;
		const std::vector<uint8_t> Present = { 1 };
		const std::vector<uint8_t> Absent = { 0 };

		std::vector<uint8_t> ParentBytes(UUID parent)
		{
			NetWriter writer;
			writer.WriteU64(parent);
			return writer.TakeBytes();
		}

		bool IsCore(uint32_t component)
		{
			const ReplicationSchema::Component* info = ReplicationSchema::Get().FindComponent(component);
			return info && info->IsCore;
		}

	}

	ReplicationWorld::ReplicationWorld(std::vector<ReplicatedEntity> entities, SpawnBuilder spawnBuilder)
		: m_Entities(std::move(entities))
		, m_SpawnBuilder(std::move(spawnBuilder))
		, m_Spawns(m_Entities.size())
	{
		for (size_t i = 0; i < m_Entities.size(); i++)
			m_Index.emplace(m_Entities[i].Entity, i);
	}

	const std::vector<uint8_t>* ReplicationWorld::GetSpawnRecord(size_t index) const
	{
		if (index >= m_Entities.size() || !m_SpawnBuilder)
			return nullptr;
		if (!m_Spawns[index])
			m_Spawns[index] = m_SpawnBuilder(m_Entities[index]);
		return *m_Spawns[index] ? &**m_Spawns[index] : nullptr;
	}

	void ReplicationSender::Tracked::Acknowledge(const std::vector<uint8_t>& value, uint32_t sequence)
	{
		// An older message's value never replaces a newer acknowledged one.
		if (HasAcked && sequence <= AckedSequence)
			return;
		Acked = value;
		HasAcked = true;
		AckedSequence = sequence;
	}

	ReplicationSender::ReplicationSender(ReplicationSenderOptions options)
		: m_Options(options)
	{
	}

	NetEntityUpdate ReplicationSender::BuildUpdate(const ReplicatedEntity& entity, EntityTrack& track, const ReplicationFilter& filter, SentRecord& record) const
	{
		NetEntityUpdate update;
		update.Entity = entity.Entity;
		record.Type = SentRecord::Kind::Update;
		record.Entity = entity.Entity;
		if (m_Options.Spawns)
		{
			std::vector<uint8_t> parent = ParentBytes(entity.Parent);
			if (track.Parent.Pending(parent))
			{
				update.Parent = entity.Parent;
				record.Parent = std::move(parent);
			}
		}
		for (const ReplicatedComponent& component : entity.Components)
		{
			if (filter && !filter(entity, component.Component))
				continue;
			ComponentTrack& tracked = track.Components[component.Component];
			tracked.Fields.resize(component.Fields.size());
			const bool added = m_Options.Spawns && !IsCore(component.Component) && tracked.Presence.Pending(Present);
			NetComponentUpdate change;
			change.Component = component.Component;
			change.Added = added;
			if (added)
				record.Values.push_back({ component.Component, SentRecord::PresenceField, Present });
			for (uint32_t field = 0; field < component.Fields.size(); field++)
			{
				const ReplicatedFieldValue& value = component.Fields[field];
				if (!added && !tracked.Fields[field].Pending(value.Bytes))
					continue;
				change.Fields.push_back({ field, value.Value });
				record.Values.push_back({ component.Component, field, value.Bytes });
			}
			if (added || !change.Fields.empty())
				update.Components.push_back(std::move(change));
		}
		if (m_Options.Spawns)
		{
			// Components the receiver has (or may have) that the entity no longer has.
			for (const auto& [index, tracked] : track.Components)
			{
				if (IsCore(index) || std::ranges::any_of(entity.Components, [index](const ReplicatedComponent& c) { return c.Component == index; }))
					continue;
				if (filter && !filter(entity, index))
					continue;
				if (!tracked.Presence.Pending(Absent))
					continue;
				update.Components.push_back({ index, true, false, {} });
				record.Values.push_back({ index, SentRecord::PresenceField, Absent });
			}
		}
		return update;
	}

	void ReplicationSender::MarkSent(EntityTrack& track, const SentRecord& record, uint32_t sequence)
	{
		switch (record.Type)
		{
			case SentRecord::Kind::Despawn:
				track.DespawnSent = sequence;
				return;
			case SentRecord::Kind::Spawn:
				track.SpawnSent = sequence;
				break;
			case SentRecord::Kind::Update:
				break;
		}
		if (record.Parent)
			track.Parent.SentSequence = sequence;
		for (const SentRecord::Value& value : record.Values)
		{
			ComponentTrack& component = track.Components[value.Component];
			if (value.Field == SentRecord::PresenceField)
				component.Presence.SentSequence = sequence;
			else
			{
				if (component.Fields.size() <= value.Field)
					component.Fields.resize(value.Field + 1);
				component.Fields[value.Field].SentSequence = sequence;
			}
		}
	}

	std::vector<uint8_t> ReplicationSender::Build(NetMessageType type, const ReplicationWorld& world, uint32_t acknowledged, size_t budget, const ReplicationFilter& filter)
	{
		const uint32_t sequence = m_NextSequence++;
		m_Deferred = 0;
		budget = std::min(budget, m_Options.MaxMessageBytes);
		const size_t maxRecordBytes = m_Options.MaxMessageBytes > HeaderBytes ? m_Options.MaxMessageBytes - HeaderBytes : 0;

		// Entities the receiver may have that are gone (destroyed, or no longer networked), in ID order.
		std::vector<UUID> gone;
		for (const auto& [uuid, track] : m_Entities)
			if (!world.Contains(uuid))
				gone.push_back(uuid);
		std::ranges::sort(gone);
		std::vector<UUID> despawns;
		for (UUID uuid : gone)
		{
			const EntityTrack& track = m_Entities.at(uuid);
			// Never sent to the receiver (or this direction has no despawns): nothing to undo.
			if (!m_Options.Spawns || (!track.Known && track.SpawnSent == 0))
				m_Entities.erase(uuid);
			else
				despawns.push_back(uuid);
		}

		struct Candidate
		{
			size_t Index = 0;
			const std::vector<uint8_t>* SpawnBytes = nullptr;
			std::vector<uint8_t> UpdateBytes;
			SentRecord Record;
			float Accumulator = 0.0f;
			uint32_t Depth = 0;
			size_t Size() const { return SpawnBytes ? SpawnBytes->size() : UpdateBytes.size(); }
		};
		std::vector<Candidate> candidates;
		const std::vector<ReplicatedEntity>& entities = world.GetEntities();
		for (size_t i = 0; i < entities.size(); i++)
		{
			const ReplicatedEntity& entity = entities[i];
			EntityTrack& track = m_Entities[entity.Entity];
			// The receiver is still being told to remove an earlier entity with this ID.
			if (track.DespawnSent != 0)
				continue;
			Candidate candidate;
			candidate.Index = i;
			if (m_Options.Spawns && !track.Known)
			{
				candidate.SpawnBytes = world.GetSpawnRecord(i);
				if (!candidate.SpawnBytes)
					continue;
				candidate.Record.Type = SentRecord::Kind::Spawn;
				candidate.Record.Entity = entity.Entity;
				candidate.Record.Parent = ParentBytes(entity.Parent);
				for (const ReplicatedComponent& component : entity.Components)
				{
					if (!IsCore(component.Component))
						candidate.Record.Values.push_back({ component.Component, SentRecord::PresenceField, Present });
					for (uint32_t field = 0; field < component.Fields.size(); field++)
						candidate.Record.Values.push_back({ component.Component, field, component.Fields[field].Bytes });
				}
			}
			else
			{
				const NetEntityUpdate update = BuildUpdate(entity, track, filter, candidate.Record);
				if (update.Components.empty() && !update.Parent)
					continue;
				NetWriter writer;
				// The values were encoded once already, when the world was captured.
				if (!NetProtocol::WriteUpdate(writer, update))
					continue;
				candidate.UpdateBytes = writer.TakeBytes();
			}
			if (candidate.Size() > maxRecordBytes)
			{
				if (!track.ReportedOversized)
					BR_CORE_ERROR("Network: entity {} needs {} bytes, more than a message can carry ({}); it is not replicated", static_cast<uint64_t>(entity.Entity), candidate.Size(), maxRecordBytes);
				track.ReportedOversized = true;
				continue;
			}
			track.Accumulator += entity.Priority;
			candidate.Accumulator = track.Accumulator;
			candidate.Depth = entity.Depth;
			candidates.push_back(std::move(candidate));
		}
		// Highest wait first; parents before children; then capture order (stable).
		std::ranges::stable_sort(candidates, [](const Candidate& a, const Candidate& b) { return a.Accumulator != b.Accumulator ? a.Accumulator > b.Accumulator : a.Depth < b.Depth; });

		size_t used = HeaderBytes;
		bool any = false;
		const auto fits = [&](size_t size) {
			if (used + size <= budget || (!any && budget > 0))
			{
				used += size;
				any = true;
				return true;
			}
			return false;
		};
		SentMessage message{ sequence, {} };
		std::vector<UUID> sentDespawns;
		for (UUID uuid : despawns)
		{
			if (sentDespawns.size() >= NetProtocol::MaxRecords || !fits(sizeof(uint64_t)))
			{
				m_Deferred++;
				continue;
			}
			sentDespawns.push_back(uuid);
			SentRecord record{ SentRecord::Kind::Despawn, uuid };
			MarkSent(m_Entities.at(uuid), record, sequence);
			message.Records.push_back(std::move(record));
		}
		std::vector<std::vector<uint8_t>> spawns;
		std::vector<std::vector<uint8_t>> updates;
		for (Candidate& candidate : candidates)
		{
			std::vector<std::vector<uint8_t>>& list = candidate.SpawnBytes ? spawns : updates;
			if (list.size() >= NetProtocol::MaxRecords || !fits(candidate.Size()))
			{
				m_Deferred++;
				continue;
			}
			if (candidate.SpawnBytes)
				list.push_back(*candidate.SpawnBytes);
			else
				list.push_back(std::move(candidate.UpdateBytes));
			EntityTrack& track = m_Entities.at(entities[candidate.Index].Entity);
			track.Accumulator = 0.0f;
			MarkSent(track, candidate.Record, sequence);
			message.Records.push_back(std::move(candidate.Record));
		}
		if (!message.Records.empty())
		{
			m_History.push_back(std::move(message));
			while (m_History.size() > m_Options.HistoryLength)
				m_History.pop_front();
		}
		return NetProtocol::AssembleState(type, sequence, acknowledged, spawns, updates, sentDespawns);
	}

	void ReplicationSender::Acknowledge(uint32_t sequence)
	{
		// Older messages are superseded: the receiver drops them, and the newer one repeated whatever they carried that
		// was still unacknowledged.
		while (!m_History.empty() && m_History.front().Sequence < sequence)
			m_History.pop_front();
		if (m_History.empty() || m_History.front().Sequence != sequence)
			return;
		for (const SentRecord& record : m_History.front().Records)
		{
			auto it = m_Entities.find(record.Entity);
			if (it == m_Entities.end())
				continue;
			EntityTrack& track = it->second;
			if (record.Type == SentRecord::Kind::Despawn)
			{
				m_Entities.erase(it);
				continue;
			}
			if (record.Type == SentRecord::Kind::Spawn)
				track.Known = true;
			if (record.Parent)
				track.Parent.Acknowledge(*record.Parent, sequence);
			for (const SentRecord::Value& value : record.Values)
			{
				ComponentTrack& component = track.Components[value.Component];
				if (value.Field == SentRecord::PresenceField)
					component.Presence.Acknowledge(value.Bytes, sequence);
				else
				{
					if (component.Fields.size() <= value.Field)
						component.Fields.resize(value.Field + 1);
					component.Fields[value.Field].Acknowledge(value.Bytes, sequence);
				}
			}
		}
		m_History.pop_front();
	}

	size_t ReplicationSender::GetKnownEntityCount() const
	{
		return static_cast<size_t>(std::ranges::count_if(m_Entities, [](const auto& entry) { return entry.second.Known && entry.second.DespawnSent == 0; }));
	}

}
