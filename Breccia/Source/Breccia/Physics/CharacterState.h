#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Breccia {

	// One fixed step of a character's movement: the velocity it walked with (MoveCharacter's, mixed with root motion) and
	// its world rotation. A network client records these for the character it predicts and replays them (StepCharacter);
	// the host applies them to the client's character.
	struct CharacterInput
	{
		glm::vec3 Velocity = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

		bool operator==(const CharacterInput&) const = default;
	};

	// What a character carries from one step to the next besides its rotation: where it is (world space), its velocity,
	// and its time in the air. Enough to put a character back where it was and step it again identically against the same
	// world (client-side prediction). Every value is finite.
	struct CharacterState
	{
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Velocity = glm::vec3(0.0f);
		// Seconds since it last stood on walkable ground, at most MaxSinceGrounded (any longer is the same: past every
		// JumpGrace).
		float SinceGrounded = 0.0f;
		// Whether it jumped since then (the coyote jump is spent).
		bool Jumped = false;

		static constexpr float MaxSinceGrounded = 11.0f;

		bool operator==(const CharacterState&) const = default;
	};

}
