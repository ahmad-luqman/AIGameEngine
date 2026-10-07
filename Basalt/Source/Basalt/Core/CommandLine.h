#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Basalt {

	// Minimal "--name value" / "--flag" parser. Arguments not starting with "--" are positional.
	// Which options take a value is declared up front, so "--frames 10" and "--headless scene.bscene"
	// parse unambiguously.
	class CommandLine
	{
	public:
		CommandLine(int argc, const char* const* argv, const std::vector<std::string>& optionsWithValues);

		bool HasFlag(const std::string& name) const;
		std::optional<std::string> GetValue(const std::string& name) const;
		std::optional<int64_t> GetInteger(const std::string& name) const;
		const std::vector<std::string>& GetPositional() const { return m_Positional; }
		// Problems found while parsing (options missing their value). Malformed numbers make GetInteger
		// return nullopt; callers report them.
		const std::vector<std::string>& GetErrors() const { return m_Errors; }

	private:
		std::unordered_map<std::string, std::string> m_Values;
		std::vector<std::string> m_Flags;
		std::vector<std::string> m_Positional;
		std::vector<std::string> m_Errors;
	};

}
