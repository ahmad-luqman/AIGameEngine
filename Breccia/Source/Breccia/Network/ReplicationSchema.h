#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace Breccia {

	struct ComponentInfo;

	// The wire identities of what replication sends, built once from ComponentRegistry: a component is its index in the
	// registry, a replicated field its index in that component's ComponentInfo::ReplicatedFields. The connection handshake
	// compares the hashes of two builds, which cover the components in order, every field's name and JSON type (list
	// elements' too), every enum's options and the replicated fields: what spawns and updates depend on. Value ranges a
	// component's Validate checks are not covered.
	class ReplicationSchema
	{
	public:
		struct Component
		{
			const ComponentInfo* Info = nullptr;
			// Core components (Transform, Tag) exist on every entity, so their presence is never sent.
			bool IsCore = false;
		};

		static const ReplicationSchema& Get();

		const std::vector<Component>& GetComponents() const { return m_Components; }
		// The component at a wire index, or null for an index this build does not have.
		const Component* FindComponent(uint32_t index) const { return index < m_Components.size() ? &m_Components[index] : nullptr; }
		std::optional<uint32_t> FindComponentIndex(std::string_view name) const;
		// FNV-1a over what the class comment lists.
		uint64_t GetHash() const { return m_Hash; }
		// The hash of these components (GetHash is that of the registry's).
		static uint64_t ComputeHash(const std::vector<ComponentInfo>& components);

	private:
		ReplicationSchema();

		std::vector<Component> m_Components;
		uint64_t m_Hash = 0;
	};

}
