#include "Breccia/Animation/AnimationClipSet.h"

#include "Breccia/Animation/AnimationJointMap.h"
#include "Breccia/Asset/AssetManager.h"
#include "Breccia/Core/Log.h"

#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <set>

namespace Breccia {

	namespace {
		size_t BakedFloats(const AnimationClip& clip)
		{
			size_t floats = 0;
			for (const AnimationTrack& track : clip.Tracks)
				floats += track.Times.size() + track.Values.size() * 4 + track.Weights.size();
			return floats;
		}
	}

	namespace {
		// An AssetManager getter as a resolver function: the asset, or null with AssetManager's reason.
		template<typename Load>
		auto ReportingLoad(Load load)
		{
			return [load](const std::string& key, std::string& outError) {
				auto asset = load(key);
				if (!asset)
					outError = AssetManager::GetError(key);
				return asset;
			};
		}
	}

	AnimationClipSet::Resolver AnimationClipSet::AssetResolver()
	{
		return { .Mesh = ReportingLoad(&AssetManager::GetMesh), .JointMap = ReportingLoad(&AssetManager::GetAnimationJointMap) };
	}

	std::vector<AnimationClipSet::Library> AnimationClipSet::Load(const AnimatorComponent& animator)
	{
		return Load(animator, AssetResolver());
	}

	std::vector<AnimationClipSet::Library> AnimationClipSet::Load(const AnimatorComponent& animator, const Resolver& resolve)
	{
		BR_CORE_ASSERT(resolve.Mesh && resolve.JointMap, "a Resolver loads both models and joint maps");
		std::vector<Library> libraries;
		for (size_t i = 0; i < animator.ClipLibraries.size(); i++)
		{
			const AnimationClipLibrary& entry = animator.ClipLibraries[i];
			// Unnamed entries are the inspector's additions.
			if (entry.Mesh.empty())
				continue;
			std::string error;
			Library& library = libraries.emplace_back(Library{ .Index = i, .Entry = entry, .Source = resolve.Mesh(entry.Mesh, error) });
			std::vector<std::string> problems;
			if (!library.Source)
				problems.push_back("cannot load: " + error);
			if (!entry.JointMap.empty())
			{
				std::string mapError;
				library.JointMap = resolve.JointMap(entry.JointMap, mapError);
				// A map that does not load adds no clips, rather than pairing joints by name alone and posing them wrongly.
				if (!library.JointMap)
					problems.push_back(fmt::format("cannot load joint map '{}': {}", entry.JointMap, mapError));
			}
			library.LoadError = fmt::format("{}", fmt::join(problems, "; "));
			library.Error = library.LoadError;
		}
		return libraries;
	}

	void AnimationClipSet::Match(const MeshSource& model, Library& library)
	{
		// Planning again starts over, from what loading found.
		library.Map = {};
		library.Warnings.clear();
		library.Clips.clear();
		library.Shadowed.clear();
		library.Error = library.LoadError;
		if (!library.Source && library.Error.empty())
			library.Error = "cannot load: no model";
		if (!library.Error.empty())
			return;
		// A reference pose that is not there adds no clips, rather than retargeting from the wrong rest style.
		RetargetReferences references{ .TargetTime = library.Entry.ModelReferenceTime, .SourceTime = library.Entry.ReferenceTime };
		const auto findReference = [&](const MeshSource& owner, const std::string& name, const char* field, const AnimationClip*& out) {
			if (name.empty())
				return true;
			const auto clip = std::ranges::find(owner.Animations, name, &AnimationClip::Name);
			if (clip == owner.Animations.end())
			{
				library.Error = fmt::format("{} '{}' is not a clip of '{}'", field, name, owner.Key);
				return false;
			}
			out = &*clip;
			return true;
		};
		if (!findReference(*library.Source, library.Entry.ReferenceClip, "ReferenceClip", references.Source) || !findReference(model, library.Entry.ModelReferenceClip, "ModelReferenceClip", references.Target))
			return;
		// Times that do nothing are most likely a half-done setup.
		const auto checkTime = [&](const AnimationClip* clip, float time, const char* field, const char* clipField) {
			if (clip && time > clip->Duration)
				library.Warnings.push_back(fmt::format("{} {} is past the end of clip '{}' ({} s), so its last pose is the reference", field, time, clip->Name, clip->Duration));
			else if (!clip && time != 0)
				library.Warnings.push_back(fmt::format("{} {} has no {}, so the rest pose is used", field, time, clipField));
		};
		checkTime(references.Source, references.SourceTime, "ReferenceTime", "ReferenceClip");
		checkTime(references.Target, references.TargetTime, "ModelReferenceTime", "ModelReferenceClip");
		library.Map = Retargeting::BuildMap(model, *library.Source, CombineJointPairs(library.Entry, library.JointMap.get()), library.Warnings, references);
		if (library.Source->Animations.empty())
			library.Error = fmt::format("'{}' has no animation clips", library.Source->Key);
		else if (library.Map.MappedCount() == 0)
			library.Error = "no joint matches the model's skeleton (pair them in the library's Joints or JointMap)";
		else if (library.Map.TargetRest.Rotations.empty())
			library.Error = references.Source || references.Target ? "a rest or reference pose is not finite" : "a rest pose is not finite";
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
		BR_CORE_ASSERT(model, "a clip set needs a model");
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
		BR_CORE_ASSERT(clip != Source->Animations.end(), "Clips names the source's clips");
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
		BR_CORE_ASSERT(resolve.Mesh && resolve.JointMap, "a Resolver loads both models and joint maps");
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
			if (library.Index != i || library.Entry != entry || library.Source != resolve.Mesh(entry.Mesh, error))
				return false;
			if (!entry.JointMap.empty() && library.JointMap != resolve.JointMap(entry.JointMap, error))
				return false;
		}
		return next == m_Libraries.size();
	}

}
