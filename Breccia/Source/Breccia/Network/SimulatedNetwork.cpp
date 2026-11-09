#include "Breccia/Network/SimulatedNetwork.h"

#include "Breccia/Core/Assert.h"
#include "Breccia/Math/Random.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

namespace Breccia {

	namespace {

		// Shortest round trip a lost reliable message waits before it is sent again, so a link without latency still
		// spaces retries.
		constexpr float MinRetryInterval = 0.05f;
		// Attempts at a reliable message before it is given up (a timeout far longer than any test's would otherwise loop
		// for as long as float precision lets the delay grow).
		constexpr int MaxReliableAttempts = 1000;

		enum class FlightKind : uint8_t
		{
			ConnectRequest,
			ConnectAccept,
			Data,
			Disconnect
		};

		// A message on its way: delivered to (Endpoint, Handle) at DeliverAt, ties broken by send order.
		struct Flight
		{
			float DeliverAt = 0.0f;
			uint64_t Order = 0;
			uint32_t Endpoint = 0;
			NetPeerHandle Handle = 0;
			FlightKind Kind = FlightKind::Data;
			NetChannel Channel = NetChannel::Reliable;
			uint32_t Code = 0;
			std::vector<uint8_t> Data;
			// ConnectRequest: the client's endpoint and handle, which the accept goes back to.
			uint32_t FromEndpoint = 0;
			NetPeerHandle FromHandle = 0;
		};

	}

	// One side of a connection: where its messages go and what it has heard.
	struct SimulatedConnection
	{
		uint32_t RemoteEndpoint = 0;
		// 0 until a client's connection is accepted.
		NetPeerHandle RemoteHandle = 0;
		bool Connected = false;
		// When this side last heard from the other (or started connecting).
		float LastHeard = 0.0f;
		// Reliable messages from this side arrive no earlier than the previous one, keeping their order.
		float LastReliableArrival = 0.0f;
	};

	struct SimulatedEndpoint
	{
		uint16_t ListenPort = 0;
		uint32_t MaxPeers = 0;
		NetPeerHandle NextHandle = 1;
		std::map<NetPeerHandle, SimulatedConnection> Connections;
		// Attempts this side gave up on before they connected, with the code: a host that accepts one hears of it, as over
		// ENet.
		std::map<NetPeerHandle, std::pair<SimulatedConnection, uint32_t>> Abandoned;
		std::vector<NetTransportEvent> Events;
	};

	struct SimulatedNetwork::State
	{
		SimulatedLinkSettings Settings;
		Random Stream;
		float Time = 0.0f;
		uint64_t NextOrder = 0;
		uint32_t NextEndpoint = 1;
		std::unordered_map<uint32_t, SimulatedEndpoint> Endpoints;
		std::map<uint16_t, uint32_t> Ports;
		std::vector<Flight> Flights;
		uint64_t Lost = 0;

		explicit State(SimulatedLinkSettings settings, uint32_t seed)
			: Settings(settings)
			, Stream(seed)
		{
		}

		bool Lose()
		{
			if (Settings.Loss <= 0.0f)
				return false;
			return Settings.Loss >= 1.0f || Stream.Float() < Settings.Loss;
		}

		// When a message sent now arrives, or a negative time when it is lost for good.
		float Arrival(SimulatedConnection& from, bool reliable)
		{
			const float latency = std::max(Settings.Latency, 0.0f);
			if (!reliable)
			{
				if (Lose())
				{
					Lost++;
					return -1.0f;
				}
				return Time + latency + (Settings.Jitter > 0.0f ? Stream.Float() * Settings.Jitter : 0.0f);
			}
			// Each lost attempt costs a round trip; give up once the wait outlasts the timeout, which then ends the
			// connection anyway.
			const float retry = std::max(2.0f * latency, MinRetryInterval);
			float delay = latency;
			for (int attempt = 1; Lose(); attempt++)
			{
				Lost++;
				delay += retry;
				if (delay > Settings.TimeoutSeconds || attempt >= MaxReliableAttempts)
					return -1.0f;
			}
			const float arrival = std::max(Time + delay, from.LastReliableArrival);
			from.LastReliableArrival = arrival;
			return arrival;
		}

