#pragma once

#include "Breccia/Core/Base.h"
#include "Breccia/Core/UUID.h"
#include "Breccia/Network/NetProtocol.h"
#include "Breccia/Network/NetTransport.h"
#include "Breccia/Network/NetworkSettings.h"
#include "Breccia/Network/Replication.h"
#include "Breccia/Physics/PhysicsWorld.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
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

	// "Host" or "Client"; "Connecting", "Connected" or "Disconnected" (Lua, automation).
	const char* NetworkRoleName(NetworkRole role);
	const char* NetConnectionStateName(NetConnectionState state);

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

	// Something a session tells the scene's scripts (Lua Network.On), taken by Scene::OnUpdate after Receive and dispatched by
	// the ScriptEngine before OnUpdate, so scripts never run inside the network session.
	struct NetworkEvent
	{
		enum class Kind : uint8_t
		{
			// A client: the host accepted it (Peer: its own peer ID).
			Connected,
			// A client: its connection ended (Reason).
			Disconnected,
			// A host: a client joined (Peer).
			PeerConnected,
			// A host: a client left (Peer, Reason).
			PeerDisconnected,
			// A script message (Name, Arguments) from Peer (0: the host).
			Message,
			// An RPC: Name is the method of Entity's script to call with Arguments, from Peer (0: the host).
			Rpc
		};

		Kind Type = Kind::Message;
		uint32_t Peer = 0;
		NetDisconnectReason Reason = NetDisconnectReason::None;
		UUID Entity = 0;
		std::string Name;
		nlohmann::json Arguments = nlohmann::json::array();
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
		// Script messages and RPCs sent, handed to scripts (an RPC a script then refuses included), and dropped: an RPC for
		// an entity that is not there (the host's) or never arrived, was despawned or was refused (a client's), the oldest
		// waiting one past MaxPendingScripts,
		// one that arrived before the Welcome, messages past the event queue's limit, one that did not decode and those
		// queued after it, and those still waiting when their connection ended without them.
		uint64_t ScriptMessagesSent = 0;
		uint64_t ScriptMessagesReceived = 0;
		uint64_t ScriptMessagesDropped = 0;
		// Prediction. A host's, in steps: inputs applied, inputs refused (every step of a batch for another client's
		// character, one not Predicted, or one it could not build, repeats included), inputs lost before they arrived (a gap
		// in the numbering, up to MaxUnacknowledgedInputs at a time), and steps it took without an input (the owner fell
		// silent). A client's: corrections (each snapshot replays its unacknowledged inputs from the host's state; a
		// correction moved the character more than PredictionTolerance).
		uint64_t InputsApplied = 0;
		uint64_t InputsRejected = 0;
		uint64_t InputsSkipped = 0;
		uint64_t IdleSteps = 0;
		uint64_t Corrections = 0;
	};

	// A playing scene's network session (owned by Scene, Scene::StartNetwork). The host is authoritative: it sends each
	// client the networked entities (NetworkedComponent) as snapshots at the project's tick rate, spawning them with
	// their UUIDs, updating ComponentInfo::ReplicatedFields when they change (ReplicationSender) and despawning them.
	// A client applies them through ComponentRegistry, so every value passes the same checks as a file's, and sends back
	// the replicated fields of entities whose Authority is Owner and whose Owner is its peer ID, which the host checks
	// and applies. Peers drive the dynamic bodies and character controllers of entities they neither decide nor predict
	// kinematically, and a host drives the characters it moves from a client's inputs (PhysicsWorld::SetDrivenExternally;
	// this system is its only caller).
	//
	// On top of that: script messages and RPCs both ways, handed to scripts as NetworkEvents (TakeEvents); a client shows
	// remote transforms InterpolationDelay behind the host clock the snapshots carry; a client predicts the Predicted
	// character controllers it owns from its own inputs, which the host applies (StepCharacter) and answers with the
	// character's state, from which the client replays; and a host keeps a second of networked bodies' poses for
	// lag-compensated raycasts (RaycastAt).
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
		// Script messages and RPCs past this many events waiting for the scripts are dropped (counted, logged once a session);
		// connection events always queue.
		static constexpr size_t MaxQueuedEvents = 4096;
		// A client keeps an RPC for an entity it does not have yet (its spawn may still be on its way) this long, and the
		// script messages that arrived after it wait behind it (they arrive in order); at most this many wait.
		static constexpr float PendingRpcSeconds = 2.0f;
		static constexpr size_t MaxPendingScripts = 256;
		// A host keeps this many of one client's script messages, and this many of their bytes, waiting for its
		// MaxMessagesPerSecond; a client that sends more is disconnected (InvalidPacket).
		static constexpr size_t MaxInboxMessages = 1024;
		static constexpr size_t MaxInboxBytes = size_t{ 1024 } * 1024;
		// How far back lag compensation rewinds (RaycastAt clamps older times).
		static constexpr double MaxRewindSeconds = 1.0;
		// A host's room for a client's inputs (seconds of its own fixed steps), half of it ahead of its steps and half behind:
		// a client that sends inputs faster than real time waits, so it cannot move faster than one that does not, and one
		// that falls silent for longer than half of this has its character stepped without inputs (gravity still pulls).
		static constexpr float InputWindowSeconds = 0.5f;
		// At most this many steps without inputs per frame.
		static constexpr uint32_t MaxIdleStepsPerFrame = 8;
		// A client remembers this many unacknowledged inputs of each character it predicts (older ones are forgotten).
		static constexpr size_t MaxUnacknowledgedInputs = 256;
		// A correction moving a predicted character less than this (meters) is not counted in NetworkStatistics.
		static constexpr float PredictionTolerance = 0.01f;

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

		// Receives and applies what arrived (a host applies clients' inputs to the characters they predict, a client
		// corrects its predicted characters), ends handshakes that took too long, hands queued script messages on as events
		// (a host at each client's MaxMessagesPerSecond, a client once their entities exist), parents children whose parent
		// arrived (or whose move became possible), drives bodies by authority, and places interpolated entities. Called by
		// Scene::OnUpdate before scripts.
		void Receive(float deltaTime);
		// Collects the steps the client's predicted characters took, then sends this tick's messages when one is due (at
		// most one tick per frame; a host records its networked bodies' poses on each). Called by Scene::OnUpdate after the
		// frame's systems.
		void Send(float deltaTime);

		// Whether this peer decides the entity: everything not networked; on a host, every networked entity except those
		// a connected client owns with Owner authority; on a connected client, only those it owns with Owner authority.
		bool HasAuthority(Entity entity) const;
		// Whether the entity's Owner names this peer: on a host Owner 0 or a client that is not connected, on a connected
		// client its peer ID. True for entities that are not networked. Scripts move what they own from local input.
		bool IsOwner(Entity entity) const;
		// Whether this client predicts the entity: a character controller (built in its physics world) it owns with Predicted
		// authority. It moves the character from its own Move calls at once, sends the inputs to the host and corrects it
		// from the host's state. Decided each Receive.
		bool IsPredicted(Entity entity) const;

		// What scripts should hear of since the last call, oldest first (Scene::OnUpdate passes them to the ScriptEngine).
		std::vector<NetworkEvent> TakeEvents();
		// Sends a script message or RPC to `peer`: from a client only to the host (0), from a host to an admitted client.
		// Reliable messages arrive once and in order with the session's other reliable messages. False, with the reason,
		// when it was not sent: an invalid message (NetProtocol::CheckScript), one too large, nested too deeply or holding text
		// that is not UTF-8, no such peer, not connected, or this connection's MaxMessagesPerSecond or bandwidth budget spent.
		bool SendScript(uint32_t peer, const NetScriptMessage& message, bool reliable, std::string& outError);
		// A host's admitted client peer IDs, in join order; a client's host (0) once connected.
		std::vector<uint32_t> GetPeers() const;
		// The round trip to a peer in seconds; nullopt for a peer this one has no admitted connection to (a client's host only
		// once it is connected; 0 while unmeasured).
		std::optional<float> GetRoundTripTime(uint32_t peer) const;

		// The session's clock: a host's seconds since its session started; a client's estimate of the host's, from the
		// snapshots it receives (0 before the first).
		double GetTime() const;
		// The host time a client shows the entities it does not decide at (GetTime minus the InterpolationDelay); a host's
		// GetTime. Lag compensation rewinds to it (RaycastAt).
		double GetViewTime() const;
		// A host's raycast as a client saw the world at `time` (its GetViewTime when it acted, clamped to the last
		// MaxRewindSeconds): the networked bodies that move stand where they were at the host's ticks around then, those the
		// host had not recorded then (created later) are not there, and everything else is where it is now. Elsewhere, or for
		// the present, a plain raycast.
		std::optional<RaycastHit> RaycastAt(double time, const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter = {}) const;

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
		// Script messages a host received from a client and has not handed to scripts yet (their encoded bytes, decoded as
		// they are released at MaxMessagesPerSecond, so a flood costs no more memory than its bytes).
		struct Inbox
		{
			std::deque<std::vector<uint8_t>> Messages;
			// The sum of their sizes: change Messages only through Push and Pop.
			size_t Bytes = 0;
			double ReleaseCredit = 0.0;

			void Push(std::vector<uint8_t> bytes)
			{
				Bytes += bytes.size();
				Messages.push_back(std::move(bytes));
			}
			std::vector<uint8_t> Pop()
			{
				std::vector<uint8_t> bytes = std::move(Messages.front());
				Messages.pop_front();
				Bytes -= bytes.size();
				return bytes;
			}
		};

		struct Connection
		{
			NetPeerHandle Handle = 0;
			// The peer's address, kept for messages after the transport forgot it.
			std::string Address;
			// 0 until its Hello is accepted.
			uint32_t PeerId = 0;
			double ConnectedAt = 0.0;
			ReplicationSender Sender = ReplicationSender(ReplicationSenderOptions{ .Direction = ReplicationDirection::HostToClient, .MaxMessageBytes = MaxHostMessageBytes });
			// The newest ClientUpdate applied, which snapshots acknowledge.
			uint32_t LastApplied = 0;
			// Bytes this connection may still send (its budget, refilled at MaxBytesPerSecond).
			double Tokens = 0.0;
			NetTraffic Traffic;
			// Problems already logged for this client.
			std::unordered_set<std::string> Reported;
			// Script messages this connection may still send this second (MaxMessagesPerSecond), and those it sent that wait.
			double SendCredit = 0.0;
			Inbox Received;

			bool IsAdmitted() const { return PeerId != 0; }
		};

		// An entity waiting for its parent to arrive, or to be a place it can go (ResolvePendingParents retries each Receive).
		struct PendingParent
		{
			UUID Parent = 0;
			double Since = 0.0;
			bool Reported = false;
		};

		// A character a client predicts: the inputs the host has not acknowledged, the first one numbered First (they are
		// consecutive: only the oldest are ever dropped), and problems already logged.
		struct PredictedCharacter
		{
			uint32_t First = 1;
			std::deque<CharacterInput> Unacknowledged;
			bool ReportedClamp = false;
			bool ReportedBacklog = false;
			bool ReportedReplay = false;

			// The number the next input takes.
			uint32_t Next() const { return First + static_cast<uint32_t>(Unacknowledged.size()); }
		};

		// A character a host moves from a client's inputs.
		struct RemoteCharacter
		{
			uint32_t Peer = 0;
			// The newest input applied (0: none).
			uint32_t LastInput = 0;
			// Inputs it may still apply (InputWindowSeconds of fixed steps at most), refilled by the host's own steps.
			float Credit = 0.0f;
			// Its owner fell silent for half the window: the host steps it without inputs, keeping Credit at half the window,
			// until an input arrives (so silence banks no burst of movement).
			bool Silent = false;
		};

		// A client's transforms of an entity it shows behind the host (InterpolationDelay): host time and value, oldest
		// first.
		struct TransformSample
		{
			double Time = 0.0;
			glm::vec3 Translation = glm::vec3(0.0f);
			glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 Scale = glm::vec3(1.0f);

			static TransformSample From(const TransformComponent& transform, double time) { return { time, transform.Translation, transform.Rotation, transform.Scale }; }
			void ApplyTo(TransformComponent& transform) const
			{
				transform.Translation = Translation;
				transform.Rotation = Rotation;
				transform.Scale = Scale;
			}
			bool Shows(const TransformComponent& transform) const { return transform.Translation == Translation && transform.Rotation == Rotation && transform.Scale == Scale; }
		};

		// An entity a client shows behind the host: its received transforms (never empty: RecordTransform creates a track with
		// its first sample), and what Interpolate last wrote (a transform that differs from it was changed locally, which
		// stops the interpolation until the next update).
		struct InterpolationTrack
		{
			std::deque<TransformSample> Samples;
			std::optional<TransformSample> Written;
		};

		// A host's record of where a networked body was at each tick, for lag compensation.
		struct PoseSample
		{
			double Time = 0.0;
			PhysicsBodyPose Pose;
		};

		// A script message a client received that waits: an RPC for an entity it does not have yet, or a message behind one.
		struct PendingScript
		{
			NetworkEvent Event;
			double Since = 0.0;
			// Came on the reliable channel: it keeps its place behind a reliable RPC that waits.
			bool Reliable = true;
		};

		void HandleEvent(const NetTransportEvent& event);
		void PushEvent(NetworkEvent event);
		// A script message from `sender` (0: the host) for scripts; `reported` is the sender's set of logged problems.
		// `reliable`: it came on the reliable channel (a client keeps those in order behind an RPC that waits).
		void HandleScriptMessage(uint32_t sender, NetScriptMessage message, bool reliable, std::unordered_set<std::string>& reported);
		// Hands a host's queued client messages to scripts, as each connection's MaxMessagesPerSecond allows (`all`: every
		// one, for a client that left), decoding them now. False when one did not decode: it and those after it are dropped
		// (counted), and the caller ends that connection.
		bool ReleaseInbox(Connection& connection, bool all, std::string& outError);
		void ReleasePendingScripts();
		// A client's record of entities it lost or never showed (despawned, a spawn it refused, an update for one it does
		// not have) for PendingRpcSeconds: an RPC for one is dropped at once rather than waiting for a spawn.
		void MarkGone(UUID uuid);
		bool IsGone(UUID uuid) const;
		// The connection a peer ID names (admitted), or null.
		Connection* FindPeer(uint32_t peer);
		const Connection* FindPeer(uint32_t peer) const;
		// Prediction: a client keeps m_Predicted in step with what it owns; a host keeps m_RemoteCharacters.
		void SyncPredictedCharacters();
		// Whether a client should predict the entity (IsPredicted once its character exists).
		bool WantsPrediction(Entity entity) const;
		// Fixed steps of input a remote character may bank (InputWindowSeconds at the scene's fixed step, at least 2).
		float InputWindowSteps() const;
		void SyncRemoteCharacters();
		void ApplyInputs(Connection& connection, const NetInputs& inputs);
		void Reconcile(const NetPrediction& prediction);
		std::vector<NetPrediction> BuildPredictions(uint32_t peer);
		std::vector<NetInputs> CollectInputs();
		// Interpolation (a client): whether the entity's transform is shown behind the host, recording a received one (with
		// what it showed before, when no track follows it yet), and placing every tracked entity at the view time.
		bool IsInterpolated(Entity entity) const;
		void RecordTransform(Entity entity, double previousSnapshot, const std::optional<TransformSample>& before);
		void Interpolate();
		// Forgets the tracks of entities whose transform something else changed since Interpolate wrote it.
		void DropEditedTracks();
		// Lag compensation (a host): records where networked bodies are at this tick.
		void RecordPoses();
		void HandleHostMessage(Connection& connection, const std::vector<uint8_t>& data);
		void HandleClientMessage(const std::vector<uint8_t>& data, NetChannel channel);
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
		// Puts child under parent; false when it cannot go there now (a cycle, or too deep).
		bool TryParent(Entity child, Entity parent);
		void WaitForParent(UUID child, UUID parent);
		void ResolvePendingParents();
		// Applies a repeated spawn to the entity an earlier one created (or an existing one it names): its root's replicated
		// fields, which components with replicated fields it has, its parent, and its descendants (ReconcileDescendants).
		// The rest of the root record (other fields and components) is not applied.
		void RefreshFromSpawn(Entity entity, const NetEntitySpawn& spawn);
		// Creates the spawn's descendant records whose IDs this client does not have, under the entities they name, and
		// moves those it has under the parents the spawn gives them (keeping their own values).
		void ReconcileDescendants(const NetEntitySpawn& spawn);
		void RemoveLocalNetworkedEntities();
		void UpdateDrivenBodies();
		ReplicationWorld CaptureWorld() const;
		std::optional<std::vector<uint8_t>> BuildSpawn(const ReplicatedEntity& entity) const;
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
		double m_Clock = 0.0;
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
		double m_HelloSentAt = 0.0;
		uint32_t m_LastSnapshot = 0;
		ReplicationSender m_ClientSender;
		double m_ClientTokens = 0.0;
		NetTraffic m_HostTraffic;
		std::unordered_set<UUID> m_RemoteEntities;
		std::map<UUID, PendingParent> m_PendingParents;

		// What the last UpdateDrivenBodies asked physics to drive, in ID order (physics rebuilds follow it), so that this system
		// can release them; physics holds what is driven now.
		std::set<UUID> m_DrivenBodies;

		// Script events waiting for TakeEvents.
		std::vector<NetworkEvent> m_Events;
		bool m_ReportedEventOverflow = false;
		// Client: script messages waiting behind an RPC for its entity, the script messages it may still send this second, the server clock
		// estimate, the host time of the newest snapshot applied, the characters it predicts, and the transforms it shows
		// behind the host.
		std::deque<PendingScript> m_PendingScripts;
		// Client: MarkGone's entities and when, oldest first (at most MaxPendingScripts).
		std::deque<std::pair<UUID, double>> m_GoneEntities;
		double m_ClientSendCredit = 0.0;
		std::optional<double> m_ServerClock;
		double m_LastSnapshotTime = 0.0;
		std::map<UUID, PredictedCharacter> m_Predicted;
		std::map<UUID, InterpolationTrack> m_Interpolation;
		// Host: characters moved by clients' inputs, the physics step count they were last credited at, and where networked
		// bodies were at each recent tick.
		std::map<UUID, RemoteCharacter> m_RemoteCharacters;
		uint64_t m_CreditedSteps = 0;
		std::map<UUID, std::deque<PoseSample>> m_PoseHistory;
	};

}
