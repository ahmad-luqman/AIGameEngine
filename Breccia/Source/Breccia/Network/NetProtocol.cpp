#include "Breccia/Network/NetProtocol.h"

#include "Breccia/Core/TextEncoding.h"
#include "Breccia/Network/NetValue.h"
#include "Breccia/Network/ReplicationSchema.h"
#include "Breccia/Scene/ComponentRegistry.h"

#include <cmath>
#include <unordered_set>

namespace Breccia {

	const char* DescribeDisconnectReason(NetDisconnectReason reason)
	{
		switch (reason)
		{
			case NetDisconnectReason::None:
				return "no reason given";
			case NetDisconnectReason::ClientQuit:
				return "the client left";
			case NetDisconnectReason::ServerShutdown:
				return "the host stopped";
			case NetDisconnectReason::ProtocolMismatch:
				return "the peers use different network protocol versions";
			case NetDisconnectReason::SchemaMismatch:
				return "the peers replicate different components (different engine builds)";
			case NetDisconnectReason::GameMismatch:
				return "the peers play different games or game versions";
			case NetDisconnectReason::ServerFull:
				return "the host is full";
			case NetDisconnectReason::Timeout:
				return "the connection timed out";
			case NetDisconnectReason::HandshakeTimeout:
				return "the client did not introduce itself in time";
			case NetDisconnectReason::InvalidPacket:
				return "a peer sent an invalid message";
			case NetDisconnectReason::ConnectionFailed:
				return "the connection could not be made";
			case NetDisconnectReason::Count:
				break;
		}
		return "unknown reason";
	}

	NetDisconnectReason DisconnectReasonFromCode(uint32_t code)
	{
		return code < static_cast<uint32_t>(NetDisconnectReason::Count) ? static_cast<NetDisconnectReason>(code) : NetDisconnectReason::None;
	}

	namespace NetProtocol {

		namespace {

			// Component entry flags.
			constexpr uint8_t ComponentRemoved = 1;
			constexpr uint8_t ComponentAdded = 2;
			// Entity update flags.
			constexpr uint8_t EntityHasParent = 1;

			bool Fail(NetReader& reader, std::string& outError, std::string message)
			{
				reader.Fail();
				if (outError.empty())
					outError = std::move(message);
				return false;
			}

			bool ReadText(NetReader& reader, std::string& text, std::string& outError, const char* what)
			{
				if (!reader.ReadString(text, MaxTextLength))
					return Fail(reader, outError, std::string(what) + " is truncated or too long");
				if (!TextEncoding::IsValidUtf8(text))
					return Fail(reader, outError, std::string(what) + " is not UTF-8");
				return true;
			}

			bool ReadUuid(NetReader& reader, UUID& uuid, std::string& outError)
			{
				uint64_t value = 0;
				if (!reader.ReadU64(value))
					return Fail(reader, outError, "an entity ID is truncated");
				if (value == 0)
					return Fail(reader, outError, "an entity ID is 0");
				uuid = value;
				return true;
			}