		void Queue(Flight flight)
		{
			flight.Order = NextOrder++;
			Flights.push_back(std::move(flight));
		}

		SimulatedEndpoint* FindEndpoint(uint32_t id)
		{
			auto it = Endpoints.find(id);
			return it != Endpoints.end() ? &it->second : nullptr;
		}

		SimulatedConnection* FindConnection(uint32_t endpoint, NetPeerHandle handle)
		{
			SimulatedEndpoint* owner = FindEndpoint(endpoint);
			if (!owner)
				return nullptr;
			auto it = owner->Connections.find(handle);
			return it != owner->Connections.end() ? &it->second : nullptr;
		}

		void Deliver(Flight& flight)
		{
			SimulatedEndpoint* endpoint = FindEndpoint(flight.Endpoint);
			if (!endpoint)
				return;
			if (flight.Kind == FlightKind::ConnectRequest)
			{
				// A full host ignores the attempt, as ENet does: the client times out.
				if (endpoint->Connections.size() >= endpoint->MaxPeers)
					return;
				const NetPeerHandle handle = endpoint->NextHandle++;
				SimulatedConnection& server = endpoint->Connections[handle];
				server = { flight.FromEndpoint, flight.FromHandle, true, Time, 0.0f };
				endpoint->Events.push_back({ NetTransportEvent::Kind::Connected, handle });
				// The accept travels like a reliable message.
				if (const float arrival = Arrival(server, true); arrival >= 0.0f)
					Queue({ .DeliverAt = arrival, .Endpoint = flight.FromEndpoint, .Handle = flight.FromHandle, .Kind = FlightKind::ConnectAccept, .Channel = NetChannel::Reliable, .FromEndpoint = flight.Endpoint, .FromHandle = handle });
				return;
			}
			auto it = endpoint->Connections.find(flight.Handle);
			if (it == endpoint->Connections.end())
			{
				// Accepted after this side gave up: tell the host now.
				auto abandoned = endpoint->Abandoned.find(flight.Handle);
				if (abandoned != endpoint->Abandoned.end() && flight.Kind == FlightKind::ConnectAccept)
				{
					auto [connection, code] = abandoned->second;
					endpoint->Abandoned.erase(abandoned);
					if (const float arrival = Arrival(connection, true); arrival >= 0.0f)
						Queue({ .DeliverAt = arrival, .Endpoint = flight.FromEndpoint, .Handle = flight.FromHandle, .Kind = FlightKind::Disconnect, .Channel = NetChannel::Reliable, .Code = code });
				}
				return;
			}
			SimulatedConnection& connection = it->second;
			connection.LastHeard = Time;
			switch (flight.Kind)
			{
				case FlightKind::ConnectAccept:
					if (!connection.Connected)
					{
						connection.Connected = true;
						connection.RemoteHandle = flight.FromHandle;
						endpoint->Events.push_back({ NetTransportEvent::Kind::Connected, flight.Handle });
					}
					break;
				case FlightKind::Data:
					if (connection.Connected)
						endpoint->Events.push_back({ NetTransportEvent::Kind::Received, flight.Handle, 0, false, flight.Channel, std::move(flight.Data) });
					break;
				case FlightKind::Disconnect:
					endpoint->Events.push_back({ NetTransportEvent::Kind::Disconnected, flight.Handle, flight.Code, false });
					endpoint->Connections.erase(it);
					break;
				case FlightKind::ConnectRequest:
					break;
			}
		}

		void CheckTimeouts()
		{
			for (auto& [id, endpoint] : Endpoints)
			{
				for (auto it = endpoint.Connections.begin(); it != endpoint.Connections.end();)
				{
					if (Time - it->second.LastHeard >= Settings.TimeoutSeconds)
					{
						endpoint.Events.push_back({ NetTransportEvent::Kind::Disconnected, it->first, 0, true });
						it = endpoint.Connections.erase(it);
					}
					else
						++it;
				}
				// An attempt nobody accepted in time is forgotten silently: this side already gave up on it.
				std::erase_if(endpoint.Abandoned, [this](const auto& entry) { return Time - entry.second.first.LastHeard >= Settings.TimeoutSeconds; });
			}
		}

