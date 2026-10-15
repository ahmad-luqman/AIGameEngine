#include "Basalt/Scene/VertexSnap.h"

#include "Basalt/Animation/AnimatedModel.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <cmath>
#include <limits>

namespace Basalt {

	std::optional<ViewportVertex> FindViewportVertex(Entity entity, const glm::mat4& viewProjection, const glm::vec2& size, const glm::vec2& pixel, float radius)
	{
		if (!entity || !std::isfinite(size.x) || !std::isfinite(size.y) || size.x <= 0 || size.y <= 0 || !std::isfinite(pixel.x) || !std::isfinite(pixel.y) || !std::isfinite(radius) || radius < 0 || pixel.x < 0 || pixel.y < 0 || pixel.x >= size.x || pixel.y >= size.y)
			return std::nullopt;
		std::optional<ViewportVertex> best;
		float bestDepth = std::numeric_limits<float>::max();
		auto visit = [&](const Ref<MeshSource>& source, uint32_t meshIndex, const glm::mat4& model, const std::vector<glm::mat4>& palette) {
			if (!source || meshIndex >= source->Meshes.size())
				return;
			for (uint32_t submeshIndex : source->Meshes[meshIndex].Submeshes)
			{
				const Submesh& submesh = source->Submeshes[submeshIndex];
				// Accessors may contain unused vertices; only indexed geometry offers snap targets.
				std::vector<bool> used(submesh.VertexCount, false);
				for (uint32_t i = 0; i < submesh.IndexCount; i++)
				{
					const uint32_t index = source->Indices[submesh.BaseIndex + i];
					if (index >= submesh.VertexCount || used[index])
						continue;
					used[index] = true;
					const Vertex& vertex = source->Vertices[submesh.BaseVertex + index];
					glm::vec4 local(vertex.Position, 1);
					if (!palette.empty())
					{
						glm::mat4 skin(0);
						float totalWeight = 0;
						for (int j = 0; j < 4; j++)
							if (vertex.Weights[j] > 0 && vertex.Joints[j] < palette.size())
							{
								skin += palette[vertex.Joints[j]] * vertex.Weights[j];
								totalWeight += vertex.Weights[j];
							}
						if (!(totalWeight > 0))
							continue;
						local = skin * local;
					}
					const glm::vec3 world(model * local);
					const glm::vec4 clip = viewProjection * glm::vec4(world, 1);
					if (!(clip.w > 0) || clip.z < 0 || clip.z > clip.w)
						continue;
					const glm::vec3 ndc = glm::vec3(clip) / clip.w;
					if (std::abs(ndc.x) > 1 || std::abs(ndc.y) > 1)
						continue;
					const glm::vec2 screen = glm::vec2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * size;
					const float distance = glm::length(screen - pixel);
					if (distance <= radius && (!best || distance < best->Distance || (distance == best->Distance && ndc.z < bestDepth)))
					{
						best = ViewportVertex{ entity.GetUUID(), world, screen, distance, ndc.z };
						bestDepth = ndc.z;
					}
				}
			}
		};
		if (const auto* mesh = entity.TryGetComponent<MeshComponent>())
			visit(AssetManager::GetMesh(mesh->Mesh), mesh->MeshIndex, entity.GetScene()->GetWorldTransform(entity), {});
		ForEachAnimatedMesh(entity, [&](const Ref<MeshSource>& source, const MeshNode& node, const glm::mat4& model, const std::vector<glm::mat4>& palette) { visit(source, static_cast<uint32_t>(node.MeshIndex), model, palette); });
		return best;
	}

	std::optional<ViewportVertex> FindVertexSnapTarget(Scene& scene, Entity moved, const glm::mat4& viewProjection, const glm::vec2& size, const glm::vec2& pixel, float radius)
	{
		std::optional<ViewportVertex> best;
		for (Entity entity : scene.GetAllEntitiesOrdered())
		{
			if (entity == moved || scene.IsDescendantOf(entity, moved))
				continue;
			auto candidate = FindViewportVertex(entity, viewProjection, size, pixel, radius);
			if (candidate && (!best || candidate->Distance < best->Distance || (candidate->Distance == best->Distance && candidate->Depth < best->Depth)))
				best = candidate;
		}
		return best;
	}

	std::optional<glm::vec3> VertexSnapTranslation(Scene& scene, Entity entity, const glm::vec3& source, const glm::vec3& target)
	{
		if (!entity || entity.GetScene() != &scene)
			return std::nullopt;
		glm::mat4 parent(1);
		if (entity.GetParent())
			parent = scene.GetWorldTransform(entity.GetParent());
		const float determinant = glm::determinant(parent);
		if (!std::isfinite(determinant) || determinant == 0)
			return std::nullopt;
		const glm::vec3 translation = entity.GetTransform().Translation + glm::vec3(glm::inverse(parent) * glm::vec4(target - source, 0));
		for (int i = 0; i < 3; i++)
			if (!std::isfinite(translation[i]))
				return std::nullopt;
		return translation;
	}

}
