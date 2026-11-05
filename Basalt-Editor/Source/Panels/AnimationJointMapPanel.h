#pragma once

#include "Panels/JointAssetPanel.h"

#include "Basalt/Automation/EditorCommandSchemas.h"

namespace Basalt {

	// Edits one .bjointmap in its own window: its pairs with their rest offsets, picked from a library model (the Source) and
	// the Animator's model (the Target), and what those two models make of the edits, as Retargeting::BuildMap reads them:
	// what matches on its own, which humanoid bones stay unmatched, and every warning.
	class AnimationJointMapPanel final : public JointAssetPanel<AnimationJointMap>
	{
	public:
		static constexpr const char* Title = EditorCommandSchemas::JointMapTitle;

		explicit AnimationJointMapPanel(EditorContext& context)
			: JointAssetPanel(context, Title)
		{
		}

		PreviewModel& GetSourceModel() { return m_Source; }
		PreviewModel& GetTargetModel() { return m_Target; }
		// Lists the automatic matches as pairs of the draft (one undo step); how many, or nullopt without both models.
		std::optional<size_t> AutoMatch();

	protected:
		void DrawBody() override;
		nlohmann::json DescribeBody() const override;
		void OnOpened() override;
		void OnReset() override;
		void OnFilesChangedInBody(const std::vector<FileChange>& changes) override;
		void OnAssetMovedInBody(const std::string& from, const std::string& to) override;

	private:
		// What the two models make of the pairs, read again when the pairs or the models change.
		struct Report
		{
			RetargetMap Map;
			std::vector<std::string> Warnings;
			std::vector<std::string> UnmatchedBones;
			size_t Mapped = 0;
		};
		const Report* GetReport() const;
		void DrawPairs();
		void DrawJointField(const char* id, std::string& joint, const MeshSource* model);
		void DrawReport();

		PreviewModel m_Source, m_Target;
		mutable std::optional<Report> m_Report;
		mutable std::vector<AnimationRetargetJoint> m_ReportedPairs;
		// Kept alive, so a reloaded model at the same address cannot pass for the one reported on.
		mutable Ref<MeshSource> m_ReportedSource, m_ReportedTarget;
	};

}
