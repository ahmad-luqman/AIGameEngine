#pragma once

#include "Panels/JointAssetPanel.h"

#include "Basalt/Automation/EditorCommandSchemas.h"

namespace Basalt {

	// Edits one .banimmask in its own window: the joints of a preview model as a tree, each listed at a weight or taking the
	// weight of the nearest listed joint above it, as layers read the mask (AnimationMaskWeights). Without a model the file's
	// entries are a plain list.
	class AnimationMaskPanel final : public JointAssetPanel<AnimationMask>
	{
	public:
		static constexpr const char* Title = EditorCommandSchemas::AnimationMaskTitle;

		explicit AnimationMaskPanel(EditorContext& context)
			: JointAssetPanel(context, Title)
		{
		}

		PreviewModel& GetModel() { return m_Model; }

	protected:
		void DrawBody() override;
		nlohmann::json DescribeBody() const override;
		void OnOpened() override;
		void OnReset() override
		{
			m_Model.Set({});
			m_ChildrenOf.reset();
		}
		void OnFilesChangedInBody(const std::vector<FileChange>& changes) override { m_Model.OnFilesChanged(changes); }
		void OnAssetMovedInBody(const std::string& from, const std::string& to) override { m_Model.OnAssetMoved(from, to); }

	private:
		void DrawTree();
		void DrawNode(uint32_t node, const std::vector<float>& weights);
		void DrawEntries();
		// The mask's entries that name no node of the model.
		std::vector<std::string> FindMissing() const;

		PreviewModel m_Model;
		// Per node of the model: its children, in order.
		std::vector<std::vector<uint32_t>> m_Children;
		std::vector<uint32_t> m_Roots;
		// The model they describe, kept alive so a reloaded one at the same address cannot pass for it.
		Ref<MeshSource> m_ChildrenOf;
	};

}
