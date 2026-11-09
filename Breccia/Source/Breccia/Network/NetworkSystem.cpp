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
#include <unordered_map>

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

		// Problems one peer gets logged, at most (the keys name entities a peer chooses, so they must not grow without end).
		constexpr size_t MaxReportedProblems = 256;

		// Logs a problem once per key (a kind, usually with the entity it concerns), up to MaxReportedProblems keys.
		void Report(std::unordered_set<std::string>& reported, const std::string& key, const std::string& message)
		{
			if (reported.size() > MaxReportedProblems || reported.contains(key))
				return;
			if (reported.size() == MaxReportedProblems)
			{
				// An empty key marks the set full: nothing else is inserted or logged. (insert({}) would insert nothing: it
				// takes the empty initializer list.)
				reported.insert(std::string());
				BR_CORE_WARN("Network: {} problems were logged for this peer; further ones are not", MaxReportedProblems);
				return;
			}
			reported.insert(key);
			BR_CORE_WARN("Network: {} (not logged again)", message);
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
			// Built-in meshes ("builtin://Cube") name no file.
			if (reference.empty() || reference.starts_with("asset-uuid:") || reference.starts_with("builtin://"))
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
		m_TickRate = m_Settings.TickRate;
		m_State = role == NetworkRole::Host ? NetConnectionState::Connected : NetConnectionState::Connecting;
	}

	NetworkSystem::~NetworkSystem()
	{
		if (const uint32_t admitted = GetAdmittedCount(); m_Role == NetworkRole::Host && admitted > 0)
			BR_CORE_INFO("Network: host stopped; {} client(s) disconnected", admitted);
		// Every connection, including a client's attempt the transport has not completed yet.
		m_Transport->DisconnectAll(static_cast<uint32_t>(m_Role == NetworkRole::Host ? NetDisconnectReason::ServerShutdown : NetDisconnectReason::ClientQuit));
		m_Transport->Flush();
		// The physics world may outlive this session (Scene::StopNetwork while playing): its bodies simulate again.
		if (PhysicsWorld* physics = m_Scene.GetPhysicsWorld())
			for (UUID uuid : m_DrivenBodies)
				if (Entity entity = m_Scene.GetEntityByUUID(uuid))
					physics->SetDrivenExternally(entity, false);
	}

	double NetworkSystem::FullBudget() const
	{
		return static_cast<double>(m_Settings.MaxBytesPerSecond) * MaxBudgetSeconds;
	}

	uint16_t NetworkSystem::GetListenPort() const
	{
		return m_Transport->GetListenPort();
	}

	bool NetworkSystem::Transmit(NetPeerHandle handle, NetChannel channel, const std::vector<uint8_t>& data, NetTraffic& traffic)
	{
		// What the transport did not take is not counted as sent.
		if (!m_Transport->Send(handle, channel, data))
			return false;
		m_Statistics.Traffic.Sent(data.size());
		traffic.Sent(data.size());
		return true;
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
					m_Connections.emplace(event.Peer, Connection{ .Handle = event.Peer, .Address = m_Transport->DescribePeer(event.Peer), .ConnectedAt = m_Clock });
				}
				else if (m_State == NetConnectionState::Connecting && !m_HostHandle)
				{
					m_HostHandle = event.Peer;
					m_HostAddress = m_Transport->DescribePeer(event.Peer);
					m_HelloSentAt = m_Clock;
					// Without the Hello the handshake cannot finish: give up now rather than time out.
					if (!Transmit(event.Peer, NetChannel::Reliable, NetProtocol::EncodeHello({ NetProtocol::Version, Schema().GetHash(), m_Identity.Game, m_Identity.GameVersion }), m_HostTraffic))
					{
						BR_CORE_ERROR("Network: the Hello to {} could not be sent", m_HostAddress);
						LeaveHost(NetDisconnectReason::ConnectionFailed, true);
					}
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
				// Only from peers still known (one kicked earlier in the batch may still have messages queued).
				if (m_Role == NetworkRole::Host)
				{
					if (Connection* connection = FindConnection(event.Peer))
					{
						m_Statistics.Traffic.Received(event.Data.size());
						connection->Traffic.Received(event.Data.size());
						HandleHostMessage(*connection, event.Data);
					}
				}
				else if (m_HostHandle && event.Peer == *m_HostHandle)
				{
					m_Statistics.Traffic.Received(event.Data.size());
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
		// Without the Welcome the client never joins: refuse it rather than count it.
		if (!Transmit(handle, NetChannel::Reliable, NetProtocol::EncodeWelcome({ m_NextPeerId, m_Settings.TickRate }), connection.Traffic))
		{
			Kick(handle, NetDisconnectReason::None, "its Welcome could not be sent");
			return;
		}
		connection.PeerId = m_NextPeerId++;
		// A full second of budget to start, so the first snapshots can carry the initial spawns.
		connection.Tokens = FullBudget();
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
				m_TickRate = std::clamp(welcome.TickRate, NetworkSettings::MinTickRate, NetworkSettings::MaxTickRate);
				if (m_TickRate != welcome.TickRate)
					BR_CORE_WARN("Network: the host's tick rate {} is outside [{}, {}]; sending at {}", welcome.TickRate, NetworkSettings::MinTickRate, NetworkSettings::MaxTickRate, m_TickRate);
				m_State = NetConnectionState::Connected;
				m_ClientTokens = FullBudget();
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
		// Descendants the host replicates on their own outlive it when it only stopped being networked (when it was
		// destroyed, their own despawns follow): they move to the root and wait for that parent, as a child whose parent
		// never arrived does.
		std::vector<Entity> stack = entity.GetChildren();
		while (!stack.empty())
		{
			Entity descendant = stack.back();
			stack.pop_back();
			if (m_RemoteEntities.contains(descendant.GetUUID()))
			{
				const UUID parent = descendant.GetParent().GetUUID();
				m_Scene.SetParent(descendant, Entity{}, false);
				// One already waiting for another parent (whose spawn the budget delayed) keeps waiting for that one.
				if (!m_PendingParents.contains(descendant.GetUUID()))
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
			Report(m_Reported, "spawn-root-" + id, "the host sent a spawn of entity " + id + " whose first record is another entity; it is not shown");
			return;
		}
		// Spawned before (a repeat), or one this client has under that ID already: an authored entity the host made
		// networked, or a member of a tree spawned earlier that is now replicated on its own.
		if (Entity existing = m_Scene.GetEntityByUUID(spawn.Entity))
		{
			if (m_RemoteEntities.insert(spawn.Entity).second)
			{
				m_Statistics.EntitiesAdopted++;
				BR_CORE_INFO("Network: the host spawned entity {} ('{}'), which this client has: it takes the host's replicated fields, parent and replicated components, and keeps the rest of its own", id, existing.GetName());
			}
			RefreshFromSpawn(existing, spawn);
			return;
		}
		if (const std::string unsafe = FindUnsafeAssetReference(spawn.Entities); !unsafe.empty())
		{
			m_Statistics.RejectedRecords++;
			Report(m_Reported, "spawn-asset-" + id, "the host spawned entity " + id + ", whose " + unsafe + " names a file outside the project; it is not shown");
			return;
		}
		// A tree holding entities this client has (an authored one the host moved under a new networked parent) keeps them:
		// the root comes alone, then the records the client lacks are created and the ones it has are moved into place.
		const bool holdsExisting = std::ranges::any_of(spawn.Entities, [this](const nlohmann::json& record) {
			const auto recordId = record.is_object() ? record.find("ID") : record.end();
			return recordId != record.end() && recordId->is_number_unsigned() && m_Scene.GetEntityByUUID(UUID(recordId->get<uint64_t>()));
		});
		const nlohmann::json rootOnly = holdsExisting ? nlohmann::json::array({ root }) : nlohmann::json();
		Entity parent = spawn.Parent.IsValid() ? m_Scene.GetEntityByUUID(spawn.Parent) : Entity{};
		std::string error;
		Entity created = SceneSerializer::CreateEntityTreeWithIds(m_Scene, holdsExisting ? rootOnly : spawn.Entities, parent, error);
		if (!created)
		{
			m_Statistics.RejectedRecords++;
			Report(m_Reported, "spawn-invalid-" + id, "the host's spawn of entity " + id + " could not be created: " + error + "; it is not shown");
			return;
		}
		if ((spawn.Parent.IsValid() && !parent) || (parent && created.GetParent() != parent))
			WaitForParent(spawn.Entity, spawn.Parent);
		m_RemoteEntities.insert(spawn.Entity);
		m_Statistics.SpawnsApplied++;
		if (holdsExisting)
			ReconcileDescendants(spawn);
	}

	void NetworkSystem::RefreshFromSpawn(Entity entity, const NetEntitySpawn& spawn)
	{
		// The repeated spawn is the host's state when it was sent, which the host will treat as acknowledged.
		NetEntityUpdate update;
		update.Entity = spawn.Entity;
		update.Parent = spawn.Parent;
		const nlohmann::json& root = spawn.Entities.front();
		const auto components = root.find("Components");
		if (components == root.end() || !components->is_object())
		{
			ApplyUpdate(entity, update, true, m_Reported);
			return;
		}
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
				if (!info.IsCore && info.Has(entity))
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
		ReconcileDescendants(spawn);
	}

	void NetworkSystem::ReconcileDescendants(const NetEntitySpawn& spawn)
	{
		// One subtree of records this client lacks, hanging from an entity it has.
		struct Subtree
		{
			UUID Parent = 0;
			nlohmann::json Records = nlohmann::json::array();
		};
		const auto readId = [](const nlohmann::json& record, const char* key) {
			const auto value = record.is_object() ? record.find(key) : record.end();
			return value != record.end() && value->is_number_unsigned() ? UUID(value->get<uint64_t>()) : UUID(0);
		};
		// The host writes parents before their children (SceneSerializer::SerializeEntityTree), so a missing record hangs
		// either from an entity this client has, starting a subtree, or from a missing record met before it.
		std::vector<Subtree> subtrees;
		std::unordered_map<UUID, size_t> subtreeOf;
		// Records this client has, and the parent the spawn gives each: they move there once the missing ones exist.
		std::vector<std::pair<UUID, UUID>> moves;
		for (size_t i = 1; i < spawn.Entities.size(); i++)
		{
			const nlohmann::json& record = spawn.Entities[i];
			const UUID id = readId(record, "ID");
			const UUID parent = readId(record, "Parent");
			if (id.IsValid() && m_Scene.GetEntityByUUID(id))
			{
				// One the host replicates on its own follows its own records.
				if (parent.IsValid() && !m_RemoteEntities.contains(id))
					moves.emplace_back(id, parent);
				continue;
			}
			if (const auto found = subtreeOf.find(parent); id.IsValid() && found != subtreeOf.end())
			{
				subtrees[found->second].Records.push_back(record);
				subtreeOf.emplace(id, found->second);
			}
			else if (id.IsValid() && parent.IsValid() && m_Scene.GetEntityByUUID(parent))
			{
				nlohmann::json root = record;
				root["Parent"] = 0;
				subtreeOf.emplace(id, subtrees.size());
				subtrees.push_back({ parent, nlohmann::json::array({ std::move(root) }) });
			}
			else
			{
				m_Statistics.RejectedRecords++;
				Report(m_Reported, "spawn-record-" + IdText(spawn.Entity), "the host's spawn of entity " + IdText(spawn.Entity) + " carries a record whose parent is not in it, or without an ID; it is not shown");
			}
		}
		for (const Subtree& subtree : subtrees)
		{
			const std::string id = IdText(readId(subtree.Records.front(), "ID"));
			if (const std::string unsafe = FindUnsafeAssetReference(subtree.Records); !unsafe.empty())
			{
				m_Statistics.RejectedRecords++;
				Report(m_Reported, "spawn-asset-" + id, fmt::format("the host's spawn of entity {} carries a subtree at entity {} whose {} names a file outside the project; it is not shown", IdText(spawn.Entity), id, unsafe));
				continue;
			}
			std::string error;
			if (!SceneSerializer::CreateEntityTreeWithIds(m_Scene, subtree.Records, m_Scene.GetEntityByUUID(subtree.Parent), error))
			{
				m_Statistics.RejectedRecords++;
				Report(m_Reported, "spawn-invalid-" + id, fmt::format("the host's spawn of entity {} carries a subtree at entity {} that could not be created: {}; it is not shown", IdText(spawn.Entity), id, error));
			}
		}
		for (const auto& [childId, parentId] : moves)
		{
			Entity child = m_Scene.GetEntityByUUID(childId);
			Entity parent = m_Scene.GetEntityByUUID(parentId);
			// A parent whose subtree was refused is missing: the entity stays where it is.
			if (child && parent && !TryParent(child, parent))
				WaitForParent(childId, parentId);
		}
	}

	void NetworkSystem::ApplyUpdate(Entity entity, const NetEntityUpdate& update, bool fromHost, std::unordered_set<std::string>& reported)
	{
		const std::string id = IdText(entity.GetUUID());
		if (update.Parent && fromHost)
			ApplyParent(entity, *update.Parent);
		const auto apply = [&](const NetComponentUpdate& change, bool authority) {
			const ReplicationSchema::Component* component = Schema().FindComponent(change.Component);
			BR_CORE_ASSERT(component, "component indices are checked when a message is decoded");
			const ComponentInfo& info = *component->Info;
			const bool networked = change.Component == NetworkedComponentIndex();
			// This client decides the entity's values; the host still decides which components it has and who owns it.
			if (authority && !networked && change.Change == NetComponentChange::Changed)
				return;
			if (change.Change == NetComponentChange::Removed)
			{
				// Losing Networked is a despawn, which the host sends as one.
				if (!info.IsCore && !networked && info.Has(entity))
					info.Remove(entity);
				return;
			}
			// A client changes only what its entity already has.
			if (!fromHost && !info.Has(entity))
			{
				m_Statistics.RejectedRecords++;
				Report(reported, "missing-" + info.Name + "-" + id, "a client changed " + info.Name + " of entity " + id + ", which the host's entity does not have; ignored");
				return;
			}
			// A component the host adds to an entity this client decides starts with the host's values.
			if (authority && !networked && info.Has(entity))
				return;
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
				return;
			}
			m_Statistics.UpdatesApplied++;
		};
		// Who owns the entity first (an update names each component once): the rest depends on it, and a spawn adopting an
		// entity adds its Networked. Only a networked entity is ever this client's to decide.
		const auto found = std::ranges::find(update.Components, NetworkedComponentIndex(), &NetComponentUpdate::Component);
		const NetComponentUpdate* ownership = found != update.Components.end() ? &*found : nullptr;
		if (ownership)
			apply(*ownership, false);
		const bool authority = fromHost && entity.HasComponent<NetworkedComponent>() && HasAuthority(entity);
		for (const NetComponentUpdate& change : update.Components)
			if (&change != ownership)
				apply(change, authority);
	}

	bool NetworkSystem::TryParent(Entity child, Entity parent)
	{
		// SetParent to the same parent is not a no-op: it moves the child to the end of its siblings.
		return child.GetParent() == parent || m_Scene.SetParent(child, parent, false);
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
		// Not there yet, or not possible yet: within one snapshot a child may move under its former child before that one's
		// own move away applies.
		Entity parent = m_Scene.GetEntityByUUID(parentId);
		if (!parent || !TryParent(entity, parent))
			WaitForParent(entity.GetUUID(), parentId);
	}

	void NetworkSystem::ResolvePendingParents()
	{
		for (auto it = m_PendingParents.begin(); it != m_PendingParents.end();)
		{
			Entity child = m_Scene.GetEntityByUUID(it->first);
			Entity parent = m_Scene.GetEntityByUUID(it->second.Parent);
			if (!child || (parent && TryParent(child, parent)))
			{
				it = m_PendingParents.erase(it);
				continue;
			}
			if (!it->second.Reported && m_Clock - it->second.Since >= PendingParentWarningSeconds)
			{
				// The parent may be an entity the host made at runtime without a Networked component, which no client has.
				if (!parent)
					BR_CORE_WARN("Network: entity {} still waits for its parent {}, which this client does not have (one the host created or stopped networking, or whose spawn this client refused); it stays at the root", IdText(it->first), IdText(it->second.Parent));
				else
					BR_CORE_WARN("Network: entity {} cannot go under entity {} (a cycle, or too deep); it stays where it is and goes there once it can", IdText(it->first), IdText(it->second.Parent));
				it->second.Reported = true;
			}
			++it;
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
			// Ownership may have changed while the update travelled: too late, not worth a log line. An entity that does not
			// exist or is not networked is a broken or cheating client: one key for all of those, whose IDs it chooses.
			if (!networked)
			{
				m_Statistics.RejectedRecords++;
				Report(connection.Reported, "not-networked", fmt::format("{} sent changes to entity {}, which is not a networked entity of the host's; ignored", client, id));
				continue;
			}
			if (networked->Authority != NetworkAuthority::Owner || networked->Owner != connection.PeerId)
			{
				m_Statistics.RejectedRecords++;
				continue;
			}
			// Parents, components and ownership are the host's: the decoder already refused a client's attempt
			// (NetProtocol::CheckUpdate for a ClientUpdate).
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
			Report(m_Reported, "spawn-size-" + id, "cannot spawn '" + entity.GetName() + "' (entity " + id + ") on clients: " + problem + " (give parts of a large tree Networked components of their own)");
			return std::nullopt;
		}
		NetWriter writer;
		if (!NetProtocol::WriteSpawn(writer, spawn))
		{
			Report(m_Reported, "spawn-text-" + id, "cannot spawn '" + entity.GetName() + "' (entity " + id + ") on clients: its data holds text that is not UTF-8");
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
		const float interval = 1.0f / m_TickRate;
		if (m_TickAccumulator < interval)
			return;
		// One tick per frame at most; a long frame does not queue a burst of them.
		m_TickAccumulator = std::fmod(m_TickAccumulator, interval);
		const double elapsed = m_SinceLastTick;
		m_SinceLastTick = 0.0f;
		const double rate = m_Settings.MaxBytesPerSecond;
		const auto refill = [&](double& tokens) {
			tokens = std::min(tokens + rate * elapsed, FullBudget());
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
				if (Transmit(handle, NetChannel::Unreliable, message, connection.Traffic))
					connection.Tokens -= static_cast<double>(message.size());
			}
		}
		else if (m_State == NetConnectionState::Connected && m_HostHandle)
		{
			const std::vector<uint8_t> message = m_ClientSender.Build(CaptureWorld(), m_LastSnapshot, refill(m_ClientTokens));
			if (Transmit(*m_HostHandle, NetChannel::Unreliable, message, m_HostTraffic))
				m_ClientTokens -= static_cast<double>(message.size());
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
