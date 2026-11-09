#include "RuntimeLayer.h"

#include "Breccia/Core/Application.h"
#include "Breccia/Core/CrashReporter.h"
#include "Breccia/Core/FileSystem.h"
#include "Breccia/Core/Input.h"
#include "Breccia/Core/Window.h"
#include "Breccia/ImGui/ImGuiLayer.h"
#include "Breccia/ImGui/StatisticsOverlay.h"
#include "Breccia/Network/EnetTransport.h"
#include "Breccia/Project/Project.h"
#include "Breccia/Renderer/GraphicsDevice.h"
#include "Breccia/Renderer/PlayStatistics.h"
#include "Breccia/Renderer/RenderUtils.h"
#include "Breccia/Scene/SceneSerializer.h"
#include "Breccia/Scripting/ScriptEngine.h"
#include "Breccia/UI/GameUI.h"

#include <imgui.h>
#include <nvrhi/utils.h>

namespace Breccia {

	RuntimeLayer::RuntimeLayer(RuntimeOptions options)
		: Layer("RuntimeLayer")
		, m_Options(std::move(options))
	{
	}

	void RuntimeLayer::OnAttach()
	{
		Application& application = Application::Get();
		const auto fail = [&application](const std::string& message) {
			BR_CORE_CRITICAL("Runtime: {}", message);
			CrashReporter::ShowErrorDialog(message);
			application.SetExitCode(1);
			application.Close();
		};

		if (m_Options.ScenePath.empty())
		{
			fail("no scene to play (set StartScene in the project or pass --scene)");
			return;
		}

		std::string error;
		m_Scene = SceneSerializer::LoadScene(Project::ResolvePath(m_Options.ScenePath), error);
		if (!m_Scene)
		{
			fail(error);
			return;
		}

		if (GraphicsDevice* device = application.GetGraphicsDevice())
		{
			m_Renderer = CreateScope<SceneRenderer>(device->GetDevice(), SceneRendererSpecification{ .GpuTimers = device->SupportsTimerQueries(), .AsyncLoading = m_Options.MaxFrames == 0 });
			const size_t errorsBefore = Log::GetHistory().GetErrorCount();
			if (!m_Renderer->Initialize())
			{
				m_Renderer.reset();
				// Only what it logged itself: an older error would name something else.
				const bool logged = Log::GetHistory().GetErrorCount() > errorsBefore;
				fail("the renderer could not start: " + (logged ? Log::GetHistory().GetLastError().value_or("") : "it reported no reason"));
				return;
			}
			m_CommandList = device->GetDevice()->createCommandList();
			m_BlitProfiler = CreateScope<GpuProfiler>(device->GetDevice(), std::vector<std::string>{ "Runtime blit" }, device->SupportsTimerQueries());
			m_Scene->OnViewportResize(device->GetBackBufferWidth(), device->GetBackBufferHeight());
		}

		m_Scene->GetRendererSettings().DebugView = m_Options.DebugView;
		auto graphics = Project::GetActiveGraphicsSettings();
		graphics.RenderScale = m_Options.RenderScale;
		graphics.VSync = m_Options.VSync;
		graphics.DynamicResolution = m_Options.DynamicResolution;
		// Before the scripts' OnCreate, which may read the view (Input.GetViewSize).
		UpdateGameView();
		// Before play starts, so a client's runtime IDs come from its own stream from the first OnCreate.
		if (m_Options.Network && !StartNetwork(*m_Options.Network, error))
		{
			fail(error);
			return;
		}
		m_Scene->OnRuntimeStart(m_Options.Overrides.Apply(graphics));
		BR_CORE_INFO("Runtime: playing '{}'", m_Options.ScenePath);
	}

	void RuntimeLayer::OnDetach()
	{
		if (m_Scene)
			m_Scene->OnRuntimeStop();
		m_Scene.reset();
		m_UIFrame.Clear();
		m_CommandList = nullptr;
		m_Renderer.reset();
		m_BlitProfiler.reset();
	}

	void RuntimeLayer::OnUpdate(Timestep ts)
	{
		if (!m_Scene)
			return;

		UpdateGameView();
		m_Scene->OnUpdate(ts);
		ReportDisconnect();
		ApplyCursorMode();
		m_RenderDeltaTime = ts;
		m_Frame++;
		if (m_Scene->IsQuitRequested())
			Application::Get().Close();
	}

