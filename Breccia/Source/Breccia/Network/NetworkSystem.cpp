#include "Breccia/Network/NetworkSystem.h"

#include "Breccia/Asset/AssetKey.h"
#include "Breccia/Core/FileSystem.h"
#include "Breccia/Core/Log.h"
#include "Breccia/Network/NetValue.h"
#include "Breccia/Network/ReplicationSchema.h"
#include "Breccia/Physics/PhysicsWorld.h"
#include "Breccia/Project/Project.h"
#include "Breccia/Scene/ComponentRegistry.h"
#include "Breccia/Scene/Components.h"
#include "Breccia/Scene/Entity.h"
#include "Breccia/Scene/Scene.h"
#include "Breccia/Scene/SceneSerializer.h"

#include <algorithm>
#include <cmath>

namespace Breccia {

	// A Hello carries the project's name and version, which hosts refuse when longer than this.
	static_assert(NetProtocol::MaxTextLength >= ProjectConfig::MaxTextLength, "a Hello must hold any project name");

	namespace {

		// A positive budget never exceeds a second of the connection's bandwidth, so a quiet connection cannot save up a
		// burst larger than that.
		constexpr double MaxBudgetSeconds = 1.0;

		const ReplicationSchema& Schema()
		{
			return ReplicationSchema::Get();
		}

		uint32_t NetworkedComponentIndex()
		{
			static const uint32_t s_Index = [] {
				const std::optional<uint32_t> index = Schema().FindComponentIndex("Networked");
				BR_CORE_ASSERT(index, "the Networked component is registered");
				return index.value_or(UINT32_MAX);
			}();
			return s_Index;
		}

		std::string IdText(UUID uuid)
		{
			return std::to_string(static_cast<uint64_t>(uuid));
		}

		// Logs a problem once per key (a kind, usually with the entity it concerns).
		void Report(std::unordered_set<std::string>& reported, const std::string& key, const std::string& message)
		{
			if (reported.insert(key).second)
				BR_CORE_WARN("Network: {}", message);
		}

		// Whether an asset reference a host sent stays inside the project: relative, without "..", so a client never
		// opens a file the game does not contain. Empty and unresolved (asset-uuid:) references are fine.
		bool IsSafeAssetReference(const nlohmann::json& value)
		{
			const nlohmann::json* text = &value;
			if (value.is_object())
			{
				const auto path = value.find("Path");
				if (path == value.end())
					return true;
				text = &*path;
			}
			if (!text->is_string())
				return true;
			const std::string& reference = text->get_ref<const std::string&>();
			if (reference.empty() || reference.starts_with("asset-uuid:"))
				return true;
			// The file a sub-asset key ("Model.gltf#material/0") names, as the asset loader splits it: any other '#' is part
			// of the path, so it is checked too.
			const std::filesystem::path path = FileSystem::PathFromUtf8(GetAssetSource(reference));
			if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
				return false;
			// A backslash separates folders, and a colon names a drive, on Windows only: refusing both everywhere keeps
			// every peer's check the same.
			if (reference.find_first_of("\\:") != std::string::npos)
				return false;
			for (const std::filesystem::path& part : path.lexically_normal())
				if (part == "..")
					return false;
			return true;
		}

		// The first asset field of a spawn's records that leaves the project, as "entity #i Component.Field"; empty when
		// none does.
		std::string FindUnsafeAssetReference(const nlohmann::json& records)
		{
			for (size_t i = 0; i < records.size(); i++)
			{
				const nlohmann::json& record = records[i];
				const auto components = record.is_object() ? record.find("Components") : record.end();
				if (components == record.end() || !components->is_object())
					continue;
				for (const auto& [name, data] : components->items())
				{
					const ComponentInfo* info = ComponentRegistry::Find(name);
					if (!info)
						continue;
					std::string unsafe;
					for (const std::string& field : info->AssetFields)
						ForEachFieldValue(data, field, [&](const nlohmann::json& value, const std::string& label) {
							if (unsafe.empty() && !IsSafeAssetReference(value))
								unsafe = label;
						});
					if (!unsafe.empty())
						return fmt::format("entity #{} {}.{}", i, name, unsafe);
				}
			}
			return {};
		}

		bool IsUsableStep(float seconds)
		{
			return std::isfinite(seconds) && seconds > 0.0f;
		}

	}

	NetworkSystem::Identity NetworkSystem::Identity::FromActiveProject()
	{
		const Ref<Project>& project = Project::GetActive();
		return project ? Identity{ project->GetConfig().Name, project->GetConfig().Version } : Identity{};
	}

	NetworkSystem::NetworkSystem(Scene& scene, NetworkRole role, Scope<NetTransport> transport, const NetworkSettings& settings, Identity identity)
		: m_Scene(scene)
		, m_Role(role)
		, m_Transport(std::move(transport))
		, m_Settings(settings)
		, m_Identity(std::move(identity))
		, m_ClientSender(ReplicationSenderOptions{ .Direction = ReplicationDirection::ClientToHost, .MaxMessageBytes = MaxClientMessageBytes })
	{
		BR_CORE_ASSERT(m_Transport, "a network session needs a transport");
		BR_CORE_ASSERT(m_Settings.Validate().empty(), "network settings out of range");
		m_State = role == NetworkRole::Host ? NetConnectionState::Connected : NetConnectionState::Connecting;
	}

