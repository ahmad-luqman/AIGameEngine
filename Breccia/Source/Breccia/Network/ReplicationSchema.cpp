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

	namespace {

		// Every field's name and type, every enum's options, and list elements' fields, depth first.
		void HashFields(uint64_t& hash, const std::vector<std::string>& fields, const std::map<std::string, ComponentFieldType>& types, const std::map<std::string, std::vector<std::string>>& enums)
		{
			for (const std::string& field : fields)
			{
				HashText(hash, field);
				const auto type = types.find(field);
				if (type == types.end())
					continue;
				HashText(hash, type->second.ToString());
				if (const auto options = enums.find(field); options != enums.end())
					for (const std::string& option : options->second)
						HashText(hash, option);
				if (const ComponentObjectSchema* element = type->second.Element.get())
				{
					HashText(hash, "[");
					HashFields(hash, element->Fields, element->FieldTypes, element->EnumOptions);
					HashText(hash, "]");
				}
			}
		}

	}

	uint64_t ReplicationSchema::ComputeHash(const std::vector<ComponentInfo>& components)
	{
		uint64_t hash = FnvOffset;
		for (const ComponentInfo& info : components)
		{
			HashText(hash, info.Name);
			// What may be added or removed (CheckUpdate refuses it for core components).
			HashText(hash, info.IsCore ? "core" : "optional");
			// Spawns carry every field, which the receiver reads strictly: a field one build lacks fails every spawn.
			HashFields(hash, info.Fields, info.FieldTypes, info.EnumOptions);
			HashText(hash, "replicated");
			for (const std::string& field : info.ReplicatedFields)
				HashText(hash, field);
		}
		return hash;
	}

	ReplicationSchema::ReplicationSchema()
		: m_Hash(ComputeHash(ComponentRegistry::GetAll()))
	{
		for (const ComponentInfo& info : ComponentRegistry::GetAll())
			m_Components.push_back({ &info, info.IsCore });
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
