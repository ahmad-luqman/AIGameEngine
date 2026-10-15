#include "Basalt/Automation/ViewportSettings.h"

#include "Basalt/Automation/CommandSchema.h"

namespace Basalt {

	ViewportSettings::ViewportSettings()
		: m_Value{ { "operation", "Translate" }, { "space", "Local" }, { "snap", false }, { "translationStep", 0.5f }, { "rotationStep", 15.0f }, { "scaleStep", 0.1f }, { "vertexSnap", false }, { "grid", true }, { "colliders", true }, { "lightGizmos", true }, { "cameraGizmos", true }, { "audioGizmos", true }, { "particleGizmos", true }, { "selectionBounds", false }, { "uiBounds", true }, { "debugLines", true }, { "selectionOutline", true } }
	{
	}

	nlohmann::json ViewportSettings::Schema()
	{
		namespace S = CommandSchema;
		auto operation = S::String("Q/W/E/R gizmo operation");
		operation["enum"] = nlohmann::json::array();
		for (const auto& entry : GizmoOperations)
			operation["enum"].push_back(entry.Name);
		auto space = S::String("Gizmo axes; scale follows local axes");
		space["enum"] = { "Local", "World" };
		auto properties = nlohmann::json{ { "operation", operation }, { "space", space }, { "translationStep", S::Number("World units", 0.0001, 10000) }, { "rotationStep", S::Number("Degrees", 0.0001, 360) }, { "scaleStep", S::Number("Scale increment", 0.0001, 100) } };
		for (const char* key : { "snap", "vertexSnap", "grid", "colliders", "lightGizmos", "cameraGizmos", "audioGizmos", "particleGizmos", "selectionBounds", "uiBounds", "debugLines", "selectionOutline" })
			properties[key] = S::Boolean();
		return S::Object(properties);
	}

	float ViewportOptions::StepFor(GizmoOperation operation) const
	{
		switch (operation)
		{
			case GizmoOperation::Rotate:
				return RotationStep;
			case GizmoOperation::Scale:
				return ScaleStep;
			default:
				return TranslationStep;
		}
	}

	ViewportOptions ViewportSettings::Options() const
	{
		GizmoOperation operation = GizmoOperation::None;
		for (const auto& entry : GizmoOperations)
			if (m_Value["operation"] == entry.Name)
				operation = entry.Operation;
		return { operation, m_Value["space"] == "World" ? GizmoSpace::World : GizmoSpace::Local,
				 m_Value["snap"].get<bool>(), m_Value["vertexSnap"].get<bool>(),
				 m_Value["translationStep"].get<float>(), m_Value["rotationStep"].get<float>(), m_Value["scaleStep"].get<float>(),
				 m_Value["grid"].get<bool>(), m_Value["colliders"].get<bool>(), m_Value["lightGizmos"].get<bool>(), m_Value["cameraGizmos"].get<bool>(), m_Value["audioGizmos"].get<bool>(), m_Value["particleGizmos"].get<bool>(),
				 m_Value["uiBounds"].get<bool>(), m_Value["debugLines"].get<bool>(), m_Value["selectionOutline"].get<bool>(), m_Value["selectionBounds"].get<bool>() };
	}

	bool ViewportSettings::Apply(const nlohmann::json& patch, std::string& error)
	{
		if (!CommandSchema::Validate(Schema(), patch, error))
			return false;
		// Validate the entire patch before updating, so a rejected preference cannot partially apply.
		m_Value.update(patch);
		return true;
	}

}
