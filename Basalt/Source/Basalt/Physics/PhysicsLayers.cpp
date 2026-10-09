#include "Basalt/Physics/PhysicsLayers.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace Basalt {

	PhysicsLayers::PhysicsLayers()
		: m_Names{ DefaultLayerName }
	{
		m_Masks.fill(0xFFFFFFFFu);
	}

	std::optional<uint32_t> PhysicsLayers::Find(std::string_view name) const
	{
		const auto it = std::ranges::find(m_Names, name);
		if (it == m_Names.end())
			return std::nullopt;
		return static_cast<uint32_t>(it - m_Names.begin());
	}

	bool PhysicsLayers::Add(const std::string& name, std::string& outError)
	{
		if (name.empty())
		{
			outError = "physics layer names must not be empty";
			return false;
		}
		if (Find(name))
		{
			outError = "duplicate physics layer '" + name + "'";
			return false;
		}
		if (m_Names.size() >= MaxLayers)
		{
			outError = "at most " + std::to_string(MaxLayers) + " physics layers are supported";
			return false;
		}
		m_Names.push_back(name);
		return true;
	}

	bool PhysicsLayers::ShouldCollide(uint32_t a, uint32_t b) const
	{
		if (a >= GetCount() || b >= GetCount())
			return false;
		return (m_Masks[a] & (1u << b)) != 0;
	}

	void PhysicsLayers::SetCollides(uint32_t a, uint32_t b, bool collides)
	{
		if (a >= GetCount() || b >= GetCount())
			return;
		if (collides)
		{
			m_Masks[a] |= 1u << b;
			m_Masks[b] |= 1u << a;
		}
		else
		{
			m_Masks[a] &= ~(1u << b);
			m_Masks[b] &= ~(1u << a);
		}
	}

	uint32_t PhysicsLayers::GetCollisionMask(uint32_t layer) const
	{
		return layer < GetCount() ? m_Masks[layer] : 0u;
	}

	std::optional<uint32_t> PhysicsLayers::MaskFromNames(const std::vector<std::string>& names, std::string& outError) const
	{
		uint32_t mask = 0;
		for (const std::string& name : names)
		{
			const auto index = Find(name);
			if (!index)
			{
				outError = "unknown physics layer '" + name + "'";
				return std::nullopt;
			}
			mask |= 1u << *index;
		}
		return mask;
	}

	nlohmann::json PhysicsLayers::ToJson() const
	{
		nlohmann::json pairs = nlohmann::json::array();
		for (uint32_t a = 0; a < GetCount(); a++)
		{
			for (uint32_t b = a; b < GetCount(); b++)
			{
				if (!ShouldCollide(a, b))
					pairs.push_back(nlohmann::json::array({ m_Names[a], m_Names[b] }));
			}
		}
		return { { "Names", m_Names }, { "IgnoredPairs", pairs } };
	}

	std::optional<PhysicsLayers> PhysicsLayers::FromJson(const nlohmann::json& data, std::string& outError)
	{
		if (!data.is_object())
		{
			outError = "PhysicsLayers must be an object";
			return std::nullopt;
		}
		for (const auto& [key, value] : data.items())
		{
			if (key != "Names" && key != "IgnoredPairs")
			{
				outError = "PhysicsLayers: unknown key '" + key + "'";
				return std::nullopt;
			}
		}

		PhysicsLayers layers;
		if (const auto names = data.find("Names"); names != data.end())
		{
			if (!names->is_array() || names->empty() || !names->front().is_string() || names->front().get<std::string>() != DefaultLayerName)
			{
				outError = "PhysicsLayers.Names must be an array of strings starting with \"Default\"";
				return std::nullopt;
			}
			for (size_t i = 1; i < names->size(); i++)
			{
				const nlohmann::json& name = (*names)[i];
				if (!name.is_string())
				{
					outError = "PhysicsLayers.Names must contain only strings";
					return std::nullopt;
				}
				std::string error;
				if (!layers.Add(name.get<std::string>(), error))
				{
					outError = "PhysicsLayers.Names: " + error;
					return std::nullopt;
				}
			}
		}

		if (const auto pairs = data.find("IgnoredPairs"); pairs != data.end())
		{
			if (!pairs->is_array())
			{
				outError = "PhysicsLayers.IgnoredPairs must be an array of [layer, layer] pairs";
				return std::nullopt;
			}
			for (const nlohmann::json& pair : *pairs)
			{
				if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string() || !pair[1].is_string())
				{
					outError = "PhysicsLayers.IgnoredPairs must be an array of [layer, layer] pairs";
					return std::nullopt;
				}
				const auto a = layers.Find(pair[0].get<std::string>());
				const auto b = layers.Find(pair[1].get<std::string>());
				if (!a || !b)
				{
					outError = "PhysicsLayers.IgnoredPairs: unknown layer '" + (a ? pair[1] : pair[0]).get<std::string>() + "'";
					return std::nullopt;
				}
				layers.SetCollides(*a, *b, false);
			}
		}
		return layers;
	}

}
