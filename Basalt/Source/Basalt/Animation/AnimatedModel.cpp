#include "Basalt/Animation/AnimatedModel.h"

#include "Basalt/Animation/AnimationSystem.h"
#include "Basalt/Animation/InverseKinematics.h"
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

	Entity GetBoneAttachmentTarget(Entity entity)
	{
		const UUID target = entity.GetComponent<BoneAttachmentComponent>().Target;
		return target != 0 ? entity.GetScene()->GetEntityByUUID(target) : entity.GetParent();
	}

	namespace {
		void AddAnimatorWarnings(const AnimatorComponent& animator, std::vector<std::string>& warnings)
		{
			if (animator.Mesh.empty())
			{
				warnings.emplace_back("Animator Mesh is empty");
				return;
			}
			const auto source = AssetManager::GetMesh(animator.Mesh);
			if (!source)
			{
				warnings.push_back("Animator asset could not be loaded: " + AssetManager::GetError(animator.Mesh));
				return;
			}
			if (source->AnimationUnsupported)
				warnings.emplace_back("Animator cannot animate this model's extra influence sets; static geometry is available through Mesh");
			const auto findClip = [&](const std::string& name) -> const AnimationClip* {
				auto it = std::ranges::find(source->Animations, name, &AnimationClip::Name);
				return it != source->Animations.end() ? &*it : nullptr;
			};
			if (animator.RootMotion && (animator.RootJoint.empty() || FindNode(*source, animator.RootJoint) < 0))
				warnings.emplace_back("Animator RootJoint must name a model node for root motion");
			if (!animator.StateMachine.empty())
			{
				const auto graph = AssetManager::GetAnimationGraph(animator.StateMachine);
				if (!graph)
					warnings.push_back("Animator state machine could not be loaded: " + AssetManager::GetError(animator.StateMachine));
				else
					for (const auto& state : graph->States)
					{
						if (state.BlendSpace.empty())
						{
							if (state.Clip.empty() ? source->Animations.empty() : !findClip(state.Clip))
								warnings.push_back(fmt::format("Animator state '{}': clip '{}' not found", state.Name, state.Clip));
						}
						else if (std::ranges::find(animator.BlendSpaces, state.BlendSpace, &AnimationBlendSpace::Name) == animator.BlendSpaces.end())
							warnings.push_back(fmt::format("Animator state '{}': blend space '{}' not found", state.Name, state.BlendSpace));
					}
			}
			const bool namesSpace = std::ranges::find(animator.BlendSpaces, animator.Clip, &AnimationBlendSpace::Name) != animator.BlendSpaces.end();
			if (!animator.Clip.empty() && !findClip(animator.Clip) && !namesSpace)
				warnings.push_back(fmt::format("Animator clip '{}' not found in '{}'", animator.Clip, animator.Mesh));
			if (source->Animations.empty() && (!animator.Events.empty() || !animator.BlendSpaces.empty()))
				warnings.push_back(fmt::format("Animator model '{}' has no animation clips, so its events and blend spaces never play", animator.Mesh));
			float longest = 0;
			for (const AnimationClip& clip : source->Animations)
				longest = std::max(longest, clip.Duration);
			for (const AnimationEvent& event : animator.Events)
			{
				if (event.Clip.empty())
				{
					if (!source->Animations.empty() && event.Time > longest)
						warnings.push_back(fmt::format("Animator event '{}' at {}s is past the end of every clip and never fires", event.Name, event.Time));
				}
				else if (const AnimationClip* clip = findClip(event.Clip); !clip)
					warnings.push_back(fmt::format("Animator event '{}': clip '{}' not found", event.Name, event.Clip));
				else if (clip->Duration <= 0)
					warnings.push_back(fmt::format("Animator event '{}': clip '{}' lasts 0 s, so its events never fire", event.Name, event.Clip));
				else if (event.Time > clip->Duration)
					warnings.push_back(fmt::format("Animator event '{}' at {}s is past the end of clip '{}' ({}s) and never fires", event.Name, event.Time, event.Clip, clip->Duration));
			}
			bool knownSpeed = false;
			for (const AnimationBlendSpace& space : animator.BlendSpaces)
			{
				if (findClip(space.Name))
					warnings.push_back(fmt::format("Animator blend space '{}' has the name of a clip; Clip selects the clip", space.Name));
				if (space.Samples.empty())
					warnings.push_back(fmt::format("Animator blend space '{}' has no samples", space.Name));
				for (const AnimationBlendSample& sample : space.Samples)
				{
					knownSpeed = knownSpeed || sample.Speed > 0;
					if (!sample.Clip.empty() && !findClip(sample.Clip))
						warnings.push_back(fmt::format("Animator blend space '{}': clip '{}' not found", space.Name, sample.Clip));
				}
			}
			if (animator.MatchVelocity && !knownSpeed)
				warnings.emplace_back("Animator MatchVelocity has no effect: no blend space sample has a Speed");
			for (const AnimationLayer& layer : animator.Layers)
			{
				if (!layer.Clip.empty() && !findClip(layer.Clip))
					warnings.push_back(fmt::format("Animator layer '{}': clip '{}' not found", layer.Name, layer.Clip));
				if (layer.Blend == AnimationLayerBlend::Additive && !layer.ReferenceClip.empty() && !findClip(layer.ReferenceClip))
					warnings.push_back(fmt::format("Animator layer '{}': reference clip '{}' not found", layer.Name, layer.ReferenceClip));
				if (layer.Blend == AnimationLayerBlend::Override && !layer.ReferenceClip.empty())
					warnings.push_back(fmt::format("Animator layer '{}': ReferenceClip only applies to Additive layers", layer.Name));
				for (const AnimationMaskJoint& joint : layer.Mask)
				{
					if (joint.Joint.empty())
						warnings.push_back(fmt::format("Animator layer '{}': a mask entry has no Joint and is ignored", layer.Name));
					else if (FindNode(*source, joint.Joint) < 0)
						warnings.push_back(fmt::format("Animator layer '{}': mask joint '{}' not found", layer.Name, joint.Joint));
				}
			}
		}

		void AddInverseKinematicsWarnings(Entity entity, const InverseKinematicsComponent& ik, std::vector<std::string>& warnings)
		{
			const auto* animator = entity.TryGetComponent<AnimatorComponent>();
			if (!animator)
			{
				warnings.emplace_back("InverseKinematics needs an Animator on the same entity");
				return;
			}
			for (const IKChain& chain : ik.Chains)
			{
				if (chain.Type == IKChainType::LookAt && glm::length(chain.Pole) > 0)
					warnings.push_back(fmt::format("InverseKinematics chain '{}': Pole only applies to TwoBone chains", chain.Name));
				if (chain.Type == IKChainType::TwoBone && chain.ChainLength != 1)
					warnings.push_back(fmt::format("InverseKinematics chain '{}': ChainLength only applies to LookAt chains", chain.Name));
				if (chain.Type == IKChainType::LookAt && chain.MaxAngle == 0)
					warnings.push_back(fmt::format("InverseKinematics chain '{}': MaxAngle 0 never turns the chain", chain.Name));
			}
			// AddAnimatorWarnings reports a model that cannot load.
			const auto source = animator->Mesh.empty() ? nullptr : AssetManager::GetMesh(animator->Mesh);
			std::vector<uint32_t> joints;
			std::vector<uint32_t> groundUppers;
			std::vector<std::pair<uint32_t, const IKChain*>> ends;
			for (const IKChain& chain : ik.Chains)
			{
				if (chain.Joint.empty())
				{
					warnings.push_back(fmt::format("InverseKinematics chain '{}': Joint is empty", chain.Name));
					continue;
				}
				if (!source)
					continue;
				if (std::string problem = ResolveIKChain(*source, chain, joints); !problem.empty())
				{
					warnings.push_back(fmt::format("InverseKinematics chain '{}': {}", chain.Name, problem));
					continue;
				}
				if (chain.Ground && chain.Type == IKChainType::TwoBone)
					groundUppers.push_back(joints.front());
				// Chains solve in order, so a later one driving the same joint overrides the earlier one.
				const uint32_t end = chain.Type == IKChainType::TwoBone ? joints.back() : joints.front();
				if (const auto other = std::ranges::find(ends, end, &std::pair<uint32_t, const IKChain*>::first); other != ends.end())
					warnings.push_back(fmt::format("InverseKinematics chains '{}' and '{}' both drive joint '{}'; the later one wins", other->second->Name, chain.Name, chain.Joint));
				else
					ends.emplace_back(end, &chain);
			}
			if (ik.Pelvis.empty())
				return;
			if (std::ranges::none_of(ik.Chains, &IKChain::Ground))
				warnings.emplace_back("InverseKinematics Pelvis has no effect: no chain is a Ground chain");
			if (!source)
				return;
			const int32_t pelvis = FindNode(*source, ik.Pelvis);
			if (pelvis < 0)
				warnings.push_back(fmt::format("InverseKinematics Pelvis joint '{}' not found", ik.Pelvis));
			else if (source->Nodes[pelvis].HasMatrix)
				warnings.push_back(fmt::format("InverseKinematics Pelvis joint '{}' has a fixed matrix and cannot move", ik.Pelvis));
			else
				for (uint32_t upper : groundUppers)
				{
					if (upper != static_cast<uint32_t>(pelvis) && !IsAncestor(*source, static_cast<uint32_t>(pelvis), upper))
						warnings.push_back(fmt::format("InverseKinematics Pelvis joint '{}' is not above the leg starting at '{}', so lowering it does not help that foot reach", ik.Pelvis, source->Nodes[upper].Name));
				}
		}

		void AddBoneAttachmentWarnings(Entity entity, const BoneAttachmentComponent& attachment, std::vector<std::string>& warnings)
		{
			Scene& scene = *entity.GetScene();
			const Entity target = GetBoneAttachmentTarget(entity);
			const auto* animator = target ? target.TryGetComponent<AnimatorComponent>() : nullptr;
			const auto source = animator ? AssetManager::GetMesh(animator->Mesh) : nullptr;
			// The attachments this one waits for, as UpdateAttachments orders them: those on its target and the target's
			// ancestors, and on its own ancestors. Reaching this entity again through them is a cycle (the runtime
			// leaves such attachments in place).
			const auto dependencies = [](Entity attached) {
				std::vector<Entity> found;
				for (Entity start : { GetBoneAttachmentTarget(attached), attached.GetParent() })
					for (Entity next = start; next; next = next.GetParent())
						if (next != attached && next.HasComponent<BoneAttachmentComponent>())
							found.push_back(next);
				return found;
			};
			bool cycle = false;
			std::vector<Entity> visited;
			std::vector<Entity> pending = dependencies(entity);
			while (!pending.empty() && !cycle)
			{
				const Entity next = pending.back();
				pending.pop_back();
				if (std::ranges::find(visited, next) != visited.end())
					continue;
				visited.push_back(next);
				for (Entity dependency : dependencies(next))
				{
					cycle = cycle || dependency == entity;
					pending.push_back(dependency);
				}
			}
			if (!target)
				warnings.emplace_back(attachment.Target != 0 ? "BoneAttachment Target entity not found" : "BoneAttachment has no Target and no parent");
			else if (target == entity)
				warnings.emplace_back("BoneAttachment cannot attach an entity to itself");
			else if (scene.IsDescendantOf(target, entity))
				warnings.push_back(fmt::format("BoneAttachment Target '{}' is a descendant of this entity", target.GetName()));
			else if (cycle)
				warnings.push_back(fmt::format("BoneAttachment Target '{}' depends on this entity's attachment (a cycle)", target.GetName()));
			else if (!animator)
				warnings.push_back(fmt::format("BoneAttachment Target '{}' has no Animator", target.GetName()));
			else if (animator->Mesh.empty())
				warnings.push_back(fmt::format("BoneAttachment Target '{}' has an empty Animator Mesh", target.GetName()));
			else if (!source)
				warnings.push_back(fmt::format("BoneAttachment Target '{}' model could not be loaded: {}", target.GetName(), AssetManager::GetError(animator->Mesh)));
			else if (source->AnimationUnsupported)
				warnings.push_back(fmt::format("BoneAttachment Target '{}' model has extra influence sets the Animator cannot animate", target.GetName()));
			else if (attachment.Joint.empty())
				warnings.emplace_back("BoneAttachment Joint is empty");
			else if (FindNode(*source, attachment.Joint) < 0)
				warnings.push_back(fmt::format("BoneAttachment joint '{}' not found in '{}'", attachment.Joint, animator->Mesh));
			if (const auto* body = entity.TryGetComponent<RigidBodyComponent>(); body && body->Type == RigidBodyType::Dynamic)
				warnings.emplace_back("BoneAttachment on a Dynamic RigidBody: physics moves it too; make it Kinematic");
		}
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
			AddAnimatorWarnings(*animator, warnings);
		if (const auto* attachment = entity.TryGetComponent<BoneAttachmentComponent>())
			AddBoneAttachmentWarnings(entity, *attachment, warnings);
		if (const auto* ik = entity.TryGetComponent<InverseKinematicsComponent>())
			AddInverseKinematicsWarnings(entity, *ik, warnings);
		return warnings;
	}
}