		void Advance(float seconds)
		{
			if (std::isfinite(seconds) && seconds > 0.0f)
				Time += seconds;
			// Arrival order, then send order: one sort keeps delivery independent of the vector's history.
			std::ranges::sort(Flights, [](const Flight& a, const Flight& b) { return a.DeliverAt != b.DeliverAt ? a.DeliverAt < b.DeliverAt : a.Order < b.Order; });
			size_t due = 0;
			while (due < Flights.size() && Flights[due].DeliverAt <= Time)
				due++;
			std::vector<Flight> arrived(std::make_move_iterator(Flights.begin()), std::make_move_iterator(Flights.begin() + static_cast<std::ptrdiff_t>(due)));
			Flights.erase(Flights.begin(), Flights.begin() + static_cast<std::ptrdiff_t>(due));
			for (Flight& flight : arrived)
				Deliver(flight);
			CheckTimeouts();
		}
	};

	namespace {

		class SimulatedTransport final : public NetTransport
		{
		public:
			SimulatedTransport(std::shared_ptr<SimulatedNetwork::State> state, uint32_t endpoint)
				: m_State(std::move(state))
				, m_Endpoint(endpoint)
			{
			}

			~SimulatedTransport() override
			{
				if (SimulatedEndpoint* endpoint = m_State->FindEndpoint(m_Endpoint); endpoint && endpoint->ListenPort != 0)
					m_State->Ports.erase(endpoint->ListenPort);
				m_State->Endpoints.erase(m_Endpoint);
			}

			bool Send(NetPeerHandle peer, NetChannel channel, std::span<const uint8_t> data) override
			{
				SimulatedConnection* connection = m_State->FindConnection(m_Endpoint, peer);
				if (!connection || !connection->Connected)
					return false;
				// A lost message was still sent: the link lost it.
				const float arrival = m_State->Arrival(*connection, channel == NetChannel::Reliable);
				if (arrival >= 0.0f)
					m_State->Queue({ .DeliverAt = arrival, .Endpoint = connection->RemoteEndpoint, .Handle = connection->RemoteHandle, .Kind = FlightKind::Data, .Channel = channel, .Data = std::vector<uint8_t>(data.begin(), data.end()) });
				return true;
			}

			void Disconnect(NetPeerHandle peer, uint32_t code) override
			{
				SimulatedEndpoint* endpoint = m_State->FindEndpoint(m_Endpoint);
				if (!endpoint)
					return;
				auto it = endpoint->Connections.find(peer);
				if (it == endpoint->Connections.end())
					return;
				SimulatedConnection connection = it->second;
				endpoint->Connections.erase(it);
				if (!connection.Connected)
				{
					if (connection.RemoteEndpoint != 0)
						endpoint->Abandoned[peer] = { connection, code };
					return;
				}
				if (const float arrival = m_State->Arrival(connection, true); arrival >= 0.0f)
					m_State->Queue({ .DeliverAt = arrival, .Endpoint = connection.RemoteEndpoint, .Handle = connection.RemoteHandle, .Kind = FlightKind::Disconnect, .Channel = NetChannel::Reliable, .Code = code });
			}

			void DisconnectAll(uint32_t code) override
			{
				SimulatedEndpoint* endpoint = m_State->FindEndpoint(m_Endpoint);
				if (!endpoint)
					return;
				// Disconnect removes each connection.
				while (!endpoint->Connections.empty())
					Disconnect(endpoint->Connections.begin()->first, code);
			}

			void Poll(std::vector<NetTransportEvent>& events) override
			{
				SimulatedEndpoint* endpoint = m_State->FindEndpoint(m_Endpoint);
				if (!endpoint)
					return;
				events.insert(events.end(), std::make_move_iterator(endpoint->Events.begin()), std::make_move_iterator(endpoint->Events.end()));
				endpoint->Events.clear();
			}

