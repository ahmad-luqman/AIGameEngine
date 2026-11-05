#include "Panels/AnimationMaskPanel.h"

#include "EditorColors.h"

#include "Basalt/Animation/AnimationPose.h"
#include "Basalt/Asset/MeshSource.h"
#include "Basalt/Automation/AutomationSession.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>

namespace Basalt {

	void AnimationMaskPanel::OnOpened()
	{
		const auto& scene = m_Context.Session.GetScene();
		m_Model.Set(scene ? JointAssetEdit::FindMaskModel(*scene, m_Draft.GetPath(), m_Context.Session.GetSelection()) : std::string());
	}

	nlohmann::json AnimationMaskPanel::DescribeBody() const
	{
		std::vector<std::string> missing;
		if (const auto& source = m_Model.GetSource())
			AnimationMaskWeights(*source, m_Draft.GetAsset().Joints, missing);
		return { { "model", m_Model.GetPath() }, { "modelError", m_Model.GetError() }, { "missing", std::move(missing) } };
	}

	void AnimationMaskPanel::DrawBody()
	{
		m_Model.Draw("Preview Model");
		if (m_Draft.GetAsset().Joints.empty())
			ImGui::TextDisabled("An empty mask shows the layer on the whole body.");
		if (m_Model.GetSource())
			DrawTree();
		else
			DrawEntries();
	}

	void AnimationMaskPanel::DrawTree()
	{
		const MeshSource& source = *m_Model.GetSource();
		std::vector<std::vector<uint32_t>> children(source.Nodes.size());
		std::vector<uint32_t> roots;
		for (uint32_t node = 0; node < source.Nodes.size(); node++)
		{
			const int32_t parent = source.Nodes[node].Parent;
			(parent >= 0 ? children[parent] : roots).push_back(node);
		}
		std::vector<std::string> missing;
		const std::vector<float> weights = AnimationMaskWeights(source, m_Draft.GetAsset().Joints, missing);
		if (ImGui::BeginTable("Joints", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, missing.empty() ? 0.0f : -ImGui::GetTextLineHeightWithSpacing() * (static_cast<float>(missing.size()) + 2.0f))))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Joint", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Listed", ImGuiTableColumnFlags_WidthFixed, 50.0f);
			ImGui::TableSetupColumn("Weight", ImGuiTableColumnFlags_WidthFixed, 120.0f);
			ImGui::TableHeadersRow();
			for (uint32_t root : roots)
				DrawNode(root, children, weights);
			ImGui::EndTable();
		}
		if (missing.empty())
			return;
		ImGui::TextColored(EditorColors::Warning(), "Listed, but not in this model:");
		for (const std::string& joint : missing)
		{
			ImGui::PushID(joint.c_str());
			if (ImGui::SmallButton("Remove"))
			{
				JointAssetEdit::RemoveMaskJoint(m_Draft.GetAsset(), joint);
				m_Draft.Record();
			}
			ImGui::SameLine();
			ImGui::TextUnformatted(joint.c_str());
			ImGui::PopID();
		}
	}

	void AnimationMaskPanel::DrawNode(uint32_t node, const std::vector<std::vector<uint32_t>>& children, const std::vector<float>& weights)
	{
		const MeshSource& source = *m_Model.GetSource();
		const std::string& name = source.Nodes[node].Name;
		auto& joints = m_Draft.GetAsset().Joints;
		ImGui::PushID(static_cast<int>(node));
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
		if (children[node].empty())
			flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		const bool open = ImGui::TreeNodeEx(name.empty() ? "(unnamed)" : name.c_str(), flags);

		const auto entry = std::ranges::find(joints, name, &AnimationMaskJoint::Joint);
		const bool wasListed = entry != joints.end();
		bool listed = wasListed;
		// An entry names the first node of a name, and an unnamed node cannot be listed.
		const bool canList = !name.empty() && FindNode(source, name) == static_cast<int32_t>(node) && (wasListed || joints.size() < AnimationLayer::MaxMaskJoints);
		ImGui::TableSetColumnIndex(1);
		ImGui::BeginDisabled(!canList);
		if (ImGui::Checkbox("##Listed", &listed))
		{
			// A new entry starts at the weight the joint already takes.
			if (listed)
				JointAssetEdit::SetMaskWeight(m_Draft.GetAsset(), name, weights[node]);
			else
				JointAssetEdit::RemoveMaskJoint(m_Draft.GetAsset(), name);
			m_Draft.Record();
		}
		ImGui::EndDisabled();
		ImGui::TableSetColumnIndex(2);
		if (listed && wasListed)
		{
			ImGui::SetNextItemWidth(-1.0f);
			// The box did not change this row's entry, so `entry` still stands.
			ImGui::SliderFloat("##Weight", &entry->Weight, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			RecordFinishedEdit();
		}
		else
			ImGui::TextDisabled("%.2f", static_cast<double>(weights[node]));
		ImGui::PopID();
		if (open && !children[node].empty())
		{
			for (uint32_t child : children[node])
				DrawNode(child, children, weights);
			ImGui::TreePop();
		}
	}

	void AnimationMaskPanel::DrawEntries()
	{
		auto& joints = m_Draft.GetAsset().Joints;
		ImGui::TextDisabled("Name a model above to pick joints from its skeleton.");
		std::optional<size_t> removed;
		for (size_t i = 0; i < joints.size(); i++)
		{
			ImGui::PushID(static_cast<int>(i));
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
			ImGui::InputText("##Joint", &joints[i].Joint);
			RecordFinishedEdit();
			ImGui::SameLine();
			ImGui::SetNextItemWidth(120.0f);
			ImGui::SliderFloat("##Weight", &joints[i].Weight, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			RecordFinishedEdit();
			ImGui::SameLine();
			if (ImGui::SmallButton("Remove"))
				removed = i;
			ImGui::PopID();
		}
		DrawListFooter(removed, "Add Joint", AnimationLayer::MaxMaskJoints);
	}

}
