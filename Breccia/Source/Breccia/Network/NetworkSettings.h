#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace Breccia {

	// A project's network session settings (ProjectConfig::Network, the project file's "Network" object).
	struct NetworkSettings
	{
		static constexpr uint16_t DefaultPort = 7777;
		static constexpr uint32_t MaxClientLimit = 1024;
		static constexpr float MinTickRate = 1.0f;
		static constexpr float MaxTickRate = 240.0f;
		static constexpr uint32_t MinBytesPerSecond = 1024;
		static constexpr uint32_t MaxBytesPerSecondLimit = 100u * 1024u * 1024u;
		static constexpr float MinTimeout = 1.0f;
		static constexpr float MaxTimeout = 600.0f;

		// The port a client connects to when --connect names none. A host listens on the port --host gives.
		uint16_t Port = DefaultPort;
		// Clients a host admits at once; the next is refused (ServerFull), while the transport's spare slots last
		// (NetworkSystem::SpareTransportSlots; beyond them a connection attempt goes unanswered).
		uint32_t MaxClients = 8;
		// Snapshots per second the host sends to each client, and updates per second clients send back; at most one per
		// frame, so a lower frame rate lowers it.
		float TickRate = 30.0f;
		// Each connection's bandwidth budget, per direction: what does not fit waits for a later tick.
		uint32_t MaxBytesPerSecond = 64u * 1024u;
		// A connection that hears nothing for this long ends (Timeout); a handshake not finished by then ends with
		// HandshakeTimeout.
		float TimeoutSeconds = 10.0f;

		bool operator==(const NetworkSettings&) const = default;

		// What is out of range, naming the field ("Network.TickRate must be ..."), or an empty string. FromJson checks it;
		// settings built in code must pass it too.
		std::string Validate() const;

		nlohmann::json ToJson() const;
		// Strict: unknown keys, wrong types and values outside the ranges above fail, naming the field. Keys left out keep
		// their defaults.
		static std::optional<NetworkSettings> FromJson(const nlohmann::json& data, std::string& outError);
	};

}