			bool ReadComponent(NetReader& reader, const ReplicationSchema& schema, NetComponentUpdate& component, std::string& outError)
			{
				if (!reader.ReadCount(component.Component, std::numeric_limits<uint32_t>::max()))
					return Fail(reader, outError, "a component index is malformed");
				const ReplicationSchema::Component* info = schema.FindComponent(component.Component);
				if (!info)
					return Fail(reader, outError, "unknown component index " + std::to_string(component.Component));
				uint8_t flags = 0;
				if (!reader.ReadU8(flags))
					return Fail(reader, outError, "a component entry is truncated");
				if ((flags & ~(ComponentRemoved | ComponentAdded)) != 0 || flags == (ComponentRemoved | ComponentAdded))
					return Fail(reader, outError, "a component entry has invalid flags");
				component.Removed = (flags & ComponentRemoved) != 0;
				component.Added = (flags & ComponentAdded) != 0;
				if ((component.Removed || component.Added) && info->IsCore)
					return Fail(reader, outError, info->Info->Name + " cannot be added or removed");
				uint32_t fieldCount = 0;
				if (!reader.ReadCount(fieldCount, MaxFieldsPerComponent))
					return Fail(reader, outError, "a field count is malformed or too large");
				if (component.Removed && fieldCount != 0)
					return Fail(reader, outError, "a removed component carries fields");
				const size_t replicated = info->Info->ReplicatedFields.size();
				if (component.Added && fieldCount != replicated)
					return Fail(reader, outError, "an added " + info->Info->Name + " must carry every replicated field");
				if (!component.Removed && !component.Added && fieldCount == 0)
					return Fail(reader, outError, "a component entry carries nothing");
				std::vector<bool> seen(replicated, false);
				component.Fields.resize(fieldCount);
				for (NetFieldValue& field : component.Fields)
				{
					if (!reader.ReadCount(field.Field, std::numeric_limits<uint32_t>::max()))
						return Fail(reader, outError, "a field index is malformed");
					if (field.Field >= replicated)
						return Fail(reader, outError, info->Info->Name + " has no replicated field " + std::to_string(field.Field));
					if (seen[field.Field])
						return Fail(reader, outError, "a field appears twice");
					seen[field.Field] = true;
					if (!NetValue::Decode(reader, field.Value))
						return Fail(reader, outError, "a field value is malformed");
				}
				return true;
			}

			bool ReadUpdate(NetReader& reader, const ReplicationSchema& schema, NetEntityUpdate& update, std::string& outError)
			{
				if (!ReadUuid(reader, update.Entity, outError))
					return false;
				uint8_t flags = 0;
				if (!reader.ReadU8(flags) || (flags & ~EntityHasParent) != 0)
					return Fail(reader, outError, "an entity update has invalid flags");
				if (flags & EntityHasParent)
				{
					uint64_t parent = 0;
					if (!reader.ReadU64(parent))
						return Fail(reader, outError, "a parent ID is truncated");
					if (parent == update.Entity)
						return Fail(reader, outError, "an entity cannot be its own parent");
					update.Parent = UUID(parent);
				}
				uint32_t componentCount = 0;
				if (!reader.ReadCount(componentCount, MaxComponentsPerEntity))
					return Fail(reader, outError, "a component count is malformed or too large");
				if (componentCount == 0 && !update.Parent)
					return Fail(reader, outError, "an entity update carries nothing");
				std::unordered_set<uint32_t> components;
				update.Components.resize(componentCount);
				for (NetComponentUpdate& component : update.Components)
				{
					if (!ReadComponent(reader, schema, component, outError))
						return false;
					if (!components.insert(component.Component).second)
						return Fail(reader, outError, "a component appears twice in one entity update");
				}
				return true;
			}

			bool ReadSpawn(NetReader& reader, NetEntitySpawn& spawn, std::string& outError)
			{
				if (!ReadUuid(reader, spawn.Entity, outError))
					return false;
				uint64_t parent = 0;
				if (!reader.ReadU64(parent))
					return Fail(reader, outError, "a spawn's parent is truncated");
				if (parent == spawn.Entity)
					return Fail(reader, outError, "an entity cannot be its own parent");
				spawn.Parent = parent;
				if (!NetValue::Decode(reader, spawn.Entities))
					return Fail(reader, outError, "a spawn's entities are malformed");
				// The records themselves are checked where they are created (SceneSerializer), as a scene file's are.
				if (!spawn.Entities.is_array() || spawn.Entities.empty() || spawn.Entities.size() > MaxSpawnEntities)
					return Fail(reader, outError, "a spawn must hold 1 to " + std::to_string(MaxSpawnEntities) + " entity records");
				return true;
			}