	NetworkSystem::~NetworkSystem()
	{
		if (m_Role == NetworkRole::Host && GetAdmittedCount() > 0)
			BR_CORE_INFO("Network: host stopped; {} client(s) disconnected", GetAdmittedCount());
		// Every connection, including a client's attempt the transport has not completed yet.
		m_Transport->DisconnectAll(static_cast<uint32_t>(m_Role == NetworkRole::Host ? NetDisconnectReason::ServerShutdown : NetDisconnectReason::ClientQuit));
		m_Transport->Flush();
		// The physics world may outlive this session (Scene::StopNetwork while playing): its bodies simulate again.
		if (PhysicsWorld* physics = m_Scene.GetPhysicsWorld())
			for (UUID uuid : m_DrivenBodies)
				if (Entity entity = m_Scene.GetEntityByUUID(uuid))
					physics->SetDrivenExternally(entity, false);
	}

	uint16_t NetworkSystem::GetListenPort() const
	{
		return m_Transport->GetListenPort();
	}

	void NetworkSystem::Transmit(NetPeerHandle handle, NetChannel channel, const std::vector<uint8_t>& data, NetTraffic& traffic)
	{
		// What the transport could not take (logged there) is not counted as sent.
		if (!m_Transport->Send(handle, channel, data))
			return;
		m_Statistics.Traffic.Sent(data.size());
		traffic.Sent(data.size());
	}

	NetworkSystem::Connection* NetworkSystem::FindConnection(NetPeerHandle handle)
	{
		auto it = m_Connections.find(handle);
		return it != m_Connections.end() ? &it->second : nullptr;
	}

	bool NetworkSystem::IsActivePeer(uint32_t peerId) const
	{
		if (peerId == 0)
			return false;
		return std::ranges::any_of(m_Connections, [peerId](const auto& entry) { return entry.second.PeerId == peerId; });
	}

	uint32_t NetworkSystem::GetAdmittedCount() const
	{
		return static_cast<uint32_t>(std::ranges::count_if(m_Connections, [](const auto& entry) { return entry.second.IsAdmitted(); }));
	}

	bool NetworkSystem::HasAuthority(Entity entity) const
	{
		const auto* networked = entity ? entity.TryGetComponent<NetworkedComponent>() : nullptr;
		if (!networked)
			return true;
		const bool ownerDecides = networked->Authority == NetworkAuthority::Owner;
		if (m_Role == NetworkRole::Host)
			return !(ownerDecides && IsActivePeer(networked->Owner));
		return m_State == NetConnectionState::Connected && ownerDecides && networked->Owner == m_LocalPeerId && m_LocalPeerId != 0;
	}

	void NetworkSystem::Receive(float deltaTime)
	{
		if (IsUsableStep(deltaTime))
			m_Clock += deltaTime;
		std::vector<NetTransportEvent> events;
		m_Transport->Poll(events);
		for (const NetTransportEvent& event : events)
			HandleEvent(event);

		if (m_Role == NetworkRole::Host)
		{
			// Connected, but never said who it is.
			std::vector<NetPeerHandle> silent;
			for (const auto& [handle, connection] : m_Connections)
				if (!connection.IsAdmitted() && m_Clock - connection.ConnectedAt >= m_Settings.TimeoutSeconds)
					silent.push_back(handle);
			for (NetPeerHandle handle : silent)
				Kick(handle, NetDisconnectReason::HandshakeTimeout, "it sent no Hello");
		}
		// Connected, but the host never answered the Hello (not a Breccia host, or a stuck one).
		else if (m_State == NetConnectionState::Connecting && m_HostHandle && m_Clock - m_HelloSentAt >= m_Settings.TimeoutSeconds)
			LeaveHost(NetDisconnectReason::HandshakeTimeout, true);
		ResolvePendingParents();
		UpdateDrivenBodies();
	}

