#pragma once

#include "Basalt/Animation/Retargeting.h"
#include "Basalt/Core/Base.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Core/MemoryTracker.h"
#include "Basalt/Scene/Components.h"

#include <functional>
#include <string>
#include <vector>

namespace Basalt {

	// The clips an Animator plays, by index: its model's own clips, then each clip library's clips retargeted onto the
	// model (Retargeting::BakeClip) whose names the model and earlier libraries do not use yet. Immutable once built;
	// Animators with the same model and clip libraries share one. Load and Plan give editors and tools the same
	// resolution without baking.
	class AnimationClipSet
	{
	public:
		// Baked floats one set holds at most (256 MiB); clips past it are left out with a warning.
		static constexpr size_t MaxBakedFloats = size_t{ 64 } * 1024 * 1024;

		// One named AnimatorComponent::ClipLibraries entry and how it resolves against the model.
		struct Library
		{
			size_t Index = 0; // Position in ClipLibraries (unnamed entries are skipped).
			AnimationClipLibrary Entry;
			Ref<MeshSource> Source; // Null when it failed to load.
			// Why the library adds no clips (a load failure, or no joint matching); empty otherwise.
			std::string Error;
			RetargetMap Map;
			// Joint map problems (Retargeting::BuildMap), clips past the bake budget, and in Build clips that do not bake.
			std::vector<std::string> Warnings;
			std::vector<std::string> Clips;    // Added to the set, in the library's order.
			std::vector<std::string> Shadowed; // Skipped: the model or an earlier library has the name.
		};

		// Loads each named ClipLibraries entry through AssetManager, or `resolve` (main thread).
		static std::vector<Library> Load(const AnimatorComponent& animator);
		static std::vector<Library> Load(const AnimatorComponent& animator, const std::function<Ref<MeshSource>(const std::string&)>& resolve);
		// Matches each loaded library to the model and lists the clips it would add, without baking.
		static void Plan(const MeshSource& model, std::vector<Library>& libraries);
		// Plans and bakes `libraries` (from Load) onto the model.
		static Ref<const AnimationClipSet> Build(const Ref<MeshSource>& model, std::vector<Library> libraries);

		// The clip named `name`, or the first clip for an empty name; -1 when there is none.
		int32_t Find(const std::string& name) const;
		size_t Size() const { return m_ModelClips + m_Baked.size(); }
		bool Empty() const { return Size() == 0; }
		// `index` in [0, Size()). The model's clips are not copied but read through the model, which this set keeps alive.
		const AnimationClip& operator[](int32_t index) const
		{
			BS_CORE_ASSERT(index >= 0 && static_cast<size_t>(index) < Size(), "clip index out of range");
			const auto i = static_cast<size_t>(index);
			return i < m_ModelClips ? m_Model->Animations[i] : m_Baked[i - m_ModelClips];
		}
		const Ref<MeshSource>& GetModel() const { return m_Model; }
		const std::vector<Library>& GetLibraries() const { return m_Libraries; }
		// Whether this set was built for `model` and these loaded libraries (same entries and the same sources).
		bool Matches(const Ref<MeshSource>& model, const std::vector<Library>& loaded) const;

	private:
		Ref<MeshSource> m_Model;
		std::vector<Library> m_Libraries;
		size_t m_ModelClips = 0;
		std::vector<AnimationClip> m_Baked;
		// The baked clips' payload, reported by memory.stats with other asset data.
		MemoryAllocation m_Memory{ MemoryCategory::Assets };
	};

}
