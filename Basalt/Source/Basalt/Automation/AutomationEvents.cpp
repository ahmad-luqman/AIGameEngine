#include "Basalt/Automation/AutomationEvents.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/EnumNames.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace Basalt {

	namespace {

		constexpr const char* const EventTypeNames[] = { "log", "scriptError", "playState", "collision" };
		static_assert(std::size(EventTypeNames) == static_cast<size_t>(AutomationEventType::Count), "one name per event type");
		constexpr const char* const PlayStateNames[] = { "play", "simulate", "pause", "resume", "stop" };
		static_assert(std::size(PlayStateNames) == static_cast<size_t>(PlayStateChange::Count), "one name per play-state change");

	}

	const char* AutomationEventTypeName(AutomationEventType type)
	{
		const char* name = EnumNameOrNull(EventTypeNames, type);
		BS_CORE_ASSERT(name, "not an event type");
		return name ? name : "unknown";
	}

	std::optional<AutomationEventType> AutomationEventTypeFromName(std::string_view name)
	{
		return EnumFromName<AutomationEventType>(EventTypeNames, name);
	}

	std::span<const char* const> AutomationEventTypeNames()
	{
		return EventTypeNames;
	}

	const char* PlayStateChangeName(PlayStateChange change)
	{
		const char* name = EnumNameOrNull(PlayStateNames, change);
		BS_CORE_ASSERT(name, "not a play-state change");
		return name ? name : "unknown";
	}

	AutomationEventHub::AutomationEventHub()
	{
		// Registered for the hub's lifetime and filtered by the wanted types, so subscribing never races with
		// adding or removing the listener. Runs on whichever thread logs; Publish does not log.
		m_LogListener = Log::GetHistory().AddListener([this](const LogMessage& message) {
			if (!Wants(AutomationEventType::Log))
				return;
			Publish(AutomationEventType::Log, { { "level", LogLevelName(message.Level) }, { "logger", message.LoggerName }, { "text", message.Text } }, message.Level);
		});
	}

	AutomationEventHub::~AutomationEventHub()
	{
		Log::GetHistory().RemoveListener(m_LogListener);
	}

	std::optional<uint64_t> AutomationEventHub::Subscribe(const Filter& filter, uint64_t owner, Delivery delivery)
	{
		BS_CORE_ASSERT((filter.Types & AllTypes) != 0, "a subscription must want some event type");
		BS_CORE_ASSERT(delivery == Delivery::Polled || owner != 0, "a streamed subscription needs an owner that drains it");
		if (delivery == Delivery::Streamed && owner == 0)
			return std::nullopt;
		std::scoped_lock lock(m_Mutex);
		if (m_Subscriptions.size() >= MaxSubscriptions)
			return std::nullopt;
		const uint64_t id = m_NextId++;
		Subscription subscription;
		subscription.Wanted = filter;
		subscription.Wanted.Types &= AllTypes;
		subscription.Owner = owner;
		subscription.Mode = delivery;
		m_Subscriptions.emplace(id, std::move(subscription));
		UpdateWantedTypes();
		return id;
	}

	bool AutomationEventHub::Unsubscribe(uint64_t id)
	{
		std::scoped_lock lock(m_Mutex);
		const bool removed = m_Subscriptions.erase(id) > 0;
		UpdateWantedTypes();
		return removed;
	}

	void AutomationEventHub::RemoveOwner(uint64_t owner)
	{
		if (owner == 0)
			return;
		std::scoped_lock lock(m_Mutex);
		std::erase_if(m_Subscriptions, [owner](const auto& entry) { return entry.second.Owner == owner; });
		UpdateWantedTypes();
	}

	size_t AutomationEventHub::GetSubscriptionCount() const
	{
		std::scoped_lock lock(m_Mutex);
		return m_Subscriptions.size();
	}

	AutomationEventHub::PollStatus AutomationEventHub::Poll(uint64_t id, size_t max, std::vector<nlohmann::json>& outEvents, uint64_t& outDropped, const PollCheck& check)
	{
		std::scoped_lock lock(m_Mutex);
		auto it = m_Subscriptions.find(id);
		if (it == m_Subscriptions.end())
			return PollStatus::UnknownSubscription;
		Subscription& subscription = it->second;
		if (subscription.Mode == Delivery::Streamed)
			return PollStatus::Streamed;
		const auto count = static_cast<std::ptrdiff_t>(std::min(max, subscription.Queue.size()));
		const auto end = subscription.Queue.begin() + count;
		outDropped = subscription.Dropped;
		if (check)
		{
			// The check takes contiguous events and the queue is a deque, so it sees the copy the caller gets.
			outEvents.assign(subscription.Queue.begin(), end);
			if (!check(outEvents))
				return PollStatus::Rejected;
		}
		else
		{
			outEvents.assign(std::make_move_iterator(subscription.Queue.begin()), std::make_move_iterator(end));
		}
		subscription.Queue.erase(subscription.Queue.begin(), end);
		subscription.Dropped = 0;
		return PollStatus::Ok;
	}

	std::string AutomationEventHub::TakeStreamed(uint64_t owner)
	{
		if (owner == 0)
			return {};
		struct Pending
		{
			uint64_t Id;
			uint64_t Dropped;
			std::deque<nlohmann::json> Events;
		};
		std::vector<Pending> pending;
		{
			// Only move the queues out under the lock: serializing can take a while, and publishers (any logging
			// thread) wait for the lock.
			std::scoped_lock lock(m_Mutex);
			for (auto& [id, subscription] : m_Subscriptions)
			{
				if (subscription.Owner != owner || subscription.Mode != Delivery::Streamed || (subscription.Queue.empty() && subscription.Dropped == 0))
					continue;
				pending.push_back({ id, subscription.Dropped, std::exchange(subscription.Queue, {}) });
				subscription.Dropped = 0;
			}
		}
		std::string frames;
		for (Pending& batch : pending)
		{
			if (batch.Dropped > 0)
				frames += nlohmann::json({ { "event", "dropped" }, { "subscription", batch.Id }, { "count", batch.Dropped } }).dump() + "\n";
			for (nlohmann::json& event : batch.Events)
			{
				event["subscription"] = batch.Id;
				// Text from scripts or files may not be valid UTF-8; the frame must still be valid JSON.
				frames += event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
			}
		}
		return frames;
	}

	void AutomationEventHub::Publish(AutomationEventType type, nlohmann::json payload, LogLevel level)
	{
		if (!Wants(type))
			return;
		std::scoped_lock lock(m_Mutex);
		payload["event"] = AutomationEventTypeName(type);
		payload["seq"] = m_NextSequence++;
		for (auto& [id, subscription] : m_Subscriptions)
		{
			if (!subscription.Wanted.Accepts(type, level))
				continue;
			if (subscription.Queue.size() >= MaxQueuedEvents)
			{
				subscription.Queue.pop_front();
				subscription.Dropped++;
			}
			subscription.Queue.push_back(payload);
		}
	}

	void AutomationEventHub::UpdateWantedTypes()
	{
		uint32_t wanted = 0;
		for (const auto& [id, subscription] : m_Subscriptions)
			wanted |= subscription.Wanted.Types;
		m_WantedTypes.store(wanted, std::memory_order_relaxed);
	}

}
