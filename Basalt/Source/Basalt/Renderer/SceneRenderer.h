#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Renderer/RenderResources.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

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

	// Renders a Scene from a camera into an internal LDR image:
	//   cascaded soft shadow maps -> depth/normal prepass -> SSAO (+ bilateral blur) -> forward PBR with
	//   IBL (opaque) -> skybox -> transparent PBR (sorted) -> exposure + tonemapping + sRGB encode.
	// The output texture can be shown through ImGui (editor viewport) or blitted to the swapchain.
	class SceneRenderer
	{
	public:
		explicit SceneRenderer(nvrhi::IDevice* device);
		~SceneRenderer();

		SceneRenderer(const SceneRenderer&) = delete;
		SceneRenderer& operator=(const SceneRenderer&) = delete;

		// Creates pipelines and constant resources. Returns false (and logs) on failure.
		bool Initialize();

		void SetViewportSize(uint32_t width, uint32_t height);
		uint32_t GetViewportWidth() const { return m_Width; }
		uint32_t GetViewportHeight() const { return m_Height; }

		// Records and submits all passes for one frame.
		void Render(Scene& scene, const RenderCamera& camera);

		// RGBA8 display-referred image of the last Render (shader resource state).
		nvrhi::ITexture* GetOutputTexture() const { return m_OutputTexture; }
		// Depth of the last frame (D32), useful for editor overlays drawn on the output.
		nvrhi::ITexture* GetDepthTexture() const { return m_DepthTexture; }
		// Framebuffer with the output color and scene depth attached (for overlays).
		nvrhi::IFramebuffer* GetOverlayFramebuffer() const { return m_OverlayFramebuffer; }

		// Draws the output image stretched over the target framebuffer.
		void Blit(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* target);

		// Copies the last output image to CPU memory (RGBA8, top row first). Waits for the GPU.
		bool ReadOutputPixels(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight);

		const SceneRendererStatistics& GetStatistics() const { return m_Statistics; }
		RenderResources& GetResources() { return *m_Resources; }

	private:
		struct Impl;
		Scope<Impl> m_Impl;
		nvrhi::IDevice* m_Device = nullptr;
		Scope<RenderResources> m_Resources;

		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		nvrhi::TextureHandle m_OutputTexture;
		nvrhi::TextureHandle m_DepthTexture;
		nvrhi::FramebufferHandle m_OverlayFramebuffer;
		SceneRendererStatistics m_Statistics;
	};

}
