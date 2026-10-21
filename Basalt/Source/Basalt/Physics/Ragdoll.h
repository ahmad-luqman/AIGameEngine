#pragma once

#include "Basalt/Scene/Components.h"

#include <string>

namespace Basalt {

	// Shared by the registry and physics: C++ component writes must meet the same rules as JSON/Lua edits.
	std::string ValidateRagdoll(const RagdollComponent& component);

}
