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
		static constexpr float MaxInterpolationDelay = 1.0f;
		static constexpr uint32_t MaxMessagesPerSecondLimit = 10000;

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
		// How far behind its estimate of the host's clock (seconds; Network.GetTime) a client shows the entities it does not
		// decide (the moment it shows is Network.GetViewTime), moving them smoothly between the snapshots around that time; 0
		// shows each snapshot as it arrives. Two or three snapshot intervals hide a lost snapshot. A client takes its own
		// project's value.
		float InterpolationDelay = 0.1f;
		// Script messages and RPCs each connection may send per second, per direction (Lua Network.Send, Network.Rpc): a
		// sender refuses more, and a host handles at most this many of a client's per second, ending the connection of one
		// that keeps sending faster.
		uint32_t MaxMessagesPerSecond = 120;

		bool operator==(const NetworkSettings&) const = default;

		// The active project's settings, or the defaults without one.
		static NetworkSettings FromActiveProject();

		// Reads "host" or "host:port" (IPv4 addresses and names only, as ENet has no IPv6, so a host holds no ':'), with
		// defaultPort when it names none. An empty string, or what is wrong: an empty host, one holding ':', or a port that
		// is not an integer in [1, 65535].
		static std::string ParseAddress(const std::string& text, uint16_t defaultPort, std::string& outHost, uint16_t& outPort);

		// What is out of range, naming the field ("Network.TickRate must be ..."), or an empty string. FromJson checks it;
		// settings built in code must pass it too.
		std::string Validate() const;

		nlohmann::json ToJson() const;
		// Strict: unknown keys, wrong types and values outside the ranges above fail, naming the field. Keys left out keep
		// their defaults.
		static std::optional<NetworkSettings> FromJson(const nlohmann::json& data, std::string& outError);
	};

}
