#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Breccia {

	struct ComponentInfo;

	// The wire identities of what replication sends, built once from ComponentRegistry: a component is its index in the
	// registry, a replicated field its index in that component's ComponentInfo::ReplicatedFields. Two builds agree on the
	// wire format exactly when their hashes agree, which the connection handshake checks.
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
		// FNV-1a over every component's name and replicated fields (names and JSON kinds) in order.
		uint64_t GetHash() const { return m_Hash; }

	private:
		ReplicationSchema();

		std::vector<Component> m_Components;
		uint64_t m_Hash = 0;
	};

}
