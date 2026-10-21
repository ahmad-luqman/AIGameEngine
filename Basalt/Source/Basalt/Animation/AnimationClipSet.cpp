#include "Basalt/Animation/AnimationClipSet.h"

#include "Basalt/Core/Log.h"

#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <set>

namespace Basalt {

	Ref<const AnimationClipSet> AnimationClipSet::Build(const Ref<MeshSource>& model, std::vector<Library> libraries, const std::vector<AnimationRetargetJoint>& joints)
	{
		auto set = CreateRef<AnimationClipSet>();
		set->m_Model = model;
		std::set<std::string> names;
		set->m_ModelClips = model->Animations.size();
		for (const AnimationClip& clip : model->Animations)
		{
			set->m_Origins.push_back(-1);
			names.insert(clip.Name);
		}
		for (size_t index = 0; index < libraries.size(); index++)
		{
			Library& library = libraries[index];
			if (!library.Source)
			{
				set->m_Problems.push_back(fmt::format("clip library '{}' cannot load: {}", library.Mesh, library.Error));
				continue;
			}
			std::vector<std::string> warnings;
			library.Map = Retargeting::BuildMap(*model, *library.Source, joints, warnings);
			for (std::string& warning : warnings)
				set->m_Problems.push_back(fmt::format("clip library '{}': {}", library.Mesh, warning));
			if (library.Map.MappedCount() == 0)
			{
				library.Error = "no joint matches the model's skeleton (name the pairs in RetargetJoints)";
				set->m_Problems.push_back(fmt::format("clip library '{}': {}", library.Mesh, library.Error));
				continue;
			}
			for (const AnimationClip& clip : library.Source->Animations)
			{
				if (names.contains(clip.Name))
				{
					library.Shadowed.push_back(clip.Name);
					continue;
				}
				AnimationClip baked;
				std::string error;
				if (!Retargeting::BakeClip(*model, *library.Source, library.Map, clip, baked, error))
				{
					set->m_Problems.push_back(fmt::format("clip library '{}': clip '{}': {}", library.Mesh, clip.Name, error));
					continue;
				}
				names.insert(clip.Name);
				library.Clips.push_back(clip.Name);
				set->m_Baked.push_back(std::move(baked));
				set->m_Origins.push_back(static_cast<int32_t>(index));
			}
			if (!library.Shadowed.empty())
				set->m_Problems.push_back(fmt::format("clip library '{}': the model or an earlier library already has clip{} {}", library.Mesh, library.Shadowed.size() > 1 ? "s" : "", fmt::join(library.Shadowed, ", ")));
		}
		set->m_Libraries = std::move(libraries);
		return set;
	}

	int32_t AnimationClipSet::Find(const std::string& name) const
	{
		if (Empty())
			return -1;
		if (name.empty())
			return 0;
		for (size_t i = 0; i < Size(); i++)
			if ((*this)[static_cast<int32_t>(i)].Name == name)
				return static_cast<int32_t>(i);
		return -1;
	}

}
