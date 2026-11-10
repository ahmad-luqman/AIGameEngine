#include "Breccia/Network/NetworkSettings.h"

#include "Breccia/Core/JsonUtils.h"
#include "Breccia/Project/Project.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <charconv>
#include <string_view>

namespace Breccia {

	namespace {

		constexpr const char* PortRange = "Network.Port must be an integer in [1, 65535]";

		bool ReadInteger(const nlohmann::json& data, const char* key, uint32_t& value, std::string& outError)
		{
			const auto it = data.find(key);
			if (it == data.end())
				return true;
			if (!JsonToUInt32(*it, value))
			{
				outError = fmt::format("Network.{} must be a non-negative integer", key);
				return false;
			}
			return true;
		}

		bool ReadNumber(const nlohmann::json& data, const char* key, float& value, std::string& outError)
		{
			const auto it = data.find(key);
			if (it == data.end())
				return true;
			if (!JsonToFloat(*it, value))
			{
				outError = fmt::format("Network.{} must be a finite number", key);
				return false;
			}
			return true;
		}

	}

	NetworkSettings NetworkSettings::FromActiveProject()
	{
		const Ref<Project>& project = Project::GetActive();
		return project ? project->GetConfig().Network : NetworkSettings{};
	}

	std::string NetworkSettings::ParseAddress(const std::string& text, uint16_t defaultPort, std::string& outHost, uint16_t& outPort)
	{
		const size_t colon = text.find(':');
		outHost = text.substr(0, colon);
		outPort = defaultPort;
		if (outHost.empty())
			return "the address names no host";
		if (colon == std::string::npos)
			return {};
		const std::string_view port = std::string_view(text).substr(colon + 1);
		if (port.find(':') != std::string_view::npos)
			return "the address holds more than one ':' (IPv6 addresses are not supported)";
		uint32_t value = 0;
		const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), value);
		if (error != std::errc() || end != port.data() + port.size() || value < 1 || value > 65535)
			return fmt::format("the port '{}' is not an integer in [1, 65535]", port);
		outPort = static_cast<uint16_t>(value);
		return {};
	}

	nlohmann::json NetworkSettings::ToJson() const
	{
		return { { "Port", Port }, { "MaxClients", MaxClients }, { "TickRate", TickRate }, { "MaxBytesPerSecond", MaxBytesPerSecond }, { "TimeoutSeconds", TimeoutSeconds }, { "InterpolationDelay", InterpolationDelay }, { "MaxMessagesPerSecond", MaxMessagesPerSecond } };
	}

	std::string NetworkSettings::Validate() const
	{
		if (Port == 0)
			return PortRange;
		if (MaxClients < 1 || MaxClients > MaxClientLimit)
			return fmt::format("Network.MaxClients must be an integer in [1, {}]", MaxClientLimit);
		// The comparisons are false for NaN, which they refuse too.
		if (!(TickRate >= MinTickRate && TickRate <= MaxTickRate))
			return fmt::format("Network.TickRate must be a number in [{}, {}]", MinTickRate, MaxTickRate);
		if (MaxBytesPerSecond < MinBytesPerSecond || MaxBytesPerSecond > MaxBytesPerSecondLimit)
			return fmt::format("Network.MaxBytesPerSecond must be an integer in [{}, {}]", MinBytesPerSecond, MaxBytesPerSecondLimit);
		if (!(TimeoutSeconds >= MinTimeout && TimeoutSeconds <= MaxTimeout))
			return fmt::format("Network.TimeoutSeconds must be a number in [{}, {}]", MinTimeout, MaxTimeout);
		if (!(InterpolationDelay >= 0.0f && InterpolationDelay <= MaxInterpolationDelay))
			return fmt::format("Network.InterpolationDelay must be a number in [0, {}]", MaxInterpolationDelay);
		if (MaxMessagesPerSecond < 1 || MaxMessagesPerSecond > MaxMessagesPerSecondLimit)
			return fmt::format("Network.MaxMessagesPerSecond must be an integer in [1, {}]", MaxMessagesPerSecondLimit);
		return {};
	}

	std::optional<NetworkSettings> NetworkSettings::FromJson(const nlohmann::json& data, std::string& outError)
	{
		if (!data.is_object())
		{
			outError = "Network must be an object";
			return std::nullopt;
		}
		if (!RejectUnknownKeys(data, { "Port", "MaxClients", "TickRate", "MaxBytesPerSecond", "TimeoutSeconds", "InterpolationDelay", "MaxMessagesPerSecond" }, "Network", outError))
			return std::nullopt;
		NetworkSettings settings;
		uint32_t port = settings.Port;
		if (!ReadInteger(data, "Port", port, outError) || !ReadInteger(data, "MaxClients", settings.MaxClients, outError) || !ReadInteger(data, "MaxBytesPerSecond", settings.MaxBytesPerSecond, outError) || !ReadNumber(data, "TickRate", settings.TickRate, outError) || !ReadNumber(data, "TimeoutSeconds", settings.TimeoutSeconds, outError) || !ReadNumber(data, "InterpolationDelay", settings.InterpolationDelay, outError) || !ReadInteger(data, "MaxMessagesPerSecond", settings.MaxMessagesPerSecond, outError))
			return std::nullopt;
		if (port > 65535)
		{
			outError = PortRange;
			return std::nullopt;
		}
		settings.Port = static_cast<uint16_t>(port);
		if (std::string problem = settings.Validate(); !problem.empty())
		{
			outError = std::move(problem);
			return std::nullopt;
		}
		return settings;
	}

}
