#include "Basalt/Animation/AnimationClipSet.h"

#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/Log.h"

#include <algorithm>
#include <set>

namespace Basalt {

	namespace {
		size_t BakedFloats(const AnimationClip& clip)
		{
			size_t floats = 0;
			for (const AnimationTrack& track : clip.Tracks)
				floats += track.Times.size() + track.Values.size() * 4 + track.Weights.size();
			return floats;
		}
	}

	std::vector<AnimationClipSet::Library> AnimationClipSet::Load(const AnimatorComponent& animator)
	{
		return Load(animator, [](const std::string& key) { return AssetManager::GetMesh(key); });
	}

	std::vector<AnimationClipSet::Library> AnimationClipSet::Load(const AnimatorComponent& animator, const std::function<Ref<MeshSource>(const std::string&)>& resolve)
	{
		std::vector<Library> libraries;
		for (size_t i = 0; i < animator.ClipLibraries.size(); i++)
		{
			const AnimationClipLibrary& entry = animator.ClipLibraries[i];
			// Unnamed entries are the inspector's additions.
			if (entry.Mesh.empty())
				continue;
			Library& library = libraries.emplace_back(Library{ .Index = i, .Entry = entry, .Source = resolve(entry.Mesh) });
			if (!library.Source)
				library.Error = "cannot load: " + AssetManager::GetError(entry.Mesh);
		}
		return libraries;
	}

	void AnimationClipSet::Plan(const MeshSource& model, std::vector<Library>& libraries)
	{
		std::set<std::string> names;
		for (const AnimationClip& clip : model.Animations)
			names.insert(clip.Name);
		for (Library& library : libraries)
		{
			if (!library.Source)
				continue;
			library.Map = Retargeting::BuildMap(model, *library.Source, library.Entry.Joints, library.Warnings);
			if (library.Map.MappedCount() == 0)
			{
				library.Error = "no joint matches the model's skeleton (pair them in the library's Joints)";
				continue;
			}
			for (const AnimationClip& clip : library.Source->Animations)
			{
				if (names.contains(clip.Name))
				{
					library.Shadowed.push_back(clip.Name);
					continue;
				}
				std::string error;
				if (!Retargeting::CheckBakeBudget(model, *library.Source, library.Map, clip, error))
				{
					library.Warnings.push_back(fmt::format("clip '{}': {}", clip.Name, error));
					continue;
				}
				names.insert(clip.Name);
				library.Clips.push_back(clip.Name);
			}
		}
	}

	Ref<const AnimationClipSet> AnimationClipSet::Build(const Ref<MeshSource>& model, std::vector<Library> libraries)
	{
		Plan(*model, libraries);
		auto set = CreateRef<AnimationClipSet>();
		set->m_Model = model;
		set->m_ModelClips = model->Animations.size();
		size_t floats = 0;
		for (Library& library : libraries)
		{
			std::vector<std::string> baked;
			for (const std::string& name : library.Clips)
			{
				const AnimationClip& clip = *std::ranges::find(library.Source->Animations, name, &AnimationClip::Name);
				AnimationClip result;
				std::string error;
				if (!Retargeting::BakeClip(*model, *library.Source, library.Map, clip, result, library.Warnings, error))
					library.Warnings.push_back(fmt::format("clip '{}' cannot be retargeted: {}", name, error));
				else if (result.Tracks.empty())
					library.Warnings.push_back(fmt::format("clip '{}' moves no joint paired with the model, so it is left out", name));
				else if (const size_t size = BakedFloats(result); size > MaxBakedFloats - floats)
					library.Warnings.push_back(fmt::format("clip '{}' is left out: the Animator's retargeted clips would exceed {} MiB", name, MaxBakedFloats * sizeof(float) / (size_t{ 1024 } * 1024)));
				else
				{
					floats += size;
					baked.push_back(name);
					set->m_Baked.push_back(std::move(result));
				}
			}
			library.Clips = std::move(baked);
		}
		set->m_Libraries = std::move(libraries);
		set->m_Memory.Resize(floats * sizeof(float));
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

	bool AnimationClipSet::Matches(const Ref<MeshSource>& model, const std::vector<Library>& loaded) const
	{
		return model == m_Model && std::ranges::equal(loaded, m_Libraries, [](const Library& a, const Library& b) { return a.Index == b.Index && a.Entry == b.Entry && a.Source == b.Source; });
	}

}
