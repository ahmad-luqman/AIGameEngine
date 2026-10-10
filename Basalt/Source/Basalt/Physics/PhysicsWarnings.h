#pragma once

// Internal to the physics module.

#include "Basalt/Core/UUID.h"
#include "Basalt/Scene/Entity.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace Basalt::PhysicsInternal {

	// Logs the problems found while building an entity's body, character or joint, but only when they differ
	// from the ones last logged for it: scripts may set a component every frame without flooding the log.
	// Messages name the offending values, so the same messages mean the same problem.
	class WarningLog
	{
	public:
		// subject names the entity in messages ("entity", "character", "joint on"). joined logs all warnings
		// on one "; "-separated line; otherwise each gets its own.
		WarningLog(const char* subject, bool joined)
			: m_Subject(subject)
			, m_Joined(joined)
		{
		}

		// An empty list clears the entity's record, so the same problem coming back is logged again.
		void Report(Entity entity, const std::vector<std::string>& warnings);
		void Forget(UUID uuid) { m_Logged.erase(uuid); }

	private:
		const char* m_Subject;
		bool m_Joined;
		std::unordered_map<UUID, std::vector<std::string>> m_Logged;
	};

}