			bool ReadState(NetReader& reader, NetMessageType type, const ReplicationSchema& schema, NetState& state, std::string& outError)
			{
				if (!reader.ReadU32(state.Sequence) || !reader.ReadU32(state.Acknowledged))
					return Fail(reader, outError, "the message header is truncated");
				if (state.Sequence == 0)
					return Fail(reader, outError, "sequence numbers start at 1");
				std::unordered_set<UUID> entities;
				const auto unique = [&](UUID uuid) {
					return entities.insert(uuid).second || Fail(reader, outError, "an entity appears twice in one message");
				};
				uint32_t count = 0;
				if (!reader.ReadCount(count, MaxRecords))
					return Fail(reader, outError, "the spawn count is malformed or too large");
				if (type == NetMessageType::ClientUpdate && count != 0)
					return Fail(reader, outError, "clients cannot spawn entities");
				state.Spawns.resize(count);
				for (NetEntitySpawn& spawn : state.Spawns)
					if (!ReadSpawn(reader, spawn, outError) || !unique(spawn.Entity))
						return false;
				if (!reader.ReadCount(count, MaxRecords))
					return Fail(reader, outError, "the update count is malformed or too large");
				state.Updates.resize(count);
				for (NetEntityUpdate& update : state.Updates)
					if (!ReadUpdate(reader, schema, update, outError) || !unique(update.Entity))
						return false;
				if (!reader.ReadCount(count, MaxRecords))
					return Fail(reader, outError, "the despawn count is malformed or too large");
				if (type == NetMessageType::ClientUpdate && count != 0)
					return Fail(reader, outError, "clients cannot despawn entities");
				state.Despawns.resize(count);
				for (UUID& uuid : state.Despawns)
					if (!ReadUuid(reader, uuid, outError) || !unique(uuid))
						return false;
				return true;
			}

		}

		std::vector<uint8_t> EncodeHello(const NetHello& hello)
		{
			NetWriter writer;
			writer.WriteU8(static_cast<uint8_t>(NetMessageType::Hello));
			writer.WriteU32(Magic);
			writer.WriteU32(hello.ProtocolVersion);
			writer.WriteU64(hello.SchemaHash);
			writer.WriteString(hello.Game);
			writer.WriteString(hello.GameVersion);
			return writer.TakeBytes();
		}

		std::vector<uint8_t> EncodeWelcome(const NetWelcome& welcome)
		{
			NetWriter writer;
			writer.WriteU8(static_cast<uint8_t>(NetMessageType::Welcome));
			writer.WriteVarUInt(welcome.PeerId);
			writer.WriteFloat(welcome.TickRate);
			return writer.TakeBytes();
		}

		bool WriteSpawn(NetWriter& writer, const NetEntitySpawn& spawn)
		{
			writer.WriteU64(spawn.Entity);
			writer.WriteU64(spawn.Parent);
			return NetValue::Encode(spawn.Entities, writer);
		}

		bool WriteUpdate(NetWriter& writer, const NetEntityUpdate& update)
		{
			writer.WriteU64(update.Entity);
			writer.WriteU8(update.Parent ? EntityHasParent : 0);
			if (update.Parent)
				writer.WriteU64(*update.Parent);
			writer.WriteVarUInt(update.Components.size());
			for (const NetComponentUpdate& component : update.Components)
			{
				writer.WriteVarUInt(component.Component);
				writer.WriteU8(static_cast<uint8_t>((component.Removed ? ComponentRemoved : 0) | (component.Added ? ComponentAdded : 0)));
				writer.WriteVarUInt(component.Fields.size());
				for (const NetFieldValue& field : component.Fields)
				{
					writer.WriteVarUInt(field.Field);
					if (!NetValue::Encode(field.Value, writer))
						return false;
				}
			}
			return true;
		}