			void Flush() override {}

			float GetRoundTripTime(NetPeerHandle peer) const override
			{
				return m_State->FindConnection(m_Endpoint, peer) ? 2.0f * std::max(m_State->Settings.Latency, 0.0f) : 0.0f;
			}

			std::string DescribePeer(NetPeerHandle peer) const override
			{
				return "simulated peer " + std::to_string(m_Endpoint) + "/" + std::to_string(peer);
			}

			uint16_t GetListenPort() const override
			{
				const SimulatedEndpoint* endpoint = m_State->FindEndpoint(m_Endpoint);
				return endpoint ? endpoint->ListenPort : 0;
			}

		private:
			std::shared_ptr<SimulatedNetwork::State> m_State;
			uint32_t m_Endpoint = 0;
		};

		[[maybe_unused]] bool IsValidLink(const SimulatedLinkSettings& settings)
		{
			// The comparisons are false for NaN, which they refuse too.
			return settings.Latency >= 0.0f && settings.Jitter >= 0.0f && settings.Loss >= 0.0f && settings.Loss <= 1.0f && settings.TimeoutSeconds > 0.0f && std::isfinite(settings.Latency) && std::isfinite(settings.Jitter) && std::isfinite(settings.TimeoutSeconds);
		}

	}

	SimulatedNetwork::SimulatedNetwork(SimulatedLinkSettings settings, uint32_t seed)
		: m_State(std::make_shared<State>(settings, seed))
	{
		BR_CORE_ASSERT(IsValidLink(settings), "simulated link settings must be finite, non-negative and Loss in [0, 1]");
	}

	SimulatedNetwork::~SimulatedNetwork() = default;

	Scope<NetTransport> SimulatedNetwork::Listen(uint16_t port, uint32_t maxPeers, std::string& outError)
	{
		if (port == 0)
		{
			port = 1;
			while (m_State->Ports.contains(port))
				port++;
		}
		else if (m_State->Ports.contains(port))
		{
			outError = "simulated port " + std::to_string(port) + " is already in use";
			return nullptr;
		}
		const uint32_t id = m_State->NextEndpoint++;
		SimulatedEndpoint& endpoint = m_State->Endpoints[id];
		endpoint.ListenPort = port;
		endpoint.MaxPeers = maxPeers;
		m_State->Ports[port] = id;
		return CreateScope<SimulatedTransport>(m_State, id);
	}

	Scope<NetTransport> SimulatedNetwork::Connect(uint16_t port)
	{
		const uint32_t id = m_State->NextEndpoint++;
		SimulatedEndpoint& endpoint = m_State->Endpoints[id];
		endpoint.MaxPeers = 1;
		const NetPeerHandle handle = endpoint.NextHandle++;
		SimulatedConnection& connection = endpoint.Connections[handle];
		connection.LastHeard = m_State->Time;
		// Nobody listening: the request goes nowhere and the attempt times out.
		if (const auto host = m_State->Ports.find(port); host != m_State->Ports.end())
		{
			connection.RemoteEndpoint = host->second;
			if (const float arrival = m_State->Arrival(connection, true); arrival >= 0.0f)
				m_State->Queue({ .DeliverAt = arrival, .Endpoint = host->second, .Kind = FlightKind::ConnectRequest, .Channel = NetChannel::Reliable, .FromEndpoint = id, .FromHandle = handle });
		}
		return CreateScope<SimulatedTransport>(m_State, id);
	}

	void SimulatedNetwork::Advance(float seconds)
	{
		m_State->Advance(seconds);
	}

	float SimulatedNetwork::GetTime() const
	{
		return m_State->Time;
	}

	void SimulatedNetwork::SetLinkSettings(const SimulatedLinkSettings& settings)
	{
		BR_CORE_ASSERT(IsValidLink(settings), "simulated link settings must be finite, non-negative and Loss in [0, 1]");
		m_State->Settings = settings;
	}

	uint64_t SimulatedNetwork::GetLostCount() const
	{
		return m_State->Lost;
	}

}
