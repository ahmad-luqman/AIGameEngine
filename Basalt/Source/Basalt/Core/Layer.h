#pragma once

#include "Basalt/Core/Timestep.h"
#include "Basalt/Events/Event.h"

#include <string>

namespace Basalt {

	// A slice of application behaviour (editor, game, debug overlay). Layers update in push order
	// and receive events in reverse order, so overlays get first chance to consume input.
	class Layer
	{
	public:
		explicit Layer(std::string name = "Layer")
			: m_DebugName(std::move(name))
		{
		}
		virtual ~Layer() = default;

		virtual void OnAttach() {}
		virtual void OnDetach() {}
		virtual void OnUpdate(Timestep ts) {}
		// Called between GraphicsDevice::BeginFrame and Present. Never called in headless mode.
		virtual void OnRender() {}
		virtual void OnImGuiRender() {}
		virtual void OnEvent(Event& event) {}

		const std::string& GetName() const { return m_DebugName; }

	protected:
		std::string m_DebugName;
	};

}
