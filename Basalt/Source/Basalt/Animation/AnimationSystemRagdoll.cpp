#include "Basalt/Animation/AnimationSystem.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <map>

namespace Basalt {

	void AnimationSystem::ClearRagdollPose(Entity entity)
	{
		if (!entity || entity.GetScene() != &m_Scene)
			return;
		if (auto it = m_Playback.find(entity.GetUUID()); it != m_Playback.end())
			DropOverlay(it->second, &Playback::RagdollOverlay);
	}

	void AnimationSystem::ApplyRagdolls()
	{
		auto* physics = m_Scene.GetPhysicsWorld();
		for (auto& [id, playback] : m_Playback)
		{
			const bool hadOverlay = !playback.RagdollOverlay.empty();
			playback.RagdollOverlay.clear();
			Entity entity = m_Scene.GetEntityByUUID(id);
			std::vector<std::pair<uint32_t, glm::mat4>> joints;
			float weight = 0;
			if (!entity || !physics || !physics->GetRagdollPose(entity, *playback.Source, joints, weight) || weight <= 0)
			{
				if (hadOverlay)
					Finish(playback);
				continue;
			}
			playback.IKOverlay.clear();
			playback.SpringOverlay.clear();
			ResetSpringBones(entity);
			auto rejectPose = [&] {
				if (hadOverlay)
					Finish(playback);
				if (!playback.OverlayDropped)
					BS_CORE_WARN("Animator '{}': ragdoll pose cannot be expressed in the animated hierarchy; showing animation until it recovers", entity.GetName());
				playback.OverlayDropped = true;
			};
			AnimationLocalPose physical = playback.Pose;
			std::vector<glm::mat4> globals;
			if (!ComposeAnimationPose(*playback.Source, physical, globals))
			{
				rejectPose();
				continue;
			}
			const std::map<uint32_t, glm::mat4> simulated(joints.begin(), joints.end());
			bool valid = true;
			for (uint32_t node : playback.Source->NodeOrder)
			{
				const int32_t parent = playback.Source->Nodes[node].Parent;
				const glm::mat4 parentWorld = parent < 0 ? glm::mat4(1) : globals[parent];
				if (auto it = simulated.find(node); it != simulated.end())
				{
					globals[node] = it->second;
					auto& local = physical.Nodes[node];
					valid = Math::DecomposeAffineTransform(glm::inverse(parentWorld) * globals[node], local.Translation, local.Rotation, local.Scale) && valid;
				}
				else
				{
					const auto& local = physical.Nodes[node];
					globals[node] = parentWorld * (playback.Source->Nodes[node].HasMatrix ? playback.Source->Nodes[node].Matrix : Math::ComposeTransform(local.Translation, local.Rotation, local.Scale));
				}
			}
			if (!valid)
			{
				rejectPose();
				continue;
			}
			// Blend local transforms, preserving the skeleton's hierarchy during the get-up transition.
			if (weight < 1.0f)
				BlendAnimationPose(physical, playback.Pose, weight);
			for (const auto& [node, matrix] : joints)
				playback.RagdollOverlay.push_back({ node, physical.Nodes[node] });
			Finish(playback);
		}
	}

}