	void NetworkSystem::HandleEvent(const NetTransportEvent& event)
	{
		switch (event.Type)
		{
			case NetTransportEvent::Kind::Connected:
				if (m_Role == NetworkRole::Host)
				{
					Connection connection{ event.Peer, m_Transport->DescribePeer(event.Peer) };
					connection.ConnectedAt = m_Clock;
					m_Connections.emplace(event.Peer, std::move(connection));
				}
				else if (m_State == NetConnectionState::Connecting && !m_HostHandle)
				{
					m_HostHandle = event.Peer;
					m_HostAddress = m_Transport->DescribePeer(event.Peer);
					m_HelloSentAt = m_Clock;
					Transmit(event.Peer, NetChannel::Reliable, NetProtocol::EncodeHello({ NetProtocol::Version, Schema().GetHash(), m_Identity.Game, m_Identity.GameVersion }), m_HostTraffic);
				}
				break;
			case NetTransportEvent::Kind::Disconnected:
			{
				NetDisconnectReason reason = event.TimedOut ? NetDisconnectReason::Timeout : DisconnectReasonFromCode(event.DisconnectCode);
				if (reason == NetDisconnectReason::Unknown)
					BR_CORE_WARN("Network: {} gave disconnect code {}, which this build does not know", m_Role == NetworkRole::Host ? "a client" : "the host", event.DisconnectCode);
				if (m_Role == NetworkRole::Host)
				{
					auto it = m_Connections.find(event.Peer);
					if (it == m_Connections.end())
						break;
					if (it->second.IsAdmitted())
						BR_CORE_INFO("Network: client {} ({}) left: {}", it->second.PeerId, it->second.Address, DescribeDisconnectReason(reason));
					m_Connections.erase(it);
				}
				else if (m_State != NetConnectionState::Disconnected)
				{
					// Never connected at the transport level: the host could not be reached.
					if (!m_HostHandle && event.TimedOut)
						reason = NetDisconnectReason::ConnectionFailed;
					LeaveHost(reason, false);
				}
				break;
			}
			case NetTransportEvent::Kind::Received:
				m_Statistics.Traffic.Received(event.Data.size());
				if (m_Role == NetworkRole::Host)
				{
					if (Connection* connection = FindConnection(event.Peer))
					{
						connection->Traffic.Received(event.Data.size());
						HandleHostMessage(*connection, event.Data);
					}
				}
				else if (m_HostHandle && event.Peer == *m_HostHandle)
				{
					m_HostTraffic.Received(event.Data.size());
					HandleClientMessage(event.Data);
				}
				break;
		}
	}

	void NetworkSystem::Kick(NetPeerHandle handle, NetDisconnectReason reason, const std::string& detail)
	{
		auto it = m_Connections.find(handle);
		if (it == m_Connections.end())
			return;
		const Connection& connection = it->second;
		const std::string who = connection.IsAdmitted() ? "client " + std::to_string(connection.PeerId) : "a connecting client";
		BR_CORE_WARN("Network: disconnected {} ({}): {} ({})", who, connection.Address, DescribeDisconnectReason(reason), detail);
		m_Transport->Disconnect(handle, static_cast<uint32_t>(reason));
		m_Connections.erase(it);
	}

	void NetworkSystem::LeaveHost(NetDisconnectReason reason, bool tellHost)
	{
		if (tellHost && m_HostHandle)
			m_Transport->Disconnect(*m_HostHandle, static_cast<uint32_t>(reason));
		m_HostHandle.reset();
		m_State = NetConnectionState::Disconnected;
		m_DisconnectReason = reason;
		BR_CORE_WARN("Network: disconnected from the host: {}", DescribeDisconnectReason(reason));
	}

	void NetworkSystem::HandleHostMessage(Connection& connection, const std::vector<uint8_t>& data)
	{
		const NetPeerHandle handle = connection.Handle;
		if (data.size() > MaxClientMessageBytes)
		{
			Kick(handle, NetDisconnectReason::InvalidPacket, "a message of " + std::to_string(data.size()) + " bytes, more than clients may send");
			return;
		}
		std::string error;
		const std::optional<NetMessage> message = NetProtocol::Decode(data, Schema(), error);
		if (!message)
		{
			Kick(handle, NetDisconnectReason::InvalidPacket, error);
			return;
		}
		if (!connection.IsAdmitted())
		{
			if (message->Type != NetMessageType::Hello)
			{
				Kick(handle, NetDisconnectReason::InvalidPacket, "it sent state before its Hello");
				return;
			}
			AcceptHello(connection, std::get<NetHello>(message->Body));
			return;
		}
		if (message->Type != NetMessageType::ClientUpdate)
		{
			Kick(handle, NetDisconnectReason::InvalidPacket, "an admitted client sends only ClientUpdate messages");
			return;
		}
		const NetState& state = std::get<NetState>(message->Body);
		if (state.Sequence <= connection.LastApplied)
		{
			m_Statistics.StaleMessages++;
			return;
		}
		if (!connection.Sender.Acknowledge(state.Acknowledged))
		{
			Kick(handle, NetDisconnectReason::InvalidPacket, "it acknowledged snapshot " + std::to_string(state.Acknowledged) + ", which the host never sent");
			return;
		}
		connection.LastApplied = state.Sequence;
		ApplyClientUpdate(connection, state);
	}

	void NetworkSystem::AcceptHello(Connection& connection, const NetHello& hello)
	{
		const NetPeerHandle handle = connection.Handle;
		if (hello.ProtocolVersion != NetProtocol::Version)
		{
			Kick(handle, NetDisconnectReason::ProtocolMismatch, "protocol " + std::to_string(hello.ProtocolVersion) + ", the host speaks " + std::to_string(NetProtocol::Version));
			return;
		}
		if (hello.SchemaHash != Schema().GetHash())
		{
			Kick(handle, NetDisconnectReason::SchemaMismatch, "its components differ from the host's");
			return;
		}
		if (hello.Game != m_Identity.Game || hello.GameVersion != m_Identity.GameVersion)
		{
			Kick(handle, NetDisconnectReason::GameMismatch, "'" + hello.Game + "' " + hello.GameVersion + ", the host plays '" + m_Identity.Game + "' " + m_Identity.GameVersion);
			return;
		}
		if (const uint32_t admitted = GetAdmittedCount(); admitted >= m_Settings.MaxClients)
		{
			Kick(handle, NetDisconnectReason::ServerFull, std::to_string(admitted) + " clients are connected");
			return;
		}
		connection.PeerId = m_NextPeerId++;
		// A full second of budget to start, so the first snapshots can carry the initial spawns.
		connection.Tokens = static_cast<double>(m_Settings.MaxBytesPerSecond) * MaxBudgetSeconds;
		Transmit(handle, NetChannel::Reliable, NetProtocol::EncodeWelcome({ connection.PeerId, m_Settings.TickRate }), connection.Traffic);
		BR_CORE_INFO("Network: client {} joined from {}", connection.PeerId, connection.Address);
	}

