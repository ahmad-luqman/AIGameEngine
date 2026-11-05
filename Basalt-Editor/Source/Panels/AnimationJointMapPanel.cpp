#include "Panels/AnimationJointMapPanel.h"

#include "Basalt/Asset/MeshSource.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <spdlog/fmt/ranges.h>

#include <algorithm>

namespace Basalt {

	void AnimationJointMapPanel::OnOpened()
	{
		const auto& scene = m_Context.Session.GetScene();
		const auto models = scene ? JointAssetEdit::FindJointMapModels(*scene, m_Draft.GetPath(), m_Context.Session.GetSelection()) : JointAssetEdit::JointMapModels{};
		m_Source.Set(models.Source);
		m_Target.Set(models.Target);
	}

	void AnimationJointMapPanel::OnReset()
	{
		m_Source.Set({});
		m_Target.Set({});
		m_Report.reset();
		m_ReportedSource.reset();
		m_ReportedTarget.reset();
	}

	void AnimationJointMapPanel::OnFilesChangedInBody(const std::vector<FileChange>& changes)
	{
		m_Source.OnFilesChanged(changes);
		m_Target.OnFilesChanged(changes);
	}

	void AnimationJointMapPanel::OnAssetMovedInBody(const std::string& from, const std::string& to)
	{
		m_Source.OnAssetMoved(from, to);
		m_Target.OnAssetMoved(from, to);
	}

	const AnimationJointMapPanel::Report* AnimationJointMapPanel::GetReport() const
	{
		const Ref<MeshSource>& source = m_Source.GetSource();
		const Ref<MeshSource>& target = m_Target.GetSource();
		if (!source || !target)
			return nullptr;
		const auto& pairs = m_Draft.GetAsset().Joints;
		if (m_Report && source == m_ReportedSource && target == m_ReportedTarget && pairs == m_ReportedPairs)
			return &*m_Report;
		Report report;
		report.Map = Retargeting::BuildMap(*target, *source, pairs, report.Warnings);
		report.Mapped = report.Map.MappedCount();
		for (size_t node = 0; node < report.Map.SourceOf.size(); node++)
			if (report.Map.SourceOf[node] < 0 && !Retargeting::HumanoidBone(target->Nodes[node].Name).empty())
				report.UnmatchedBones.push_back(target->Nodes[node].Name);
		m_Report = std::move(report);
		m_ReportedPairs = pairs;
		m_ReportedSource = source;
		m_ReportedTarget = target;
		return &*m_Report;
	}

	std::optional<size_t> AnimationJointMapPanel::AutoMatch()
	{
		const Report* report = GetReport();
		if (!report)
			return std::nullopt;
		const size_t added = JointAssetEdit::AddMatchedPairs(m_Draft.GetAsset(), report->Map, *m_Target.GetSource(), *m_Source.GetSource());
		m_Draft.Record();
		return added;
	}

	nlohmann::json AnimationJointMapPanel::DescribeBody() const
	{
		nlohmann::json state = { { "source", m_Source.GetPath() }, { "target", m_Target.GetPath() }, { "sourceError", m_Source.GetError() }, { "targetError", m_Target.GetError() } };
		if (const Report* report = GetReport())
			state.update({ { "mapped", report->Mapped }, { "unmatchedHumanoidBones", report->UnmatchedBones }, { "warnings", report->Warnings } });
		return state;
	}

	void AnimationJointMapPanel::DrawBody()
	{
		m_Source.Draw("Library Model (Source)");
		m_Target.Draw("Animator Model (Target)");
		ImGui::BeginDisabled(!m_Source.GetSource() || !m_Target.GetSource());
		if (ImGui::Button("List Automatic Matches"))
			if (const auto added = AutoMatch())
				m_Context.SetStatus(fmt::format("Listed {} automatic matches", *added), false);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Adds a pair for every joint the name and humanoid matching finds, so the map says them outright");
		DrawPairs();
		DrawReport();
	}

	void AnimationJointMapPanel::DrawJointField(const char* id, std::string& joint, const MeshSource* model)
	{
		ImGui::SetNextItemWidth(-1.0f);
		if (!model)
		{
			ImGui::InputText(id, &joint);
			if (ImGui::IsItemDeactivatedAfterEdit())
				m_Draft.Record();
			return;
		}
		if (ImGui::BeginCombo(id, joint.empty() ? "(none)" : joint.c_str(), ImGuiComboFlags_HeightLarge))
		{
			for (const MeshNode& node : model->Nodes)
				if (!node.Name.empty() && ImGui::Selectable(node.Name.c_str(), node.Name == joint))
				{
					joint = node.Name;
					m_Draft.Record();
				}
			ImGui::EndCombo();
		}
	}

	void AnimationJointMapPanel::DrawPairs()
	{
		auto& pairs = m_Draft.GetAsset().Joints;
		std::optional<size_t> removed;
		if (ImGui::BeginTable("Pairs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y * 0.6f)))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Source");
			ImGui::TableSetupColumn("Target");
			ImGui::TableSetupColumn("Rest Offset", ImGuiTableColumnFlags_WidthFixed, 170.0f);
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 22.0f);
			ImGui::TableHeadersRow();
			for (size_t i = 0; i < pairs.size(); i++)
			{
				ImGui::PushID(static_cast<int>(i));
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				DrawJointField("##Source", pairs[i].Source, m_Source.GetSource().get());
				ImGui::TableSetColumnIndex(1);
				DrawJointField("##Target", pairs[i].Target, m_Target.GetSource().get());
				ImGui::TableSetColumnIndex(2);
				ImGui::SetNextItemWidth(-1.0f);
				ImGui::DragFloat3("##Offset", &pairs[i].Offset.x, 0.5f, -360.0f, 360.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("How this library joint turns so the library stands like the model at rest: Euler degrees, in the library's model space. Joints below it turn along.");
				if (ImGui::IsItemDeactivatedAfterEdit())
					m_Draft.Record();
				ImGui::TableSetColumnIndex(3);
				if (ImGui::SmallButton("x"))
					removed = i;
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		if (removed)
		{
			pairs.erase(pairs.begin() + static_cast<std::ptrdiff_t>(*removed));
			m_Draft.Record();
		}
		ImGui::BeginDisabled(pairs.size() >= AnimationClipLibrary::MaxJoints);
		if (ImGui::Button("Add Pair"))
		{
			pairs.push_back({});
			m_Draft.Record();
		}
		ImGui::EndDisabled();
	}

	void AnimationJointMapPanel::DrawReport()
	{
		const Report* report = GetReport();
		if (!report)
		{
			ImGui::TextDisabled("Name both models to see what matches.");
			return;
		}
		ImGui::Text("%zu joints of the model are mapped.", report->Mapped);
		if (!report->UnmatchedBones.empty())
		{
			ImGui::TextColored(EditorColors::Warning(), "Humanoid bones without a match:");
			ImGui::SameLine();
			ImGui::TextWrapped("%s", fmt::format("{}", fmt::join(report->UnmatchedBones, ", ")).c_str());
		}
		for (const std::string& warning : report->Warnings)
			ImGui::TextColored(EditorColors::Warning(), "%s", warning.c_str());
	}

}
