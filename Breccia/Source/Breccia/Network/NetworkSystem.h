#pragma once

#include "Breccia/Core/Base.h"
#include "Breccia/Core/UUID.h"
#include "Breccia/Network/NetProtocol.h"
#include "Breccia/Network/NetTransport.h"
#include "Breccia/Network/NetworkSettings.h"
#include "Breccia/Network/Replication.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Breccia {

	class Entity;
	class Scene;

	enum class NetworkRole : uint8_t
	{
		// Owns the shared state: admits clients, simulates, and sends them snapshots (a listen server or a dedicated one).
		Host,
		// Joins a host and shows its state.
		Client
	};

	enum class NetConnectionState : uint8_t
	{
		// A client waiting for the host to accept it.
		Connecting,
		Connected,
		// A client whose connection ended (GetDisconnectReason says why). A host is always Connected.
		Disconnected
	};

	// One connection as a host or client sees it.
	struct NetConnectionInfo
	{
		// The client's peer ID (a client's own connection to its host: 0, the host's).
		uint32_t PeerId = 0;
		std::string Address;
		float RoundTripTime = 0.0f;
		// Entities the peer has acknowledged (a host's connections only).
		size_t KnownEntities = 0;
		uint64_t MessagesSent = 0;
		uint64_t MessagesReceived = 0;
		uint64_t BytesSent = 0;
		uint64_t BytesReceived = 0;
	};

	// Totals since the session started.
	struct NetworkStatistics
	{
		uint64_t MessagesSent = 0;
		uint64_t MessagesReceived = 0;
		uint64_t BytesSent = 0;
		uint64_t BytesReceived = 0;
		// State messages older than one already applied (dropped).
		uint64_t StaleMessages = 0;
		uint64_t SpawnsApplied = 0;
		uint64_t DespawnsApplied = 0;
		uint64_t UpdatesApplied = 0;
		// Replicated values the receiving component refused (ComponentRegistry validation), and records naming entities the
		// sender may not change.
		uint64_t RejectedValues = 0;
		uint64_t RejectedRecords = 0;
	};

	// A playing scene's network session (owned by Scene, Scene::StartNetwork). The host is authoritative: it sends each
	// client the networked entities (NetworkedComponent) as snapshots at the project's tick rate, spawning them with
	// their UUIDs, updating ComponentInfo::ReplicatedFields when they change (ReplicationSender) and despawning them.
	// A client applies them through ComponentRegistry, so every value passes the same checks as a file's, and sends back
	// the replicated fields of entities whose Authority is Owner and whose Owner is its peer ID, which the host checks
	// and applies. Peers drive the dynamic bodies of entities they have no authority over kinematically
	// (PhysicsWorld::SetDrivenExternally).
	//
	// Receive runs before the frame's scripts and Send after its last system, both on the main thread; time comes only
	// from the frame steps they are given, so a fixed timestep over a SimulatedNetwork replays exactly. A client removes
	// the networked entities of its own copy of the scene when the host accepts it: the host spawns its own. Clients
	// should not destroy networked entities; one a client destroys stays gone until the host despawns it.
	class NetworkSystem
	{
	public:
		// The largest message a host sends, and a client: the host refuses (InvalidPacket) a larger one before decoding.
		static constexpr size_t MaxHostMessageBytes = size_t{ 1024 } * 1024;
		static constexpr size_t MaxClientMessageBytes = size_t{ 64 } * 1024;

		// What a client must match to join: the game and its version (the project's Name and Version).
		struct Identity
		{
			std::string Game;
			std::string GameVersion;

			static Identity FromActiveProject();
		};

		// transport: a listening transport (Host) or a connecting one (Client). Attach the system to a scene with
		// Scene::StartNetwork.
		NetworkSystem(Scene& scene, NetworkRole role, Scope<NetTransport> transport, const NetworkSettings& settings, Identity identity);
		// Tells every peer the session ended (ServerShutdown from a host, ClientQuit from a client).
		~NetworkSystem();

		NetworkSystem(const NetworkSystem&) = delete;
		NetworkSystem& operator=(const NetworkSystem&) = delete;

		// Receives and applies what arrived, then drives bodies by authority. Called by Scene::OnUpdate before scripts.
		void Receive(float deltaTime);
		// Sends this tick's messages when one is due. Called by Scene::OnUpdate after the frame's systems.
		void Send(float deltaTime);

		// Whether this peer decides the entity: everything not networked; on a host, every networked entity except those
		// a connected client owns with Owner authority; on a connected client, only those it owns with Owner authority.
		bool HasAuthority(Entity entity) const;

		NetworkRole GetRole() const { return m_Role; }
		NetConnectionState GetState() const { return m_State; }
		// The host is 0; a client's is assigned when the host accepts it (0 before).
		uint32_t GetLocalPeerId() const { return m_LocalPeerId; }
		// Why a client's connection ended (None while it lasts).
		NetDisconnectReason GetDisconnectReason() const { return m_DisconnectReason; }
		uint16_t GetListenPort() const;
		const NetworkSettings& GetSettings() const { return m_Settings; }
		// A host's admitted clients, in join order; a client's connection to its host (none before it is accepted).
		std::vector<NetConnectionInfo> GetConnections() const;
		const NetworkStatistics& GetStatistics() const { return m_Statistics; }
		// Entities a client received from the host and still has.
		size_t GetRemoteEntityCount() const { return m_RemoteEntities.size(); }

	private:
		struct Connection
		{
			NetPeerHandle Handle = 0;
			// The peer's address, kept for messages after the transport forgot it.
			std::string Address;
			// 0 until its Hello is accepted.
			uint32_t PeerId = 0;
			float ConnectedAt = 0.0f;
			ReplicationSender Sender = ReplicationSender(ReplicationSenderOptions{ .Spawns = true, .MaxMessageBytes = MaxHostMessageBytes });
			// The newest ClientUpdate applied, which snapshots acknowledge.
			uint32_t LastApplied = 0;
			// Bytes this connection may still send (its budget, refilled at MaxBytesPerSecond).
			double Tokens = 0.0;
			uint64_t MessagesSent = 0;
			uint64_t MessagesReceived = 0;
			uint64_t BytesSent = 0;
			uint64_t BytesReceived = 0;
			// Kinds of problem already logged for this client.
			std::unordered_set<std::string> Reported;
		};

		void HandleEvent(NetTransportEvent& event);
		void HandleHostMessage(Connection& connection, const std::vector<uint8_t>& data);
		void HandleClientMessage(const std::vector<uint8_t>& data);
		// Ends a host's connection with a reason (logged).
		void Kick(Connection& connection, NetDisconnectReason reason, const std::string& detail);
		// Ends a client's connection to its host.
		void LeaveHost(NetDisconnectReason reason, bool tellHost);
		void AcceptHello(Connection& connection, const NetHello& hello);
		void ApplyClientUpdate(Connection& connection, const NetState& state);
		void ApplySnapshot(const NetState& state);
		void ApplySpawn(const NetEntitySpawn& spawn);
		void ApplyUpdate(Entity entity, const NetEntityUpdate& update, bool fromHost, std::unordered_set<std::string>& reported);
		void ApplyParent(Entity entity, UUID parent);
		void ResolvePendingParents();
		// Applies a repeated spawn to the entity an earlier one created: its replicated fields, components and parent.
		void RefreshFromSpawn(Entity entity, const NetEntitySpawn& spawn);
		void RemoveLocalNetworkedEntities();
		void UpdateDrivenBodies();
		ReplicationWorld CaptureWorld() const;
		std::optional<std::vector<uint8_t>> BuildSpawn(const ReplicatedEntity& entity) const;
		bool IsActivePeer(uint32_t peerId) const;
		Connection* FindConnection(NetPeerHandle handle);
		// Logs a problem once per kind (per client on a host).
		void Report(std::unordered_set<std::string>& reported, const std::string& kind, const std::string& message) const;
		void Transmit(NetPeerHandle handle, NetChannel channel, const std::vector<uint8_t>& data, Connection* connection);

		Scene& m_Scene;
		NetworkRole m_Role;
		Scope<NetTransport> m_Transport;
		NetworkSettings m_Settings;
		Identity m_Identity;
		NetConnectionState m_State = NetConnectionState::Connecting;
		NetDisconnectReason m_DisconnectReason = NetDisconnectReason::None;
		uint32_t m_LocalPeerId = 0;
		float m_Clock = 0.0f;
		float m_TickAccumulator = 0.0f;
		float m_SinceLastTick = 0.0f;
		NetworkStatistics m_Statistics;

		// Host: connections by transport handle (admitted or waiting for their Hello), and the next peer ID.
		std::map<NetPeerHandle, Connection> m_Connections;
		uint32_t m_NextPeerId = 1;

		// Client: the host's connection, the newest snapshot applied, what goes back, and what the host spawned here.
		std::optional<NetPeerHandle> m_HostHandle;
		std::string m_HostAddress;
		bool m_HostConnected = false;
		uint32_t m_LastSnapshot = 0;
		ReplicationSender m_ClientSender;
		double m_ClientTokens = 0.0;
		uint64_t m_HostMessagesSent = 0;
		uint64_t m_HostMessagesReceived = 0;
		uint64_t m_HostBytesSent = 0;
		uint64_t m_HostBytesReceived = 0;
		std::unordered_set<UUID> m_RemoteEntities;
		// Entities waiting for a parent that has not arrived yet (child -> parent).
		std::map<UUID, UUID> m_PendingParents;
		mutable std::unordered_set<std::string> m_Reported;

		// Entities whose body this peer drives kinematically.
		std::unordered_set<UUID> m_DrivenBodies;
	};

}