	void NetworkSystem::HandleClientMessage(const std::vector<uint8_t>& data)
	{
		std::string error;
		const std::optional<NetMessage> message = NetProtocol::Decode(data, Schema(), error);
		if (!message)
		{
			BR_CORE_ERROR("Network: the host sent an invalid message: {}", error);
			LeaveHost(NetDisconnectReason::InvalidPacket, true);
			return;
		}
		switch (message->Type)
		{
			case NetMessageType::Welcome:
			{
				if (m_State != NetConnectionState::Connecting)
				{
					BR_CORE_ERROR("Network: the host sent a second Welcome");
					LeaveHost(NetDisconnectReason::InvalidPacket, true);
					return;
				}
				const NetWelcome& welcome = std::get<NetWelcome>(message->Body);
				m_LocalPeerId = welcome.PeerId;
				// The host decides the rate both directions run at, within what this build accepts.
				m_Settings.TickRate = std::clamp(welcome.TickRate, NetworkSettings::MinTickRate, NetworkSettings::MaxTickRate);
				if (m_Settings.TickRate != welcome.TickRate)
					BR_CORE_WARN("Network: the host's tick rate {} is outside [{}, {}]; sending at {}", welcome.TickRate, NetworkSettings::MinTickRate, NetworkSettings::MaxTickRate, m_Settings.TickRate);
				m_State = NetConnectionState::Connected;
				m_ClientTokens = static_cast<double>(m_Settings.MaxBytesPerSecond) * MaxBudgetSeconds;
				BR_CORE_INFO("Network: connected to {} as client {}", m_HostAddress, m_LocalPeerId);
				RemoveLocalNetworkedEntities();
				return;
			}
			case NetMessageType::Snapshot:
			{
				// One that overtook the Welcome: the next repeats it.
				if (m_State != NetConnectionState::Connected)
					return;
				const NetState& state = std::get<NetState>(message->Body);
				if (state.Sequence <= m_LastSnapshot)
				{
					m_Statistics.StaleMessages++;
					return;
				}
				if (!m_ClientSender.Acknowledge(state.Acknowledged))
				{
					BR_CORE_ERROR("Network: the host acknowledged update {}, which this client never sent", state.Acknowledged);
					LeaveHost(NetDisconnectReason::InvalidPacket, true);
					return;
				}
				m_LastSnapshot = state.Sequence;
				ApplySnapshot(state);
				return;
			}
			case NetMessageType::Hello:
			case NetMessageType::ClientUpdate:
				break;
		}
		BR_CORE_ERROR("Network: the host sent a message only clients send");
		LeaveHost(NetDisconnectReason::InvalidPacket, true);
	}

	void NetworkSystem::RemoveLocalNetworkedEntities()
	{
		std::vector<UUID> networked;
		for (auto handle : m_Scene.GetRegistry().view<NetworkedComponent>())
			networked.push_back(Entity(handle, &m_Scene).GetUUID());
		size_t removed = 0;
		for (UUID uuid : networked)
		{
			// An ancestor's destruction may have taken it already.
			if (Entity entity = m_Scene.GetEntityByUUID(uuid); entity && !m_Scene.IsEntityPendingDestruction(entity))
			{
				m_Scene.DestroyEntity(entity);
				removed++;
			}
		}
		if (removed > 0)
			BR_CORE_INFO("Network: removed {} networked entities of the local scene; the host spawns its own", removed);
	}

	void NetworkSystem::ApplySnapshot(const NetState& state)
	{
		for (const NetEntitySpawn& spawn : state.Spawns)
			ApplySpawn(spawn);
		for (const NetEntityUpdate& update : state.Updates)
		{
			const Entity entity = m_Scene.GetEntityByUUID(update.Entity);
			// Not one the host spawned here: a spawn this client refused, or an entity a local script destroyed.
			if (!entity || !m_RemoteEntities.contains(update.Entity))
			{
				m_Statistics.RejectedRecords++;
				Report(m_Reported, "unknown-" + IdText(update.Entity), "the host updated entity " + IdText(update.Entity) + ", which this client does not have (a spawn it refused, or one a script destroyed); its updates are ignored");
				continue;
			}
			ApplyUpdate(entity, update, true, m_Reported);
		}
		for (UUID uuid : state.Despawns)
			ApplyDespawn(uuid);
		// A script may destroy what the host spawned.
		std::erase_if(m_RemoteEntities, [this](UUID uuid) { return !m_Scene.GetEntityByUUID(uuid); });
	}

