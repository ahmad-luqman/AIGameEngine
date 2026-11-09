#include "Breccia/Animation/AnimationSystem.h"

#include "Breccia/Math/Math.h"
#include "Breccia/Physics/PhysicsWorld.h"
#include "Breccia/Physics/RagdollRules.h"
#include "Breccia/Scene/Entity.h"
#include "Breccia/Scene/Scene.h"

#include <map>

namespace Breccia {

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
			const bool ownsRoot = physics->RagdollOwnsRoot(entity);
			if (ownsRoot)
			{
				playback.IKOverlay.clear();
				playback.SpringOverlay.clear();
				ResetSpringBones(entity);
			}
			auto rejectPose = [&] {
				Finish(playback);
				// Do not read playback after this: contact callbacks may remove its Animator or entity.
				physics->RejectRagdollPose(entity);
			};
			AnimationLocalPose physical = playback.Pose;
			// A rig below an animated root hangs from it and blends back to it, IK included (a rig that owns its root just
			// cleared IK).
			ApplyOverlay(physical, playback.IKOverlay);
			const AnimationLocalPose animated = weight < 1.0f ? physical : AnimationLocalPose{};
			std::vector<glm::mat4> globals;
			if (!ComposeAnimationPose(*playback.Source, physical, globals))
			{
				rejectPose();
				continue;
			}
			if (!ownsRoot && !joints.empty())
			{
				// The root body is where the last fixed step put it and the entity may have moved on since, so the limbs are
				// placed relative to it and hung from the animated root, which stays the animation's.
				const glm::mat4 anchor = globals[joints.front().first] * glm::inverse(joints.front().second);
				joints.erase(joints.begin());
				for (auto& [node, matrix] : joints)
					matrix = anchor * matrix;
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
					const auto& target = animated.Nodes[node];
					pose.Translation = glm::mix(target.Translation, pose.Translation, weight);
					pose.Scale = glm::mix(target.Scale, pose.Scale, weight);
					pose.Rotation = Math::Slerp(target.Rotation, pose.Rotation, weight);
				}
			for (const auto& [node, matrix] : joints)
				playback.RagdollOverlay.push_back({ node, physical.Nodes[node] });
			Finish(playback);
		}
	}

}
