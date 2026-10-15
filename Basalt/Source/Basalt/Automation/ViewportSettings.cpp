#include "Basalt/Automation/ViewportSettings.h"

#include "Basalt/Automation/CommandSchema.h"

namespace Basalt {

	ViewportSettings::ViewportSettings()
		: m_Value{ { "operation", "Translate" }, { "space", "Local" }, { "snap", false }, { "translationStep", 0.5f }, { "rotationStep", 15.0f }, { "scaleStep", 0.1f }, { "vertexSnap", false }, { "grid", true }, { "colliders", true }, { "lightGizmos", true }, { "cameraGizmos", true }, { "uiBounds", true }, { "debugLines", true }, { "selectionOutline", true } }
	{
	}

	nlohmann::json ViewportSettings::Schema()
	{
		namespace S = CommandSchema;
		auto operation = S::String("Q/W/E/R gizmo operation");
		operation["enum"] = { "None", "Translate", "Rotate", "Scale" };
		auto space = S::String("Gizmo axes; scale follows local axes");
		space["enum"] = { "Local", "World" };
		auto properties = nlohmann::json{ { "operation", operation }, { "space", space }, { "translationStep", S::Number("World units", 0.0001, 10000) }, { "rotationStep", S::Number("Degrees", 0.0001, 360) }, { "scaleStep", S::Number("Scale increment", 0.0001, 100) } };
		for (const char* key : { "snap", "vertexSnap", "grid", "colliders", "lightGizmos", "cameraGizmos", "uiBounds", "debugLines", "selectionOutline" })
			properties[key] = S::Boolean();
		return S::Object(properties);
	}

	bool ViewportSettings::Apply(const nlohmann::json& patch, std::string& error)
	{
		if (!CommandSchema::Validate(Schema(), patch, error))
			return false;
		m_Value.update(patch);
		return true;
	}

}
