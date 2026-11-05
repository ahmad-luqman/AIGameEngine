#include "Panels/AnimationJointMapPanel.h"

#include "EditorColors.h"

#include "Basalt/Asset/MeshSource.h"
#include "Basalt/Automation/AutomationSession.h"

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

	const AnimationJointMapPanel::Report* AnimationJointMapPanel::GetReport() const
	{
		const Ref<MeshSource>& source = m_Source.GetSource();
		const Ref<MeshSource>& target = m_Target.GetSource();
		if (!source || !target)
		{
			m_Report.reset();
			return nullptr;
		}
		const auto& pairs = m_Draft.GetAsset().Joints;
		if (m_Report && source == m_Report->Source.lock() && target == m_Report->Target.lock() && pairs == m_Report->Pairs)
			return &*m_Report;
		Report report{ .Pairs = pairs, .Source = source, .Target = target };
		report.Map = Retargeting::BuildMap(*target, *source, pairs, report.Warnings);
		report.UnmatchedBones = Retargeting::UnmatchedHumanoidBones(*target, report.Map);
		m_Report = std::move(report);
		return &*m_Report;
	}

	std::optional<JointAssetEdit::MatchedPairs> AnimationJointMapPanel::ListMatches()
	{
		const Report* report = IsOpen() ? GetReport() : nullptr;
		if (!report)
			return std::nullopt;
		const auto matched = JointAssetEdit::AddMatchedPairs(m_Draft.GetAsset(), report->Map, *m_Target.GetSource(), *m_Source.GetSource());
		m_Draft.Record();
		return matched;
	}

	nlohmann::json AnimationJointMapPanel::DescribeBody() const
	{
		nlohmann::json state = { { "source", m_Source.GetPath() }, { "target", m_Target.GetPath() }, { "sourceError", m_Source.GetError() }, { "targetError", m_Target.GetError() } };
		if (const Report* report = GetReport())
			state.update({ { "mapped", report->Map.MappedCount() }, { "unmatchedHumanoidBones", report->UnmatchedBones }, { "warnings", report->Warnings } });
		return state;
	}

	void AnimationJointMapPanel::DrawBody()
	{
		m_Source.Draw("Library Model (Source)");
		m_Target.Draw("Animator Model (Target)");
		ImGui::BeginDisabled(!m_Source.GetSource() || !m_Target.GetSource());
		if (ImGui::Button("List Automatic Matches"))
			if (const auto matched = ListMatches())
			{
				std::string status = fmt::format("Listed {} automatic matches", matched->Added);
				if (matched->Skipped() > 0)
					status += fmt::format("; skipped {} (a joint another pair names: {}, over the limit: {}, a name two nodes share: {})", matched->Skipped(), matched->PairedElsewhere, matched->OverLimit, matched->SharedNames);
				m_Context.SetStatus(status, false);
			}
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
			RecordFinishedEdit();
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
					ImGui::SetTooltip("How this library joint turns so the library stands like the model at rest: degrees about the library model's fixed axes, X, then Y, then Z. Joints below it turn along.");
				RecordFinishedEdit();
				ImGui::TableSetColumnIndex(3);
				if (ImGui::SmallButton("x"))
					removed = i;
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		DrawListFooter(removed, "Add Pair", AnimationClipLibrary::MaxJoints);
	}

	void AnimationJointMapPanel::DrawReport()
	{
		const Report* report = GetReport();
		if (!report)
		{
			ImGui::TextDisabled("Name both models to see what matches.");
			return;
		}
		ImGui::Text("%zu joints of the model are mapped.", report->Map.MappedCount());
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
