#pragma once

#include "Panels/JointAssetPanel.h"

#include "Breccia/Automation/EditorCommandSchemas.h"

namespace Breccia {

	// Edits one .banimmask in its own window: the joints of a preview model as a tree, each listed at a weight or taking the
	// weight of the nearest listed joint above it (0 with none, once the mask lists anything), as layers read the mask
	// (AnimationMaskWeights). Without a model the file's entries are a plain list.
	class AnimationMaskPanel final : public JointAssetPanel<AnimationMask>
	{
	public:
		static constexpr const char* Title = EditorCommandSchemas::AnimationMaskTitle;

		explicit AnimationMaskPanel(EditorContext& context)
			: JointAssetPanel(context, Title, { &m_Model })
		{
		}

		PreviewModel& GetModel() { return m_Model; }

	protected:
		void DrawBody() override;
		nlohmann::json DescribeBody() const override;
		void OnOpened() override;

	private:
		void DrawTree();
		void DrawNode(uint32_t node, const std::vector<std::vector<uint32_t>>& children, const std::vector<float>& weights);
		void DrawEntries();

		PreviewModel m_Model;
	};

}