	void NetworkSystem::ApplyDespawn(UUID uuid)
	{
		if (!m_RemoteEntities.erase(uuid))
			return;
		m_PendingParents.erase(uuid);
		m_Statistics.DespawnsApplied++;
		Entity entity = m_Scene.GetEntityByUUID(uuid);
		if (!entity)
			return;
		// Descendants the host replicates on their own outlive it (the host keeps them under an entity no longer
		// networked): they move to the root and wait for that parent, as a child whose parent never arrived does.
		std::vector<Entity> stack = entity.GetChildren();
		while (!stack.empty())
		{
			Entity descendant = stack.back();
			stack.pop_back();
			if (m_RemoteEntities.contains(descendant.GetUUID()))
			{
				const UUID parent = descendant.GetParent().GetUUID();
				m_Scene.SetParent(descendant, Entity{}, false);
				WaitForParent(descendant.GetUUID(), parent);
				continue;
			}
			for (Entity child : descendant.GetChildren())
				stack.push_back(child);
		}
		m_Scene.DestroyEntity(entity);
	}

	void NetworkSystem::ApplySpawn(const NetEntitySpawn& spawn)
	{
		const std::string id = IdText(spawn.Entity);
		const nlohmann::json& root = spawn.Entities.front();
		const auto rootId = root.is_object() ? root.find("ID") : root.end();
		if (rootId == root.end() || !rootId->is_number_unsigned() || rootId->get<uint64_t>() != static_cast<uint64_t>(spawn.Entity))
		{
			m_Statistics.RejectedRecords++;
			Report(m_Reported, "spawn-" + id, "the host sent a spawn of entity " + id + " whose first record is another entity; it is not shown");
			return;
		}
		// Spawned before (a repeat), or one this client has under that ID already: an authored entity the host made
		// networked, or a member of a tree spawned earlier that is now replicated on its own.
		if (Entity existing = m_Scene.GetEntityByUUID(spawn.Entity))
		{
			m_RemoteEntities.insert(spawn.Entity);
			RefreshFromSpawn(existing, spawn);
			return;
		}
		if (const std::string unsafe = FindUnsafeAssetReference(spawn.Entities); !unsafe.empty())
		{
			m_Statistics.RejectedRecords++;
			Report(m_Reported, "spawn-" + id, "the host spawned entity " + id + ", whose " + unsafe + " names a file outside the project; it is not shown");
			return;
		}
		Entity parent = spawn.Parent.IsValid() ? m_Scene.GetEntityByUUID(spawn.Parent) : Entity{};
		std::string error;
		Entity created = SceneSerializer::CreateEntityTreeWithIds(m_Scene, spawn.Entities, parent, error);
		if (!created)
		{
			m_Statistics.RejectedRecords++;
			Report(m_Reported, "spawn-" + id, "the host's spawn of entity " + id + " could not be created: " + error + "; it is not shown");
			return;
		}
		if (spawn.Parent.IsValid() && !parent)
			WaitForParent(spawn.Entity, spawn.Parent);
		else if (parent && created.GetParent() != parent)
			Report(m_Reported, "parent-" + id, "entity " + id + " cannot go under entity " + IdText(spawn.Parent) + " (the hierarchy would be too deep); it stays at the root");
		m_RemoteEntities.insert(spawn.Entity);
		m_Statistics.SpawnsApplied++;
	}

	void NetworkSystem::RefreshFromSpawn(Entity entity, const NetEntitySpawn& spawn)
	{
		const nlohmann::json& root = spawn.Entities.front();
		const auto components = root.find("Components");
		if (components == root.end() || !components->is_object())
			return;
		// The repeated spawn is the host's state when it was sent, which the host will treat as acknowledged.
		NetEntityUpdate update;
		update.Entity = spawn.Entity;
		update.Parent = spawn.Parent;
		const std::vector<ReplicationSchema::Component>& schema = Schema().GetComponents();
		for (uint32_t index = 0; index < schema.size(); index++)
		{
			const ReplicationSchema::Component& component = schema[index];
			const ComponentInfo& info = *component.Info;
			if (info.ReplicatedFields.empty())
				continue;
			const auto data = components->find(info.Name);
			if (data == components->end())
			{
				if (!component.IsCore && info.Has(entity))
					update.Components.push_back({ index, NetComponentChange::Removed, {} });
				continue;
			}
			NetComponentUpdate change{ index, info.Has(entity) ? NetComponentChange::Changed : NetComponentChange::Added, {} };
			for (uint32_t field = 0; field < info.ReplicatedFields.size(); field++)
				if (const auto value = data->find(info.ReplicatedFields[field]); value != data->end())
					change.Fields.push_back({ field, *value });
			if (!change.Fields.empty() || change.Change == NetComponentChange::Added)
				update.Components.push_back(std::move(change));
		}
		ApplyUpdate(entity, update, true, m_Reported);
	}

