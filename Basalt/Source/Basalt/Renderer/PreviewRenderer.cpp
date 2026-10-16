#include "Basalt/Renderer/PreviewRenderer.h"

#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Asset/MeshSource.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Renderer/DebugDraw.h"
#include "Basalt/Renderer/SceneRenderer.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scene/SceneSerializer.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace Basalt {

	namespace {

		// Vertical field of view of the preview camera; narrow, so models are not distorted.
		constexpr float FieldOfView = glm::radians(30.0f);
		// The camera looks from the front-right, slightly above, as asset browsers usually do.
		const glm::vec3 s_ViewDirection = glm::normalize(glm::vec3(0.8f, 0.55f, 1.0f));

		// A scene with the studio lighting: the procedural sky (and its IBL) and one key light.
		void AddStudio(Scene& scene)
		{
			Entity sun = scene.CreateEntity("PreviewSun");
			sun.AddComponent<DirectionalLightComponent>().Intensity = 3.0f;
			sun.GetTransform().SetRotationEuler(glm::radians(glm::vec3(-40.0f, 35.0f, 0.0f)));
		}

		// World bounds of every mesh and animated model in the scene (Edit mode: rest poses).
		AABB SceneBounds(Scene& scene)
		{
			AABB bounds;
			for (auto handle : scene.GetAllEntitiesWith<MeshComponent>())
			{
				Entity entity(handle, &scene);
				const auto& component = entity.GetComponent<MeshComponent>();
				const Ref<MeshSource> mesh = AssetManager::GetMesh(component.Mesh);
				if (mesh && component.MeshIndex < mesh->Meshes.size())
					bounds.Expand(mesh->Meshes[component.MeshIndex].Bounds.Transformed(scene.GetWorldTransform(entity)));
			}
			for (auto handle : scene.GetAllEntitiesWith<AnimatorComponent>())
			{
				Entity entity(handle, &scene);
				if (const Ref<MeshSource> mesh = AssetManager::GetMesh(entity.GetComponent<AnimatorComponent>().Mesh))
					bounds.Expand(mesh->Bounds.Transformed(scene.GetWorldTransform(entity)));
			}
			return bounds;
		}

	}

	MaterialComponent ToMaterialComponent(const MaterialData& material)
	{
		MaterialComponent component;
		component.AlbedoColor = material.AlbedoColor;
		component.Metallic = material.Metallic;
		component.Roughness = material.Roughness;
		component.EmissiveColor = material.EmissiveColor;
		component.EmissiveIntensity = material.EmissiveIntensity;
		component.AlbedoMap = material.AlbedoMap;
		component.NormalMap = material.NormalMap;
		component.MetallicRoughnessMap = material.MetallicRoughnessMap;
		component.OcclusionMap = material.OcclusionMap;
		component.EmissiveMap = material.EmissiveMap;
		return component;
	}

	PreviewRenderer::PreviewRenderer(nvrhi::IDevice* device)
		: m_Device(device)
	{
	}

	PreviewRenderer::~PreviewRenderer() = default;

	bool PreviewRenderer::Initialize()
	{
		// No GPU timers (previews never use dynamic resolution) and no picking.
		m_Renderer = CreateScope<SceneRenderer>(m_Device, SceneRendererSpecification{});
		if (!m_Renderer->Initialize())
		{
			m_Renderer.reset();
			return false;
		}
		m_Renderer->SetViewportSize(RenderSize, RenderSize);
		return true;
	}

	std::optional<ThumbnailImage> PreviewRenderer::RenderModel(const std::string& meshKey, std::string& outError)
	{
		Scene scene("ModelPreview");
		AddStudio(scene);
		if (!AssetManager::ImportModel(scene, meshKey, {}, outError))
			return std::nullopt;
		return RenderFramed(scene, outError);
	}

	std::optional<ThumbnailImage> PreviewRenderer::RenderPrefab(const std::string& prefabPath, std::string& outError)
	{
		Scene scene("PrefabPreview");
		AddStudio(scene);
		if (!SceneSerializer::InstantiatePrefab(scene, Project::ResolvePath(prefabPath), prefabPath, {}, outError))
			return std::nullopt;
		return RenderFramed(scene, outError);
	}

	std::optional<ThumbnailImage> PreviewRenderer::RenderMaterial(const MaterialComponent& material, std::string& outError)
	{
		Scene scene("MaterialPreview");
		AddStudio(scene);
		Entity sphere = scene.CreateEntity("Sphere");
		sphere.AddComponent<MeshComponent>().Mesh = "builtin://Sphere";
		sphere.AddComponent<MaterialComponent>() = material;
		// The unit sphere's radius is 0.5; a little margin keeps its silhouette clear of the edges.
		return Capture(scene, glm::vec3(0.0f), 0.56f, outError);
	}

	std::optional<ThumbnailImage> PreviewRenderer::RenderFramed(Scene& scene, std::string& outError)
	{
		const AABB bounds = SceneBounds(scene);
		if (!bounds.IsValid())
		{
			outError = "nothing to show (no meshes)";
			return std::nullopt;
		}
		// The bounding sphere fills the view; a flat model (radius from one axis only) still gets a sensible size.
		const float radius = std::max(glm::length(bounds.GetExtents()), 1e-3f);
		return Capture(scene, bounds.GetCenter(), radius, outError);
	}

	std::optional<ThumbnailImage> PreviewRenderer::Capture(Scene& scene, const glm::vec3& target, float radius, std::string& outError)
	{
		if (!m_Renderer)
		{
			outError = "the preview renderer is not initialized";
			return std::nullopt;
		}
		const float distance = radius / std::sin(FieldOfView * 0.5f);
		RenderCamera camera;
		camera.Position = target + s_ViewDirection * distance;
		camera.Near = std::max(distance - radius * 1.5f, distance * 0.01f);
		camera.Far = distance + radius * 1.5f;
		camera.View = glm::lookAt(camera.Position, target, glm::vec3(0.0f, 1.0f, 0.0f));
		camera.Projection = glm::perspective(FieldOfView, 1.0f, camera.Near, camera.Far);

		// Previews are rendered between frames: keep the viewport's pending debug lines out of the image.
		{
			const DebugLineStash stash;
			// A fresh scene each time, so every preview starts from the same exposure.
			m_Renderer->ResetExposure();
			m_Renderer->Render(scene, camera, 0.0f);
		}
		std::vector<uint8_t> pixels;
		uint32_t width = 0, height = 0;
		const bool failed = m_Renderer->GetStatistics().Failed;
		const bool read = !failed && m_Renderer->ReadOutputPixels(pixels, width, height);
		// Previews visit many assets once; keep no GPU copies of them.
		m_Renderer->GetResources().Clear();
		if (failed || !read)
		{
			outError = failed ? "the preview render failed; see the renderer error in the log" : "cannot read the preview back from the GPU";
			return std::nullopt;
		}
		return DownsampleImage(pixels, width, height, OutputSize, OutputSize);
	}

}
