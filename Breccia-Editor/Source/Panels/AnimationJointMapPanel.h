#pragma once

#include "Panels/JointAssetPanel.h"

#include "Breccia/Automation/EditorCommandSchemas.h"

#include <memory>

namespace Breccia {

	// Edits one .bjointmap in its own window: its pairs with their rest offsets, picked from a library model (the Source) and
	// the Animator's model (the Target), and what those two models make of the edits, as Retargeting::BuildMap reads them:
	// what matches on its own, which humanoid bones stay unmatched, and every warning.
	class AnimationJointMapPanel final : public JointAssetPanel<AnimationJointMap>
	{
	public:
		static constexpr const char* Title = EditorCommandSchemas::JointMapTitle;

		explicit AnimationJointMapPanel(EditorContext& context)
			: JointAssetPanel(context, Title, { &m_Source, &m_Target })
		{
		}

		PreviewModel& GetSourceModel() { return m_Source; }
		PreviewModel& GetTargetModel() { return m_Target; }
		// Lists the automatic matches as pairs of the draft (one undo step); nullopt without an open file and both models.
		std::optional<JointAssetEdit::MatchedPairs> ListMatches();

	protected:
		void DrawBody() override;
		nlohmann::json DescribeBody() const override;
		void OnOpened() override;

	private:
		// What the two models make of the pairs, read again when the pairs or the models change.
		struct Report
		{
			// What it was read from. Weak, so a closed window keeps no model alive; an expired one, even at the same address,
			// never matches a reloaded model.
			std::vector<AnimationRetargetJoint> Pairs;
			std::weak_ptr<MeshSource> Source, Target;
			RetargetMap Map;
			std::vector<std::string> Warnings;
			std::vector<std::string> UnmatchedBones;
		};
		const Report* GetReport() const;
		void DrawPairs();
		void DrawJointField(const char* id, std::string& joint, const MeshSource* model);
		void DrawReport();

		PreviewModel m_Source, m_Target;
		// A cache: editor.joint_map reads it with no frame drawn, and BuildMap is too heavy for every frame.
		mutable std::optional<Report> m_Report;
	};

}
