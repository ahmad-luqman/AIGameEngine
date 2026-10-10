#include "Basalt/Physics/PhysicsWarnings.h"

#include "Basalt/Core/Log.h"

namespace Basalt::PhysicsInternal {

	void WarningLog::Report(Entity entity, const std::vector<std::string>& warnings)
	{
		auto it = m_Logged.find(entity.GetUUID());
		if (warnings.empty())
		{
			if (it != m_Logged.end())
				m_Logged.erase(it);
			return;
		}
		if (it != m_Logged.end() && it->second == warnings)
			return;
		if (m_Joined)
		{
			std::string joined;
			for (const std::string& warning : warnings)
				joined += (joined.empty() ? "" : "; ") + warning;
			BS_CORE_WARN("Physics: {} '{}': {}", m_Subject, entity.GetName(), joined);
		}
		else
		{
			for (const std::string& warning : warnings)
				BS_CORE_WARN("Physics: {} '{}': {}", m_Subject, entity.GetName(), warning);
		}
		m_Logged[entity.GetUUID()] = warnings;
	}

}