	bool RuntimeLayer::StartNetwork(NetworkRole role, std::string& outError)
	{
		const NetworkSettings& settings = Project::GetActive()->GetConfig().Network;
		const EnetTransportSettings transportSettings{ .TimeoutSeconds = settings.TimeoutSeconds, .MaxMessageBytes = NetworkSystem::MaxHostMessageBytes };
		Scope<NetTransport> transport;
		if (role == NetworkRole::Host)
		{
			transport = EnetTransport::Listen(m_Options.NetworkPort, settings.MaxClients + NetworkSystem::SpareTransportSlots, transportSettings, outError);
			if (!transport)
				return false;
			// One parseable line: tests that ask for port 0 read the port from it.
			BR_CORE_INFO("Network: hosting on UDP port {} (up to {} clients)", transport->GetListenPort(), settings.MaxClients);
		}
		else
		{
			transport = EnetTransport::Connect(m_Options.ConnectAddress, m_Options.NetworkPort, transportSettings, outError);
			if (!transport)
				return false;
			BR_CORE_INFO("Network: connecting to {}:{}", m_Options.ConnectAddress, m_Options.NetworkPort);
		}
		m_Scene->StartNetwork(CreateScope<NetworkSystem>(*m_Scene, role, std::move(transport), settings, NetworkSystem::Identity::FromActiveProject()));
		return true;
	}

	void RuntimeLayer::ReportDisconnect()
	{
		const NetworkSystem* network = m_Scene ? m_Scene->GetNetworkSystem() : nullptr;
		if (!network || m_ReportedDisconnect || network->GetState() != NetConnectionState::Disconnected)
			return;
		m_ReportedDisconnect = true;
		// A player sees why the game stopped updating (a dialog in windowed runs; the log already has it).
		CrashReporter::ShowErrorDialog(fmt::format("Disconnected from the host: {}.", DescribeDisconnectReason(network->GetDisconnectReason())));
	}

	void RuntimeLayer::CheckNetworkResults(const std::function<void(int)>& fail)
	{
		const NetworkSystem* network = m_Scene ? m_Scene->GetNetworkSystem() : nullptr;
		if (!network)
			return;
		const NetworkStatistics& statistics = network->GetStatistics();
		const bool host = network->GetRole() == NetworkRole::Host;
		BR_CORE_INFO("Runtime: network {} connections={} remoteEntities={} spawns={} updates={} despawns={} sent={} ({} bytes) received={} ({} bytes) stale={} rejectedValues={} rejectedRecords={}",
					 host ? "host" : "client", network->GetConnections().size(), network->GetRemoteEntityCount(), statistics.SpawnsApplied, statistics.UpdatesApplied, statistics.DespawnsApplied, statistics.Traffic.MessagesSent,
					 statistics.Traffic.BytesSent, statistics.Traffic.MessagesReceived, statistics.Traffic.BytesReceived, statistics.StaleMessages, statistics.RejectedValues, statistics.RejectedRecords);
		if (!host && network->GetState() != NetConnectionState::Connected)
		{
			const NetDisconnectReason reason = network->GetDisconnectReason();
			BR_CORE_ERROR("Runtime: --connect given but the client was not connected at the end of the run ({})", reason == NetDisconnectReason::None ? "still connecting" : DescribeDisconnectReason(reason));
			fail(8);
		}
	}

	void RuntimeLayer::UpdateGameView()
	{
		// The widgets and scripts map the mouse (window units) onto the view, so the view is in window units, which
		// differ from back-buffer pixels on high-DPI displays. Drawing and hit testing then lay out the same canvas even
		// when fractional scaling rounds the two sizes to slightly different aspects. Headless, the default view stays.
		const Application& application = Application::Get();
		const Window* window = application.GetWindow();
		const GraphicsDevice* device = application.GetGraphicsDevice();
		glm::vec2 size(0.0f);
		if (window && window->GetWidth() > 0 && window->GetHeight() > 0)
			size = { static_cast<float>(window->GetWidth()), static_cast<float>(window->GetHeight()) };
		else if (device && device->GetBackBufferWidth() > 0 && device->GetBackBufferHeight() > 0)
			size = { static_cast<float>(device->GetBackBufferWidth()), static_cast<float>(device->GetBackBufferHeight()) };
		else
			return;
		GameUI::SetViewportSize(size);
		GameUI::SetViewportOrigin({ 0.0f, 0.0f });
	}

	void RuntimeLayer::ApplyCursorMode()
	{
		Window* window = Application::Get().GetWindow();
		const CursorMode mode = Input::GetCursorMode();
		if (!window || mode == m_AppliedCursorMode)
			return;
		window->SetCursorMode(mode);
		m_AppliedCursorMode = mode;
	}

