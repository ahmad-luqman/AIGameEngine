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
		return Load(animator, [](const std::string& key, std::string& outError) {
			Ref<MeshSource> source = AssetManager::GetMesh(key);
			if (!source)
				outError = AssetManager::GetError(key);
			return source;
		});
	}

	std::vector<AnimationClipSet::Library> AnimationClipSet::Load(const AnimatorComponent& animator, const Resolver& resolve)
	{
		std::vector<Library> libraries;
		for (size_t i = 0; i < animator.ClipLibraries.size(); i++)
		{
			const AnimationClipLibrary& entry = animator.ClipLibraries[i];
			// Unnamed entries are the inspector's additions.
			if (entry.Mesh.empty())
				continue;
			std::string error;
			Library& library = libraries.emplace_back(Library{ .Index = i, .Entry = entry, .Source = resolve(entry.Mesh, error) });
			if (!library.Source)
				library.Error = "cannot load: " + error;
		}
		return libraries;
	}

	void AnimationClipSet::Match(const MeshSource& model, Library& library)
	{
		// Planning again starts over (a load failure stays).
		library.Map = {};
		library.Warnings.clear();
		library.Clips.clear();
		library.Shadowed.clear();
		if (!library.Source)
			return;
		library.Error.clear();
		library.Map = Retargeting::BuildMap(model, *library.Source, library.Entry.Joints, library.Warnings);
		if (library.Source->Animations.empty())
			library.Error = fmt::format("'{}' has no animation clips", library.Source->Key);
		else if (library.Map.MappedCount() == 0)
			library.Error = "no joint matches the model's skeleton (pair them in the library's Joints)";
		else if (library.Map.TargetRest.Rotations.empty())
			library.Error = "a rest pose is not finite";
	}

	void AnimationClipSet::Plan(const MeshSource& model, std::vector<Library>& libraries)
	{
		std::set<std::string> names;
		for (const AnimationClip& clip : model.Animations)
			names.insert(clip.Name);
		for (Library& library : libraries)
		{
			Match(model, library);
			if (!library.Error.empty())
				continue;
			// The source nodes whose motion can reach a paired joint: the paired ones and everything above them.
			std::vector<bool> relevant(library.Source->Nodes.size(), false);
			for (int32_t node : library.Map.SourceOf)
				for (int32_t walk = node; walk >= 0 && !relevant[walk]; walk = library.Source->Nodes[walk].Parent)
					relevant[walk] = true;
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
				const bool moves = std::ranges::any_of(clip.Tracks, [&](const AnimationTrack& track) { return track.Path == AnimationPath::Weights || (track.Node < relevant.size() && relevant[track.Node]); });
				if (!moves)
				{
					library.Warnings.push_back(fmt::format("clip '{}' moves no joint paired with the model, so it is left out", clip.Name));
					continue;
				}
				names.insert(clip.Name);
				library.Clips.push_back(clip.Name);
			}
		}
	}

	Ref<const AnimationClipSet> AnimationClipSet::Build(const Ref<MeshSource>& model, std::vector<Library> libraries, size_t maxBakedFloats)
	{
		BS_CORE_ASSERT(model, "a clip set needs a model");
		auto set = CreateRef<AnimationClipSet>();
		set->m_Model = model;
		set->m_ModelClips = model->Animations.size();
		// Names are taken only by clips that bake, so a later library supplies a name an earlier one could not.
		std::set<std::string> names;
		for (const AnimationClip& clip : model->Animations)
			names.insert(clip.Name);
		size_t floats = 0;
		for (Library& library : libraries)
		{
			Match(*model, library);
			if (!library.Error.empty())
				continue;
			for (const AnimationClip& clip : library.Source->Animations)
			{
				if (names.contains(clip.Name))
				{
					library.Shadowed.push_back(clip.Name);
					continue;
				}
				AnimationClip result;
				std::string error;
				if (!Retargeting::BakeClip(*model, *library.Source, library.Map, clip, result, library.Warnings, error))
					library.Warnings.push_back(fmt::format("clip '{}' cannot be retargeted: {}", clip.Name, error));
				else if (result.Tracks.empty())
					library.Warnings.push_back(fmt::format("clip '{}' moves no joint paired with the model, so it is left out", clip.Name));
				else if (const size_t size = BakedFloats(result); size > maxBakedFloats - floats)
					library.Warnings.push_back(fmt::format("clip '{}' is left out: the Animator's retargeted clips would exceed {:.1f} MiB", clip.Name, static_cast<double>(maxBakedFloats * sizeof(float)) / (1024.0 * 1024.0)));
				else
				{
					floats += size;
					names.insert(clip.Name);
					library.Clips.push_back(clip.Name);
					set->m_Baked.push_back(std::move(result));
				}
			}
		}
		set->m_Libraries = std::move(libraries);
		set->m_Memory.Resize(floats * sizeof(float));
		return set;
	}

	const AnimationClip& AnimationClipSet::Library::SourceClip(const std::string& name) const
	{
		const auto clip = std::ranges::find(Source->Animations, name, &AnimationClip::Name);
		BS_CORE_ASSERT(clip != Source->Animations.end(), "Clips names the source's clips");
		return *clip;
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

	bool AnimationClipSet::Matches(const Ref<MeshSource>& model, const AnimatorComponent& animator, const Resolver& resolve) const
	{
		if (model != m_Model)
			return false;
		size_t next = 0;
		for (size_t i = 0; i < animator.ClipLibraries.size(); i++)
		{
			const AnimationClipLibrary& entry = animator.ClipLibraries[i];
			if (entry.Mesh.empty())
				continue;
			if (next >= m_Libraries.size())
				return false;
			const Library& library = m_Libraries[next++];
			std::string error;
			if (library.Index != i || library.Entry != entry || library.Source != resolve(entry.Mesh, error))
				return false;
		}
		return next == m_Libraries.size();
	}

}
