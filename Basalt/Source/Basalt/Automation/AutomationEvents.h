#pragma once

#include "Basalt/Core/Log.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
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
	// Lowercase log level names ("trace" ... "critical"), as log.get reports them.
	const char* LogLevelName(LogLevel level);
	std::optional<LogLevel> LogLevelFromName(std::string_view name);

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
		static constexpr uint32_t AllTypes = (1u << static_cast<uint32_t>(AutomationEventType::Count)) - 1;

		struct Filter
		{
			// Bit (1 << AutomationEventType) per wanted type.
			uint32_t Types = AllTypes;
			// Log events below this level are skipped (other types have no level).
			LogLevel MinLevel = LogLevel::Info;
		};

		enum class PollStatus
		{
			Ok,
			UnknownSubscription,
			// The subscription belongs to a connection that streams it; polling would steal its events.
			Streamed
		};

		AutomationEventHub();
		~AutomationEventHub();
		AutomationEventHub(const AutomationEventHub&) = delete;
		AutomationEventHub& operator=(const AutomationEventHub&) = delete;

		// owner 0: drained with Poll. Any other owner: a connection that drains it with TakeStreamed. Returns the
		// subscription ID (never 0), or nullopt when MaxSubscriptions are active.
		std::optional<uint64_t> Subscribe(const Filter& filter, uint64_t owner);
		bool Unsubscribe(uint64_t id);
		// Ends every subscription of an owner (its connection closed).
		void RemoveOwner(uint64_t owner);
		size_t GetSubscriptionCount() const;

		// Moves up to `max` queued events out of a polled subscription, oldest first; outDropped is the number
		// lost to the queue limit since the previous poll.
		PollStatus Poll(uint64_t id, size_t max, std::vector<nlohmann::json>& outEvents, uint64_t& outDropped);
		// Every event queued for the owner's subscriptions as newline-terminated JSON frames, each with its
		// "subscription"; a dropped count becomes a {"event":"dropped"} frame. Empty when nothing is pending.
		std::string TakeStreamed(uint64_t owner);

		// Cheap check publishers make before building an event.
		bool Wants(AutomationEventType type) const { return (m_WantedTypes.load(std::memory_order_relaxed) & Bit(type)) != 0; }
		// Adds "event" and "seq" to the payload and queues it for every subscription that wants it.
		void Publish(AutomationEventType type, nlohmann::json payload, LogLevel level = LogLevel::Info);

	private:
		struct Subscription
		{
			Filter Wanted;
			uint64_t Owner = 0;
			std::deque<nlohmann::json> Queue;
			uint64_t Dropped = 0;
		};

		static uint32_t Bit(AutomationEventType type) { return 1u << static_cast<uint32_t>(type); }
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
