#pragma once

#include "Breccia/Core/Base.h"
#include "Breccia/Network/NetTransport.h"

#include <cstdint>
#include <memory>
#include <string>

namespace Breccia {

	// How the simulated link treats every message, in both directions.
	struct SimulatedLinkSettings
	{
		// One-way delay in seconds.
		float Latency = 0.0f;
		// Up to this much extra delay per unreliable message, drawn uniformly, which reorders them.
		float Jitter = 0.0f;
		// Chance in [0, 1] that a message is lost. A lost unreliable message never arrives; a lost reliable one is sent
		// again a round trip later (behind it, the later reliable messages wait, as over UDP), and at 1 nothing arrives.
		float Loss = 0.0f;
		// A side that hears nothing from the other for this long reports a timed-out disconnect, and a connection attempt
		// nobody answers fails after it.
		float TimeoutSeconds = 10.0f;
	};

	// An in-process network for tests: transports created here talk to each other through a simulated link driven by
	// Advance, never by the wall clock, so a test with a fixed timestep runs identically every time (losses and jitter
	// come from a seeded stream drawn in send order). Hosts listen on virtual ports. The link has no keepalive of its
	// own: idle connections time out, as a session's per-tick messages keep live ones busy. The network and its
	// transports share state, so they may be destroyed in any order; a destroyed transport just goes silent.
	class SimulatedNetwork
	{
	public:
		// The settings must be finite and non-negative, with Loss in [0, 1] and TimeoutSeconds positive.
		explicit SimulatedNetwork(SimulatedLinkSettings settings = {}, uint32_t seed = 1);
		~SimulatedNetwork();

		SimulatedNetwork(const SimulatedNetwork&) = delete;
		SimulatedNetwork& operator=(const SimulatedNetwork&) = delete;

		// A host transport accepting up to maxPeers connections on a virtual port (0 picks a free one). Null, with the
		// reason, when the port is taken.
		Scope<NetTransport> Listen(uint16_t port, uint32_t maxPeers, std::string& outError);
		// A client transport connecting to a virtual port: it reports Connected once a host accepts, or a timed-out
		// Disconnected when none does (no host there, or the host is full).
		Scope<NetTransport> Connect(uint16_t port);

		// Moves simulated time forward and delivers what has arrived by then.
		void Advance(float seconds);
		float GetTime() const;
		// Applies to messages sent from now on.
		void SetLinkSettings(const SimulatedLinkSettings& settings);
		// Messages lost so far, for tests that check the link did what they meant.
		uint64_t GetLostCount() const;

		struct State;

	private:
		std::shared_ptr<State> m_State;
	};

}
