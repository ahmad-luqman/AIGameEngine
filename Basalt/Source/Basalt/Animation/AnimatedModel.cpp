#include "Basalt/Animation/AnimatedModel.h"

#include "Basalt/Animation/AnimationSystem.h"
#include "Basalt/Animation/InverseKinematics.h"
#include "Basalt/Animation/AnimationClipSet.h"
#include "Basalt/Animation/AnimationMask.h"
#include "Basalt/Animation/PropertyAnimation.h"
#include "Basalt/Animation/SpringBones.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Physics/RagdollRules.h"
#include "Basalt/Renderer/RenderMath.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <algorithm>
#include <map>
#include <set>
#include <span>

namespace Basalt {
	void ForEachAnimatedMesh(Entity entity, const std::function<void(const Ref<MeshSource>&, const MeshNode&, const glm::mat4&, const std::vector<glm::mat4>&, std::span<const float>)>& visit)
	{
		const auto* animator = entity.TryGetComponent<AnimatorComponent>();
		if (!animator)
			return;
		auto& scene = *entity.GetScene();
		// A running scene (or an edit preview) draws the playback's own model, so the pose and weights always fit it.
		AnimationSystem* system = scene.GetPoseAnimationSystem();
		const auto view = system ? system->GetPoseView(entity) : std::nullopt;
		const auto source = view ? view->Source : AssetManager::GetMesh(animator->Mesh);
		if (!source || source->AnimationUnsupported)
			return;
		std::vector<glm::mat4> rest;
		std::span<const glm::mat4> globals = view ? view->Globals : std::span<const glm::mat4>();
		if (!view)
		{
			const bool valid = EvaluateAnimation(*source, nullptr, 0, rest);
			BS_CORE_ASSERT(valid, "Imported rest pose must be finite");
			if (!valid)
				return;
			globals = rest;
		}
		const glm::mat4 root = scene.GetWorldTransform(entity);
		std::vector<std::vector<glm::mat4>> palettes(source->Skins.size());
		for (size_t i = 0; i < source->Nodes.size(); i++)
		{
			const auto& node = source->Nodes[i];
			if (!node.Active || node.MeshIndex < 0)
				continue;
			std::span<const float> weights = node.MorphWeights;
			if (view && node.MorphWeightOffset >= 0)
				weights = view->MorphWeights.subspan(node.MorphWeightOffset, node.MorphWeights.size());
			if (node.SkinIndex < 0)
				visit(source, node, root * globals[i], {}, weights);
			else
			{
				auto& palette = palettes[node.SkinIndex];
				if (palette.empty())
					palette = BuildSkinPalette(source->Skins[node.SkinIndex], globals);
				BS_CORE_ASSERT(!palette.empty(), "Validated animation pose must have a finite skin palette");
				if (!palette.empty())
					visit(source, node, root, palette, weights);
			}
		}
	}

