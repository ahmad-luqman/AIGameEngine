#pragma once

#include "Basalt/Animation/Retargeting.h"
#include "Basalt/Core/Base.h"

#include <string>
#include <vector>

namespace Basalt {

	// The clips an Animator plays, by index: its model's own clips, then each clip library's clips retargeted onto the
	// model (Retargeting::BakeClip) whose names the model and earlier libraries do not use yet. Immutable once built;
	// Animators with the same model, libraries and joint map share one.
	class AnimationClipSet
	{
	public:
		// One AnimatorComponent::ClipLibraries entry as resolved for this set.
		struct Library
		{
			std::string Mesh;
			Ref<MeshSource> Source; // Null when it failed to load (Error says why).
			RetargetMap Map;
			std::vector<std::string> Clips;    // Added to the set.
			std::vector<std::string> Shadowed; // Skipped: an earlier clip has the name.
			std::string Error;
		};

		// `libraries` carries each entry's Mesh, and its Source when it loaded (otherwise its Error). Problems found while
		// retargeting (unmapped skeletons, clips that cannot bake) go to Problems.
		static Ref<const AnimationClipSet> Build(const Ref<MeshSource>& model, std::vector<Library> libraries, const std::vector<AnimationRetargetJoint>& joints);

		// The clip named `name`, or the first clip for an empty name; -1 when there is none.
		int32_t Find(const std::string& name) const;
		size_t Size() const { return m_ModelClips + m_Baked.size(); }
		bool Empty() const { return Size() == 0; }
		// The model's clips are read through the model on every call, not cached by address.
		const AnimationClip& operator[](int32_t index) const
		{
			const auto i = static_cast<size_t>(index);
			return i < m_ModelClips ? m_Model->Animations[i] : m_Baked[i - m_ModelClips];
		}
		// The library a clip came from, or -1 for the model's own.
		int32_t GetLibraryIndex(int32_t clip) const { return m_Origins[static_cast<size_t>(clip)]; }
		const std::vector<Library>& GetLibraries() const { return m_Libraries; }
		const std::vector<std::string>& GetProblems() const { return m_Problems; }
		const Ref<MeshSource>& GetModel() const { return m_Model; }

	private:
		Ref<MeshSource> m_Model;
		std::vector<Library> m_Libraries;
		size_t m_ModelClips = 0;
		std::vector<AnimationClip> m_Baked;
		// Per clip index.
		std::vector<int32_t> m_Origins;
		std::vector<std::string> m_Problems;
	};

}
