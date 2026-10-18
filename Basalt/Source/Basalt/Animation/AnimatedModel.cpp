#include "Basalt/Animation/AnimatedModel.h"

#include "Basalt/Animation/AnimationSystem.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

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
				const auto findClip = [&](const std::string& name) -> const AnimationClip* {
					auto it = std::ranges::find(source->Animations, name, &AnimationClip::Name);
					return it != source->Animations.end() ? &*it : nullptr;
				};
				const bool namesSpace = std::ranges::find(animator->BlendSpaces, animator->Clip, &AnimationBlendSpace::Name) != animator->BlendSpaces.end();
				if (!animator->Clip.empty() && !findClip(animator->Clip) && !namesSpace)
					warnings.push_back("Animator clip '" + animator->Clip + "' not found in '" + animator->Mesh + "'");
				for (const AnimationEvent& event : animator->Events)
				{
					if (event.Clip.empty())
						continue;
					if (const AnimationClip* clip = findClip(event.Clip); !clip)
						warnings.push_back(fmt::format("Animator event '{}': clip '{}' not found", event.Name, event.Clip));
					else if (event.Time > clip->Duration)
						warnings.push_back(fmt::format("Animator event '{}' at {}s is past the end of clip '{}' ({}s) and never fires", event.Name, event.Time, event.Clip, clip->Duration));
				}
				for (const AnimationBlendSpace& space : animator->BlendSpaces)
				{
					if (space.Samples.empty())
						warnings.push_back("Animator blend space '" + space.Name + "' has no samples");
					for (const AnimationBlendSample& sample : space.Samples)
						if (!sample.Clip.empty() && !findClip(sample.Clip))
							warnings.push_back("Animator blend space '" + space.Name + "': clip '" + sample.Clip + "' not found");
				}
			}
			else
				warnings.push_back("Animator asset could not be loaded: " + AssetManager::GetError(animator->Mesh));
		}
		if (const auto* attachment = entity.TryGetComponent<BoneAttachmentComponent>())
		{
			Scene& scene = *entity.GetScene();
			const Entity target = attachment->Target != 0 ? scene.GetEntityByUUID(attachment->Target) : entity.GetParent();
			const auto* targetAnimator = target ? target.TryGetComponent<AnimatorComponent>() : nullptr;
			if (!target)
				warnings.emplace_back(attachment->Target != 0 ? "BoneAttachment Target entity not found" : "BoneAttachment has no Target and no parent");
			else if (target == entity)
				warnings.emplace_back("BoneAttachment cannot attach an entity to itself");
			else if (!targetAnimator)
				warnings.push_back("BoneAttachment Target '" + target.GetName() + "' has no Animator");
			else if (const auto source = AssetManager::GetMesh(targetAnimator->Mesh); source && std::ranges::find(source->Nodes, attachment->Joint, &MeshNode::Name) == source->Nodes.end())
				warnings.push_back("BoneAttachment joint '" + attachment->Joint + "' not found in '" + targetAnimator->Mesh + "'");
			if (const auto* body = entity.TryGetComponent<RigidBodyComponent>(); body && body->Type == RigidBodyType::Dynamic)
				warnings.emplace_back("BoneAttachment on a Dynamic RigidBody: physics moves it too; make it Kinematic");
		}
		return warnings;
	}
}
