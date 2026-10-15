#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace Basalt {

	// Transient editor view preferences, validated by the same schema for UI and automation.
	class ViewportSettings
	{
	public:
		ViewportSettings();
		static nlohmann::json Schema();
		const nlohmann::json& Get() const { return m_Value; }
		bool Apply(const nlohmann::json& patch, std::string& error);

	private:
		nlohmann::json m_Value;
	};

}