	void NetworkSystem::ApplyUpdate(Entity entity, const NetEntityUpdate& update, bool fromHost, std::unordered_set<std::string>& reported)
	{
		const std::string id = IdText(entity.GetUUID());
		if (update.Parent && fromHost)
			ApplyParent(entity, *update.Parent);
		// Who owns the entity first: the rest of the update depends on it (a spawn adopting an entity adds its Networked).
		std::vector<const NetComponentUpdate*> changes;
		changes.reserve(update.Components.size());
		for (const NetComponentUpdate& change : update.Components)
			changes.push_back(&change);
		std::ranges::stable_partition(changes, [](const NetComponentUpdate* change) { return change->Component == NetworkedComponentIndex(); });
		bool authority = false;
		for (size_t i = 0; i < changes.size(); i++)
		{
			const NetComponentUpdate& change = *changes[i];
			// Decided once ownership is applied. Only a networked entity is ever this client's to decide.
			if (i == 0 || changes[i - 1]->Component == NetworkedComponentIndex())
				authority = fromHost && entity.HasComponent<NetworkedComponent>() && HasAuthority(entity);
			const ReplicationSchema::Component* component = Schema().FindComponent(change.Component);
			if (!component)
				continue;
			const ComponentInfo& info = *component->Info;
			const bool networked = change.Component == NetworkedComponentIndex();
			// This client decides the entity's values; the host still decides which components it has and who owns it.
			if (authority && !networked && change.Change == NetComponentChange::Changed)
				continue;
			if (change.Change == NetComponentChange::Removed)
			{
				// Losing Networked is a despawn, which the host sends as one.
				if (!component->IsCore && !networked && info.Has(entity))
					info.Remove(entity);
				continue;
			}
			// A client changes only what its entity already has.
			if (!fromHost && !info.Has(entity))
			{
				m_Statistics.RejectedRecords++;
				Report(reported, "missing-" + info.Name + "-" + id, "a client changed " + info.Name + " of entity " + id + ", which the host's entity does not have; ignored");
				continue;
			}
			// A component the host adds to an entity this client decides starts with the host's values.
			if (authority && !networked && info.Has(entity))
				continue;
			nlohmann::json patch = nlohmann::json::object();
			for (const NetFieldValue& field : change.Fields)
			{
				BR_CORE_ASSERT(field.Field < info.ReplicatedFields.size(), "field indices are checked when a message is decoded");
				patch[info.ReplicatedFields[field.Field]] = field.Value;
			}
			std::string error;
			if (!ComponentRegistry::AddOrPatch(entity, info.Name, patch, error))
			{
				m_Statistics.RejectedValues++;
				Report(reported, "value-" + info.Name + "-" + id, fmt::format("{} sent a value for entity {} that {} refuses: {}", fromHost ? "the host" : "a client", id, info.Name, error));
				continue;
			}
			m_Statistics.UpdatesApplied++;
		}
	}

	void NetworkSystem::WaitForParent(UUID child, UUID parent)
	{
		m_PendingParents[child] = { parent, m_Clock, false };
	}

	void NetworkSystem::ApplyParent(Entity entity, UUID parentId)
	{
		m_PendingParents.erase(entity.GetUUID());
		if (!parentId.IsValid())
		{
			if (entity.GetParent())
				m_Scene.SetParent(entity, Entity{}, false);
			return;
		}
		Entity parent = m_Scene.GetEntityByUUID(parentId);
		if (!parent)
		{
			WaitForParent(entity.GetUUID(), parentId);
			return;
		}
		if (entity.GetParent() != parent && !m_Scene.SetParent(entity, parent, false))
			Report(m_Reported, "parent-" + IdText(entity.GetUUID()), "the host parented entity " + IdText(entity.GetUUID()) + " where it cannot go (a cycle, or too deep)");
	}

	void NetworkSystem::ResolvePendingParents()
	{
		for (auto it = m_PendingParents.begin(); it != m_PendingParents.end();)
		{
			Entity child = m_Scene.GetEntityByUUID(it->first);
			Entity parent = m_Scene.GetEntityByUUID(it->second.Parent);
			if (!child)
				it = m_PendingParents.erase(it);
			else if (parent)
			{
				if (!m_Scene.SetParent(child, parent, false))
					Report(m_Reported, "parent-" + IdText(it->first), "the host parented entity " + IdText(it->first) + " where it cannot go (a cycle, or too deep)");
				it = m_PendingParents.erase(it);
			}
			else
			{
				// The parent may be an entity the host made at runtime without a Networked component, which no client has.
				if (!it->second.Reported && m_Clock - it->second.Since >= PendingParentWarningSeconds)
				{
					BR_CORE_WARN("Network: entity {} still waits for its parent {}, which this client does not have (one the host created without a Networked component?); it stays at the root", IdText(it->first), IdText(it->second.Parent));
					it->second.Reported = true;
				}
				++it;
			}
		}
	}

