#include "Breccia/Network/ReplicationSchema.h"

#include "Breccia/Scene/ComponentRegistry.h"

namespace Breccia {

	namespace {

		constexpr uint64_t FnvOffset = 0xcbf29ce484222325ull;
		constexpr uint64_t FnvPrime = 0x100000001b3ull;

		void HashText(uint64_t& hash, std::string_view text)
		{
			for (const char c : text)
			{
				hash ^= static_cast<uint8_t>(c);
				hash *= FnvPrime;
			}
			// A separator, so "ab" + "c" and "a" + "bc" differ.
			hash ^= 0xFF;
			hash *= FnvPrime;
		}

	}

	ReplicationSchema::ReplicationSchema()
	{
		m_Hash = FnvOffset;
		for (const ComponentInfo& info : ComponentRegistry::GetAll())
		{
			m_Components.push_back({ &info, info.IsCore });
			HashText(m_Hash, info.Name);
			for (const std::string& field : info.ReplicatedFields)
			{
				HashText(m_Hash, field);
				const auto type = info.FieldTypes.find(field);
				HashText(m_Hash, type != info.FieldTypes.end() ? type->second.ToString() : "?");
			}
		}
	}

	const ReplicationSchema& ReplicationSchema::Get()
	{
		static const ReplicationSchema s_Schema;
		return s_Schema;
	}

	std::optional<uint32_t> ReplicationSchema::FindComponentIndex(std::string_view name) const
	{
		for (size_t i = 0; i < m_Components.size(); i++)
			if (m_Components[i].Info->Name == name)
				return static_cast<uint32_t>(i);
		return std::nullopt;
	}

}
