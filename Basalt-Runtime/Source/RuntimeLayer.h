#pragma once

#include "Basalt/Core/Layer.h"
#include "Basalt/Renderer/SceneRenderer.h"
#include "Basalt/Scene/Scene.h"

#include <nvrhi/nvrhi.h>

#include <string>

namespace Basalt {

	struct RuntimeOptions
	{
		std::string ScenePath;
		// When non-zero: run this many frames, optionally save a screenshot, then exit.
		uint64_t MaxFrames = 0;
		std::string ScreenshotPath;
		RendererDebugView DebugView = RendererDebugView::None;
	};

	// Plays a scene: scripts, physics and audio run every frame and the primary camera's view is
	// rendered to the window.
	class RuntimeLayer : public Layer
	{
	public:
		explicit RuntimeLayer(RuntimeOptions options);

		void OnAttach() override;
		void OnDetach() override;
		void OnUpdate(Timestep ts) override;
		void OnRender() override;

	private:
		RuntimeOptions m_Options;
		Ref<Scene> m_Scene;
		Scope<SceneRenderer> m_Renderer;
		nvrhi::CommandListHandle m_CommandList;
		uint64_t m_Frame = 0;
		bool m_WarnedNoCamera = false;
	};

}