	void NetworkSystem::ApplyClientUpdate(Connection& connection, const NetState& state)
	{
		const std::string client = "client " + std::to_string(connection.PeerId);
		for (const NetEntityUpdate& update : state.Updates)
		{
			const std::string id = IdText(update.Entity);
			Entity entity = m_Scene.GetEntityByUUID(update.Entity);
			const auto* networked = entity ? entity.TryGetComponent<NetworkedComponent>() : nullptr;
			// Ownership may have changed while the update travelled (too late, not an error), but a client that keeps
			// sending for entities it never owned is broken or cheating: logged once per entity.
			if (!networked || networked->Authority != NetworkAuthority::Owner || networked->Owner != connection.PeerId)
			{
				m_Statistics.RejectedRecords++;
				Report(connection.Reported, "not-owned-" + id, fmt::format("{} sent changes to entity {}, which it does not own (or no longer); ignored", client, id));
				continue;
			}
			const bool hierarchy = update.Parent.has_value();
			const bool structure = std::ranges::any_of(update.Components, [](const NetComponentUpdate& change) { return change.Change != NetComponentChange::Changed || change.Component == NetworkedComponentIndex(); });
			if (hierarchy || structure)
			{
				m_Statistics.RejectedRecords++;
				Report(connection.Reported, "structure", client + " tried to change an entity's parent, components or ownership, which only the host decides; ignored");
				continue;
			}
			ApplyUpdate(entity, update, false, connection.Reported);
		}
	}

	void NetworkSystem::UpdateDrivenBodies()
	{
		PhysicsWorld* physics = m_Scene.GetPhysicsWorld();
		if (!physics)
			return;
		// Sorted: the order bodies are marked is the order physics rebuilds them, which must not follow hash order.
		std::set<UUID> driven;
		for (auto [handle, networked, rigidBody] : m_Scene.GetRegistry().view<NetworkedComponent, RigidBodyComponent>().each())
		{
			Entity entity(handle, &m_Scene);
			if (rigidBody.Type == RigidBodyType::Dynamic && !HasAuthority(entity))
				driven.insert(entity.GetUUID());
		}
		for (UUID uuid : m_DrivenBodies)
			if (!driven.contains(uuid))
				if (Entity entity = m_Scene.GetEntityByUUID(uuid))
					physics->SetDrivenExternally(entity, false);
		for (UUID uuid : driven)
			if (!m_DrivenBodies.contains(uuid))
				physics->SetDrivenExternally(m_Scene.GetEntityByUUID(uuid), true);
		m_DrivenBodies = std::move(driven);
	}

	ReplicationWorld NetworkSystem::CaptureWorld() const
	{
		struct Item
		{
			uint32_t Depth = 0;
			UUID Id = 0;
			Entity Handle;
		};
		std::vector<Item> items;
		for (auto handle : m_Scene.GetRegistry().view<NetworkedComponent>())
		{
			Entity entity(handle, &m_Scene);
			if (m_Scene.IsEntityPendingDestruction(entity))
				continue;
			// A client sends only what it decides.
			if (m_Role == NetworkRole::Client && !HasAuthority(entity))
				continue;
			items.push_back({ m_Scene.GetDepth(entity), entity.GetUUID(), entity });
		}
		// Parents first, then ID order: the same on every run and platform.
		std::ranges::sort(items, [](const Item& a, const Item& b) { return a.Depth != b.Depth ? a.Depth < b.Depth : a.Id < b.Id; });

		const std::vector<ReplicationSchema::Component>& schema = Schema().GetComponents();
		std::vector<ReplicatedEntity> entities;
		entities.reserve(items.size());
		for (const Item& item : items)
		{
			const Entity entity = item.Handle;
			const NetworkedComponent& networked = entity.GetComponent<NetworkedComponent>();
			ReplicatedEntity replicated;
			replicated.Entity = item.Id;
			replicated.Parent = entity.GetParent() ? entity.GetParent().GetUUID() : UUID(0);
			replicated.Depth = item.Depth;
			replicated.Priority = networked.Priority;
			replicated.Owner = networked.Owner;
			replicated.Authority = networked.Authority;
			for (uint32_t index = 0; index < schema.size(); index++)
			{
				const ComponentInfo& info = *schema[index].Info;
				// Ownership is the host's to change.
				if (info.ReplicatedFields.empty() || !info.Has(entity) || (m_Role == NetworkRole::Client && index == NetworkedComponentIndex()))
					continue;
				const nlohmann::json data = info.Serialize(entity);
				ReplicatedComponent component{ index, {} };
				for (const std::string& field : info.ReplicatedFields)
				{
					ReplicatedFieldValue value;
					const auto it = data.find(field);
					value.Value = it != data.end() ? *it : nlohmann::json();
					NetWriter writer;
					if (!NetValue::Encode(value.Value, writer))
					{
						// Present, but nothing about it can be sent until the value is valid again.
						component.Unsendable = true;
						component.Fields.clear();
						Report(m_Reported, "encode-" + info.Name + "." + field + "-" + IdText(item.Id), "cannot send " + info.Name + "." + field + " of '" + entity.GetName() + "' (text that is not UTF-8); the component is not updated on " + (m_Role == NetworkRole::Host ? "clients" : "the host") + " until it can be");
						break;
					}
					value.Bytes = writer.TakeBytes();
					component.Fields.push_back(std::move(value));
				}
				replicated.Components.push_back(std::move(component));
			}
			entities.push_back(std::move(replicated));
		}
		ReplicationWorld::SpawnBuilder builder;
		if (m_Role == NetworkRole::Host)
			builder = [this](const ReplicatedEntity& entity) { return BuildSpawn(entity); };
		return ReplicationWorld(std::move(entities), std::move(builder));
	}

