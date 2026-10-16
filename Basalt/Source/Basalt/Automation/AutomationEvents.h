#pragma once

#include "Basalt/Core/Log.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Basalt {

	enum class AutomationEventType : uint8_t
	{
		Log = 0,
		ScriptError,
		PlayState,
		Collision,
		Count
	};

	// Wire names ("log", "scriptError", "playState", "collision"), used in filters and in each event's "event" key.
	const char* AutomationEventTypeName(AutomationEventType type);
	std::optional<AutomationEventType> AutomationEventTypeFromName(std::string_view name);
	// Every type's wire name in enum order (command schemas list them).
	std::span<const char* const> AutomationEventTypeNames();

	// The play-state changes playState events report ("play", "simulate", "pause", "resume", "stop").
	enum class PlayStateChange : uint8_t
	{
		Play = 0,
		Simulate,
		Pause,
		Resume,
		Stop,
		Count
	};
	const char* PlayStateChangeName(PlayStateChange change);

	// How a subscription's events leave the hub.
	enum class AutomationEventDelivery : uint8_t
	{
		// Read with AutomationEventHub::Poll (events.poll).
		Polled = 0,
		// Sent by the owner's connection (TakeStreamed); Poll refuses it, so nobody else steals the events.
		Streamed
	};

	// Fans engine events (log lines, script errors, play-state changes, collisions) out to automation
	// subscriptions, so tools follow a running game without polling log.get. Thread-safe: log lines arrive on any
	// thread, the other events on the thread that updates the scene, and the TCP server thread drains streamed
	// subscriptions. Events are finished JSON when published, so draining never touches the scene. Nothing is
	// captured for a type no subscription wants. Each subscription keeps at most MaxQueuedEvents and counts the
	// ones it had to drop (oldest first).
	class AutomationEventHub
	{
	public:
		static constexpr size_t MaxQueuedEvents = 1024;
		static constexpr size_t MaxSubscriptions = 64;
		static constexpr uint32_t TypeBit(AutomationEventType type) { return 1u << static_cast<uint32_t>(type); }
		static constexpr uint32_t AllTypes = (1u << static_cast<uint32_t>(AutomationEventType::Count)) - 1;

		struct Filter
		{
			// TypeBit per wanted type.
			uint32_t Types = AllTypes;
			// Log events below this level are skipped (other types have no level).
			LogLevel MinLevel = LogLevel::Info;

			bool Accepts(AutomationEventType type, LogLevel level) const
			{
				return (Types & TypeBit(type)) != 0 && (type != AutomationEventType::Log || level >= MinLevel);
			}
		};

		using Delivery = AutomationEventDelivery;

		enum class PollStatus
		{
			Ok,
			UnknownSubscription,
			// The subscription streams to its connection.
			Streamed,
			// The events lack a required type; they stay queued.
			Rejected
		};

		AutomationEventHub();
		~AutomationEventHub();
		AutomationEventHub(const AutomationEventHub&) = delete;
		AutomationEventHub& operator=(const AutomationEventHub&) = delete;

		// The owner is the client whose subscriptions end with it (RemoveOwner); 0 for none, whose subscriptions last
		// until unsubscribed or the hub goes (the CLI's, which end with the process). A streamed subscription needs an
		// owner to drain it. Returns the subscription ID (never 0), or nullopt when MaxSubscriptions are active or a
		// streamed subscription has no owner.
		std::optional<uint64_t> Subscribe(const Filter& filter, uint64_t owner, Delivery delivery);
		bool Unsubscribe(uint64_t id);
		// Ends every subscription of an owner (its connection closed).
		void RemoveOwner(uint64_t owner);
		size_t GetSubscriptionCount() const;

		// Replaces outEvents with up to `max` queued events of a polled subscription, oldest first, and removes them;
		// outDropped is the number lost to the queue limit since the previous successful poll. With requiredTypes
		// (TypeBits), the events stay queued unless they include every required type: Rejected, with outEvents and
		// outDropped filled so the caller can say what was there.
		PollStatus Poll(uint64_t id, size_t max, std::vector<nlohmann::json>& outEvents, uint64_t& outDropped, uint32_t requiredTypes = 0);
		// Every event queued for the owner's streamed subscriptions as newline-terminated JSON frames, each with its
		// "subscription": grouped by subscription in ID order, each oldest first (clients order across subscriptions by
		// "seq"); a dropped count becomes a {"event":"dropped"} frame before them. Empty when nothing is pending.
		std::string TakeStreamed(uint64_t owner);

		// Cheap check publishers make before building an event.
		bool Wants(AutomationEventType type) const { return (m_WantedTypes.load(std::memory_order_relaxed) & TypeBit(type)) != 0; }
		// Adds "event" and "seq" (one counter for the hub, so a subscription sees gaps) to the payload and queues it for
		// every subscription that wants it. Never logs: it runs inside log listeners.
		void Publish(AutomationEventType type, nlohmann::json payload, LogLevel level = LogLevel::Info);

	private:
		struct Subscription
		{
			Filter Wanted;
			uint64_t Owner = 0;
			Delivery Mode = Delivery::Polled;
			std::deque<nlohmann::json> Queue;
			uint64_t Dropped = 0;
		};

		// Call with m_Mutex held.
		void UpdateWantedTypes();

		mutable std::mutex m_Mutex;
		std::map<uint64_t, Subscription> m_Subscriptions;
		uint64_t m_NextId = 1;
		uint64_t m_NextSequence = 1;
		std::atomic<uint32_t> m_WantedTypes = 0;
		uint64_t m_LogListener = 0;
	};

}