		std::vector<uint8_t> AssembleState(NetMessageType type, uint32_t sequence, uint32_t acknowledged, std::span<const std::vector<uint8_t>> spawns, std::span<const std::vector<uint8_t>> updates, std::span<const UUID> despawns)
		{
			NetWriter writer;
			writer.WriteU8(static_cast<uint8_t>(type));
			writer.WriteU32(sequence);
			writer.WriteU32(acknowledged);
			writer.WriteVarUInt(spawns.size());
			for (const std::vector<uint8_t>& spawn : spawns)
				writer.WriteBytes(spawn);
			writer.WriteVarUInt(updates.size());
			for (const std::vector<uint8_t>& update : updates)
				writer.WriteBytes(update);
			writer.WriteVarUInt(despawns.size());
			for (UUID uuid : despawns)
				writer.WriteU64(uuid);
			return writer.TakeBytes();
		}

		std::vector<uint8_t> EncodeState(NetMessageType type, const NetState& state)
		{
			std::vector<std::vector<uint8_t>> spawns;
			std::vector<std::vector<uint8_t>> updates;
			for (const NetEntitySpawn& spawn : state.Spawns)
			{
				NetWriter writer;
				if (!WriteSpawn(writer, spawn))
					return {};
				spawns.push_back(writer.TakeBytes());
			}
			for (const NetEntityUpdate& update : state.Updates)
			{
				NetWriter writer;
				if (!WriteUpdate(writer, update))
					return {};
				updates.push_back(writer.TakeBytes());
			}
			return AssembleState(type, state.Sequence, state.Acknowledged, spawns, updates, state.Despawns);
		}

		std::optional<NetMessage> Decode(std::span<const uint8_t> bytes, const ReplicationSchema& schema, std::string& outError)
		{
			outError.clear();
			NetReader reader(bytes);
			uint8_t typeByte = 0;
			if (!reader.ReadU8(typeByte))
			{
				outError = "the message is empty";
				return std::nullopt;
			}
			NetMessage message;
			message.Type = static_cast<NetMessageType>(typeByte);
			switch (message.Type)
			{
				case NetMessageType::Hello:
				{
					NetHello hello;
					uint32_t magic = 0;
					if (!reader.ReadU32(magic) || magic != Magic)
					{
						outError = "not a Breccia client";
						return std::nullopt;
					}
					if (!reader.ReadU32(hello.ProtocolVersion))
					{
						outError = "the protocol version is truncated";
						return std::nullopt;
					}
					// Another version's Hello may continue differently: report the version alone.
					if (hello.ProtocolVersion != Version)
						return NetMessage{ message.Type, std::move(hello) };
					if (!reader.ReadU64(hello.SchemaHash) || !ReadText(reader, hello.Game, outError, "the game name") || !ReadText(reader, hello.GameVersion, outError, "the game version"))
					{
						if (outError.empty())
							outError = "the Hello is truncated";
						return std::nullopt;
					}
					message.Body = std::move(hello);
					break;
				}
				case NetMessageType::Welcome:
				{
					NetWelcome welcome;
					if (!reader.ReadCount(welcome.PeerId, std::numeric_limits<uint32_t>::max()) || !reader.ReadFloat(welcome.TickRate))
					{
						outError = "the Welcome is truncated";
						return std::nullopt;
					}
					if (welcome.PeerId == 0 || !std::isfinite(welcome.TickRate) || welcome.TickRate <= 0.0f)
					{
						outError = "the Welcome holds an invalid peer ID or tick rate";
						return std::nullopt;
					}
					message.Body = welcome;
					break;
				}
				case NetMessageType::Snapshot:
				case NetMessageType::ClientUpdate:
				{
					NetState state;
					if (!ReadState(reader, message.Type, schema, state, outError))
					{
						if (outError.empty())
							outError = "the message is truncated";
						return std::nullopt;
					}
					message.Body = std::move(state);
					break;
				}
				default:
					outError = "unknown message type " + std::to_string(typeByte);
					return std::nullopt;
			}
			if (!reader.AtEnd())
			{
				outError = "the message has trailing bytes";
				return std::nullopt;
			}
			return message;
		}

	}

}
