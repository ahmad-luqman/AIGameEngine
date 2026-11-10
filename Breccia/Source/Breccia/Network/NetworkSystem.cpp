#include "Breccia/Network/NetworkSystem.h"

#include "Breccia/Asset/AssetKey.h"
#include "Breccia/Core/FileSystem.h"
#include "Breccia/Core/Log.h"
#include "Breccia/Math/Math.h"
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
#include <cstddef>
#include <unordered_map>
#include <utility>

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

		uint32_t TransformComponentIndex()
		{
			static const uint32_t s_Index = [] {
				const std::optional<uint32_t> index = Schema().FindComponentIndex("Transform");
				BR_CORE_ASSERT(index, "the Transform component is registered");
				return index.value_or(UINT32_MAX);
			}();
			return s_Index;
		}

		// How a client's estimate of the host clock follows the snapshots' times: a fraction of each difference, so jitter
		// averages out; a difference beyond the limit (a stall, a host that restarted its clock) is taken at once.
		constexpr double ServerClockSmoothing = 0.1;
		constexpr double ServerClockSnapSeconds = 0.5;
		// Transforms a client keeps per interpolated entity at most.
		constexpr size_t MaxTransformSamples = 32;

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
		{
			for (UUID uuid : m_DrivenBodies)
				if (Entity entity = m_Scene.GetEntityByUUID(uuid))
					physics->SetDrivenExternally(entity, false);
			for (const auto& [uuid, character] : m_Predicted)
				if (Entity entity = m_Scene.GetEntityByUUID(uuid))
					physics->SetCharacterInputRecording(entity, false);
		}
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
		// Predicted entities are the host's: the owner only predicts them.
		const bool ownerDecides = networked->Authority == NetworkAuthority::Owner;
		if (m_Role == NetworkRole::Host)
			return !(ownerDecides && IsActivePeer(networked->Owner));
		return m_State == NetConnectionState::Connected && ownerDecides && networked->Owner == m_LocalPeerId && m_LocalPeerId != 0;
	}

	bool NetworkSystem::IsOwner(Entity entity) const
	{
		const auto* networked = entity ? entity.TryGetComponent<NetworkedComponent>() : nullptr;
		if (!networked)
			return true;
		if (m_Role == NetworkRole::Host)
			return !IsActivePeer(networked->Owner);
		return m_State == NetConnectionState::Connected && m_LocalPeerId != 0 && networked->Owner == m_LocalPeerId;
	}

	bool NetworkSystem::IsPredicted(Entity entity) const
	{
		const auto* networked = entity ? entity.TryGetComponent<NetworkedComponent>() : nullptr;
		return m_Role == NetworkRole::Client && networked && networked->Authority == NetworkAuthority::Predicted && entity.HasComponent<CharacterControllerComponent>() && IsOwner(entity);
	}

	NetworkSystem::Connection* NetworkSystem::FindPeer(uint32_t peer)
	{
		if (peer == 0)
			return nullptr;
		for (auto& [handle, connection] : m_Connections)
			if (connection.PeerId == peer)
				return &connection;
		return nullptr;
	}

	const NetworkSystem::Connection* NetworkSystem::FindPeer(uint32_t peer) const
	{
		if (peer == 0)
			return nullptr;
		for (const auto& [handle, connection] : m_Connections)
			if (connection.PeerId == peer)
				return &connection;
		return nullptr;
	}

	std::vector<uint32_t> NetworkSystem::GetPeers() const
	{
		std::vector<uint32_t> peers;
		if (m_Role == NetworkRole::Host)
		{
			for (const auto& [handle, connection] : m_Connections)
				if (connection.IsAdmitted())
					peers.push_back(connection.PeerId);
			std::ranges::sort(peers);
		}
		else if (m_State == NetConnectionState::Connected && m_HostHandle)
			peers.push_back(0);
		return peers;
	}

	float NetworkSystem::GetRoundTripTime(uint32_t peer) const
	{
		if (m_Role == NetworkRole::Host)
		{
			const Connection* connection = FindPeer(peer);
			return connection ? m_Transport->GetRoundTripTime(connection->Handle) : 0.0f;
		}
		return peer == 0 && m_HostHandle ? m_Transport->GetRoundTripTime(*m_HostHandle) : 0.0f;
	}

	double NetworkSystem::GetTime() const
	{
		return m_Role == NetworkRole::Host ? m_Clock : m_ServerClock.value_or(0.0);
	}

	double NetworkSystem::GetViewTime() const
	{
		if (m_Role == NetworkRole::Host)
			return m_Clock;
		return std::max(0.0, GetTime() - static_cast<double>(m_Settings.InterpolationDelay));
	}

	std::vector<NetworkEvent> NetworkSystem::TakeEvents()
	{
		m_ReportedEventOverflow = false;
		return std::exchange(m_Events, {});
	}

	void NetworkSystem::PushEvent(NetworkEvent event)
	{
		if (m_Events.size() >= MaxQueuedEvents)
		{
			m_Statistics.ScriptMessagesDropped++;
			if (!std::exchange(m_ReportedEventOverflow, true))
				BR_CORE_WARN("Network: more than {} events wait for the scripts; later ones are dropped", MaxQueuedEvents);
			return;
		}
		if (event.Type == NetworkEvent::Kind::Message || event.Type == NetworkEvent::Kind::Rpc)
			m_Statistics.ScriptMessagesReceived++;
		m_Events.push_back(std::move(event));
	}

	void NetworkSystem::Receive(float deltaTime)
	{
		const double rate = m_Settings.MaxMessagesPerSecond;
		if (IsUsableStep(deltaTime))
		{
			m_Clock += deltaTime;
			if (m_ServerClock)
				*m_ServerClock += deltaTime;
			const double refill = rate * deltaTime;
			m_ClientMessageCredit = std::min(rate, m_ClientMessageCredit + refill);
			for (auto& [handle, connection] : m_Connections)
				connection.MessageCredit = std::min(rate, connection.MessageCredit + refill);
		}
		std::vector<NetTransportEvent> events;
		m_Transport->Poll(events);
		// Before the messages: inputs go only to characters their sender predicts now, and a transform a script changed
		// since the last frame is not overwritten by the interpolation it stopped.
		if (m_Role == NetworkRole::Host)
			SyncRemoteCharacters();
		else
			DropEditedTracks();
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
		if (m_Role == NetworkRole::Host)
			ReleaseInboxes(IsUsableStep(deltaTime) ? deltaTime : 0.0f);
		else
		{
			SyncPredictedCharacters();
			ReleasePendingRpcs();
		}
		ResolvePendingParents();
		UpdateDrivenBodies();
		if (m_Role == NetworkRole::Client)
			Interpolate();
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
					{
						BR_CORE_INFO("Network: client {} ({}) left: {}", it->second.PeerId, it->second.Address, DescribeDisconnectReason(reason));
						PushEvent({ .Type = NetworkEvent::Kind::PeerDisconnected, .Peer = it->second.PeerId, .Reason = reason });
					}
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
		if (connection.IsAdmitted())
			PushEvent({ .Type = NetworkEvent::Kind::PeerDisconnected, .Peer = connection.PeerId, .Reason = reason });
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
		PushEvent({ .Type = NetworkEvent::Kind::Disconnected, .Reason = reason });
		// What waited for the host will not come now.
		m_Statistics.ScriptMessagesDropped += m_PendingRpcs.size();
		m_PendingRpcs.clear();
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
		std::optional<NetMessage> message = NetProtocol::Decode(data, Schema(), error);
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
		if (message->Type == NetMessageType::Script)
		{
			// Handed to scripts at MaxMessagesPerSecond: a burst waits, a flood ends the connection.
			if (connection.Received.Messages.size() >= MaxInboxMessages)
			{
				Kick(handle, NetDisconnectReason::InvalidPacket, fmt::format("it sent script messages faster than Network.MaxMessagesPerSecond ({}) allows for too long", m_Settings.MaxMessagesPerSecond));
				return;
			}
			connection.Received.Messages.push_back(std::get<NetScriptMessage>(std::move(message->Body)));
			return;
		}
		if (message->Type != NetMessageType::ClientUpdate)
		{
			Kick(handle, NetDisconnectReason::InvalidPacket, "an admitted client sends only ClientUpdate and Script messages");
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
		for (const NetInputs& inputs : state.Extras.Inputs)
			ApplyInputs(connection, inputs);
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
		connection.MessageCredit = m_Settings.MaxMessagesPerSecond;
		connection.Received.Credit = m_Settings.MaxMessagesPerSecond;
		BR_CORE_INFO("Network: client {} joined from {}", connection.PeerId, connection.Address);
		PushEvent({ .Type = NetworkEvent::Kind::PeerConnected, .Peer = connection.PeerId });
	}

	void NetworkSystem::HandleClientMessage(const std::vector<uint8_t>& data)
	{
		std::string error;
		std::optional<NetMessage> message = NetProtocol::Decode(data, Schema(), error);
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
				m_ClientMessageCredit = m_Settings.MaxMessagesPerSecond;
				BR_CORE_INFO("Network: connected to {} as client {}", m_HostAddress, m_LocalPeerId);
				RemoveLocalNetworkedEntities();
				PushEvent({ .Type = NetworkEvent::Kind::Connected, .Peer = m_LocalPeerId });
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
			case NetMessageType::Script:
				// One that overtook the Welcome (unreliable) is lost, as a lost datagram would be.
				if (m_State == NetConnectionState::Connected)
					HandleScriptMessage(0, std::get<NetScriptMessage>(std::move(message->Body)));
				return;
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
		// The host's clock as of this snapshot: absent fields mean "unchanged as of the previous one".
		const double previousSnapshot = m_ServerClock ? m_LastSnapshotTime : state.Extras.Time;
		m_LastSnapshotTime = state.Extras.Time;
		if (!m_ServerClock || std::abs(state.Extras.Time - *m_ServerClock) > ServerClockSnapSeconds)
			m_ServerClock = state.Extras.Time;
		else
			*m_ServerClock += (state.Extras.Time - *m_ServerClock) * ServerClockSmoothing;

		for (const NetEntitySpawn& spawn : state.Spawns)
			ApplySpawn(spawn);
		const uint32_t transformIndex = TransformComponentIndex();
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
			// A new parent changes what the local transform means: the entity jumps there instead of sliding.
			if (update.Parent)
				m_Interpolation.erase(update.Entity);
			const bool moves = !update.Parent && std::ranges::any_of(update.Components, [transformIndex](const NetComponentUpdate& change) { return change.Component == transformIndex; });
			const bool interpolate = moves && IsInterpolated(entity);
			// The fields this update leaves out keep their newest values, not the ones shown now.
			if (const auto track = m_Interpolation.find(update.Entity); interpolate && track != m_Interpolation.end() && !track->second.Samples.empty())
			{
				TransformComponent& transform = entity.GetComponent<TransformComponent>();
				const TransformSample& newest = track->second.Samples.back();
				transform.Translation = newest.Translation;
				transform.Rotation = newest.Rotation;
				transform.Scale = newest.Scale;
			}
			std::optional<TransformSample> before;
			if (interpolate && !m_Interpolation.contains(update.Entity))
			{
				const TransformComponent& transform = entity.GetComponent<TransformComponent>();
				before = TransformSample{ previousSnapshot, transform.Translation, transform.Rotation, transform.Scale };
			}
			ApplyUpdate(entity, update, true, m_Reported);
			if (interpolate)
				RecordTransform(entity, previousSnapshot, before);
		}
		for (UUID uuid : state.Despawns)
			ApplyDespawn(uuid);
		for (const NetPrediction& prediction : state.Extras.Predictions)
			Reconcile(prediction);
		// A script may destroy what the host spawned.
		std::erase_if(m_RemoteEntities, [this](UUID uuid) { return !m_Scene.GetEntityByUUID(uuid); });
	}

	void NetworkSystem::ApplyDespawn(UUID uuid)
	{
		if (!m_RemoteEntities.erase(uuid))
			return;
		m_PendingParents.erase(uuid);
		m_Interpolation.erase(uuid);
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
		// Set before the components other than ownership, which may hand the entity to this client.
		bool predicted = false;
		const auto apply = [&](const NetComponentUpdate& change, bool authority) {
			const ReplicationSchema::Component* component = Schema().FindComponent(change.Component);
			BR_CORE_ASSERT(component, "component indices are checked when a message is decoded");
			const ComponentInfo& info = *component->Info;
			const bool networked = change.Component == NetworkedComponentIndex();
			// This client decides the entity's values; the host still decides which components it has and who owns it.
			if (authority && !networked && change.Change == NetComponentChange::Changed)
				return;
			// A character this client predicts takes the host's position through Reconcile, never as a field.
			if (predicted && change.Component == TransformComponentIndex())
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
		predicted = fromHost && IsPredicted(entity);
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
		for (auto handle : m_Scene.GetRegistry().view<NetworkedComponent>())
		{
			Entity entity(handle, &m_Scene);
			const auto* rigidBody = entity.TryGetComponent<RigidBodyComponent>();
			const bool character = entity.HasComponent<CharacterControllerComponent>();
			if (!character && !(rigidBody && rigidBody->Type == RigidBodyType::Dynamic))
				continue;
			// A character a host moves from its client's inputs moves only by them; one a client predicts moves itself.
			const bool remoteInputs = m_RemoteCharacters.contains(entity.GetUUID());
			if ((!HasAuthority(entity) && !(character && IsPredicted(entity))) || remoteInputs)
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
		// Every frame, so the steps the physics world recorded never pile up there.
		std::vector<NetInputs> inputs;
		if (m_Role == NetworkRole::Client)
			inputs = CollectInputs();
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
			RecordPoses();
			const uint32_t networkedIndex = NetworkedComponentIndex();
			const uint32_t transformIndex = TransformComponentIndex();
			for (auto& [handle, connection] : m_Connections)
			{
				if (!connection.IsAdmitted())
					continue;
				const uint32_t peer = connection.PeerId;
				// The client decides the values of what it owns, and the position of the characters it predicts (which it
				// corrects from their states instead); it hears who owns them and which components they have.
				const ReplicationFilter filter = [peer, networkedIndex, transformIndex](const ReplicatedEntity& entity, uint32_t component) {
					if (entity.Owner != peer)
						return ReplicationScope::Everything;
					if (entity.Authority == NetworkAuthority::Owner && component != networkedIndex)
						return ReplicationScope::PresenceOnly;
					if (entity.Authority == NetworkAuthority::Predicted && component == transformIndex)
						return ReplicationScope::PresenceOnly;
					return ReplicationScope::Everything;
				};
				const NetStateExtras extras{ .Time = m_Clock, .Predictions = BuildPredictions(peer) };
				const std::vector<uint8_t> message = connection.Sender.Build(world, connection.LastApplied, refill(connection.Tokens), filter, extras);
				if (Transmit(handle, NetChannel::Unreliable, message, connection.Traffic))
					connection.Tokens -= static_cast<double>(message.size());
			}
		}
		// A client that spent its budget skips the tick: what it would send waits, and the host never receives more than
		// MaxBytesPerSecond from it (one message of debt at most).
		else if (m_State == NetConnectionState::Connected && m_HostHandle && refill(m_ClientTokens) > 0)
		{
			const NetStateExtras extras{ .Inputs = std::move(inputs) };
			const std::vector<uint8_t> message = m_ClientSender.Build(CaptureWorld(), m_LastSnapshot, static_cast<size_t>(m_ClientTokens), {}, extras);
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

	bool NetworkSystem::SendScript(uint32_t peer, const NetScriptMessage& message, bool reliable, std::string& outError)
	{
		if (std::string problem = NetProtocol::CheckScript(message); !problem.empty())
		{
			outError = std::move(problem);
			return false;
		}
		const std::vector<uint8_t> bytes = NetProtocol::EncodeScript(message);
		if (bytes.empty())
		{
			outError = fmt::format("the message is larger than {} bytes, or holds text that is not UTF-8", NetProtocol::MaxScriptMessageBytes);
			return false;
		}
		NetPeerHandle handle = 0;
		double* credit = nullptr;
		double* tokens = nullptr;
		NetTraffic* traffic = nullptr;
		if (m_Role == NetworkRole::Host)
		{
			Connection* connection = FindPeer(peer);
			if (!connection)
			{
				outError = peer == 0 ? "the host sends to clients (peer 1 and up)" : fmt::format("no client {} is connected", peer);
				return false;
			}
			handle = connection->Handle;
			credit = &connection->MessageCredit;
			tokens = &connection->Tokens;
			traffic = &connection->Traffic;
		}
		else
		{
			if (peer != 0)
			{
				outError = "a client sends only to the host (peer 0)";
				return false;
			}
			if (m_State != NetConnectionState::Connected || !m_HostHandle)
			{
				outError = "not connected to a host";
				return false;
			}
			handle = *m_HostHandle;
			credit = &m_ClientMessageCredit;
			tokens = &m_ClientTokens;
			traffic = &m_HostTraffic;
		}
		if (*credit < 1.0)
		{
			outError = fmt::format("more than Network.MaxMessagesPerSecond ({}) messages this second", m_Settings.MaxMessagesPerSecond);
			return false;
		}
		// Messages share the connection's bandwidth with snapshots; one may run the budget into debt, as a record may.
		if (*tokens <= 0.0)
		{
			outError = fmt::format("the connection's Network.MaxBytesPerSecond ({}) is spent", m_Settings.MaxBytesPerSecond);
			return false;
		}
		if (!Transmit(handle, reliable ? NetChannel::Reliable : NetChannel::Unreliable, bytes, *traffic))
		{
			outError = "the transport did not take the message";
			return false;
		}
		*credit -= 1.0;
		*tokens -= static_cast<double>(bytes.size());
		m_Statistics.ScriptMessagesSent++;
		return true;
	}

	void NetworkSystem::HandleScriptMessage(uint32_t sender, NetScriptMessage message)
	{
		const bool rpc = message.Kind == NetScriptKind::Rpc;
		NetworkEvent event{ .Type = rpc ? NetworkEvent::Kind::Rpc : NetworkEvent::Kind::Message, .Peer = sender, .Entity = message.Entity, .Name = std::move(message.Name), .Arguments = std::move(message.Arguments) };
		if (rpc && !m_Scene.GetEntityByUUID(event.Entity))
		{
			if (m_Role == NetworkRole::Host)
			{
				// The entity is gone, or never existed: the client acted on what it saw a moment ago.
				m_Statistics.ScriptMessagesDropped++;
				if (Connection* connection = FindPeer(sender))
					Report(connection->Reported, "rpc-entity", fmt::format("client {} called {} on entity {}, which the host does not have; ignored", sender, event.Name, IdText(event.Entity)));
				return;
			}
			// Its spawn may still be on its way (snapshots are unreliable, RPCs need not be).
			if (m_PendingRpcs.size() >= MaxPendingRpcs)
			{
				m_Statistics.ScriptMessagesDropped++;
				Report(m_Reported, "rpc-overflow", fmt::format("more than {} RPCs wait for entities this client does not have; the oldest is dropped", MaxPendingRpcs));
				m_PendingRpcs.pop_front();
			}
			m_PendingRpcs.push_back({ std::move(event), m_Clock });
			return;
		}
		PushEvent(std::move(event));
	}

	void NetworkSystem::ReleaseInboxes(float deltaTime)
	{
		const double rate = m_Settings.MaxMessagesPerSecond;
		for (auto& [handle, connection] : m_Connections)
		{
			if (!connection.IsAdmitted())
				continue;
			Inbox& inbox = connection.Received;
			inbox.Credit = std::min(rate, inbox.Credit + rate * deltaTime);
			while (!inbox.Messages.empty() && inbox.Credit >= 1.0)
			{
				NetScriptMessage message = std::move(inbox.Messages.front());
				inbox.Messages.pop_front();
				inbox.Credit -= 1.0;
				HandleScriptMessage(connection.PeerId, std::move(message));
			}
		}
	}

	void NetworkSystem::ReleasePendingRpcs()
	{
		for (auto it = m_PendingRpcs.begin(); it != m_PendingRpcs.end();)
		{
			if (m_Scene.GetEntityByUUID(it->Event.Entity))
			{
				PushEvent(std::move(it->Event));
				it = m_PendingRpcs.erase(it);
			}
			else if (m_Clock - it->Since >= PendingRpcSeconds)
			{
				m_Statistics.ScriptMessagesDropped++;
				Report(m_Reported, "rpc-timeout-" + IdText(it->Event.Entity), fmt::format("the host called {} on entity {}, which did not arrive within {} seconds; dropped", it->Event.Name, IdText(it->Event.Entity), PendingRpcSeconds));
				it = m_PendingRpcs.erase(it);
			}
			else
				++it;
		}
	}

	void NetworkSystem::SyncPredictedCharacters()
	{
		PhysicsWorld* physics = m_Scene.GetPhysicsWorld();
		if (!physics)
			return;
		std::set<UUID> wanted;
		for (auto handle : m_Scene.GetRegistry().view<NetworkedComponent, CharacterControllerComponent>())
		{
			Entity entity(handle, &m_Scene);
			if (IsPredicted(entity) && physics->HasCharacter(entity))
				wanted.insert(entity.GetUUID());
		}
		for (auto it = m_Predicted.begin(); it != m_Predicted.end();)
		{
			if (wanted.contains(it->first))
			{
				++it;
				continue;
			}
			if (Entity entity = m_Scene.GetEntityByUUID(it->first))
				physics->SetCharacterInputRecording(entity, false);
			it = m_Predicted.erase(it);
		}
		for (UUID uuid : wanted)
		{
			// Every time: a rebuilt character starts without recording.
			physics->SetCharacterInputRecording(m_Scene.GetEntityByUUID(uuid), true);
			if (m_Predicted.try_emplace(uuid).second)
				m_Interpolation.erase(uuid);
		}
	}

	std::vector<NetInputs> NetworkSystem::CollectInputs()
	{
		std::vector<NetInputs> result;
		PhysicsWorld* physics = m_Scene.GetPhysicsWorld();
		if (!physics)
			return result;
		for (auto& [uuid, character] : m_Predicted)
		{
			Entity entity = m_Scene.GetEntityByUUID(uuid);
			if (!entity)
				continue;
			for (CharacterInput input : physics->TakeCharacterInputs(entity))
			{
				// What the host accepts: a faster request is clamped here, and the correction shows the difference.
				if (const float speed = glm::length(input.Velocity); speed > NetProtocol::MaxInputSpeed)
					input.Velocity *= NetProtocol::MaxInputSpeed / speed;
				input.Rotation = glm::normalize(input.Rotation);
				character.Unacknowledged.emplace_back(character.NextInput++, input);
			}
			while (character.Unacknowledged.size() > MaxUnacknowledgedInputs)
				character.Unacknowledged.pop_front();
			if (character.Unacknowledged.empty() || result.size() >= NetProtocol::MaxPredictedCharacters)
				continue;
			// The newest ones: consecutive, as only the oldest are ever dropped.
			const size_t count = std::min<size_t>(character.Unacknowledged.size(), NetProtocol::MaxInputsPerCharacter);
			NetInputs& inputs = result.emplace_back();
			inputs.Entity = uuid;
			const auto first = character.Unacknowledged.end() - static_cast<std::ptrdiff_t>(count);
			inputs.First = first->first;
			for (auto it = first; it != character.Unacknowledged.end(); ++it)
				inputs.Steps.push_back(it->second);
		}
		return result;
	}

	void NetworkSystem::Reconcile(const NetPrediction& prediction)
	{
		const auto found = m_Predicted.find(prediction.Entity);
		PhysicsWorld* physics = m_Scene.GetPhysicsWorld();
		Entity entity = m_Scene.GetEntityByUUID(prediction.Entity);
		if (found == m_Predicted.end() || !physics || !entity)
			return;
		PredictedCharacter& character = found->second;
		while (!character.Unacknowledged.empty() && character.Unacknowledged.front().first <= prediction.Input)
			character.Unacknowledged.pop_front();
		const std::optional<CharacterState> before = physics->GetCharacterState(entity);
		// Replaying turns the character as each input did; the rotation a script gave it since stays.
		const glm::quat rotation = entity.GetComponent<TransformComponent>().Rotation;
		if (!physics->SetCharacterState(entity, prediction.State))
			return;
		for (const auto& [sequence, input] : character.Unacknowledged)
			physics->StepCharacter(entity, input);
		entity.GetComponent<TransformComponent>().Rotation = rotation;
		const std::optional<CharacterState> after = physics->GetCharacterState(entity);
		if (before && after && glm::length(after->Position - before->Position) > PredictionTolerance)
			m_Statistics.Corrections++;
	}

	void NetworkSystem::SyncRemoteCharacters()
	{
		PhysicsWorld* physics = m_Scene.GetPhysicsWorld();
		if (!physics)
		{
			m_RemoteCharacters.clear();
			return;
		}
		const uint64_t steps = physics->GetStepCount();
		const float credited = static_cast<float>(steps - std::min(steps, m_CreditedSteps));
		m_CreditedSteps = steps;
		const float maxCredit = std::max(1.0f, std::floor(MaxInputLeadSeconds / m_Scene.GetFixedTimestep()));
		std::set<UUID> live;
		for (auto [handle, networked, controller] : m_Scene.GetRegistry().view<NetworkedComponent, CharacterControllerComponent>().each())
		{
			Entity entity(handle, &m_Scene);
			if (networked.Authority != NetworkAuthority::Predicted || !IsActivePeer(networked.Owner) || !physics->HasCharacter(entity) || m_Scene.IsEntityPendingDestruction(entity))
				continue;
			live.insert(entity.GetUUID());
			auto [it, inserted] = m_RemoteCharacters.try_emplace(entity.GetUUID(), RemoteCharacter{ networked.Owner, 0, maxCredit });
			// Another client's now: its inputs start afresh.
			if (!inserted && it->second.Peer != networked.Owner)
				it->second = RemoteCharacter{ networked.Owner, 0, maxCredit };
			else if (!inserted)
				it->second.Credit = std::min(maxCredit, it->second.Credit + credited);
		}
		std::erase_if(m_RemoteCharacters, [&live](const auto& entry) { return !live.contains(entry.first); });
	}

	void NetworkSystem::ApplyInputs(Connection& connection, const NetInputs& inputs)
	{
		const auto found = m_RemoteCharacters.find(inputs.Entity);
		// Ownership may have changed while the inputs travelled.
		if (found == m_RemoteCharacters.end() || found->second.Peer != connection.PeerId)
		{
			m_Statistics.RejectedRecords++;
			return;
		}
		PhysicsWorld* physics = m_Scene.GetPhysicsWorld();
		Entity entity = m_Scene.GetEntityByUUID(inputs.Entity);
		if (!physics || !entity)
			return;
		RemoteCharacter& character = found->second;
		for (size_t i = 0; i < inputs.Steps.size(); i++)
		{
			const uint32_t sequence = inputs.First + static_cast<uint32_t>(i);
			if (sequence <= character.LastInput)
				continue;
			// Faster than the host's own steps: the rest waits for the next message, which repeats them.
			if (character.Credit < 1.0f)
				break;
			if (!physics->StepCharacter(entity, inputs.Steps[i]))
				break;
			character.LastInput = sequence;
			character.Credit -= 1.0f;
			m_Statistics.InputsApplied++;
		}
	}

	std::vector<NetPrediction> NetworkSystem::BuildPredictions(uint32_t peer)
	{
		std::vector<NetPrediction> predictions;
		PhysicsWorld* physics = m_Scene.GetPhysicsWorld();
		if (!physics)
			return predictions;
		for (const auto& [uuid, character] : m_RemoteCharacters)
		{
			if (character.Peer != peer)
				continue;
			if (predictions.size() >= NetProtocol::MaxPredictedCharacters)
			{
				Report(m_Reported, "predicted-" + std::to_string(peer), fmt::format("client {} predicts more than {} characters; the others are not corrected", peer, NetProtocol::MaxPredictedCharacters));
				break;
			}
			if (const std::optional<CharacterState> state = physics->GetCharacterState(m_Scene.GetEntityByUUID(uuid)))
				predictions.push_back({ uuid, character.LastInput, *state });
		}
		return predictions;
	}

	bool NetworkSystem::IsInterpolated(Entity entity) const
	{
		return m_Role == NetworkRole::Client && m_Settings.InterpolationDelay > 0.0f && m_RemoteEntities.contains(entity.GetUUID()) && !HasAuthority(entity) && !IsPredicted(entity);
	}

	void NetworkSystem::RecordTransform(Entity entity, double previousSnapshot, const std::optional<TransformSample>& before)
	{
		std::deque<TransformSample>& track = m_Interpolation[entity.GetUUID()].Samples;
		if (track.empty() && before)
			track.push_back(*before);
		// Unchanged until the previous snapshot: it starts moving from there, not from its last change.
		else if (!track.empty() && track.back().Time < previousSnapshot)
		{
			TransformSample held = track.back();
			held.Time = previousSnapshot;
			track.push_back(held);
		}
		const TransformComponent& transform = entity.GetComponent<TransformComponent>();
		const TransformSample now{ m_LastSnapshotTime, transform.Translation, transform.Rotation, transform.Scale };
		if (!track.empty() && track.back().Time >= now.Time)
			track.back() = now;
		else
			track.push_back(now);
		while (track.size() > MaxTransformSamples)
			track.pop_front();
	}

	void NetworkSystem::Interpolate()
	{
		const double viewTime = GetViewTime();
		for (auto it = m_Interpolation.begin(); it != m_Interpolation.end();)
		{
			Entity entity = m_Scene.GetEntityByUUID(it->first);
			std::deque<TransformSample>& track = it->second.Samples;
			if (!entity || track.empty() || !IsInterpolated(entity))
			{
				it = m_Interpolation.erase(it);
				continue;
			}
			while (track.size() >= 2 && track[1].Time <= viewTime)
				track.pop_front();
			TransformComponent& transform = entity.GetComponent<TransformComponent>();
			const TransformSample& from = track.front();
			TransformSample shown = from;
			if (track.size() >= 2 && viewTime > from.Time)
			{
				const TransformSample& to = track[1];
				const float weight = static_cast<float>(std::clamp((viewTime - from.Time) / (to.Time - from.Time), 0.0, 1.0));
				shown.Translation = glm::mix(from.Translation, to.Translation, weight);
				shown.Rotation = Math::Slerp(from.Rotation, to.Rotation, weight);
				shown.Scale = glm::mix(from.Scale, to.Scale, weight);
			}
			transform.Translation = shown.Translation;
			transform.Rotation = shown.Rotation;
			transform.Scale = shown.Scale;
			// Arrived and at rest: nothing to follow until the next change.
			if (track.size() == 1 && from.Time <= viewTime)
			{
				it = m_Interpolation.erase(it);
				continue;
			}
			it->second.Written = shown;
			++it;
		}
	}

	void NetworkSystem::DropEditedTracks()
	{
		std::erase_if(m_Interpolation, [this](const auto& entry) {
			const InterpolationTrack& track = entry.second;
			if (!track.Written)
				return false;
			const Entity entity = m_Scene.GetEntityByUUID(entry.first);
			if (!entity)
				return true;
			const TransformComponent& transform = entity.GetComponent<TransformComponent>();
			return transform.Translation != track.Written->Translation || transform.Rotation != track.Written->Rotation || transform.Scale != track.Written->Scale;
		});
	}

	void NetworkSystem::RecordPoses()
	{
		PhysicsWorld* physics = m_Scene.GetPhysicsWorld();
		if (!physics)
		{
			m_PoseHistory.clear();
			return;
		}
		std::set<UUID> live;
		for (auto handle : m_Scene.GetRegistry().view<NetworkedComponent>())
		{
			Entity entity(handle, &m_Scene);
			const auto* rigidBody = entity.TryGetComponent<RigidBodyComponent>();
			// Static bodies never move: the present is their past.
			const bool moves = entity.HasComponent<CharacterControllerComponent>() || (rigidBody && rigidBody->Type != RigidBodyType::Static);
			if (!moves || m_Scene.IsEntityPendingDestruction(entity))
				continue;
			const std::optional<PhysicsBodyPose> pose = physics->GetBodyPose(entity);
			if (!pose)
				continue;
			live.insert(entity.GetUUID());
			std::deque<PoseSample>& track = m_PoseHistory[entity.GetUUID()];
			track.push_back({ m_Clock, *pose });
			// Keep one sample at or before the oldest time a rewind may ask for.
			while (track.size() >= 2 && track[1].Time <= m_Clock - MaxRewindSeconds)
				track.pop_front();
		}
		std::erase_if(m_PoseHistory, [&live](const auto& entry) { return !live.contains(entry.first); });
	}

	std::optional<RaycastHit> NetworkSystem::RaycastAt(double time, const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter) const
	{
		const PhysicsWorld* physics = m_Scene.GetPhysicsWorld();
		if (!physics)
			return std::nullopt;
		if (m_Role != NetworkRole::Host || !std::isfinite(time) || time >= m_Clock)
			return physics->Raycast(origin, direction, maxDistance, filter);
		const double rewind = std::max(time, m_Clock - MaxRewindSeconds);
		std::map<UUID, std::optional<PhysicsBodyPose>> poses;
		for (const auto& [uuid, track] : m_PoseHistory)
		{
			// Recorded after that time: it was not there yet.
			if (track.front().Time > rewind)
			{
				poses.emplace(uuid, std::nullopt);
				continue;
			}
			// Since the last tick it is where it is.
			if (track.back().Time <= rewind)
				continue;
			const auto next = std::ranges::upper_bound(track, rewind, {}, &PoseSample::Time);
			const PoseSample& from = *(next - 1);
			const PoseSample& to = *next;
			const float weight = static_cast<float>((rewind - from.Time) / (to.Time - from.Time));
			poses.emplace(uuid, PhysicsBodyPose{ glm::mix(from.Pose.Position, to.Pose.Position, weight), Math::Slerp(from.Pose.Rotation, to.Pose.Rotation, weight) });
		}
		return physics->RaycastWithPoses(origin, direction, maxDistance, poses, filter);
	}

}
