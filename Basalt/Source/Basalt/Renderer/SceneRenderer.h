#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Renderer/DynamicResolution.h"
#include "Basalt/Renderer/GpuProfiler.h"
#include "Basalt/Renderer/RenderResources.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <optional>
#include <vector>

namespace Basalt {

	class Scene;

	struct RenderCamera
	{
		glm::mat4 View = glm::mat4(1.0f);
		// nvrhi clip space (depth 0..1, +Y up), as produced by SceneCamera / EditorCamera.
		glm::mat4 Projection = glm::mat4(1.0f);
		glm::vec3 Position = { 0.0f, 0.0f, 0.0f };
		float Near = 0.1f;
		float Far = 1000.0f;
	};

	struct SceneRendererStatistics
	{
		uint32_t DrawCalls = 0;
		uint32_t ShadowDrawCalls = 0;
		uint32_t CulledSubmeshes = 0;
		uint32_t Lights = 0;
		uint32_t Materials = 0;
	};

	struct SceneRendererSpecification
	{
		// Time the passes with GPU timer queries (GraphicsDevice::SupportsTimerQueries). Needed for
		// GetGpuTimings and dynamic resolution; renderers whose output must not depend on GPU speed (the
		// editor's screenshots) leave it off.
		bool GpuTimers = false;
	};

	// Renders a Scene from a camera into an internal LDR image:
	//   cascaded soft shadow maps -> depth/normal prepass -> SSAO (+ bilateral blur) -> forward PBR with
	//   IBL (opaque) -> skybox -> transparent PBR (sorted) -> exposure + tonemapping + sRGB encode.
	// The scene renders at the viewport size times the render scale; the tonemap pass resamples it to the
	// viewport size. With dynamic resolution the scale follows the GPU time measured by the renderer's
	// GpuProfiler; the targets keep the size of the configured render scale and the scene renders into
	// their top-left corner, so scale changes never reallocate. The output texture can be shown through
	// ImGui (editor viewport) or blitted to the swapchain.
	class SceneRenderer
	{
	public:
		explicit SceneRenderer(nvrhi::IDevice* device, const SceneRendererSpecification& specification = {});
		~SceneRenderer();

		SceneRenderer(const SceneRenderer&) = delete;
		SceneRenderer& operator=(const SceneRenderer&) = delete;

		// Creates pipelines and constant resources. Returns false (and logs) on failure.
		bool Initialize();

		void SetViewportSize(uint32_t width, uint32_t height);
		uint32_t GetViewportWidth() const { return m_Width; }
		uint32_t GetViewportHeight() const { return m_Height; }
		// Fraction of the viewport size the scene renders at, clamped to [MinRenderScale, MaxRenderScale]
		// (RenderMath.h); the highest scale when dynamic resolution is on. Takes effect at the next
		// SetViewportSize or Render, which recreate the targets.
		void SetRenderScale(float scale);
		float GetRenderScale() const { return m_RenderScale; }
		// Lets the render scale drop (down to the minimum) to keep the GPU time within the target frame time.
		// Values are sanitized (SanitizeDynamicResolution). Inactive, with a warning, without GPU timers or when
		// the minimum is not below the render scale.
		void SetDynamicResolution(const DynamicResolutionSettings& settings);
		const DynamicResolutionSettings& GetDynamicResolution() const { return m_DynamicResolution; }
		bool IsDynamicResolutionActive() const;
		// Scale requested for the last frame (below the render scale when dynamic resolution lowered it; the
		// render scale after a resize). GetRenderWidth/Height give the exact size of the area rendered.
		float GetCurrentRenderScale() const { return m_CurrentRenderScale; }
		uint32_t GetRenderWidth() const { return m_RenderWidth; }
		uint32_t GetRenderHeight() const { return m_RenderHeight; }

		// Records and submits all passes for one frame.
		void Render(Scene& scene, const RenderCamera& camera);

		// RGBA8 display-referred image of the last Render (shader resource state), at the viewport size.
		nvrhi::ITexture* GetOutputTexture() const { return m_OutputTexture; }
		// Scene depth of the last frame (D32). The scene covers its top-left GetRenderWidth() x GetRenderHeight()
		// texels; the texture is larger while dynamic resolution renders below the render scale.
		nvrhi::ITexture* GetDepthTexture() const { return m_DepthTexture; }
		// Framebuffer with the output color and the scene depth at the viewport size attached (for overlays).
		nvrhi::IFramebuffer* GetOverlayFramebuffer() const { return m_OverlayFramebuffer; }

		// Draws the output image stretched over the target framebuffer.
		void Blit(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* target);

		// Copies the last output image to CPU memory (RGBA8, top row first). Waits for the GPU.
		bool ReadOutputPixels(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight);

		const SceneRendererStatistics& GetStatistics() const { return m_Statistics; }
		// GPU time per pass, averaged over the last GpuProfiler::AverageWindow frames. SampleCount is 0 (every
		// pass listed at 0 ms) until results arrive, and always without GPU timers.
		const GpuTimings& GetGpuTimings() const { return m_Profiler->GetTimings(); }
		bool HasGpuTimers() const { return m_Profiler->IsEnabled(); }
		RenderResources& GetResources() { return *m_Resources; }

	private:
		void ResizeTargets(uint32_t width, uint32_t height, float renderScale);
		// Picks this frame's render size from the configured scale and, with dynamic resolution, the GPU time
		// of an earlier frame.
		void UpdateRenderSize(const std::optional<GpuFrameSample>& sample);
		// Warns (once per change of state) when dynamic resolution is enabled but cannot adjust anything.
		void CheckDynamicResolution();

		struct Impl;
		Scope<Impl> m_Impl;
		nvrhi::IDevice* m_Device = nullptr;
		Scope<RenderResources> m_Resources;
		Scope<GpuProfiler> m_Profiler;

		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		float m_RenderScale = 1.0f;
		// Size of the scene targets: the viewport size times the render scale.
		uint32_t m_TargetWidth = 0;
		uint32_t m_TargetHeight = 0;
		// The targets were created for dynamic resolution (overlay depth and resampling even at full size).
		bool m_TargetsDynamic = false;
		// Area of the targets the scene renders into this frame.
		uint32_t m_RenderWidth = 0;
		uint32_t m_RenderHeight = 0;
		float m_CurrentRenderScale = 1.0f;
		DynamicResolutionSettings m_DynamicResolution;
		DynamicResolutionController m_DynamicController;
		bool m_WarnedNoGpuTimers = false;
		bool m_WarnedEmptyRange = false;
		// Samples in a row the controller rejected (zero or broken GPU times); warned about once.
		uint32_t m_UnusableSamples = 0;
		bool m_WarnedUnusableSamples = false;
		nvrhi::TextureHandle m_OutputTexture;
		nvrhi::TextureHandle m_DepthTexture;
		nvrhi::FramebufferHandle m_OverlayFramebuffer;
		SceneRendererStatistics m_Statistics;
	};

}