	void RuntimeLayer::OnImGuiRender()
	{
		// ImGui's platform backend shows the OS cursor again every frame unless ImGui asks for none.
		if (Input::GetCursorMode() == CursorMode::Hidden)
			ImGui::SetMouseCursor(ImGuiMouseCursor_None);
		const ImGuiIO& io = ImGui::GetIO();
		m_UIFrame.Draw(ImGui::GetForegroundDrawList(), { 0.0f, 0.0f }, { io.DisplaySize.x, io.DisplaySize.y });
		if (m_Options.ShowStats && m_Scene)
			DrawStatisticsOverlay(ImGui::GetForegroundDrawList(), { 0, 0 }, { io.DisplaySize.x, io.DisplaySize.y },
								  CollectPlayStatistics(*m_Scene, { .FrameMilliseconds = Application::Get().GetFrameMilliseconds(),
																	.Renderer = m_Rendered ? m_Renderer.get() : nullptr,
																	.ImGui = Application::Get().GetImGuiLayer()->GetProfiler(),
																	.Blit = m_Rendered ? m_BlitProfiler.get() : nullptr }));
	}

	void RuntimeLayer::OnRender()
	{
		m_Rendered = false;
		Application& application = Application::Get();
		GraphicsDevice* device = application.GetGraphicsDevice();
		if (!m_Scene || !m_Renderer || !device)
			return;

		const uint32_t width = device->GetBackBufferWidth();
		const uint32_t height = device->GetBackBufferHeight();
		UpdateGameView();
		const glm::vec2 viewportSize = GameUI::GetViewportSize();
		m_Scene->OnViewportResize(width, height);
		const auto graphics = m_Options.Overrides.Apply(m_Scene->GetEffectiveGraphicsSettings());
		m_Renderer->SetRenderScale(graphics.RenderScale);
		m_Renderer->SetDynamicResolution(graphics.DynamicResolution);
		device->SetVSync(graphics.VSync);
		m_Renderer->SetViewportSize(width, height);

		const auto camera = GetPrimaryRenderCamera(*m_Scene, width, height);
		if (camera)
		{
			m_Renderer->Render(*m_Scene, *camera, m_RenderDeltaTime);
			if (m_Renderer->GetStatistics().Failed)
			{
				application.SetExitCode(1);
				application.Close();
				return;
			}
			m_Rendered = true;
		}
		else if (!m_WarnedNoCamera)
		{
			BR_CORE_WARN("Runtime: the scene has no primary camera");
			m_WarnedNoCamera = true;
		}

		// Debug views show the renderer's raw buffers: no game UI over them.
		const bool drawUI = m_Options.DebugView == RendererDebugView::None;
		m_UIFrame.Build(drawUI ? m_Scene.get() : nullptr, viewportSize, drawUI);

		m_CommandList->open();
		m_UIFrame.Prepare([&](const std::string& path) { return m_Renderer->GetUITexture(path, m_CommandList); });
		if (camera)
		{
			m_BlitProfiler->BeginFrame(m_CommandList);
			{
				const GpuPassScope pass(*m_BlitProfiler, m_CommandList, 0);
				m_Renderer->Blit(m_CommandList, device->GetCurrentFramebuffer());
			}
			m_BlitProfiler->EndFrame(m_CommandList, 0);
		}
		else
			nvrhi::utils::ClearColorAttachment(m_CommandList, device->GetCurrentFramebuffer(), 0, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
		m_CommandList->close();
		device->GetDevice()->executeCommandList(m_CommandList);

		if (m_Options.MaxFrames > 0 && m_Frame >= m_Options.MaxFrames)
		{
			// Validation is checked in OnRunFinished, after this frame and its capture have executed.
			if (!m_Options.ScreenshotPath.empty())
			{
				// Captured right before present, so the image includes the game UI drawn after the scene.
				const std::string path = m_Options.ScreenshotPath;
				device->RequestBackBufferCapture([path](const std::vector<uint8_t>& pixels, uint32_t captureWidth, uint32_t captureHeight) {
					std::string error;
					if (WritePng(FileSystem::PathFromUtf8(path), pixels, captureWidth, captureHeight, error))
					{
						BR_CORE_INFO("Runtime: screenshot saved to '{}'", path);
					}
					else
					{
						BR_CORE_ERROR("Runtime: {}", error);
						Application::Get().SetExitCode(4);
					}
				});
			}
			application.Close();
		}
	}

	void RuntimeLayer::OnRunFinished()
	{
		// Automated runs (--frames) report problems through the exit code. The first problem keeps its code
		// (a failed screenshot, 4, is set while the last frame is presented); every one is logged.
		Application& application = Application::Get();
		auto fail = [&application](int exitCode) {
			if (application.GetExitCode() == 0)
				application.SetExitCode(exitCode);
		};
		if (m_Options.MaxFrames == 0)
			return;
		// Before the scene stops below, which ends the session.
		CheckNetworkResults(fail);
		if (m_Options.FailOnScriptErrors && m_Scene)
		{
			// Stop the game here rather than in OnDetach, so errors its OnDestroy callbacks raise are counted too.
			m_Scene->OnRuntimeStop();
			if (const auto& errors = m_Scene->GetStoppedSessionScriptErrors(); !errors.empty())
			{
				BR_CORE_ERROR("Runtime: {} script error(s) occurred; the first: {}", errors.size(), errors.front());
				fail(7);
			}
		}
		const GraphicsDevice* device = application.GetGraphicsDevice();
		if (!device)
			return;
		if (m_Renderer)
			CheckRendererResults(fail);
		if (device->GetValidationErrorCount() > 0)
		{
			BR_CORE_ERROR("Runtime: {} GPU validation error(s) occurred", device->GetValidationErrorCount());
			fail(3);
		}
		else if (m_Options.RequireValidation && !device->IsValidationActive())
		{
			BR_CORE_ERROR("Runtime: --require-validation given but the Vulkan validation layer is not active");
			fail(5);
		}
	}

	void RuntimeLayer::CheckRendererResults(const std::function<void(int)>& fail)
	{
		// Results arrive Latency frames late, so a shorter run cannot have any.
		const bool tooShort = m_Options.MaxFrames <= GpuProfiler::Latency;
		auto logTiming = [&](const char* name, const GpuProfiler* profiler) {
			if (!profiler)
				return;
			const auto statistics = CollectGpuStatistics(profiler);
			const auto text = FormatGpuStatistics(statistics, true);
			if (profiler->IsStalled() || (profiler->IsEnabled() && !statistics->Timings.SampleCount && !tooShort))
				BR_CORE_WARN("Runtime: {} GPU time {}", name, text);
			else
				BR_CORE_INFO("Runtime: {} GPU time {}{}", name, text, tooShort ? " (run shorter than query latency)" : "");
		};
		logTiming("scene", &m_Renderer->GetGpuProfiler());
		logTiming("blit", m_BlitProfiler.get());
		if (const auto* imgui = Application::Get().GetImGuiLayer())
			logTiming("ImGui", imgui->GetProfiler());
		const auto& stats = m_Renderer->GetStatistics();
		BR_CORE_INFO("Runtime: render stats localShadowMaps={} localShadowsSkipped={} lightsSkipped={} localShadowAtlasSize={} fsr1={} vsync={}",
					 stats.LocalShadowMaps, stats.LocalShadowsSkipped, stats.LightsSkipped, stats.LocalShadowAtlasSize, stats.FSR1Active ? 1 : 0, Application::Get().GetGraphicsDevice()->IsVSync() ? 1 : 0);
		const GpuTimings& timings = m_Renderer->GetGpuTimings();
		BR_CORE_INFO("Runtime: last frame rendered at {}x{} ({:.0f}% render scale)", m_Renderer->GetRenderWidth(), m_Renderer->GetRenderHeight(), m_Renderer->GetCurrentRenderScale() * 100.0f);

		const DynamicResolutionSampleCounts& samples = m_Renderer->GetDynamicResolutionSamples();
		if (samples.Rejected > 0 && samples.Accepted == 0)
			BR_CORE_WARN("Runtime: all {} GPU timer results dynamic resolution received were unusable (not positive and finite)", samples.Rejected);
		if (!m_Options.RequireDynamicResolution)
			return;
		std::string reason;
		if (!m_Renderer->IsDynamicResolutionActive())
			reason = DescribeDynamicResolutionState(m_Renderer->GetDynamicResolutionState(), m_Renderer->GetDynamicResolution(), m_Renderer->GetRenderScale());
		else if (tooShort)
			reason = fmt::format("the run is too short: GPU timer results arrive {} frames late, use more --frames", GpuProfiler::Latency);
		else if (m_Renderer->GetGpuProfiler().IsStalled())
			reason = "GPU timer results stopped arriving";
		else if (timings.SampleCount == 0)
			reason = "no GPU timer results arrived";
		else if (samples.Accepted == 0)
			reason = fmt::format("none of the {} GPU timer results was usable", samples.Rejected);
		if (!reason.empty())
		{
			BR_CORE_ERROR("Runtime: --dynamic-resolution given but dynamic resolution did not run ({})", reason);
			fail(6);
		}
	}

}
