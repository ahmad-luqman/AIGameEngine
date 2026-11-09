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
#include <set>
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

	// Messages and bytes the transport accepted to send, and received.
	struct NetTraffic
	{
		uint64_t MessagesSent = 0;
		uint64_t MessagesReceived = 0;
		uint64_t BytesSent = 0;
		uint64_t BytesReceived = 0;

		void Sent(size_t bytes)
		{
			MessagesSent++;
			BytesSent += bytes;
		}
		void Received(size_t bytes)
		{
			MessagesReceived++;
			BytesReceived += bytes;
		}
	};

	// One connection as a host or client sees it.
	struct NetConnectionInfo
	{
		// The client's peer ID (a client's own connection to its host: 0, the host's).
		uint32_t PeerId = 0;
		std::string Address;
		float RoundTripTime = 0.0f;
		// A host's connection: the entities whose spawn the client acknowledged and that it has not been told to despawn. A
		// client's: the entities it received from the host and still has.
		size_t KnownEntities = 0;
		NetTraffic Traffic;
	};

	// Totals since the session started.
	struct NetworkStatistics
	{
		NetTraffic Traffic;
		// State messages not newer than the last one applied (dropped).
		uint64_t StaleMessages = 0;
		uint64_t SpawnsApplied = 0;
		uint64_t DespawnsApplied = 0;
		uint64_t UpdatesApplied = 0;
		// Spawns a client took as an entity it already had (an authored one the host made networked).
		uint64_t EntitiesAdopted = 0;
		// Replicated values the receiving component refused (ComponentRegistry validation).
		uint64_t RejectedValues = 0;
		// Records not applied: spawns refused (invalid records, files outside the project), updates naming an entity the
		// receiver does not have or the sender may not change, and structure changes from clients.
		uint64_t RejectedRecords = 0;
	};

	// A playing scene's network session (owned by Scene, Scene::StartNetwork). The host is authoritative: it sends each
	// client the networked entities (NetworkedComponent) as snapshots at the project's tick rate, spawning them with
	// their UUIDs, updating ComponentInfo::ReplicatedFields when they change (ReplicationSender) and despawning them.
	// A client applies them through ComponentRegistry, so every value passes the same checks as a file's, and sends back
	// the replicated fields of entities whose Authority is Owner and whose Owner is its peer ID, which the host checks
	// and applies. Peers drive the dynamic bodies of entities they have no authority over kinematically
	// (PhysicsWorld::SetDrivenExternally; this system is its only caller).
	//
	// Receive runs before the frame's scripts and Send after its last system, both on the main thread; time comes only
	// from the frame steps they are given, so a fixed timestep over a SimulatedNetwork replays exactly. A client removes
	// the networked entities of its own copy of the scene when the host accepts it (the host spawns its own), and takes a
	// spawn naming an entity it already has (an authored one the host made networked) as that entity (RefreshFromSpawn).
	// A client should not destroy networked entities: one it destroys stays gone until the host despawns and spawns it
	// again (an acknowledged spawn is never resent), as does one whose spawn it refused (logged).
	class NetworkSystem
	{
	public:
		// The largest message a host sends, and a client: the host refuses (InvalidPacket) a larger one before decoding.
		static constexpr size_t MaxHostMessageBytes = size_t{ 1024 } * 1024;
		static constexpr size_t MaxClientMessageBytes = size_t{ 64 } * 1024;
		// Connections a host's transport should accept beyond MaxClients (the runtime adds them when it listens), so that a
		// client past the limit is told the host is full (ServerFull) instead of never hearing back.
		static constexpr uint32_t SpareTransportSlots = 4;
		// A child waiting this long for a parent that has not arrived is reported.
		static constexpr float PendingParentWarningSeconds = 1.0f;

		// What a client must match to join: the game and its version (the project's Name and Version).
		struct Identity
		{
			std::string Game;
			std::string GameVersion;

			static Identity FromActiveProject();
		};

		// transport: a listening transport (Host) or a connecting one (Client). settings must pass
		// NetworkSettings::Validate. Attach the system to a scene with Scene::StartNetwork.
		NetworkSystem(Scene& scene, NetworkRole role, Scope<NetTransport> transport, const NetworkSettings& settings, Identity identity);
		// Tells every peer the session ended (ServerShutdown from a host, ClientQuit from a client, even one still
		// connecting).
		~NetworkSystem();

		NetworkSystem(const NetworkSystem&) = delete;
		NetworkSystem& operator=(const NetworkSystem&) = delete;

		// Receives and applies what arrived, ends handshakes that took too long, parents children whose parent arrived, then
		// drives bodies by authority. Called by Scene::OnUpdate before scripts.
		void Receive(float deltaTime);
		// Sends this tick's messages when one is due (at most one tick per frame). Called by Scene::OnUpdate after the
		// frame's systems.
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
			ReplicationSender Sender = ReplicationSender(ReplicationSenderOptions{ .Direction = ReplicationDirection::HostToClient, .MaxMessageBytes = MaxHostMessageBytes });
			// The newest ClientUpdate applied, which snapshots acknowledge.
			uint32_t LastApplied = 0;
			// Bytes this connection may still send (its budget, refilled at MaxBytesPerSecond).
			double Tokens = 0.0;
			NetTraffic Traffic;
			// Problems already logged for this client.
			std::unordered_set<std::string> Reported;

			bool IsAdmitted() const { return PeerId != 0; }
		};

		// An entity waiting for its parent to arrive.
		struct PendingParent
		{
			UUID Parent = 0;
			float Since = 0.0f;
			bool Reported = false;
		};

		void HandleEvent(const NetTransportEvent& event);
		void HandleHostMessage(Connection& connection, const std::vector<uint8_t>& data);
		void HandleClientMessage(const std::vector<uint8_t>& data);
		// Ends a host's connection with a reason (logged).
		void Kick(NetPeerHandle handle, NetDisconnectReason reason, const std::string& detail);
		// Ends a client's connection to its host.
		void LeaveHost(NetDisconnectReason reason, bool tellHost);
		void AcceptHello(Connection& connection, const NetHello& hello);
		void ApplyClientUpdate(Connection& connection, const NetState& state);
		void ApplySnapshot(const NetState& state);
		void ApplySpawn(const NetEntitySpawn& spawn);
		void ApplyDespawn(UUID uuid);
		void ApplyUpdate(Entity entity, const NetEntityUpdate& update, bool fromHost, std::unordered_set<std::string>& reported);
		void ApplyParent(Entity entity, UUID parent);
		// Puts child under parent (reported when it cannot go there).
		void TryParent(Entity child, Entity parent);
		void WaitForParent(UUID child, UUID parent);
		void ResolvePendingParents();
		// Applies a repeated spawn to the entity an earlier one created (or an existing one it names): its root's replicated
		// fields, which components with replicated fields it has, its parent, and the descendant records this client lacks
		// (CreateMissingDescendants). The rest of the root record (other fields and components) is not applied, and
		// descendants the client has keep their own values.
		void RefreshFromSpawn(Entity entity, const NetEntitySpawn& spawn);
		// Creates the spawn's descendant records whose IDs this client does not have, under the entities they name.
		void CreateMissingDescendants(const NetEntitySpawn& spawn);
		void RemoveLocalNetworkedEntities();
		void UpdateDrivenBodies();
		ReplicationWorld CaptureWorld() const;
		std::optional<std::vector<uint8_t>> BuildSpawn(const ReplicatedEntity& entity) const;
		bool IsActivePeer(uint32_t peerId) const;
		uint32_t GetAdmittedCount() const;
		Connection* FindConnection(NetPeerHandle handle);
		// The most bytes a connection's budget holds: a second of MaxBytesPerSecond.
		double FullBudget() const;
		// False when the transport did not take the message.
		bool Transmit(NetPeerHandle handle, NetChannel channel, const std::vector<uint8_t>& data, NetTraffic& traffic);

		Scene& m_Scene;
		NetworkRole m_Role;
		Scope<NetTransport> m_Transport;
		NetworkSettings m_Settings;
		Identity m_Identity;
		NetConnectionState m_State = NetConnectionState::Connecting;
		NetDisconnectReason m_DisconnectReason = NetDisconnectReason::None;
		uint32_t m_LocalPeerId = 0;
		float m_Clock = 0.0f;
		// The project's TickRate; a client's becomes the host's when it is accepted.
		float m_TickRate = 0.0f;
		float m_TickAccumulator = 0.0f;
		float m_SinceLastTick = 0.0f;
		NetworkStatistics m_Statistics;
		// Problems already logged (a host's own, or a client's about its host).
		mutable std::unordered_set<std::string> m_Reported;

		// Host: connections by transport handle (admitted or waiting for their Hello), and the next peer ID.
		std::map<NetPeerHandle, Connection> m_Connections;
		uint32_t m_NextPeerId = 1;

		// Client: the host's connection (set once the transport connects, when the Hello goes), the newest snapshot applied,
		// what goes back, and what the host spawned here.
		std::optional<NetPeerHandle> m_HostHandle;
		std::string m_HostAddress;
		float m_HelloSentAt = 0.0f;
		uint32_t m_LastSnapshot = 0;
		ReplicationSender m_ClientSender;
		double m_ClientTokens = 0.0;
		NetTraffic m_HostTraffic;
		std::unordered_set<UUID> m_RemoteEntities;
		std::map<UUID, PendingParent> m_PendingParents;

		// Entities whose body this peer drives kinematically, in ID order (physics rebuilds follow it).
		std::set<UUID> m_DrivenBodies;
	};

}