	std::optional<std::vector<uint8_t>> NetworkSystem::BuildSpawn(const ReplicatedEntity& replicated) const
	{
		Entity entity = m_Scene.GetEntityByUUID(replicated.Entity);
		if (!entity)
			return std::nullopt;
		const std::string id = IdText(replicated.Entity);
		nlohmann::json records = SceneSerializer::SerializeEntityTree(entity);
		// Descendants with a Networked component of their own spawn separately, under this one; a prefab's tracking means
		// nothing on a client (instances created during Play keep none either).
		std::unordered_set<uint64_t> excluded;
		nlohmann::json kept = nlohmann::json::array();
		for (size_t i = 0; i < records.size(); i++)
		{
			nlohmann::json& record = records[i];
			const uint64_t recordId = record["ID"].get<uint64_t>();
			const uint64_t parent = record["Parent"].get<uint64_t>();
			nlohmann::json& components = record["Components"];
			if (i > 0 && (excluded.contains(parent) || components.contains("Networked")))
			{
				excluded.insert(recordId);
				continue;
			}
			components.erase("Prefab");
			kept.push_back(std::move(record));
		}
		const NetEntitySpawn spawn{ replicated.Entity, replicated.Parent, std::move(kept) };
		if (std::string problem = NetProtocol::CheckSpawn(spawn); !problem.empty())
		{
			Report(m_Reported, "spawn-" + id, "cannot spawn '" + entity.GetName() + "' (entity " + id + ") on clients: " + problem + " (give parts of a large tree Networked components of their own)");
			return std::nullopt;
		}
		NetWriter writer;
		if (!NetProtocol::WriteSpawn(writer, spawn))
		{
			Report(m_Reported, "spawn-" + id, "cannot spawn '" + entity.GetName() + "' (entity " + id + ") on clients: its data holds text that is not UTF-8");
			return std::nullopt;
		}
		return writer.TakeBytes();
	}

	void NetworkSystem::Send(float deltaTime)
	{
		if (IsUsableStep(deltaTime))
		{
			m_TickAccumulator += deltaTime;
			m_SinceLastTick += deltaTime;
		}
		const float interval = 1.0f / m_Settings.TickRate;
		if (m_TickAccumulator < interval)
			return;
		// One tick per frame at most; a long frame does not queue a burst of them.
		m_TickAccumulator = std::fmod(m_TickAccumulator, interval);
		const double elapsed = m_SinceLastTick;
		m_SinceLastTick = 0.0f;
		const double rate = m_Settings.MaxBytesPerSecond;
		const auto refill = [&](double& tokens) {
			tokens = std::min(tokens + rate * elapsed, rate * MaxBudgetSeconds);
			return tokens > 0.0 ? static_cast<size_t>(tokens) : size_t{ 0 };
		};

		if (m_Role == NetworkRole::Host)
		{
			if (GetAdmittedCount() == 0)
				return;
			const ReplicationWorld world = CaptureWorld();
			const uint32_t networkedIndex = NetworkedComponentIndex();
			for (auto& [handle, connection] : m_Connections)
			{
				if (!connection.IsAdmitted())
					continue;
				const uint32_t peer = connection.PeerId;
				// The client decides the values of what it owns; it hears who owns it and which components it has.
				const ReplicationFilter filter = [peer, networkedIndex](const ReplicatedEntity& entity, uint32_t component) {
					const bool owned = entity.Authority == NetworkAuthority::Owner && entity.Owner == peer;
					return owned && component != networkedIndex ? ReplicationScope::PresenceOnly : ReplicationScope::Everything;
				};
				const std::vector<uint8_t> message = connection.Sender.Build(world, connection.LastApplied, refill(connection.Tokens), filter);
				connection.Tokens -= static_cast<double>(message.size());
				Transmit(handle, NetChannel::Unreliable, message, connection.Traffic);
			}
		}
		else if (m_State == NetConnectionState::Connected && m_HostHandle)
		{
			const std::vector<uint8_t> message = m_ClientSender.Build(CaptureWorld(), m_LastSnapshot, refill(m_ClientTokens));
			m_ClientTokens -= static_cast<double>(message.size());
			Transmit(*m_HostHandle, NetChannel::Unreliable, message, m_HostTraffic);
		}
		// Now rather than at the next frame's Receive: a paced headless host would otherwise add a frame of latency.
		m_Transport->Flush();
	}

	std::vector<NetConnectionInfo> NetworkSystem::GetConnections() const
	{
		std::vector<NetConnectionInfo> result;
		if (m_Role == NetworkRole::Host)
		{
			for (const auto& [handle, connection] : m_Connections)
				if (connection.IsAdmitted())
					result.push_back({ connection.PeerId, connection.Address, m_Transport->GetRoundTripTime(handle), connection.Sender.GetKnownEntityCount(), connection.Traffic });
			std::ranges::sort(result, {}, &NetConnectionInfo::PeerId);
		}
		else if (m_State == NetConnectionState::Connected && m_HostHandle)
			result.push_back({ 0, m_HostAddress, m_Transport->GetRoundTripTime(*m_HostHandle), m_RemoteEntities.size(), m_HostTraffic });
		return result;
	}

}
