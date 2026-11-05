#include "Panels/AnimationMaskPanel.h"

#include "Basalt/Animation/AnimationPose.h"
#include "Basalt/Asset/MeshSource.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>

namespace Basalt {

	void AnimationMaskPanel::OnOpened()
	{
		const auto& scene = m_Context.Session.GetScene();
		m_Model.Set(scene ? JointAssetEdit::FindMaskModel(*scene, m_Draft.GetPath(), m_Context.Session.GetSelection()) : std::string());
	}

	std::vector<std::string> AnimationMaskPanel::FindMissing() const
	{
		std::vector<std::string> missing;
		if (const auto& source = m_Model.GetSource())
			AnimationMaskWeights(*source, m_Draft.GetAsset().Joints, missing);
		return missing;
	}

	nlohmann::json AnimationMaskPanel::DescribeBody() const
	{
		return { { "model", m_Model.GetPath() }, { "modelError", m_Model.GetError() }, { "missing", FindMissing() } };
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
		if (m_ChildrenOf != m_Model.GetSource())
		{
			m_ChildrenOf = m_Model.GetSource();
			m_Children.assign(source.Nodes.size(), {});
			m_Roots.clear();
			for (uint32_t node = 0; node < source.Nodes.size(); node++)
			{
				const int32_t parent = source.Nodes[node].Parent;
				(parent >= 0 ? m_Children[parent] : m_Roots).push_back(node);
			}
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
			for (uint32_t root : m_Roots)
				DrawNode(root, weights);
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

	void AnimationMaskPanel::DrawNode(uint32_t node, const std::vector<float>& weights)
	{
		const MeshSource& source = *m_Model.GetSource();
		const std::string& name = source.Nodes[node].Name;
		auto& joints = m_Draft.GetAsset().Joints;
		ImGui::PushID(static_cast<int>(node));
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
		if (m_Children[node].empty())
			flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		const bool open = ImGui::TreeNodeEx(name.empty() ? "(unnamed)" : name.c_str(), flags);

		const auto entry = std::ranges::find(joints, name, &AnimationMaskJoint::Joint);
		const bool wasListed = entry != joints.end();
		bool listed = wasListed;
		// A second node of a name reads the first one's entry, as FindNode does; an unnamed node cannot be listed.
		const bool canList = !name.empty() && FindNode(source, name) == static_cast<int32_t>(node) && (wasListed || joints.size() < AnimationLayer::MaxMaskJoints);
		ImGui::TableSetColumnIndex(1);
		ImGui::BeginDisabled(!canList);
		if (ImGui::Checkbox("##Listed", &listed))
		{
			// A new entry starts at the weight the joint already takes, so listing it changes nothing yet.
			if (listed)
				JointAssetEdit::SetMaskWeight(m_Draft.GetAsset(), name, joints.empty() ? 1.0f : weights[node]);
			else
				JointAssetEdit::RemoveMaskJoint(m_Draft.GetAsset(), name);
			m_Draft.Record();
		}
		ImGui::EndDisabled();
		ImGui::TableSetColumnIndex(2);
		if (listed && wasListed)
		{
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##Weight", &std::ranges::find(joints, name, &AnimationMaskJoint::Joint)->Weight, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (ImGui::IsItemDeactivatedAfterEdit())
				m_Draft.Record();
		}
		else
			ImGui::TextDisabled("%.2f", static_cast<double>(weights[node]));
		ImGui::PopID();
		if (open && !m_Children[node].empty())
		{
			for (uint32_t child : m_Children[node])
				DrawNode(child, weights);
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
			if (ImGui::IsItemDeactivatedAfterEdit())
				m_Draft.Record();
			ImGui::SameLine();
			ImGui::SetNextItemWidth(120.0f);
			ImGui::SliderFloat("##Weight", &joints[i].Weight, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (ImGui::IsItemDeactivatedAfterEdit())
				m_Draft.Record();
			ImGui::SameLine();
			if (ImGui::SmallButton("Remove"))
				removed = i;
			ImGui::PopID();
		}
		if (removed)
		{
			joints.erase(joints.begin() + static_cast<std::ptrdiff_t>(*removed));
			m_Draft.Record();
		}
		ImGui::BeginDisabled(joints.size() >= AnimationLayer::MaxMaskJoints);
		if (ImGui::Button("Add Joint"))
		{
			joints.push_back({});
			m_Draft.Record();
		}
		ImGui::EndDisabled();
	}

}
