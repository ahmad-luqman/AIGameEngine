#pragma once

#include "Basalt/Physics/PhysicsWorld.h"

#include <optional>
#include <string>

namespace Basalt {

	class Entity;
	class Scene;

	// Observes gameplay events of a running scene for tools (the automation event stream) without being part
	// of the game: it sees contacts in Simulate too, where no scripts run. Called on the thread that updates the
	// scene, while the entities are still alive.
	class SceneEventListener
	{
	public:
		virtual ~SceneEventListener() = default;
		// The transitions scripts receive as OnCollisionBegin/End and OnTriggerEnter/Exit, before the scripts do.
		virtual void OnContact(Scene& scene, ContactEventType type, Entity first, Entity second, const std::optional<ContactInfo>& contact) = 0;
		// An Animator event marker playback crossed, before scripts receive it as OnAnimationEvent.
		virtual void OnAnimationEvent(Scene& scene, Entity entity, const std::string& name, const std::string& clip) = 0;
		// Every error ScriptEngine::ReportError records.
		virtual void OnScriptError(Scene& scene, const std::string& message) = 0;
	};

}
