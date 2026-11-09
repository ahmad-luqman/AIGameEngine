#include "Breccia/Network/NetworkSettings.h"

#include "Breccia/Core/JsonUtils.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

namespace Breccia {

	namespace {

		bool ReadInteger(const nlohmann::json& data, const char* key, uint64_t min, uint64_t max, uint64_t& value, std::string& outError)
		{
			const auto it = data.find(key);
			if (it == data.end())
				return true;
			// Negative integers are signed values: check before reading as unsigned, which would wrap.
			if (!it->is_number_integer() || (!it->is_number_unsigned() && it->get<int64_t>() < 0) || it->get<uint64_t>() < min || it->get<uint64_t>() > max)
			{
				outError = fmt::format("Network.{} must be an integer in [{}, {}]", key, min, max);
				return false;
			}
			value = it->get<uint64_t>();
			return true;
		}

		bool ReadNumber(const nlohmann::json& data, const char* key, float min, float max, float& value, std::string& outError)
		{
			const auto it = data.find(key);
			if (it == data.end())
				return true;
			float number = 0.0f;
			if (!JsonToFloat(*it, number) || number < min || number > max)
			{
				outError = fmt::format("Network.{} must be a number in [{}, {}]", key, min, max);
				return false;
			}
			value = number;
			return true;
		}

	}

	nlohmann::json NetworkSettings::ToJson() const
	{
		return { { "Port", Port }, { "MaxClients", MaxClients }, { "TickRate", TickRate }, { "MaxBytesPerSecond", MaxBytesPerSecond }, { "TimeoutSeconds", TimeoutSeconds } };
	}

	std::optional<NetworkSettings> NetworkSettings::FromJson(const nlohmann::json& data, std::string& outError)
	{
		if (!data.is_object())
		{
			outError = "Network must be an object";
			return std::nullopt;
		}
		if (!RejectUnknownKeys(data, { "Port", "MaxClients", "TickRate", "MaxBytesPerSecond", "TimeoutSeconds" }, "Network", outError))
			return std::nullopt;
		NetworkSettings settings;
		uint64_t port = settings.Port;
		uint64_t maxClients = settings.MaxClients;
		uint64_t bytes = settings.MaxBytesPerSecond;
		if (!ReadInteger(data, "Port", 1, 65535, port, outError) || !ReadInteger(data, "MaxClients", 1, MaxClientLimit, maxClients, outError) || !ReadInteger(data, "MaxBytesPerSecond", MinBytesPerSecond, MaxBytesPerSecondLimit, bytes, outError) || !ReadNumber(data, "TickRate", MinTickRate, MaxTickRate, settings.TickRate, outError) || !ReadNumber(data, "TimeoutSeconds", MinTimeout, MaxTimeout, settings.TimeoutSeconds, outError))
			return std::nullopt;
		settings.Port = static_cast<uint16_t>(port);
		settings.MaxClients = static_cast<uint32_t>(maxClients);
		settings.MaxBytesPerSecond = static_cast<uint32_t>(bytes);
		return settings;
	}

}
