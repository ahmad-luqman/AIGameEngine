#include "Basalt/Core/CommandLine.h"

#include <algorithm>
#include <charconv>

namespace Basalt {

	CommandLine::CommandLine(int argc, const char* const* argv, const std::vector<std::string>& optionsWithValues)
	{
		for (int i = 1; i < argc; i++)
		{
			const std::string argument = argv[i] ? argv[i] : "";
			if (argument.rfind("--", 0) != 0 || argument.size() == 2)
			{
				m_Positional.push_back(argument);
				continue;
			}

			std::string name = argument.substr(2);
			// Also accept "--name=value".
			const size_t equals = name.find('=');
			if (equals != std::string::npos)
			{
				m_Values[name.substr(0, equals)] = name.substr(equals + 1);
				continue;
			}

			if (std::find(optionsWithValues.begin(), optionsWithValues.end(), name) != optionsWithValues.end())
			{
				if (i + 1 >= argc)
				{
					m_Errors.push_back("option --" + name + " requires a value");
					continue;
				}
				m_Values[name] = argv[++i];
			}
			else
			{
				m_Flags.push_back(name);
			}
		}
	}

	bool CommandLine::HasFlag(const std::string& name) const
	{
		return std::find(m_Flags.begin(), m_Flags.end(), name) != m_Flags.end();
	}

	std::optional<std::string> CommandLine::GetValue(const std::string& name) const
	{
		auto it = m_Values.find(name);
		if (it == m_Values.end())
			return std::nullopt;
		return it->second;
	}

	std::optional<int64_t> CommandLine::GetInteger(const std::string& name) const
	{
		const auto value = GetValue(name);
		if (!value)
			return std::nullopt;
		int64_t result = 0;
		const char* end = value->data() + value->size();
		const auto [pointer, error] = std::from_chars(value->data(), end, result);
		if (error != std::errc() || pointer != end)
			return std::nullopt;
		return result;
	}

}
