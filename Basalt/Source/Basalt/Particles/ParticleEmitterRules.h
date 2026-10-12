#pragma once

#include "Basalt/Scene/Components.h"

#include <string>
#include <vector>

namespace Basalt {

	// The value rules of ParticleEmitterComponent, in one place for the two places that apply them: the
	// component registry rejects values outside them (files, Lua, automation, inspector), and the particle
	// system repairs components that C++ code wrote directly. One walk over the rules does both, so a
	// component is valid exactly when SanitizeParticleEmitter leaves it unchanged.

	// Every rule `emitter` breaks, each naming the field and its value, in field order; empty when it is valid.
	std::vector<std::string> FindParticleEmitterProblems(const ParticleEmitterComponent& emitter);

	// The first rule `emitter` breaks; empty when it is valid.
	std::string ValidateParticleEmitter(const ParticleEmitterComponent& emitter);

	// `emitter` with every value moved into its range: non-finite values and out-of-range enums take the
	// default component's value, everything else is clamped.
	ParticleEmitterComponent SanitizeParticleEmitter(ParticleEmitterComponent emitter);

}
