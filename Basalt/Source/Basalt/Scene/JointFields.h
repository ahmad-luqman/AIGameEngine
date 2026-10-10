#pragma once

#include "Basalt/Scene/Components.h"

#include <string>
#include <string_view>
#include <vector>

namespace Basalt {

	// Which JointComponent fields (by their JSON names) a joint actually reads. A field applies when the
	// joint's Type uses it and, for limit fields, when UseLimits is on. The inspector hides fields that do
	// not apply, and physics warns about the ones set away from their defaults, so both follow this one rule.
	bool JointFieldApplies(const JointComponent& joint, std::string_view field);

	// Whether a change from `built` to `current` must rebuild a joint in play (which resets its rest pose)
	// rather than apply in place. Each field's entry in the JointFields table says which it needs.
	bool JointNeedsRebuild(const JointComponent& built, const JointComponent& current);

	// Every JointComponent field name, in declaration order (the same list the component registry reads).
	const std::vector<std::string>& GetJointFieldNames();

	// The fields set away from their defaults that this joint ignores, in declaration order.
	std::vector<std::string> GetIgnoredJointFields(const JointComponent& joint);

}
