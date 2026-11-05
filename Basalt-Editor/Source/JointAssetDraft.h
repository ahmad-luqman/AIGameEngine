#pragma once

#include "Basalt/Animation/AnimationJointMap.h"
#include "Basalt/Animation/AnimationMask.h"
#include "Basalt/Animation/Retargeting.h"
#include "Basalt/Core/UUID.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	class Scene;

	// The editor's working copy of one joint list file, a .banimmask or a .bjointmap (headless, so tests cover it). Widgets
	// change GetAsset() in place and call Record when an edit is finished; a save writes MakeFileText() through asset.write
	// and then calls MarkSaved. Dirtiness and undo follow the asset's Joints, which is all these files hold.
	template<typename Asset>
	class JointAssetDraft
	{
	public:
		// Reads a project-relative file. On failure (unreadable or invalid) the draft is left unchanged.
		bool Load(const std::string& path, std::string& outError);
		bool IsOpen() const { return !m_Path.empty(); }
		void Close();

		const std::string& GetPath() const { return m_Path; }
		Asset& GetAsset() { return m_Asset; }
		const Asset& GetAsset() const { return m_Asset; }
		// The edits differ from the file (an edit that is undone is not one).
		bool IsDirty() const { return m_Asset.Joints != m_Saved; }
		// Whether the edits make a valid file, and the reason otherwise (a pair without a joint, a joint listed twice).
		bool Validate(std::string& outError) const;
		std::string MakeFileText() const;
		// The save went through: the edits are the new baseline.
		void MarkSaved();
		// Discards the edits and the undo steps, reading the file again (it may have changed on disk); when it cannot be read
		// they stay.
		bool Revert(std::string& outError);
		// The file was changed by something else: a clean draft follows it (true when it read the new version), a dirty one
		// keeps its edits and reports the conflict (HasExternalChange) until Revert or a save. A new version that cannot be
		// read is a conflict too.
		bool OnFileChanged(std::string& outError);
		bool HasExternalChange() const { return m_ExternalChange; }
		// asset.move/asset.rename moved `from` to `to`: the draft follows its own file.
		void OnAssetMoved(const std::string& from, const std::string& to);

		// Takes the edits as one undo step; the same edits record nothing. Needs an open file.
		void Record();
		bool Undo();
		bool Redo();
		bool CanUndo() const { return m_Undo.size() > 1 || (!m_Undo.empty() && m_Undo.back() != m_Asset.Joints); }
		bool CanRedo() const { return !m_Redo.empty(); }

	private:
		using Joints = decltype(Asset::Joints);
		static constexpr size_t MaxUndoSteps = 64;

		std::string m_Path;
		Asset m_Asset;
		Joints m_Saved;
		// Oldest first; the last entry is the state the edits started from.
		std::vector<Joints> m_Undo, m_Redo;
		bool m_ExternalChange = false;
	};
	using AnimationMaskDraft = JointAssetDraft<AnimationMask>;
	using AnimationJointMapDraft = JointAssetDraft<AnimationJointMap>;

	// The edits the mask and joint map editors make, and what they read from the edit scene.
	namespace JointAssetEdit {
		// Gives `joint` the weight, adding its entry; false when the mask is full (AnimationLayer::MaxMaskJoints).
		bool SetMaskWeight(AnimationMask& mask, const std::string& joint, float weight);
		void RemoveMaskJoint(AnimationMask& mask, const std::string& joint);
		// What AddMatchedPairs did with the automatic matches.
		struct MatchedPairs
		{
			size_t Added = 0;
			// Left out because a pair already names one of the joints (one that did not apply, for a joint missing from a
			// model): a second entry would make the file invalid.
			size_t PairedElsewhere = 0;
			// Left out at AnimationClipLibrary::MaxJoints.
			size_t OverLimit = 0;
			// Left out because a name stands for the first node that has it, and this was not that node.
			size_t SharedNames = 0;

			size_t Skipped() const { return PairedElsewhere + OverLimit + SharedNames; }
		};
		// Lists what `retarget` (built for `map`'s pairs) matched by name or humanoid bone as pairs of `map`, in the target's
		// node order, so those joints no longer depend on automatic matching.
		MatchedPairs AddMatchedPairs(AnimationJointMap& map, const RetargetMap& retarget, const MeshSource& target, const MeshSource& source);

		// The project-relative path of a new asset called `name` in `directory`, or nullopt with the reason: a name that is
		// no safe file name on every platform, or a file already there (asset.write would replace it).
		std::optional<std::string> MakeNewAssetPath(const std::filesystem::path& directory, const std::string& name, const std::string& extension, std::string& outError);

		// The model to show an asset's joints on: the Animator that uses it (the first in the scene), else the selected
		// entity's. Empty when no Animator says.
		std::string FindMaskModel(Scene& scene, const std::string& maskPath, UUID selection);
		struct JointMapModels
		{
			std::string Source, Target;
		};
		// The library and the model of the first Animator whose ClipLibraries name the map, else the selected entity's model
		// and its first library.
		JointMapModels FindJointMapModels(Scene& scene, const std::string& mapPath, UUID selection);
	}

}
