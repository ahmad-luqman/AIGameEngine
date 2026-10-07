#include "Basalt/Renderer/DeviceSelection.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <vector>

namespace Basalt {

	namespace {

		int KindRank(PhysicalDeviceKind kind)
		{
			switch (kind)
			{
				case PhysicalDeviceKind::Discrete:
					return 4;
				case PhysicalDeviceKind::Integrated:
					return 3;
				case PhysicalDeviceKind::Virtual:
				case PhysicalDeviceKind::Other:
					return 2;
				case PhysicalDeviceKind::Cpu:
					return 1;
			}
			return 0;
		}

		std::optional<size_t> SelectAutomatically(std::span<const PhysicalDeviceInfo> devices, std::string& outError)
		{
			std::optional<size_t> best;
			for (size_t i = 0; i < devices.size(); i++)
			{
				if (devices[i].Suitable && (!best || KindRank(devices[i].Kind) > KindRank(devices[*best].Kind)))
					best = i;
			}
			if (!best)
				outError = "no GPU supports Vulkan 1.3 with dynamic rendering, synchronization2 and timeline semaphores";
			return best;
		}

		std::string ToLower(std::string_view text)
		{
			std::string result(text);
			std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return result;
		}

		std::string DescribeDevices(std::span<const PhysicalDeviceInfo> devices, const std::vector<size_t>& indices)
		{
			std::string result;
			for (size_t index : indices)
				result += (result.empty() ? "" : ", ") + std::to_string(index) + ": " + devices[index].Name;
			return result.empty() ? "none" : result;
		}

		// An all-digit selector is an index (matching the "GPU N:" startup log lines); anything else is a
		// case-insensitive substring of the device name that must match exactly one device. Ambiguity is an
		// error rather than "first match" so a command line always means the same GPU on a given machine.
		std::optional<size_t> SelectExplicitly(std::span<const PhysicalDeviceInfo> devices, std::string_view selector, std::string& outError)
		{
			std::vector<size_t> all(devices.size());
			for (size_t i = 0; i < all.size(); i++)
				all[i] = i;

			std::optional<size_t> chosen;
			if (std::all_of(selector.begin(), selector.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
			{
				size_t index = 0;
				const auto [end, error] = std::from_chars(selector.data(), selector.data() + selector.size(), index);
				if (error != std::errc() || end != selector.data() + selector.size() || index >= devices.size())
				{
					outError = "--gpu " + std::string(selector) + ": no GPU with that index (available: " + DescribeDevices(devices, all) + ")";
					return std::nullopt;
				}
				chosen = index;
			}
			else
			{
				const std::string needle = ToLower(selector);
				std::vector<size_t> matches;
				for (size_t i = 0; i < devices.size(); i++)
				{
					if (ToLower(devices[i].Name).find(needle) != std::string::npos)
						matches.push_back(i);
				}
				if (matches.size() != 1)
				{
					outError = "--gpu '" + std::string(selector) + "' " + (matches.empty() ? "matches no GPU" : "is ambiguous; use an index") +
							   " (" + (matches.empty() ? "available: " + DescribeDevices(devices, all) : "matches: " + DescribeDevices(devices, matches)) + ")";
					return std::nullopt;
				}
				chosen = matches.front();
			}

			if (!devices[*chosen].Suitable)
			{
				outError = "--gpu selects '" + devices[*chosen].Name + "', which lacks Vulkan 1.3 dynamic rendering, synchronization2, timeline semaphores or a usable queue";
				return std::nullopt;
			}
			return chosen;
		}

	}

	const char* ToString(PhysicalDeviceKind kind)
	{
		switch (kind)
		{
			case PhysicalDeviceKind::Discrete:
				return "discrete";
			case PhysicalDeviceKind::Integrated:
				return "integrated";
			case PhysicalDeviceKind::Virtual:
				return "virtual";
			case PhysicalDeviceKind::Cpu:
				return "cpu";
			case PhysicalDeviceKind::Other:
				return "other";
		}
		return "unknown";
	}

	std::optional<size_t> SelectPhysicalDevice(std::span<const PhysicalDeviceInfo> devices, std::string_view selector, std::string& outError)
	{
		if (selector.empty())
			return SelectAutomatically(devices, outError);
		return SelectExplicitly(devices, selector, outError);
	}

}
