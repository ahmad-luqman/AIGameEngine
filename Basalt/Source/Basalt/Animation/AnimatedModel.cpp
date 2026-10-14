#include "Basalt/Animation/AnimatedModel.h"

#include "Basalt/Animation/AnimationSystem.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Scene/Entity.h"

#include <algorithm>

namespace Basalt {
	void ForEachAnimatedMesh(Entity entity, const std::function<void(const Ref<MeshSource>&, const MeshNode&, const glm::mat4&, const std::vector<glm::mat4>&)>& visit)
	{
		const auto* animator = entity.TryGetComponent<AnimatorComponent>();
		const auto source = animator ? AssetManager::GetMesh(animator->Mesh) : nullptr;
		if (!source || source->AnimationUnsupported)
			return;
		auto& scene = *entity.GetScene();
		std::vector<glm::mat4> rest;
		const auto* globals = scene.GetAnimationSystem() ? scene.GetAnimationSystem()->GetPose(entity) : nullptr;
		if (!globals)
		{
			const bool valid = EvaluateAnimation(*source, nullptr, 0, rest);
			BS_CORE_ASSERT(valid, "Imported rest pose must be finite");
			if (!valid)
				return;
			globals = &rest;
		}
		const glm::mat4 root = scene.GetWorldTransform(entity);
		std::vector<std::vector<glm::mat4>> palettes(source->Skins.size());
		for (size_t i = 0; i < source->Nodes.size(); i++)
		{
			const auto& node = source->Nodes[i];
			if (!node.Active || node.MeshIndex < 0)
				continue;
			if (node.SkinIndex < 0)
				visit(source, node, root * (*globals)[i], {});
			else
			{
				auto& palette = palettes[node.SkinIndex];
				if (palette.empty())
					palette = BuildSkinPalette(source->Skins[node.SkinIndex], *globals);
				BS_CORE_ASSERT(!palette.empty(), "Validated animation pose must have a finite skin palette");
				if (!palette.empty())
					visit(source, node, root, palette);
			}
		}
	}

	AABB AnimatedModelBounds(Entity entity)
	{
		AABB bounds;
		ForEachAnimatedMesh(entity, [&](const Ref<MeshSource>& source, const MeshNode& node, const glm::mat4& model, const std::vector<glm::mat4>& palette) {
			const auto& mesh = source->Meshes[node.MeshIndex];
			if (palette.empty())
				bounds.Expand(mesh.Bounds.Transformed(model));
			else
				for (uint32_t submesh : mesh.Submeshes)
					bounds.Expand(SkinBounds(*source, source->Submeshes[submesh], palette).Transformed(model));
		});
		return bounds;
	}

	std::vector<std::string> GetAnimationWarnings(Entity entity)
	{
		std::vector<std::string> warnings;
		const auto* animator = entity.TryGetComponent<AnimatorComponent>();
		const auto* mesh = entity.TryGetComponent<MeshComponent>();
		if (animator && mesh)
			warnings.emplace_back("Animator and Mesh both render on this entity; remove one to avoid duplicate geometry");
		if (mesh)
			if (const auto source = AssetManager::GetMesh(mesh->Mesh); source && !source->Skins.empty())
				warnings.emplace_back("Mesh references a skinned model and displays undeformed vertices; use Animator for skinning");
		if (animator)
		{
			if (animator->Mesh.empty())
				warnings.emplace_back("Animator Mesh is empty");
			else if (const auto source = AssetManager::GetMesh(animator->Mesh))
			{
				if (source->AnimationUnsupported)
					warnings.emplace_back("Animator cannot animate this model's extra influence sets; static geometry is available through Mesh");
				if (!animator->Clip.empty() && std::none_of(source->Animations.begin(), source->Animations.end(), [&](const AnimationClip& clip) { return clip.Name == animator->Clip; }))
					warnings.push_back("Animator clip '" + animator->Clip + "' not found in '" + animator->Mesh + "'");
			}
			else
				warnings.push_back("Animator asset could not be loaded: " + AssetManager::GetError(animator->Mesh));
		}
		return warnings;
	}
}
