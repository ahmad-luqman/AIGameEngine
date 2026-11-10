#include "Breccia/Network/NetProtocol.h"

#include "Breccia/Core/TextEncoding.h"
#include "Breccia/Network/NetValue.h"
#include "Breccia/Network/ReplicationSchema.h"
#include "Breccia/Scene/ComponentRegistry.h"

#include <glm/glm.hpp>

#include <cmath>
#include <limits>
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
				return "the handshake did not finish in time";
			case NetDisconnectReason::InvalidPacket:
				return "a peer sent an invalid message";
			case NetDisconnectReason::ConnectionFailed:
				return "the connection could not be made";
			case NetDisconnectReason::Unknown:
			case NetDisconnectReason::Count:
				break;
		}
		return "an unknown reason (a newer peer's)";
	}

	const char* NetDisconnectReasonName(NetDisconnectReason reason)
	{
		switch (reason)
		{
			case NetDisconnectReason::None:
				return "None";
			case NetDisconnectReason::ClientQuit:
				return "ClientQuit";
			case NetDisconnectReason::ServerShutdown:
				return "ServerShutdown";
			case NetDisconnectReason::ProtocolMismatch:
				return "ProtocolMismatch";
			case NetDisconnectReason::SchemaMismatch:
				return "SchemaMismatch";
			case NetDisconnectReason::GameMismatch:
				return "GameMismatch";
			case NetDisconnectReason::ServerFull:
				return "ServerFull";
			case NetDisconnectReason::Timeout:
				return "Timeout";
			case NetDisconnectReason::HandshakeTimeout:
				return "HandshakeTimeout";
			case NetDisconnectReason::InvalidPacket:
				return "InvalidPacket";
			case NetDisconnectReason::ConnectionFailed:
				return "ConnectionFailed";
			case NetDisconnectReason::Unknown:
			case NetDisconnectReason::Count:
				break;
		}
		return "Unknown";
	}

	NetDisconnectReason DisconnectReasonFromCode(uint32_t code)
	{
		return code < static_cast<uint32_t>(NetDisconnectReason::Unknown) ? static_cast<NetDisconnectReason>(code) : NetDisconnectReason::Unknown;
	}

	namespace NetProtocol {

		namespace {

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
				// Built only for a message: decoding is the hot path for untrusted input.
				const auto name = [&] {
					const ReplicationSchema::Component* info = schema.FindComponent(component.Component);
					return info ? info->Info->Name : "component " + std::to_string(component.Component);
				};
				// Added is the last kind: a new one must move this check.
				static_assert(static_cast<uint8_t>(NetComponentChange::Added) == 2, "a new NetComponentChange must be decoded");
				uint8_t change = 0;
				if (!reader.ReadU8(change) || change > static_cast<uint8_t>(NetComponentChange::Added))
					return Fail(reader, outError, "a " + name() + " entry is truncated or has an invalid change");
				component.Change = static_cast<NetComponentChange>(change);
				uint32_t fieldCount = 0;
				if (!reader.ReadCount(fieldCount, MaxFieldsPerComponent))
					return Fail(reader, outError, "a " + name() + " field count is malformed or too large");
				component.Fields.resize(fieldCount);
				for (NetFieldValue& field : component.Fields)
				{
					if (!reader.ReadCount(field.Field, std::numeric_limits<uint32_t>::max()))
						return Fail(reader, outError, "a " + name() + " field index is malformed");
					if (!NetValue::Decode(reader, field.Value))
						return Fail(reader, outError, "the value of " + name() + " field " + std::to_string(field.Field) + " is malformed (truncated, too deep, not UTF-8 or with a repeated key)");
				}
				return true;
			}

			bool ReadParent(NetReader& reader, uint64_t& parent, std::string& outError)
			{
				return reader.ReadU64(parent) || Fail(reader, outError, "a parent ID is truncated");
			}

			bool ReadUpdate(NetReader& reader, NetMessageType type, const ReplicationSchema& schema, NetEntityUpdate& update, std::string& outError)
			{
				if (!ReadUuid(reader, update.Entity, outError))
					return false;
				uint8_t flags = 0;
				if (!reader.ReadU8(flags) || (flags & ~EntityHasParent) != 0)
					return Fail(reader, outError, "an entity update has invalid flags");
				if (flags & EntityHasParent)
				{
					uint64_t parent = 0;
					if (!ReadParent(reader, parent, outError))
						return false;
					update.Parent = UUID(parent);
				}
				uint32_t componentCount = 0;
				if (!reader.ReadCount(componentCount, MaxComponentsPerEntity))
					return Fail(reader, outError, "a component count is malformed or too large");
				update.Components.resize(componentCount);
				for (NetComponentUpdate& component : update.Components)
					if (!ReadComponent(reader, schema, component, outError))
						return false;
				if (std::string problem = CheckUpdate(update, schema, type); !problem.empty())
					return Fail(reader, outError, std::move(problem));
				return true;
			}

			bool ReadSpawn(NetReader& reader, NetEntitySpawn& spawn, std::string& outError)
			{
				if (!ReadUuid(reader, spawn.Entity, outError))
					return false;
				uint64_t parent = 0;
				if (!ReadParent(reader, parent, outError))
					return false;
				spawn.Parent = parent;
				if (!NetValue::Decode(reader, spawn.Entities))
					return Fail(reader, outError, "a spawn's entities are malformed (truncated, too deep, not UTF-8 or with a repeated key)");
				if (std::string problem = CheckSpawn(spawn); !problem.empty())
					return Fail(reader, outError, std::move(problem));
				return true;
			}

			bool ReadVec3(NetReader& reader, glm::vec3& value)
			{
				return reader.ReadFloat(value.x) && reader.ReadFloat(value.y) && reader.ReadFloat(value.z) && std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
			}

			void WriteVec3(NetWriter& writer, const glm::vec3& value)
			{
				writer.WriteFloat(value.x);
				writer.WriteFloat(value.y);
				writer.WriteFloat(value.z);
			}

			bool ReadPrediction(NetReader& reader, NetPrediction& prediction, std::string& outError)
			{
				if (!ReadUuid(reader, prediction.Entity, outError))
					return false;
				uint8_t jumped = 0;
				CharacterState& state = prediction.State;
				if (!reader.ReadU32(prediction.Input) || !ReadVec3(reader, state.Position) || !ReadVec3(reader, state.Velocity) || !reader.ReadFloat(state.SinceGrounded) || !reader.ReadU8(jumped))
					return Fail(reader, outError, "a predicted character's state is truncated or not finite");
				if (!(state.SinceGrounded >= 0.0f && state.SinceGrounded <= CharacterState::MaxSinceGrounded) || jumped > 1)
					return Fail(reader, outError, "a predicted character's time in the air or jump flag is out of range");
				state.Jumped = jumped != 0;
				return true;
			}

			bool ReadInputs(NetReader& reader, NetInputs& inputs, std::string& outError)
			{
				if (!ReadUuid(reader, inputs.Entity, outError))
					return false;
				uint32_t count = 0;
				if (!reader.ReadU32(inputs.First) || !reader.ReadCount(count, MaxInputsPerCharacter))
					return Fail(reader, outError, "a character's inputs are truncated or too many");
				// Wrapping past UINT32_MAX would reorder them; at 240 steps per second that takes 207 days.
				if (inputs.First == 0 || count == 0 || inputs.First > std::numeric_limits<uint32_t>::max() - (count - 1))
					return Fail(reader, outError, "a character's inputs are numbered from 0, empty, or past the last sequence number");
				inputs.Steps.resize(count);
				for (CharacterInput& step : inputs.Steps)
				{
					glm::quat& rotation = step.Rotation;
					if (!ReadVec3(reader, step.Velocity) || !reader.ReadFloat(rotation.w) || !reader.ReadFloat(rotation.x) || !reader.ReadFloat(rotation.y) || !reader.ReadFloat(rotation.z))
						return Fail(reader, outError, "a character input is truncated or not finite");
					const float length = glm::length(rotation);
					// A sender normalizes its rotations: far from unit length is no rotation (and NaN fails here too).
					if (!(length > 0.5f && length < 2.0f))
						return Fail(reader, outError, "a character input's rotation is not a unit quaternion");
					if (!(glm::length(step.Velocity) <= MaxInputSpeed))
						return Fail(reader, outError, "a character input is faster than " + std::to_string(static_cast<int>(MaxInputSpeed)) + " m/s");
				}
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
					if (!ReadUpdate(reader, type, schema, update, outError) || !unique(update.Entity))
						return false;
				if (!reader.ReadCount(count, MaxRecords))
					return Fail(reader, outError, "the despawn count is malformed or too large");
				if (type == NetMessageType::ClientUpdate && count != 0)
					return Fail(reader, outError, "clients cannot despawn entities");
				state.Despawns.resize(count);
				for (UUID& uuid : state.Despawns)
					if (!ReadUuid(reader, uuid, outError) || !unique(uuid))
						return false;

				NetStateExtras& extras = state.Extras;
				if (type == NetMessageType::Snapshot)
				{
					if (!reader.ReadDouble(extras.Time) || !std::isfinite(extras.Time) || extras.Time < 0.0)
						return Fail(reader, outError, "the snapshot's time is truncated, negative or not finite");
					if (!reader.ReadCount(count, MaxPredictedCharacters))
						return Fail(reader, outError, "the predicted character count is malformed or too large");
					extras.Predictions.resize(count);
					std::unordered_set<UUID> predicted;
					for (NetPrediction& prediction : extras.Predictions)
					{
						if (!ReadPrediction(reader, prediction, outError))
							return false;
						if (!predicted.insert(prediction.Entity).second)
							return Fail(reader, outError, "a character is predicted twice in one message");
					}
				}
				else
				{
					if (!reader.ReadCount(count, MaxPredictedCharacters))
						return Fail(reader, outError, "the input count is malformed or too large");
					extras.Inputs.resize(count);
					std::unordered_set<UUID> characters;
					for (NetInputs& inputs : extras.Inputs)
					{
						if (!ReadInputs(reader, inputs, outError))
							return false;
						if (!characters.insert(inputs.Entity).second)
							return Fail(reader, outError, "a character's inputs appear twice in one message");
					}
				}
				return true;
			}

			bool ReadScript(NetReader& reader, NetScriptMessage& message, std::string& outError)
			{
				uint8_t kind = 0;
				if (!reader.ReadU8(kind) || kind > static_cast<uint8_t>(NetScriptKind::Rpc))
					return Fail(reader, outError, "a script message has an invalid kind");
				message.Kind = static_cast<NetScriptKind>(kind);
				uint64_t entity = 0;
				if (!reader.ReadU64(entity))
					return Fail(reader, outError, "a script message's entity is truncated");
				message.Entity = entity;
				if (!reader.ReadString(message.Name, MaxScriptNameLength))
					return Fail(reader, outError, "a script message's name is truncated or too long");
				if (!NetValue::Decode(reader, message.Arguments))
					return Fail(reader, outError, "a script message's arguments are malformed (truncated, too deep, not UTF-8 or with a repeated key)");
				if (std::string problem = CheckScript(message); !problem.empty())
					return Fail(reader, outError, std::move(problem));
				return true;
			}

		}

		std::string CheckUpdate(const NetEntityUpdate& update, const ReplicationSchema& schema, NetMessageType type)
		{
			if (!update.Entity.IsValid())
				return "an entity ID is 0";
			const bool fromClient = type == NetMessageType::ClientUpdate;
			if (fromClient && update.Parent)
				return "clients cannot change an entity's parent";
			if (update.Parent && *update.Parent == update.Entity)
				return "an entity cannot be its own parent";
			if (update.Components.empty() && !update.Parent)
				return "an entity update carries nothing";
			if (update.Components.size() > MaxComponentsPerEntity)
				return "an entity update has too many components";
			std::unordered_set<uint32_t> components;
			for (const NetComponentUpdate& component : update.Components)
			{
				const ReplicationSchema::Component* info = schema.FindComponent(component.Component);
				if (!info)
					return "unknown component index " + std::to_string(component.Component);
				const std::string& name = info->Info->Name;
				const size_t replicated = info->Info->ReplicatedFields.size();
				if (!components.insert(component.Component).second)
					return name + " appears twice in one entity update";
				// Senders track only components with replicated fields: nothing else is added or removed after a spawn.
				if (replicated == 0)
					return name + " has no replicated fields";
				if (component.Change != NetComponentChange::Changed && info->Info->IsCore)
					return name + " cannot be added or removed";
				if (fromClient && component.Change != NetComponentChange::Changed)
					return "clients cannot add or remove components";
				if (fromClient && name == "Networked")
					return "clients cannot change who owns an entity";
				if (component.Change == NetComponentChange::Removed && !component.Fields.empty())
					return "a removed " + name + " carries fields";
				if (component.Change == NetComponentChange::Added && component.Fields.size() != replicated)
					return "an added " + name + " must carry every replicated field";
				if (component.Change == NetComponentChange::Changed && component.Fields.empty())
					return "a changed " + name + " carries no field";
				std::vector<bool> seen(replicated, false);
				for (const NetFieldValue& field : component.Fields)
				{
					if (field.Field >= replicated)
						return name + " has no replicated field " + std::to_string(field.Field);
					if (seen[field.Field])
						return name + " field " + std::to_string(field.Field) + " appears twice";
					seen[field.Field] = true;
				}
			}
			return {};
		}

		std::string CheckSpawn(const NetEntitySpawn& spawn)
		{
			if (!spawn.Entity.IsValid())
				return "an entity ID is 0";
			if (spawn.Parent == spawn.Entity)
				return "an entity cannot be its own parent";
			// The records themselves are checked where they are created (SceneSerializer), as a scene file's are.
			if (!spawn.Entities.is_array() || spawn.Entities.empty() || spawn.Entities.size() > MaxSpawnEntities)
				return "a spawn must hold 1 to " + std::to_string(MaxSpawnEntities) + " entity records";
			return {};
		}

		std::string CheckScript(const NetScriptMessage& message)
		{
			if (message.Name.empty() || message.Name.size() > MaxScriptNameLength)
				return "a script message's name must be 1 to " + std::to_string(MaxScriptNameLength) + " bytes";
			if (!TextEncoding::IsValidUtf8(message.Name))
				return "a script message's name is not UTF-8";
			if (!message.Arguments.is_array() || message.Arguments.size() > MaxScriptArguments)
				return "a script message carries at most " + std::to_string(MaxScriptArguments) + " arguments, as an array";
			if (message.Kind == NetScriptKind::Rpc && !message.Entity.IsValid())
				return "an RPC names no entity";
			if (message.Kind == NetScriptKind::Message && message.Entity.IsValid())
				return "a message names an entity";
			return {};
		}

		std::vector<uint8_t> EncodeScript(const NetScriptMessage& message)
		{
			NetWriter writer;
			writer.WriteU8(static_cast<uint8_t>(NetMessageType::Script));
			writer.WriteU8(static_cast<uint8_t>(message.Kind));
			writer.WriteU64(message.Entity);
			writer.WriteString(message.Name);
			if (!NetValue::Encode(message.Arguments, writer) || writer.GetBytes().size() > MaxScriptMessageBytes)
				return {};
			return writer.TakeBytes();
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
				writer.WriteU8(static_cast<uint8_t>(component.Change));
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

		std::vector<uint8_t> AssembleState(NetMessageType type, uint32_t sequence, uint32_t acknowledged, std::span<const std::vector<uint8_t>> spawns, std::span<const std::vector<uint8_t>> updates, std::span<const UUID> despawns, const NetStateExtras& extras)
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
			if (type == NetMessageType::Snapshot)
			{
				writer.WriteDouble(extras.Time);
				writer.WriteVarUInt(extras.Predictions.size());
				for (const NetPrediction& prediction : extras.Predictions)
				{
					writer.WriteU64(prediction.Entity);
					writer.WriteU32(prediction.Input);
					WriteVec3(writer, prediction.State.Position);
					WriteVec3(writer, prediction.State.Velocity);
					writer.WriteFloat(prediction.State.SinceGrounded);
					writer.WriteU8(prediction.State.Jumped ? 1 : 0);
				}
			}
			else
			{
				writer.WriteVarUInt(extras.Inputs.size());
				for (const NetInputs& inputs : extras.Inputs)
				{
					writer.WriteU64(inputs.Entity);
					writer.WriteU32(inputs.First);
					writer.WriteVarUInt(inputs.Steps.size());
					for (const CharacterInput& step : inputs.Steps)
					{
						WriteVec3(writer, step.Velocity);
						writer.WriteFloat(step.Rotation.w);
						writer.WriteFloat(step.Rotation.x);
						writer.WriteFloat(step.Rotation.y);
						writer.WriteFloat(step.Rotation.z);
					}
				}
			}
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
			return AssembleState(type, state.Sequence, state.Acknowledged, spawns, updates, state.Despawns, state.Extras);
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
					// Every failure in ReadState says why.
					if (!ReadState(reader, message.Type, schema, state, outError))
						return std::nullopt;
					message.Body = std::move(state);
					break;
				}
				case NetMessageType::Script:
				{
					if (bytes.size() > MaxScriptMessageBytes)
					{
						outError = "a script message is larger than " + std::to_string(MaxScriptMessageBytes) + " bytes";
						return std::nullopt;
					}
					NetScriptMessage script;
					if (!ReadScript(reader, script, outError))
						return std::nullopt;
					message.Body = std::move(script);
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
