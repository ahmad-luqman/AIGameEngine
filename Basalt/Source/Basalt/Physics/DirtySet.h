#pragma once

// Internal to the physics module.

#include "Basalt/Core/UUID.h"

#include <unordered_set>
#include <utility>
#include <vector>

namespace Basalt::PhysicsInternal {

	// Entities waiting to be rebuilt, in the order they were first marked. Rebuilds create and activate Jolt
	// bodies, and that order steers the simulation, so it must never follow hash order (which differs between
	// standard libraries). Members may have been destroyed since they were marked; consumers skip those.
	class DirtySet
	{
	public:
		void Insert(UUID uuid)
		{
			if (m_Members.contains(uuid))
				return;
			// Order first: if the set insert throws, the duplicate is only rebuilt twice, never skipped.
			m_Order.push_back(uuid);
			m_Members.insert(uuid);
		}
		void Clear()
		{
			m_Members.clear();
			m_Order.clear();
		}
		// Empties the set and returns its members in marking order.
		std::vector<UUID> Take()
		{
			std::vector<UUID> order = std::move(m_Order);
			Clear();
			return order;
		}

	private:
		std::unordered_set<UUID> m_Members;
		std::vector<UUID> m_Order;
	};

}
