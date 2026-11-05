#include "JointAssetDraft.h"

#include "Basalt/Animation/AnimationPose.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/Components.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <algorithm>
#include <utility>

namespace Basalt {

	template<typename Asset>
	bool JointAssetDraft<Asset>::Load(const std::string& path, std::string& outError)
	{
		const auto text = FileSystem::ReadTextFile(Project::ResolvePath(path));
		if (!text)
		{
			outError = "cannot read '" + path + "'";
			return false;
		}
		auto asset = Asset::FromText(*text, outError);
		if (!asset)
			return false;
		m_Path = path;
		m_Asset = std::move(*asset);
		m_Saved = m_Asset.Joints;
		m_Undo = { m_Saved };
		m_Redo.clear();
		m_ExternalChange = false;
		return true;
	}

	template<typename Asset>
	void JointAssetDraft<Asset>::Close()
	{
		*this = {};
	}

	template<typename Asset>
	bool JointAssetDraft<Asset>::Validate(std::string& outError) const
	{
		// The parser that loads the file decides; asset.write runs it again.
		return Asset::FromJson(m_Asset.ToJson(), outError).has_value();
	}

	template<typename Asset>
	std::string JointAssetDraft<Asset>::MakeFileText() const
	{
		return m_Asset.ToJson().dump(2) + "\n";
	}

	template<typename Asset>
	void JointAssetDraft<Asset>::MarkSaved()
	{
		m_Saved = m_Asset.Joints;
		m_ExternalChange = false;
		Record();
	}

	template<typename Asset>
	bool JointAssetDraft<Asset>::Revert(std::string& outError)
	{
		return Load(m_Path, outError);
	}

	template<typename Asset>
	bool JointAssetDraft<Asset>::OnFileChanged(std::string& outError)
	{
		if (!IsDirty() && Load(m_Path, outError))
			return true;
		m_ExternalChange = true;
		return false;
	}

	template<typename Asset>
	void JointAssetDraft<Asset>::OnAssetMoved(const std::string& from, const std::string& to)
	{
		if (m_Path == from)
			m_Path = to;
	}

	template<typename Asset>
	void JointAssetDraft<Asset>::Record()
	{
		if (!m_Undo.empty() && m_Undo.back() == m_Asset.Joints)
			return;
		m_Undo.push_back(m_Asset.Joints);
		if (m_Undo.size() > MaxUndoSteps)
			m_Undo.erase(m_Undo.begin());
		m_Redo.clear();
	}

	template<typename Asset>
	bool JointAssetDraft<Asset>::Undo()
	{
		Record();
		if (m_Undo.size() < 2)
			return false;
		m_Redo.push_back(std::move(m_Undo.back()));
		m_Undo.pop_back();
		m_Asset.Joints = m_Undo.back();
		return true;
	}

	template<typename Asset>
	bool JointAssetDraft<Asset>::Redo()
	{
		Record();
		if (m_Redo.empty())
			return false;
		m_Undo.push_back(std::move(m_Redo.back()));
		m_Redo.pop_back();
		m_Asset.Joints = m_Undo.back();
		return true;
	}

	template class JointAssetDraft<AnimationMask>;
	template class JointAssetDraft<AnimationJointMap>;

	namespace JointAssetEdit {

		bool SetMaskWeight(AnimationMask& mask, const std::string& joint, float weight)
		{
			if (const auto entry = std::ranges::find(mask.Joints, joint, &AnimationMaskJoint::Joint); entry != mask.Joints.end())
			{
				entry->Weight = weight;
				return true;
			}
			if (mask.Joints.size() >= AnimationLayer::MaxMaskJoints)
				return false;
			mask.Joints.push_back({ .Joint = joint, .Weight = weight });
			return true;
		}

		void RemoveMaskJoint(AnimationMask& mask, const std::string& joint)
		{
			std::erase_if(mask.Joints, [&](const AnimationMaskJoint& entry) { return entry.Joint == joint; });
		}

		size_t AddMatchedPairs(AnimationJointMap& map, const RetargetMap& retarget, const MeshSource& target, const MeshSource& source)
		{
			size_t added = 0;
			for (uint32_t node : target.NodeOrder)
			{
				if (map.Joints.size() >= AnimationClipLibrary::MaxJoints)
					break;
				if (retarget.SourceOf[node] < 0 || retarget.Matches[node] == RetargetMatch::JointMap)
					continue;
				const auto sourceNode = static_cast<uint32_t>(retarget.SourceOf[node]);
				const std::string& targetName = target.Nodes[node].Name;
				const std::string& sourceName = source.Nodes[sourceNode].Name;
				// A pair names joints, and a name stands for the first node that has it.
				if (FindNode(target, targetName) != static_cast<int32_t>(node) || FindNode(source, sourceName) != static_cast<int32_t>(sourceNode))
					continue;
				map.Joints.push_back({ .Source = sourceName, .Target = targetName });
				added++;
			}
			return added;
		}

		std::string FindMaskModel(Scene& scene, const std::string& maskPath, UUID selection)
		{
			for (entt::entity handle : scene.GetAllEntitiesWith<AnimatorComponent>())
			{
				const auto& animator = Entity(handle, &scene).GetComponent<AnimatorComponent>();
				if (std::ranges::any_of(animator.Layers, [&](const AnimationLayer& layer) { return layer.MaskAsset == maskPath; }))
					return animator.Mesh;
			}
			if (const Entity selected = scene.GetEntityByUUID(selection))
				if (const auto* animator = selected.TryGetComponent<AnimatorComponent>())
					return animator->Mesh;
			return {};
		}

		JointMapModels FindJointMapModels(Scene& scene, const std::string& mapPath, UUID selection)
		{
			for (entt::entity handle : scene.GetAllEntitiesWith<AnimatorComponent>())
			{
				const auto& animator = Entity(handle, &scene).GetComponent<AnimatorComponent>();
				const auto library = std::ranges::find(animator.ClipLibraries, mapPath, &AnimationClipLibrary::JointMap);
				if (library != animator.ClipLibraries.end())
					return { library->Mesh, animator.Mesh };
			}
			if (const Entity selected = scene.GetEntityByUUID(selection))
				if (const auto* animator = selected.TryGetComponent<AnimatorComponent>())
					return { animator->ClipLibraries.empty() ? std::string() : animator->ClipLibraries.front().Mesh, animator->Mesh };
			return {};
		}

	}

}
