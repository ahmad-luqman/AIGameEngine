#include "Breccia/Network/NetworkSystem.h"

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
			static const uint32_t s_Index = Schema().FindComponentIndex("Networked").value_or(UINT32_MAX);
			return s_Index;
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
			std::string reference = text->get<std::string>();
			if (reference.empty() || reference.starts_with("asset-uuid:"))
				return true;
			// A sub-asset key ("Model.gltf#material/0") names a file before its '#'.
			reference = reference.substr(0, reference.find('#'));
			const std::filesystem::path path = FileSystem::PathFromUtf8(reference);
			if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
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
				const auto components = records[i].is_object() ? records[i].find("Components") : nlohmann::json::const_iterator{};
				if (!records[i].is_object() || components == records[i].end() || !components->is_object())
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
		, m_ClientSender(ReplicationSenderOptions{ .Spawns = false, .MaxMessageBytes = MaxClientMessageBytes })
	{
		BR_CORE_ASSERT(m_Transport, "a network session needs a transport");
		m_State = role == NetworkRole::Host ? NetConnectionState::Connected : NetConnectionState::Connecting;
	}

	NetworkSystem::~NetworkSystem()
	{
		if (m_Role == NetworkRole::Host)
		{
			for (auto& [handle, connection] : m_Connections)
				m_Transport->Disconnect(handle, static_cast<uint32_t>(NetDisconnectReason::ServerShutdown));
			if (!m_Connections.empty())
				BR_CORE_INFO("Network: host stopped; {} client(s) disconnected", m_Connections.size());
		}
		else if (m_HostHandle)
			m_Transport->Disconnect(*m_HostHandle, static_cast<uint32_t>(NetDisconnectReason::ClientQuit));
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

	void NetworkSystem::Report(std::unordered_set<std::string>& reported, const std::string& kind, const std::string& message) const
	{
		if (reported.insert(kind).second)
			BR_CORE_WARN("Network: {} (further problems of this kind are not logged)", message);
	}

	void NetworkSystem::Transmit(NetPeerHandle handle, NetChannel channel, const std::vector<uint8_t>& data, Connection* connection)
	{
		m_Transport->Send(handle, channel, data);
		m_Statistics.MessagesSent++;
		m_Statistics.BytesSent += data.size();
		if (connection)
		{
			connection->MessagesSent++;
			connection->BytesSent += data.size();
		}
		else
		{
			m_HostMessagesSent++;
			m_HostBytesSent += data.size();
		}
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
		if (std::isfinite(deltaTime) && deltaTime > 0.0f)
			m_Clock += deltaTime;
		std::vector<NetTransportEvent> events;
		m_Transport->Poll(events);
		for (NetTransportEvent& event : events)
			HandleEvent(event);

		if (m_Role == NetworkRole::Host)
		{
			// Connected, but never said who it is.
			std::vector<NetPeerHandle> silent;
			for (const auto& [handle, connection] : m_Connections)
				if (connection.PeerId == 0 && m_Clock - connection.ConnectedAt >= m_Settings.TimeoutSeconds)
					silent.push_back(handle);
			for (NetPeerHandle handle : silent)
				Kick(m_Connections.at(handle), NetDisconnectReason::HandshakeTimeout, "it sent no Hello");
		}
		ResolvePendingParents();
		UpdateDrivenBodies();
	}

	void NetworkSystem::HandleEvent(NetTransportEvent& event)
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
				else if (m_State == NetConnectionState::Connecting && !m_HostConnected)
				{
					m_HostHandle = event.Peer;
					m_HostAddress = m_Transport->DescribePeer(event.Peer);
					m_HostConnected = true;
					Transmit(event.Peer, NetChannel::Reliable, NetProtocol::EncodeHello({ NetProtocol::Version, Schema().GetHash(), m_Identity.Game, m_Identity.GameVersion }), nullptr);
				}
				break;
			case NetTransportEvent::Kind::Disconnected:
			{
				const NetDisconnectReason reason = event.TimedOut ? NetDisconnectReason::Timeout : DisconnectReasonFromCode(event.DisconnectCode);
				if (m_Role == NetworkRole::Host)
				{
					auto it = m_Connections.find(event.Peer);
					if (it == m_Connections.end())
						break;
					if (it->second.PeerId != 0)
						BR_CORE_INFO("Network: client {} ({}) left: {}", it->second.PeerId, it->second.Address, DescribeDisconnectReason(reason));
					m_Connections.erase(it);
				}
				else if (m_State != NetConnectionState::Disconnected)
				{
					// Never accepted at the transport level: the host could not be reached.
					const NetDisconnectReason clientReason = !m_HostConnected && event.TimedOut ? NetDisconnectReason::ConnectionFailed : reason;
					m_HostHandle.reset();
					LeaveHost(clientReason, false);
				}
				break;
			}
			case NetTransportEvent::Kind::Received:
				m_Statistics.MessagesReceived++;
				m_Statistics.BytesReceived += event.Data.size();
				if (m_Role == NetworkRole::Host)
				{
					if (Connection* connection = FindConnection(event.Peer))
					{
						connection->MessagesReceived++;
						connection->BytesReceived += event.Data.size();
						HandleHostMessage(*connection, event.Data);
					}
				}
				else if (m_HostHandle && event.Peer == *m_HostHandle)
				{
					m_HostMessagesReceived++;
					m_HostBytesReceived += event.Data.size();
					HandleClientMessage(event.Data);
				}
				break;
		}
	}

	void NetworkSystem::Kick(Connection& connection, NetDisconnectReason reason, const std::string& detail)
	{
		const std::string who = connection.PeerId != 0 ? "client " + std::to_string(connection.PeerId) : "a connecting client";
		BR_CORE_WARN("Network: disconnected {} ({}): {} ({})", who, connection.Address, DescribeDisconnectReason(reason), detail);
		const NetPeerHandle handle = connection.Handle;
		m_Transport->Disconnect(handle, static_cast<uint32_t>(reason));
		m_Connections.erase(handle);
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
		if (data.size() > MaxClientMessageBytes)
		{
			Kick(connection, NetDisconnectReason::InvalidPacket, "a message of " + std::to_string(data.size()) + " bytes, more than clients may send");
			return;
		}
		std::string error;
		const std::optional<NetMessage> message = NetProtocol::Decode(data, Schema(), error);
		if (!message)
		{
			Kick(connection, NetDisconnectReason::InvalidPacket, error);
			return;
		}
		if (connection.PeerId == 0)
		{
			if (message->Type != NetMessageType::Hello)
			{
				Kick(connection, NetDisconnectReason::InvalidPacket, "it sent state before its Hello");
				return;
			}
			AcceptHello(connection, std::get<NetHello>(message->Body));
			return;
		}
		if (message->Type != NetMessageType::ClientUpdate)
		{
			Kick(connection, NetDisconnectReason::InvalidPacket, "clients send only Hello and ClientUpdate messages");
			return;
		}
		const NetState& state = std::get<NetState>(message->Body);
		if (state.Sequence <= connection.LastApplied)
		{
			m_Statistics.StaleMessages++;
			return;
		}
		connection.LastApplied = state.Sequence;
		connection.Sender.Acknowledge(state.Acknowledged);
		ApplyClientUpdate(connection, state);
	}

	void NetworkSystem::AcceptHello(Connection& connection, const NetHello& hello)
	{
		if (hello.ProtocolVersion != NetProtocol::Version)
		{
			Kick(connection, NetDisconnectReason::ProtocolMismatch, "protocol " + std::to_string(hello.ProtocolVersion) + ", the host speaks " + std::to_string(NetProtocol::Version));
			return;
		}
		if (hello.SchemaHash != Schema().GetHash())
		{
			Kick(connection, NetDisconnectReason::SchemaMismatch, "its components differ from the host's");
			return;
		}
		if (hello.Game != m_Identity.Game || hello.GameVersion != m_Identity.GameVersion)
		{
			Kick(connection, NetDisconnectReason::GameMismatch, "'" + hello.Game + "' " + hello.GameVersion + ", the host plays '" + m_Identity.Game + "' " + m_Identity.GameVersion);
			return;
		}
		const auto admitted = static_cast<uint32_t>(std::ranges::count_if(m_Connections, [](const auto& entry) { return entry.second.PeerId != 0; }));
		if (admitted >= m_Settings.MaxClients)
		{
			Kick(connection, NetDisconnectReason::ServerFull, std::to_string(admitted) + " clients are connected");
			return;
		}
		connection.PeerId = m_NextPeerId++;
		// A full second of budget to start: the first snapshots carry every spawn.
		connection.Tokens = static_cast<double>(m_Settings.MaxBytesPerSecond) * MaxBudgetSeconds;
		Transmit(connection.Handle, NetChannel::Reliable, NetProtocol::EncodeWelcome({ connection.PeerId, m_Settings.TickRate }), &connection);
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
				// The host decides the rate both directions run at.
				m_Settings.TickRate = std::clamp(welcome.TickRate, NetworkSettings::MinTickRate, NetworkSettings::MaxTickRate);
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
				m_LastSnapshot = state.Sequence;
				m_ClientSender.Acknowledge(state.Acknowledged);
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
			// Not one the host spawned here (or one a local script destroyed): nothing to update.
			if (!entity || !m_RemoteEntities.contains(update.Entity))
			{
				m_Statistics.RejectedRecords++;
				continue;
			}
			ApplyUpdate(entity, update, true, m_Reported);
		}
		for (UUID uuid : state.Despawns)
		{
			if (!m_RemoteEntities.erase(uuid))
				continue;
			m_PendingParents.erase(uuid);
			if (Entity entity = m_Scene.GetEntityByUUID(uuid))
				m_Scene.DestroyEntity(entity);
			m_Statistics.DespawnsApplied++;
		}
		// Networked descendants go with a despawned ancestor, and a script may destroy one: a spawn the host repeats for
		// such an ID creates it again.
		std::erase_if(m_RemoteEntities, [this](UUID uuid) { return !m_Scene.GetEntityByUUID(uuid); });
	}

	void NetworkSystem::ApplySpawn(const NetEntitySpawn& spawn)
	{
		const nlohmann::json& root = spawn.Entities.front();
		const auto rootId = root.is_object() ? root.find("ID") : nlohmann::json::const_iterator{};
		if (!root.is_object() || rootId == root.end() || !rootId->is_number_unsigned() || rootId->get<uint64_t>() != static_cast<uint64_t>(spawn.Entity))
		{
			m_Statistics.RejectedRecords++;
			Report(m_Reported, "spawn-root", "the host sent a spawn whose first record is not the spawned entity");
			return;
		}
		if (Entity existing = m_Scene.GetEntityByUUID(spawn.Entity))
		{
			if (m_RemoteEntities.contains(spawn.Entity))
				RefreshFromSpawn(existing, spawn);
			else
			{
				m_Statistics.RejectedRecords++;
				Report(m_Reported, "spawn-collision", "the host spawned entity " + std::to_string(static_cast<uint64_t>(spawn.Entity)) + ", whose ID a local entity has; it is not shown");
			}
			return;
		}
		if (const std::string unsafe = FindUnsafeAssetReference(spawn.Entities); !unsafe.empty())
		{
			m_Statistics.RejectedRecords++;
			Report(m_Reported, "spawn-asset", "the host spawned an entity whose " + unsafe + " names a file outside the project; it is not shown");
			return;
		}
		Entity parent = spawn.Parent.IsValid() ? m_Scene.GetEntityByUUID(spawn.Parent) : Entity{};
		std::string error;
		if (!SceneSerializer::CreateEntityTreeWithIds(m_Scene, spawn.Entities, parent, error))
		{
			m_Statistics.RejectedRecords++;
			Report(m_Reported, "spawn-invalid", "a spawn from the host could not be created: " + error);
			return;
		}
		if (spawn.Parent.IsValid() && !parent)
			m_PendingParents[spawn.Entity] = spawn.Parent;
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
		const bool authority = HasAuthority(entity);
		for (uint32_t index = 0; index < Schema().GetComponents().size(); index++)
		{
			const ReplicationSchema::Component& component = Schema().GetComponents()[index];
			const ComponentInfo& info = *component.Info;
			if (info.ReplicatedFields.empty() || (authority && index != NetworkedComponentIndex()))
				continue;
			const auto data = components->find(info.Name);
			if (data == components->end())
			{
				if (!component.IsCore && info.Has(entity) && index != NetworkedComponentIndex())
					update.Components.push_back({ index, true, false, {} });
				continue;
			}
			NetComponentUpdate change{ index, false, !info.Has(entity), {} };
			for (uint32_t field = 0; field < info.ReplicatedFields.size(); field++)
				if (const auto value = data->find(info.ReplicatedFields[field]); value != data->end())
					change.Fields.push_back({ field, *value });
			if (!change.Fields.empty())
				update.Components.push_back(std::move(change));
		}
		ApplyUpdate(entity, update, true, m_Reported);
	}

	void NetworkSystem::ApplyUpdate(Entity entity, const NetEntityUpdate& update, bool fromHost, std::unordered_set<std::string>& reported)
	{
		if (update.Parent && fromHost)
			ApplyParent(entity, *update.Parent);
		const bool authority = fromHost && HasAuthority(entity);
		for (const NetComponentUpdate& change : update.Components)
		{
			const ReplicationSchema::Component* component = Schema().FindComponent(change.Component);
			if (!component)
				continue;
			const ComponentInfo& info = *component->Info;
			const bool networked = change.Component == NetworkedComponentIndex();
			// This client decides the entity: only the host's say on who owns it applies.
			if (authority && !networked)
				continue;
			if (change.Removed)
			{
				// Losing Networked is a despawn, which the host sends as one.
				if (!component->IsCore && !networked && info.Has(entity))
					info.Remove(entity);
				continue;
			}
			// A client changes only what its entity already has.
			if (!fromHost && !info.Has(entity))
				continue;
			nlohmann::json patch = nlohmann::json::object();
			for (const NetFieldValue& field : change.Fields)
				patch[info.ReplicatedFields[field.Field]] = field.Value;
			std::string error;
			if (!ComponentRegistry::AddOrPatch(entity, info.Name, patch, error))
			{
				m_Statistics.RejectedValues++;
				Report(reported, "value-" + info.Name, std::string(fromHost ? "the host" : "a client") + " sent a value " + info.Name + " refuses: " + error);
				continue;
			}
			m_Statistics.UpdatesApplied++;
		}
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
			m_PendingParents[entity.GetUUID()] = parentId;
			return;
		}
		if (entity.GetParent() != parent && !m_Scene.SetParent(entity, parent, false))
			Report(m_Reported, "parent", "the host parented entity " + std::to_string(static_cast<uint64_t>(entity.GetUUID())) + " where it cannot go (a cycle, or too deep)");
	}

	void NetworkSystem::ResolvePendingParents()
	{
		for (auto it = m_PendingParents.begin(); it != m_PendingParents.end();)
		{
			Entity child = m_Scene.GetEntityByUUID(it->first);
			Entity parent = m_Scene.GetEntityByUUID(it->second);
			if (!child)
				it = m_PendingParents.erase(it);
			else if (parent)
			{
				if (!m_Scene.SetParent(child, parent, false))
					Report(m_Reported, "parent", "the host parented entity " + std::to_string(static_cast<uint64_t>(it->first)) + " where it cannot go (a cycle, or too deep)");
				it = m_PendingParents.erase(it);
			}
			else
				++it;
		}
	}

	void NetworkSystem::ApplyClientUpdate(Connection& connection, const NetState& state)
	{
		for (const NetEntityUpdate& update : state.Updates)
		{
			Entity entity = m_Scene.GetEntityByUUID(update.Entity);
			const auto* networked = entity ? entity.TryGetComponent<NetworkedComponent>() : nullptr;
			// Ownership may have changed while the update travelled: not an error, just too late.
			if (!networked || networked->Authority != NetworkAuthority::Owner || networked->Owner != connection.PeerId)
			{
				m_Statistics.RejectedRecords++;
				continue;
			}
			const bool hierarchy = update.Parent.has_value();
			const bool structure = std::ranges::any_of(update.Components, [](const NetComponentUpdate& change) { return change.Added || change.Removed || change.Component == NetworkedComponentIndex(); });
			if (hierarchy || structure)
			{
				m_Statistics.RejectedRecords++;
				Report(connection.Reported, "structure", "client " + std::to_string(connection.PeerId) + " tried to change an entity's parent, components or ownership, which only the host decides; ignored");
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
		std::unordered_set<UUID> driven;
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
			const auto& components = Schema().GetComponents();
			for (uint32_t index = 0; index < components.size(); index++)
			{
				const ComponentInfo& info = *components[index].Info;
				// Ownership is the host's to change.
				if (info.ReplicatedFields.empty() || !info.Has(entity) || (m_Role == NetworkRole::Client && index == NetworkedComponentIndex()))
					continue;
				const nlohmann::json data = info.Serialize(entity);
				ReplicatedComponent component{ index, {} };
				bool encoded = true;
				for (const std::string& field : info.ReplicatedFields)
				{
					ReplicatedFieldValue value;
					const auto it = data.find(field);
					value.Value = it != data.end() ? *it : nlohmann::json();
					NetWriter writer;
					if (!NetValue::Encode(value.Value, writer))
					{
						encoded = false;
						Report(m_Reported, "encode-" + info.Name + "." + field, "cannot send " + info.Name + "." + field + " of '" + entity.GetName() + "' (text that is not UTF-8)");
						break;
					}
					value.Bytes = writer.TakeBytes();
					component.Fields.push_back(std::move(value));
				}
				if (encoded)
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
		nlohmann::json records = SceneSerializer::SerializeEntityTree(entity);
		// Descendants with a Networked component of their own spawn separately, under this one; a prefab's tracking means
		// nothing on a client (instances created during Play keep none either).
		std::unordered_set<uint64_t> excluded;
		nlohmann::json kept = nlohmann::json::array();
		for (size_t i = 0; i < records.size(); i++)
		{
			nlohmann::json& record = records[i];
			const uint64_t id = record["ID"].get<uint64_t>();
			const uint64_t parent = record["Parent"].get<uint64_t>();
			nlohmann::json& components = record["Components"];
			if (i > 0 && (excluded.contains(parent) || components.contains("Networked")))
			{
				excluded.insert(id);
				continue;
			}
			components.erase("Prefab");
			kept.push_back(std::move(record));
		}
		NetWriter writer;
		if (!NetProtocol::WriteSpawn(writer, { replicated.Entity, replicated.Parent, std::move(kept) }))
		{
			Report(m_Reported, "spawn-encode-" + std::to_string(static_cast<uint64_t>(replicated.Entity)), "cannot spawn '" + entity.GetName() + "' on clients: its data holds text that is not UTF-8");
			return std::nullopt;
		}
		return writer.TakeBytes();
	}

	void NetworkSystem::Send(float deltaTime)
	{
		if (std::isfinite(deltaTime) && deltaTime > 0.0f)
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
			if (!std::ranges::any_of(m_Connections, [](const auto& entry) { return entry.second.PeerId != 0; }))
				return;
			const ReplicationWorld world = CaptureWorld();
			const uint32_t networkedIndex = NetworkedComponentIndex();
			for (auto& [handle, connection] : m_Connections)
			{
				if (connection.PeerId == 0)
					continue;
				const uint32_t peer = connection.PeerId;
				// The client sends what it decides; it hears only who owns it.
				const ReplicationFilter filter = [peer, networkedIndex](const ReplicatedEntity& entity, uint32_t component) {
					return !(entity.Authority == NetworkAuthority::Owner && entity.Owner == peer && component != networkedIndex);
				};
				const std::vector<uint8_t> message = connection.Sender.Build(NetMessageType::Snapshot, world, connection.LastApplied, refill(connection.Tokens), filter);
				connection.Tokens -= static_cast<double>(message.size());
				Transmit(handle, NetChannel::Unreliable, message, &connection);
			}
			return;
		}
		if (m_State != NetConnectionState::Connected || !m_HostHandle)
			return;
		const std::vector<uint8_t> message = m_ClientSender.Build(NetMessageType::ClientUpdate, CaptureWorld(), m_LastSnapshot, refill(m_ClientTokens));
		m_ClientTokens -= static_cast<double>(message.size());
		Transmit(*m_HostHandle, NetChannel::Unreliable, message, nullptr);
	}

	std::vector<NetConnectionInfo> NetworkSystem::GetConnections() const
	{
		std::vector<NetConnectionInfo> result;
		if (m_Role == NetworkRole::Host)
		{
			for (const auto& [handle, connection] : m_Connections)
				if (connection.PeerId != 0)
					result.push_back({ connection.PeerId, connection.Address, m_Transport->GetRoundTripTime(handle), connection.Sender.GetKnownEntityCount(), connection.MessagesSent, connection.MessagesReceived, connection.BytesSent, connection.BytesReceived });
			std::ranges::sort(result, {}, &NetConnectionInfo::PeerId);
		}
		else if (m_State == NetConnectionState::Connected && m_HostHandle)
			result.push_back({ 0, m_HostAddress, m_Transport->GetRoundTripTime(*m_HostHandle), m_RemoteEntities.size(), m_HostMessagesSent, m_HostMessagesReceived, m_HostBytesSent, m_HostBytesReceived });
		return result;
	}

}
