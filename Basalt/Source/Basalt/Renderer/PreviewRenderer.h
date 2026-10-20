#pragma once

#include "Basalt/Asset/Thumbnails.h"
#include "Basalt/Core/Base.h"
#include "Basalt/Scene/Components.h"

#include <nvrhi/nvrhi.h>

#include <optional>
#include <string>

namespace Basalt {

	class Scene;
	class SceneRenderer;
	struct MaterialData;

	// Renders small studio previews for the editor: a model or prefab framed by its bounds, or a material on a
	// sphere, under the procedural sky and one key light. Each preview is rendered at RenderSize and averaged
	// down to OutputSize (2x supersampling). One renderer of a fixed size, never resized, so previews never
	// reallocate render targets; GPU copies of the assets are released after each preview.
	class PreviewRenderer
	{
	public:
		static constexpr uint32_t OutputSize = 128;
		static constexpr uint32_t RenderSize = OutputSize * 2;

		explicit PreviewRenderer(nvrhi::IDevice* device);
		~PreviewRenderer();
		PreviewRenderer(const PreviewRenderer&) = delete;
		PreviewRenderer& operator=(const PreviewRenderer&) = delete;

		bool Initialize();

		// meshKey: a model file or builtin:// mesh (AssetManager keys). Every mesh of the model is shown.
		std::optional<ThumbnailImage> RenderModel(const std::string& meshKey, std::string& outError);
		// prefabPath: project-relative .bprefab.
		std::optional<ThumbnailImage> RenderPrefab(const std::string& prefabPath, std::string& outError);
		// A material component on a sphere. A Material asset that does not load fails with its error rather than showing
		// the inline fallback. Material files and models' materials ("<model>#material/<n>") preview this way losslessly.
		std::optional<ThumbnailImage> RenderMaterial(const MaterialComponent& material, std::string& outError);
		// A material by value (a material file's thumbnail, a model's imported material, an editor draft) on the same
		// sphere, losslessly; fails when MaterialAsset::Validate rejects it.
		std::optional<ThumbnailImage> RenderMaterial(const MaterialData& material, std::string& outError);

	private:
		bool IsReady(std::string& outError) const;
		std::optional<ThumbnailImage> RenderFramed(Scene& scene, std::string& outError);
		std::optional<ThumbnailImage> RenderSphere(const MaterialComponent& material, std::string& outError);
		std::optional<ThumbnailImage> Capture(Scene& scene, const glm::vec3& target, float radius, std::string& outError);

		nvrhi::IDevice* m_Device = nullptr;
		Scope<SceneRenderer> m_Renderer;
	};

}
