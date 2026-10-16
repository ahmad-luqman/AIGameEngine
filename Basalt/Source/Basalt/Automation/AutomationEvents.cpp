#include "Basalt/Automation/AutomationEvents.h"

#include <algorithm>
#include <array>

namespace Basalt {

	namespace {

		constexpr std::array<const char*, static_cast<size_t>(AutomationEventType::Count)> s_TypeNames = { "log", "scriptError", "playState", "collision" };
		constexpr std::array<const char*, 5> s_LevelNames = { "trace", "info", "warn", "error", "critical" };

	}

	const char* AutomationEventTypeName(AutomationEventType type)
	{
		return s_TypeNames[static_cast<size_t>(type)];
	}

	std::optional<AutomationEventType> AutomationEventTypeFromName(std::string_view name)
	{
		for (size_t i = 0; i < s_TypeNames.size(); i++)
		{
			if (name == s_TypeNames[i])
				return static_cast<AutomationEventType>(i);
		}
		return std::nullopt;
	}

	const char* LogLevelName(LogLevel level)
	{
		return s_LevelNames[static_cast<size_t>(level)];
	}

	std::optional<LogLevel> LogLevelFromName(std::string_view name)
	{
		for (size_t i = 0; i < s_LevelNames.size(); i++)
		{
			if (name == s_LevelNames[i])
				return static_cast<LogLevel>(i);
		}
		return std::nullopt;
	}

	AutomationEventHub::AutomationEventHub()
	{
		// Registered for the hub's lifetime and filtered by the wanted types, so subscribing never races with
		// adding or removing the listener. Runs on the logging thread; Publish does not log.
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

	std::optional<uint64_t> AutomationEventHub::Subscribe(const Filter& filter, uint64_t owner)
	{
		std::scoped_lock lock(m_Mutex);
		if (m_Subscriptions.size() >= MaxSubscriptions)
			return std::nullopt;
		const uint64_t id = m_NextId++;
		m_Subscriptions[id] = Subscription{ filter, owner, {}, 0 };
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

	AutomationEventHub::PollStatus AutomationEventHub::Poll(uint64_t id, size_t max, std::vector<nlohmann::json>& outEvents, uint64_t& outDropped)
	{
		std::scoped_lock lock(m_Mutex);
		auto it = m_Subscriptions.find(id);
		if (it == m_Subscriptions.end())
			return PollStatus::UnknownSubscription;
		Subscription& subscription = it->second;
		if (subscription.Owner != 0)
			return PollStatus::Streamed;
		const size_t count = std::min(max, subscription.Queue.size());
		outEvents.reserve(outEvents.size() + count);
		for (size_t i = 0; i < count; i++)
		{
			outEvents.push_back(std::move(subscription.Queue.front()));
			subscription.Queue.pop_front();
		}
		outDropped = subscription.Dropped;
		subscription.Dropped = 0;
		return PollStatus::Ok;
	}

	std::string AutomationEventHub::TakeStreamed(uint64_t owner)
	{
		if (owner == 0)
			return {};
		std::string frames;
		std::scoped_lock lock(m_Mutex);
		for (auto& [id, subscription] : m_Subscriptions)
		{
			if (subscription.Owner != owner)
				continue;
			if (subscription.Dropped > 0)
			{
				const nlohmann::json dropped = { { "event", "dropped" }, { "subscription", id }, { "count", subscription.Dropped } };
				frames += dropped.dump() + "\n";
				subscription.Dropped = 0;
			}
			for (nlohmann::json& event : subscription.Queue)
			{
				event["subscription"] = id;
				// Text from scripts or files may not be valid UTF-8; the frame must still be valid JSON.
				frames += event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
			}
			subscription.Queue.clear();
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
			if ((subscription.Wanted.Types & Bit(type)) == 0)
				continue;
			if (type == AutomationEventType::Log && level < subscription.Wanted.MinLevel)
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