	AABB AnimatedModelBounds(Entity entity)
	{
		AABB bounds;
		ForEachAnimatedMesh(entity, [&](const Ref<MeshSource>& source, const MeshNode& node, const glm::mat4& model, const std::vector<glm::mat4>& palette, std::span<const float> morphWeights) {
			// Exact bounds of what is drawn: dropped weights could move them away from drawn vertices.
			const std::vector<float> drawn = DrawnMorphWeights(morphWeights);
			for (uint32_t index : source->Meshes[node.MeshIndex].Submeshes)
			{
				const Submesh& submesh = source->Submeshes[index];
				bounds.Expand((palette.empty() ? MorphedBounds(*source, submesh, drawn) : SkinBounds(*source, submesh, palette, drawn)).Transformed(model));
			}
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
			// Entries without a Mesh are the inspector's additions and are skipped, so settings on them do nothing yet.
			for (size_t i = 0; i < animator.ClipLibraries.size(); i++)
			{
				const AnimationClipLibrary& library = animator.ClipLibraries[i];
				if (library.Mesh.empty() && library != AnimationClipLibrary{})
					warnings.push_back(fmt::format("Animator ClipLibraries[{}] has no Mesh, so it adds no clips and its other settings do nothing", i));
			}
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
			// The model's clips, then the clip libraries' (the source clips stand in for their retargeted copies, which
			// keep names and durations).
			std::vector<const AnimationClip*> clips;
			for (const AnimationClip& clip : source->Animations)
				clips.push_back(&clip);
			const auto findClip = [&](const std::string& name) -> const AnimationClip* {
				auto it = std::ranges::find(clips, name, &AnimationClip::Name);
				return it != clips.end() ? *it : nullptr;
			};
			// The same resolution as playback (AnimationClipSet), without baking.
			std::vector<AnimationClipSet::Library> libraries = AnimationClipSet::Load(animator);
			AnimationClipSet::Plan(*source, libraries);
			for (const AnimationClipSet::Library& library : libraries)
			{
				const std::string& key = library.Entry.Mesh;
				if (!library.Error.empty())
					warnings.push_back(fmt::format("Animator clip library '{}' adds no clips: {}", key, library.Error));
				for (const std::string& warning : library.Warnings)
					warnings.push_back(fmt::format("Animator clip library '{}': {}", key, warning));
				for (const std::string& name : library.Shadowed)
					warnings.push_back(fmt::format("Animator clip library '{}': clip '{}' is hidden by an earlier clip of that name", key, name));
				for (const std::string& name : library.Clips)
					clips.push_back(&library.SourceClip(name));
			}
			// The base graph's and each layer graph's states, named by `prefix`.
			const auto addGraphStateWarnings = [&](const AnimationGraph& graph, const std::string& prefix) {
				for (const auto& state : graph.States)
				{
					if (state.BlendSpace.empty())
					{
						if (state.Clip.empty() ? clips.empty() : !findClip(state.Clip))
							warnings.push_back(fmt::format("{} state '{}': clip '{}' not found", prefix, state.Name, state.Clip));
					}
					else if (std::ranges::find(animator.BlendSpaces, state.BlendSpace, &AnimationBlendSpace::Name) == animator.BlendSpaces.end())
						warnings.push_back(fmt::format("{} state '{}': blend space '{}' not found", prefix, state.Name, state.BlendSpace));
				}
			};
			if (animator.RootMotion && (animator.RootJoint.empty() || FindNode(*source, animator.RootJoint) < 0))
				warnings.emplace_back("Animator RootJoint must name a model node for root motion");
			if (!animator.StateMachine.empty())
			{
				const auto graph = AssetManager::GetAnimationGraph(animator.StateMachine);
				if (!graph)
					warnings.push_back("Animator state machine could not be loaded: " + AssetManager::GetError(animator.StateMachine));
				else
					addGraphStateWarnings(*graph, "Animator");
			}
			const bool namesSpace = std::ranges::find(animator.BlendSpaces, animator.Clip, &AnimationBlendSpace::Name) != animator.BlendSpaces.end();
			if (!animator.Clip.empty() && !findClip(animator.Clip) && !namesSpace)
				warnings.push_back(fmt::format("Animator clip '{}' not found in '{}'{}", animator.Clip, animator.Mesh, libraries.empty() ? std::string() : fmt::format(" or its {} clip librar{}", libraries.size(), libraries.size() == 1 ? "y" : "ies")));
			if (clips.empty() && (!animator.Events.empty() || !animator.BlendSpaces.empty()))
				warnings.push_back(fmt::format("Animator model '{}' has no animation clips, so its events and blend spaces never play", animator.Mesh));
			float longest = 0;
			for (const AnimationClip* clip : clips)
				longest = std::max(longest, clip->Duration);
			for (const AnimationEvent& event : animator.Events)
			{
				if (event.Clip.empty())
				{
					if (!clips.empty() && event.Time > longest)
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
				if (layer.OwnParameters && layer.StateMachine.empty())
					warnings.push_back(fmt::format("Animator layer '{}': OwnParameters has no effect without a StateMachine", layer.Name));
				if (layer.Events && animator.Events.empty())
					warnings.push_back(fmt::format("Animator layer '{}': Events has no effect: the Animator has no Events", layer.Name));
				if (!layer.StateMachine.empty())
				{
					if (!layer.Clip.empty())
						warnings.push_back(fmt::format("Animator layer '{}': Clip is unused; the StateMachine replaces it", layer.Name));
					if (!layer.Looping)
						warnings.push_back(fmt::format("Animator layer '{}': Looping is unused; each state of the StateMachine has its own", layer.Name));
					if (layer.Blend == AnimationLayerBlend::Additive && layer.ReferenceClip.empty())
						warnings.push_back(fmt::format("Animator layer '{}': an Additive layer with a StateMachine needs a ReferenceClip", layer.Name));
					if (const auto graph = AssetManager::GetAnimationGraph(layer.StateMachine); !graph)
						warnings.push_back(fmt::format("Animator layer '{}': state machine could not be loaded: {}", layer.Name, AssetManager::GetError(layer.StateMachine)));
					else
						addGraphStateWarnings(*graph, fmt::format("Animator layer '{}'", layer.Name));
				}
				else if (!layer.Clip.empty() && !findClip(layer.Clip))
					warnings.push_back(fmt::format("Animator layer '{}': clip '{}' not found", layer.Name, layer.Clip));
				if (layer.Blend == AnimationLayerBlend::Additive && !layer.ReferenceClip.empty() && !findClip(layer.ReferenceClip))
					warnings.push_back(fmt::format("Animator layer '{}': reference clip '{}' not found", layer.Name, layer.ReferenceClip));
				if (layer.Blend == AnimationLayerBlend::Override && !layer.ReferenceClip.empty())
					warnings.push_back(fmt::format("Animator layer '{}': ReferenceClip only applies to Additive layers", layer.Name));
				if (!layer.MaskAsset.empty() && !layer.Mask.empty())
					warnings.push_back(fmt::format("Animator layer '{}': Mask is unused; the MaskAsset replaces it", layer.Name));
				const auto asset = AssetManager::GetAnimationMask(layer.MaskAsset);
				const std::vector<AnimationMaskJoint>* mask = SelectLayerMask(layer, asset.get());
				if (!mask)
				{
					warnings.push_back(fmt::format("Animator layer '{}': mask could not be loaded, so the layer shows nothing: {}", layer.Name, AssetManager::GetError(layer.MaskAsset)));
					continue;
				}
				for (const AnimationMaskJoint& joint : *mask)
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
					warnings.push_back(fmt::format("InverseKinematics chain '{}': Pole only applies to TwoBone and Fabrik chains", chain.Name));
				if (chain.Type == IKChainType::TwoBone && chain.ChainLength != 1)
					warnings.push_back(fmt::format("InverseKinematics chain '{}': ChainLength only applies to LookAt and Fabrik chains", chain.Name));
				if (chain.FollowTarget && ik.Target == 0)
					warnings.push_back(fmt::format("InverseKinematics chain '{}': FollowTarget has no effect: the component has no Target", chain.Name));
				if (chain.FootLock && ik.LockSpeed <= 0)
					warnings.push_back(fmt::format("InverseKinematics chain '{}': FootLock never locks: LockSpeed is 0", chain.Name));
				if (chain.FootLock && chain.FollowTarget && ik.Target != 0)
					warnings.push_back(fmt::format("InverseKinematics chain '{}': FootLock has no effect while FollowTarget reaches for the Target", chain.Name));
				if (chain.Type == IKChainType::LookAt && chain.MaxAngle == 0)
					warnings.push_back(fmt::format("InverseKinematics chain '{}': MaxAngle 0 never turns the chain", chain.Name));
				if (chain.Type != IKChainType::Fabrik && HasIKJointLimits(chain))
					warnings.push_back(fmt::format("InverseKinematics chain '{}': SwingAngle, TwistMin, TwistMax and JointLimits only apply to Fabrik chains", chain.Name));
			}
			if (ik.Target != 0)
			{
				if (!entity.GetScene()->GetEntityByUUID(ik.Target))
					warnings.emplace_back("InverseKinematics Target entity not found");
				else if (std::ranges::none_of(ik.Chains, &IKChain::FollowTarget))
					warnings.emplace_back("InverseKinematics Target has no effect: no chain has FollowTarget");
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
				if (chain.Type == IKChainType::Fabrik)
				{
					std::vector<FabrikJointLimit> limits;
					std::vector<std::string> problems;
					ResolveFabrikLimits(*source, chain, joints, limits, problems);
					for (const std::string& problem : problems)
						warnings.push_back(fmt::format("InverseKinematics chain '{}': {}", chain.Name, problem));
					if (!limits.empty() && std::ranges::all_of(limits, [](const FabrikJointLimit& limit) { return limit.SwingAngle == 0 && limit.TwistMin == 0 && limit.TwistMax == 0; }))
						warnings.push_back(fmt::format("InverseKinematics chain '{}': its limits hold every joint at the rest pose, so it never reaches for a target", chain.Name));
				}
				if (chain.Ground && IKChainReaches(chain.Type))
					groundUppers.push_back(joints.front());
				// Chains solve in order, so a later one driving the same joint overrides the earlier one.
				const uint32_t end = IKChainReaches(chain.Type) ? joints.back() : joints.front();
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

		void AddSpringBoneWarnings(Entity entity, const SpringBoneComponent& springs, std::vector<std::string>& warnings)
		{
			const auto* animator = entity.TryGetComponent<AnimatorComponent>();
			if (!animator)
			{
				warnings.emplace_back("SpringBone needs an Animator on the same entity");
				return;
			}
			// AddAnimatorWarnings reports a model that cannot load.
			const auto source = animator->Mesh.empty() ? nullptr : AssetManager::GetMesh(animator->Mesh);
			std::vector<uint32_t> joints;
			std::vector<glm::mat4> restGlobals;
			const bool restPose = source && EvaluateAnimation(*source, nullptr, 0, restGlobals);
			const PhysicsLayers layers = entity.GetScene()->ResolvePhysicsLayers();
			// The first chain turning each joint.
			std::map<uint32_t, const SpringChain*> turned;
			for (const SpringChain& chain : springs.Chains)
			{
				// Chains name colliders by Name, which needs no model.
				for (size_t i = 0; i < chain.Colliders.size(); i++)
				{
					const SpringChainCollider& named = chain.Colliders[i];
					if (NamesMissingSpringCollider(named, springs.Colliders))
						warnings.push_back(fmt::format("SpringBone chain '{}': Colliders: no collider is named '{}'", chain.Name, named.Name));
					else if (named.Name.empty())
						warnings.push_back(fmt::format("SpringBone chain '{}': Colliders[{}] has no Name, so it names no collider (a list without names keeps every collider)", chain.Name, i));
				}
				if (!chain.Layer.empty() && !chain.CollidePhysics)
					warnings.push_back(fmt::format("SpringBone chain '{}': Layer has no effect without CollidePhysics", chain.Name));
				else if (!chain.Layer.empty())
				{
					const auto layer = layers.Find(chain.Layer);
					if (!layer)
						warnings.push_back(fmt::format("SpringBone chain '{}': the physics layer '{}' is not defined; it counts as Default", chain.Name, chain.Layer));
					else if (layers.GetCollisionMask(*layer) == 0)
						warnings.push_back(fmt::format("SpringBone chain '{}': the physics layer '{}' collides with no layer", chain.Name, chain.Layer));
				}
				if (chain.Continuous && !chain.CollidePhysics && springs.Colliders.empty())
					warnings.push_back(fmt::format("SpringBone chain '{}': Continuous has no effect: there is nothing to sweep against", chain.Name));
				if (chain.Joint.empty())
				{
					warnings.push_back(fmt::format("SpringBone chain '{}': Joint is empty", chain.Name));
					continue;
				}
				if (!source)
					continue;
				if (std::string problem = ResolveSpringChain(*source, chain, joints); !problem.empty())
				{
					warnings.push_back(fmt::format("SpringBone chain '{}': {}", chain.Name, problem));
					continue;
				}
				const auto chainJoints = std::span(joints).first(joints.size() - 1);
				// Chains on the joints a partial ragdoll simulates wait for it; a whole-body rig stops every chain, as documented.
				if (const auto* ragdoll = entity.TryGetComponent<RagdollComponent>(); ragdoll && ragdoll->KinematicRoot && std::ranges::any_of(chainJoints, [&](uint32_t joint) {
																						  const std::set<uint32_t> selected = SelectRagdollJoints(*source, ragdoll->RootJoint.empty() ? -1 : FindNode(*source, ragdoll->RootJoint));
																						  return selected.contains(joint) && joint != FindRagdollRoot(*source, selected);
																					  }))
					warnings.push_back(fmt::format("SpringBone chain '{}': its joints belong to the Ragdoll, so it does not swing while the rig is active", chain.Name));
				// Chains swing in order, so a later one turning the same joint overrides the earlier one.
				if (const auto other = std::ranges::find_if(chainJoints, [&](uint32_t joint) { return turned.contains(joint); }); other != chainJoints.end())
					warnings.push_back(fmt::format("SpringBone chains '{}' and '{}' both turn joint '{}'; the later one wins", turned.at(*other)->Name, chain.Name, source->Nodes[*other].Name));
				for (size_t i = 0; i < chainJoints.size(); i++)
				{
					turned.emplace(joints[i], &chain);
					// ResolveSpringChain follows first children: the next joint is the first child.
					if (const size_t children = ChildCount(*source, joints[i]); children > 1)
						warnings.push_back(fmt::format("SpringBone chain '{}': joint '{}' has {} children; the chain follows the first, '{}'", chain.Name, source->Nodes[joints[i]].Name, children, source->Nodes[joints[i + 1]].Name));
				}
				// Zero-length bones in the model's own pose have nothing to swing (the animation hiding a chain by scaling
				// it to nothing is no problem).
				bool flat = restPose;
				for (size_t i = 0; flat && i < chainJoints.size(); i++)
					flat = glm::length(glm::vec3(restGlobals[joints[i + 1]][3]) - glm::vec3(restGlobals[joints[i]][3])) < 1e-6f;
				if (flat)
					warnings.push_back(fmt::format("SpringBone chain '{}': its bones have zero length in the model's rest pose, so nothing can swing", chain.Name));
				if (chain.ChainLength == 0 && FirstChild(*source, joints.back()) >= 0)
					warnings.push_back(fmt::format("SpringBone chain '{}': the joints go on past the first {}; the rest follow the animation", chain.Name, SpringChain::MaxChainLength));
			}
			// An unnamed collider is one a chain that lists colliders can never pick.
			if (std::ranges::any_of(springs.Chains, NamesSpringColliders))
				for (size_t i = 0; i < springs.Colliders.size(); i++)
					if (springs.Colliders[i].Name.empty())
						warnings.push_back(fmt::format("SpringBone Colliders[{}] has no Name, so chains that list colliders cannot pick it", i));
			if (!source)
				return;
			for (size_t i = 0; i < springs.Colliders.size(); i++)
			{
				const SpringCollider& collider = springs.Colliders[i];
				if (!collider.Joint.empty() && FindNode(*source, collider.Joint) < 0)
					warnings.push_back(fmt::format("SpringBone Colliders[{}]: joint '{}' not found", i, collider.Joint));
			}
		}

		// Keyed tracks whose target is not there now (an entity or component removed, a list element renamed or removed), which
		// would fail and suspend the timeline, and keyed tracks a later one hides by reaching the same field.
		void AddPropertyAnimationWarnings(Entity entity, const PropertyAnimationComponent& timeline, std::vector<std::string>& warnings)
		{
			// The registry validates what it writes; C++ code may not.
			if (std::string problem = ValidatePropertyAnimation(timeline); !problem.empty())
			{
				warnings.push_back("PropertyAnimation: " + problem);
				return;
			}
			std::map<std::pair<uint64_t, std::string>, nlohmann::json> components;
			// The track reaching each field last, by the field's place in its component's JSON.
			std::map<const nlohmann::json*, size_t> reachedBy;
			for (size_t i = 0; i < timeline.Tracks.size(); i++)
			{
				const PropertyAnimationTrack& track = timeline.Tracks[i];
				// Empty tracks do nothing, so they cannot fail.
				if (track.Keys.empty())
					continue;
				// Validated above, so this only keeps the optional access visibly safe.
				const auto field = PropertyAnimation::ResolveField(track.Component, track.Field);
				if (!field)
					continue;
				const auto* info = ComponentRegistry::Find(track.Component);
				std::string problem;
				const nlohmann::json* reached = nullptr;
				const Entity target = PropertyAnimation::ResolveTarget(entity, track.Target);
				if (!target)
					problem = fmt::format("entity {} is not in the scene", static_cast<uint64_t>(track.Target));
				else if (!info->Has(target))
					problem = "the entity has no " + track.Component;
				else
				{
					auto [data, inserted] = components.try_emplace({ static_cast<uint64_t>(target.GetUUID()), track.Component });
					if (inserted)
						data->second = info->Serialize(target);
					reached = field->Find(data->second, problem);
				}
				if (!problem.empty())
					warnings.push_back(fmt::format("PropertyAnimation Tracks[{}] ({}.{}): {}", i, track.Component, track.Field, problem));
				else if (const auto [earlier, added] = reachedBy.try_emplace(reached, i); !added)
				{
					const PropertyAnimationTrack& hidden = timeline.Tracks[earlier->second];
					warnings.push_back(fmt::format("PropertyAnimation Tracks[{}] ({}.{}) reaches the same field as Tracks[{}] ({}.{}), so only the later one plays", earlier->second, hidden.Component, hidden.Field, i, track.Component, track.Field));
					earlier->second = i;
				}
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

	namespace {
		void AddRagdollWarnings(const AnimatorComponent* animator, const RagdollComponent& ragdoll, std::vector<std::string>& warnings)
		{
			if (ragdoll.MotorStrength > 0 && ragdoll.MotorTorque <= 0)
				warnings.emplace_back("Ragdoll MotorStrength has no effect: MotorTorque is 0");
			if (ragdoll.BalanceStrength > 0)
			{
				if (ragdoll.KinematicRoot)
					warnings.emplace_back("Ragdoll BalanceStrength has no effect: a KinematicRoot rig cannot fall");
				else if (ragdoll.MotorTorque <= 0)
					warnings.emplace_back("Ragdoll BalanceStrength has no effect: balance needs running motors, and MotorTorque is 0");
				else if (ragdoll.MotorStrength <= 0)
					warnings.emplace_back("Ragdoll BalanceStrength has no effect while the motor strength is 0: set MotorStrength, or raise it from a script");
				else if (ragdoll.BalanceMaxTilt <= 0)
					warnings.emplace_back("Ragdoll BalanceStrength has no effect: BalanceMaxTilt is 0");
			}
			// AddAnimatorWarnings reports a model that cannot load.
			const auto source = !animator || animator->Mesh.empty() ? nullptr : AssetManager::GetMesh(animator->Mesh);
			if (!source)
				return;
			const std::set<uint32_t> selected = SelectRagdollJoints(*source, ragdoll.RootJoint.empty() ? -1 : FindNode(*source, ragdoll.RootJoint));
			if (selected.empty())
				return;
			if (ragdoll.KinematicRoot && selected.size() == 1)
				warnings.emplace_back("Ragdoll KinematicRoot leaves nothing to simulate: no skin joint hangs from the root");
			const std::string& root = source->Nodes[FindRagdollRoot(*source, selected)].Name;
			for (const RagdollBone& bone : ragdoll.Bones)
				if (bone.MotorStrength != 1 && bone.Joint == root)
					warnings.push_back(fmt::format("Ragdoll Bones: '{}' is the rig's root, with no joint above it for MotorStrength to scale", bone.Joint));
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
		if (const auto* springs = entity.TryGetComponent<SpringBoneComponent>())
			AddSpringBoneWarnings(entity, *springs, warnings);
		if (const auto* ragdoll = entity.TryGetComponent<RagdollComponent>())
			AddRagdollWarnings(animator, *ragdoll, warnings);
		if (const auto* timeline = entity.TryGetComponent<PropertyAnimationComponent>())
			AddPropertyAnimationWarnings(entity, *timeline, warnings);
		return warnings;
	}
}
