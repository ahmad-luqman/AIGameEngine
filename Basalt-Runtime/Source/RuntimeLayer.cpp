#include "RuntimeLayer.h"

#include "Basalt/Core/Application.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Renderer/GraphicsDevice.h"
#include "Basalt/Renderer/RenderUtils.h"
#include "Basalt/Scene/SceneSerializer.h"
#include "Basalt/UI/GameUI.h"

#include <imgui.h>
#include <nvrhi/utils.h>

namespace Basalt {

	RuntimeLayer::RuntimeLayer(RuntimeOptions options)
		: Layer("RuntimeLayer")
		, m_Options(std::move(options))
	{
	}

	void RuntimeLayer::OnAttach()
	{
		Application& application = Application::Get();

		if (m_Options.ScenePath.empty())
		{
			BS_CORE_CRITICAL("Runtime: no scene to play (set StartScene in the project or pass --scene)");
			application.Close();
			return;
		}

		std::string error;
		m_Scene = SceneSerializer::LoadScene(Project::ResolvePath(m_Options.ScenePath), error);
		if (!m_Scene)
		{
			BS_CORE_CRITICAL("Runtime: {}", error);
			application.SetExitCode(1);
			application.Close();
			return;
		}

		if (GraphicsDevice* device = application.GetGraphicsDevice())
		{
			m_Renderer = CreateScope<SceneRenderer>(device->GetDevice(), SceneRendererSpecification{ .GpuTimers = device->SupportsTimerQueries() });
			if (!m_Renderer->Initialize())
			{
				application.Close();
				return;
			}
			m_Renderer->SetRenderScale(m_Options.RenderScale);
			m_Renderer->SetDynamicResolution(m_Options.DynamicResolution);
			m_CommandList = device->GetDevice()->createCommandList();
			m_Scene->OnViewportResize(device->GetBackBufferWidth(), device->GetBackBufferHeight());
		}

		m_Scene->GetRendererSettings().DebugView = m_Options.DebugView;
		m_Scene->OnRuntimeStart();
		BS_CORE_INFO("Runtime: playing '{}'", m_Options.ScenePath);
	}

	void RuntimeLayer::OnDetach()
	{
		if (m_Scene)
			m_Scene->OnRuntimeStop();
		m_Scene.reset();
		m_UIFrame.Clear();
		m_CommandList = nullptr;
		m_Renderer.reset();
	}

	void RuntimeLayer::OnUpdate(Timestep ts)
	{
		if (!m_Scene)
			return;

		m_Scene->OnUpdate(ts);
		m_Frame++;
		if (m_Scene->IsQuitRequested())
			Application::Get().Close();
	}

	void RuntimeLayer::OnImGuiRender()
	{
		const ImGuiIO& io = ImGui::GetIO();
		m_UIFrame.Draw(ImGui::GetForegroundDrawList(), { 0.0f, 0.0f }, { io.DisplaySize.x, io.DisplaySize.y });
	}

	void RuntimeLayer::OnRender()
	{
		Application& application = Application::Get();
		GraphicsDevice* device = application.GetGraphicsDevice();
		if (!m_Scene || !m_Renderer || !device)
			return;

		const uint32_t width = device->GetBackBufferWidth();
		const uint32_t height = device->GetBackBufferHeight();
		const glm::vec2 viewportSize(static_cast<float>(width), static_cast<float>(height));
		GameUI::SetViewportSize(viewportSize);
		m_Scene->OnViewportResize(width, height);
		m_Renderer->SetViewportSize(width, height);

		const auto camera = GetPrimaryRenderCamera(*m_Scene, width, height);
		if (camera)
			m_Renderer->Render(*m_Scene, *camera);
		else if (!m_WarnedNoCamera)
		{
			BS_CORE_WARN("Runtime: the scene has no primary camera");
			m_WarnedNoCamera = true;
		}

		// Debug views show the renderer's raw buffers: no game UI over them.
		const bool drawUI = m_Options.DebugView == RendererDebugView::None;
		m_UIFrame.Build(drawUI ? m_Scene.get() : nullptr, viewportSize, drawUI);

		m_CommandList->open();
		m_UIFrame.Prepare([&](const std::string& path) { return m_Renderer->GetUITexture(path, m_CommandList); });
		if (camera)
			m_Renderer->Blit(m_CommandList, device->GetCurrentFramebuffer());
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
					if (WritePng(path, pixels, captureWidth, captureHeight, error))
					{
						BS_CORE_INFO("Runtime: screenshot saved to '{}'", path);
					}
					else
					{
						BS_CORE_ERROR("Runtime: {}", error);
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
		const GraphicsDevice* device = application.GetGraphicsDevice();
		if (m_Options.MaxFrames == 0 || !device)
			return;
		if (m_Renderer)
			CheckRendererResults(fail);
		if (device->GetValidationErrorCount() > 0)
		{
			BS_CORE_ERROR("Runtime: {} GPU validation error(s) occurred", device->GetValidationErrorCount());
			fail(3);
		}
		else if (m_Options.RequireValidation && !device->IsValidationActive())
		{
			BS_CORE_ERROR("Runtime: --require-validation given but the Vulkan validation layer is not active");
			fail(5);
		}
	}

	void RuntimeLayer::CheckRendererResults(const std::function<void(int)>& fail)
	{
		const bool timers = m_Renderer->HasGpuTimers();
		const GpuTimings& timings = m_Renderer->GetGpuTimings();
		const std::string text = FormatGpuTimings(timings, timers);
		// Results arrive GpuProfiler::Latency frames late, so a shorter run cannot have any.
		const bool tooShort = m_Options.MaxFrames <= GpuProfiler::Latency;
		if (timers && timings.SampleCount == 0 && tooShort)
			BS_CORE_INFO("Runtime: scene GPU time {} (the run is too short: results arrive {} frames late)", text, GpuProfiler::Latency);
		else if (timers && timings.SampleCount == 0)
			BS_CORE_WARN("Runtime: scene GPU time {}", text); // a whole run without results: a driver or profiler problem
		else
			BS_CORE_INFO("Runtime: scene GPU time {}", text);
		BS_CORE_INFO("Runtime: last frame rendered at {}x{} ({:.0f}% render scale)", m_Renderer->GetRenderWidth(), m_Renderer->GetRenderHeight(), m_Renderer->GetCurrentRenderScale() * 100.0f);

		const DynamicResolutionSampleCounts& samples = m_Renderer->GetDynamicResolutionSamples();
		if (samples.Rejected > 0 && samples.Accepted == 0)
			BS_CORE_WARN("Runtime: all {} GPU timer results dynamic resolution received were unusable (not positive and finite)", samples.Rejected);
		if (!m_Options.RequireDynamicResolution)
			return;
		std::string reason;
		if (!m_Renderer->IsDynamicResolutionActive())
			reason = DescribeDynamicResolutionState(m_Renderer->GetDynamicResolutionState(), m_Renderer->GetDynamicResolution(), m_Renderer->GetRenderScale());
		else if (tooShort)
			reason = fmt::format("the run is too short: GPU timer results arrive {} frames late, use more --frames", GpuProfiler::Latency);
		else if (timings.SampleCount == 0)
			reason = "no GPU timer results arrived";
		else if (samples.Accepted == 0)
			reason = fmt::format("none of the {} GPU timer results was usable", samples.Rejected);
		if (!reason.empty())
		{
			BS_CORE_ERROR("Runtime: --dynamic-resolution given but dynamic resolution did not run ({})", reason);
			fail(6);
		}
	}

}
