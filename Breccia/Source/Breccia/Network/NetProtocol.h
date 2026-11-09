#pragma once

#include "Breccia/Core/UUID.h"
#include "Breccia/Network/NetBuffer.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Breccia {

	class ReplicationSchema;

	// Why a connection ended. Sent as the transport's disconnect code, so a peer learns it with the disconnect (unless that
	// datagram is lost: then it times out).
	enum class NetDisconnectReason : uint32_t
	{
		None = 0,
		// The client left (Scene::StopNetwork, or the end of Play).
		ClientQuit,
		// The host stopped its session.
		ServerShutdown,
		// The two peers speak different protocol versions (NetProtocol::Version).
		ProtocolMismatch,
		// They replicate different components or fields (ReplicationSchema::GetHash).
		SchemaMismatch,
		// The client plays another game, or another version of it (the project's Name and Version).
		GameMismatch,
		ServerFull,
		// Nothing arrived from the peer for the session's timeout.
		Timeout,
		// The client connected but never sent its Hello.
		HandshakeTimeout,
		// The peer sent something that does not decode, or a message it must not send.
		InvalidPacket,
		// The host could not be reached, or refused the connection at the transport level.
		ConnectionFailed,
		// A code this build does not know (a newer peer's); never sent.
		Unknown,
		Count
	};

	// "the client left", "the connection timed out", ...
	const char* DescribeDisconnectReason(NetDisconnectReason reason);
	// Reads a transport disconnect code: 0 is None, codes this build does not know are Unknown.
	NetDisconnectReason DisconnectReasonFromCode(uint32_t code);

	enum class NetMessageType : uint8_t
	{
		// Client to host, reliable: who is connecting.
		Hello = 1,
		// Host to client, reliable: the client's peer ID.
		Welcome,
		// Host to client, unreliable: entity state.
		Snapshot,
		// Client to host, unreliable: acknowledgements and the state of entities it has authority over.
		ClientUpdate
	};

	struct NetHello
	{
		uint32_t ProtocolVersion = 0;
		// Only meaningful when ProtocolVersion is this build's: a newer peer's Hello may hold anything after it.
		uint64_t SchemaHash = 0;
		std::string Game;
		std::string GameVersion;
	};

	struct NetWelcome
	{
		uint32_t PeerId = 0;
		// Snapshots per second the host sends; the client sends its updates at the same rate.
		float TickRate = 0.0f;
	};

	struct NetFieldValue
	{
		// Index into the component's ComponentInfo::ReplicatedFields.
		uint32_t Field = 0;
		nlohmann::json Value;
	};

	// What happened to a component of an updated entity.
	enum class NetComponentChange : uint8_t
	{
		// Some of its replicated fields changed (at least one is listed).
		Changed = 0,
		// It is gone from the entity (no fields).
		Removed,
		// It is new on the entity (every replicated field is listed); repeated until acknowledged.
		Added
	};

	// One component of an updated entity. Only components that declare replicated fields appear.
	struct NetComponentUpdate
	{
		// Index into ReplicationSchema::GetComponents.
		uint32_t Component = 0;
		NetComponentChange Change = NetComponentChange::Changed;
		std::vector<NetFieldValue> Fields;
	};

	struct NetEntityUpdate
	{
		UUID Entity = 0;
		// Set when the entity's parent changed (0: a root).
		std::optional<UUID> Parent;
		std::vector<NetComponentUpdate> Components;
	};

	// A networked entity appearing on a client: its tree as scene-file entity records (SceneSerializer), root first.
	struct NetEntitySpawn
	{
		UUID Entity = 0;
		UUID Parent = 0;
		nlohmann::json Entities;
	};

	// A Snapshot (host to client) or a ClientUpdate (client to host, which never holds spawns or despawns). Each entity
	// appears at most once across the three lists.
	struct NetState
	{
		uint32_t Sequence = 0;
		// The newest message of the other direction the sender applied (0: none yet).
		uint32_t Acknowledged = 0;
		std::vector<NetEntitySpawn> Spawns;
		std::vector<NetEntityUpdate> Updates;
		std::vector<UUID> Despawns;
	};

	struct NetMessage
	{
		NetMessageType Type = NetMessageType::Hello;
		std::variant<NetHello, NetWelcome, NetState> Body;
	};

	// The wire format of every message: a type byte, then the body. Decoders treat their input as hostile (NetReader),
	// bound every count and check every record with CheckUpdate and CheckSpawn; encoders write whatever they are given (the
	// tests encode invalid records on purpose), so producers check their records with the same functions.
	namespace NetProtocol {

		// "BRNT", first in a Hello: anything else is not a Breccia client.
		inline constexpr uint32_t Magic = 0x544E5242u;
		// Bump with every change to the format below.
		inline constexpr uint32_t Version = 1;
		// Longest game name or version in a Hello (ProjectConfig::MaxTextLength, checked where both are known).
		inline constexpr size_t MaxTextLength = 256;
		inline constexpr uint32_t MaxRecords = 4096;
		inline constexpr uint32_t MaxComponentsPerEntity = 256;
		inline constexpr uint32_t MaxFieldsPerComponent = 256;
		inline constexpr uint32_t MaxSpawnEntities = 4096;

		std::vector<uint8_t> EncodeHello(const NetHello& hello);
		std::vector<uint8_t> EncodeWelcome(const NetWelcome& welcome);
		// False when a value cannot be encoded (NetValue::Encode).
		bool WriteSpawn(NetWriter& writer, const NetEntitySpawn& spawn);
		bool WriteUpdate(NetWriter& writer, const NetEntityUpdate& update);
		// A Snapshot or ClientUpdate from records WriteSpawn and WriteUpdate wrote.
		std::vector<uint8_t> AssembleState(NetMessageType type, uint32_t sequence, uint32_t acknowledged, std::span<const std::vector<uint8_t>> spawns, std::span<const std::vector<uint8_t>> updates, std::span<const UUID> despawns);
		// Empty when a value cannot be encoded.
		std::vector<uint8_t> EncodeState(NetMessageType type, const NetState& state);

		// What is wrong with a record, or an empty string: an entity ID of 0 or its own parent, nothing to say, a component
		// the schema does not have or that declares no replicated fields, a core component added or removed, a removed one
		// with fields, an added one without every replicated field, a changed one without any, a field index out of range or
		// repeated.
		std::string CheckUpdate(const NetEntityUpdate& update, const ReplicationSchema& schema);
		// The same for a spawn: its IDs, and 1 to MaxSpawnEntities records (whose contents SceneSerializer checks).
		std::string CheckSpawn(const NetEntitySpawn& spawn);

		// Decodes one message, checking component and field indices against the schema. A Hello whose version is not
		// Version decodes with only ProtocolVersion set, so the host can answer ProtocolMismatch. nullopt (with
		// outError) for anything malformed, including trailing bytes.
		std::optional<NetMessage> Decode(std::span<const uint8_t> bytes, const ReplicationSchema& schema, std::string& outError);

	}

}
