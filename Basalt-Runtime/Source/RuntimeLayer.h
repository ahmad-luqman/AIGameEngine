#pragma once

#include "Basalt/Core/Layer.h"
#include "Basalt/Renderer/SceneRenderer.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/UI/UIRenderer.h"

#include <nvrhi/nvrhi.h>

#include <functional>
#include <string>

namespace Basalt {

	struct RuntimeOptions
	{
		std::string ScenePath;
		// When non-zero: run this many frames, optionally save a screenshot, then exit.
		uint64_t MaxFrames = 0;
		std::string ScreenshotPath;
		// Fail (exit code 5) when GPU validation is not active, so validation-based tests cannot pass vacuously.
		bool RequireValidation = false;
		RendererDebugView DebugView = RendererDebugView::None;
		// The project's RenderScale unless --render-scale overrides it.
		float RenderScale = 1.0f;
		// The project's settings unless --dynamic-resolution / --min-render-scale override them.
		DynamicResolutionSettings DynamicResolution;
		// --dynamic-resolution asked for it explicitly: a --frames run fails (exit code 6) when dynamic
		// resolution could not run (no GPU timers, nothing to adjust, a run too short for timer results, or no
		// usable results).
		bool RequireDynamicResolution = false;
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
		void OnImGuiRender() override;
		void OnRunFinished() override;

	private:
		// Logs the GPU timings and fails (through `fail`) a run whose --dynamic-resolution did not run.
		void CheckRendererResults(const std::function<void(int)>& fail);

		RuntimeOptions m_Options;
		Ref<Scene> m_Scene;
		Scope<SceneRenderer> m_Renderer;
		nvrhi::CommandListHandle m_CommandList;
		// The game UI of the frame being rendered (built in OnRender, drawn in OnImGuiRender).
		UIFrame m_UIFrame;
		uint64_t m_Frame = 0;
		bool m_WarnedNoCamera = false;
	};

}
