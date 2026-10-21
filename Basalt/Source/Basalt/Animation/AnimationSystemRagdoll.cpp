#include "Basalt/Animation/AnimationSystem.h"

#include "Basalt/Math/Math.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Physics/RagdollRules.h"
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

	void AnimationSystem::RefreshRagdollPoses()
	{
		ApplyRagdolls();
	}

	void AnimationSystem::ApplyRagdolls()
	{
		auto* physics = m_Scene.GetPhysicsWorld();
		// Rejecting an unsupported hierarchy ends contacts, whose callbacks can erase any playback.
		// Snapshot only rigs needing this pass, keeping ordinary animation-only crowds out of the work list.
		std::vector<UUID> order;
		for (const auto& [id, playback] : m_Playback)
			if (!playback.RagdollOverlay.empty() || (physics && physics->HasRagdollPose(m_Scene.GetEntityByUUID(id))))
				order.push_back(id);
		for (UUID id : order)
		{
			auto found = m_Playback.find(id);
			if (found == m_Playback.end())
				continue;
			auto& playback = found->second;
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
				Finish(playback);
				// Do not read playback after this: contact callbacks may remove its Animator or entity.
				physics->RejectRagdollPose(entity);
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
					valid = DecomposeRagdollTransform(glm::inverse(parentWorld) * globals[node], local.Translation, local.Rotation, local.Scale) && valid;
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
				for (const auto& [node, matrix] : joints)
				{
					auto& pose = physical.Nodes[node];
					const auto& animated = playback.Pose.Nodes[node];
					pose.Translation = glm::mix(animated.Translation, pose.Translation, weight);
					pose.Scale = glm::mix(animated.Scale, pose.Scale, weight);
					pose.Rotation = BlendRagdollRotation(animated.Rotation, pose.Rotation, weight);
				}
			for (const auto& [node, matrix] : joints)
				playback.RagdollOverlay.push_back({ node, physical.Nodes[node] });
			Finish(playback);
		}
	}

}
