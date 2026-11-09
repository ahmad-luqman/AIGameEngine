#pragma once

#include "Breccia/Core/Layer.h"
#include "Breccia/Network/NetworkSystem.h"
#include "Breccia/Renderer/SceneRenderer.h"
#include "Breccia/Scene/Scene.h"
#include "Breccia/UI/UIRenderer.h"

#include <nvrhi/nvrhi.h>

#include <functional>
#include <optional>
#include <string>

namespace Breccia {

	struct RuntimeOptions
	{
		std::string ScenePath;
		// When non-zero: run this many frames, optionally save a screenshot, then exit.
		uint64_t MaxFrames = 0;
		std::string ScreenshotPath;
		// Fail (exit code 5) when GPU validation is not active, so validation-based tests cannot pass vacuously.
		bool RequireValidation = false;
		// Passive runtime diagnostics over the game image (--stats).
		bool ShowStats = false;
		// Fail (exit code 7) when a script reported an error during the run (export smoke tests). Script errors
		// never stop a game, so without this a broken script would still exit 0.
		bool FailOnScriptErrors = false;
		RendererDebugView DebugView = RendererDebugView::None;
		// The project's RenderScale unless --render-scale overrides it.
		float RenderScale = 1.0f;
		// Initial project/CLI preference; explicit CLI choices also persist in Overrides.
		bool VSync = true;
		GraphicsOverrides Overrides;
		// The project's settings unless --dynamic-resolution / --min-render-scale override them.
		DynamicResolutionSettings DynamicResolution;
		// --dynamic-resolution asked for it explicitly: a --frames run fails (exit code 6) when dynamic
		// resolution could not run (no GPU timers, nothing to adjust, a run too short for timer results, or no
		// usable results).
		bool RequireDynamicResolution = false;
		// --host <port> (a listen server, or a dedicated one with --headless) or --connect <address[:port]>.
		std::optional<NetworkRole> Network;
		// Host: the port to listen on (0: any free one). Client: the host's port.
		uint16_t NetworkPort = 0;
		std::string ConnectAddress;
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
		// Gives the window the cursor the game asks for (Input::GetCursorMode) when it changed.
		void ApplyCursorMode();
		// Sets the game view (GameUI viewport) to the window, before scripts run and every frame.
		void UpdateGameView();
		// Starts the --host or --connect session on the scene; false (logged) when its socket cannot be opened.
		bool StartNetwork(NetworkRole role, std::string& outError);
		// Logs the session's totals and fails (through `fail`) a client that is not connected at the end of the run.
		void CheckNetworkResults(const std::function<void(int)>& fail);
		// Shows a client's lost connection to the player once (CrashReporter's dialog, which automated runs never show).
		void ReportDisconnect();

		RuntimeOptions m_Options;
		Ref<Scene> m_Scene;
		Scope<SceneRenderer> m_Renderer;
		Scope<GpuProfiler> m_BlitProfiler;
		nvrhi::CommandListHandle m_CommandList;
		// The game UI of the frame being rendered (built in OnRender, drawn in OnImGuiRender).
		UIFrame m_UIFrame;
		uint64_t m_Frame = 0;
		float m_RenderDeltaTime = 0.0f;
		bool m_WarnedNoCamera = false;
		// This frame succeeded, rather than merely having a camera or stale renderer counters.
		bool m_Rendered = false;
		CursorMode m_AppliedCursorMode = CursorMode::Normal;
		bool m_ReportedDisconnect = false;
	};

}
