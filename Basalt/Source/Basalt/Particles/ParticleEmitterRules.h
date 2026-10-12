#pragma once

#include "Basalt/Scene/Components.h"

#include <string>

namespace Basalt {

	// The value rules of ParticleEmitterComponent, in one place for the two places that apply them: the
	// component registry rejects values outside them (files, Lua, automation, inspector), and the particle
	// system repairs components that C++ code wrote directly. For every component c,
	// ValidateParticleEmitter(c) is empty exactly when SanitizeParticleEmitter(c) == c.

	// The first rule `emitter` breaks, naming the field and its value; empty when it follows every rule.
	std::string ValidateParticleEmitter(const ParticleEmitterComponent& emitter);

	// `emitter` with every value moved into its range: non-finite and out-of-range enum values take the
	// default component's value, everything else is clamped.
	ParticleEmitterComponent SanitizeParticleEmitter(ParticleEmitterComponent emitter);

}
