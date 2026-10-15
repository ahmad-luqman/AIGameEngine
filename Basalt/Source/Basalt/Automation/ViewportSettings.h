#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <string>

namespace Basalt {

	enum class GizmoOperation
	{
		None,
		Translate,
		Rotate,
		Scale
	};
	enum class GizmoSpace
	{
		Local,
		World
	};

	// Shared operation names and keyboard bindings for schemas and editor controls.
	struct GizmoOperationInfo
	{
		GizmoOperation Operation;
		const char* Name;
		char Shortcut;
	};
	inline constexpr std::array GizmoOperations{
		GizmoOperationInfo{ GizmoOperation::None, "None", 'Q' },
		GizmoOperationInfo{ GizmoOperation::Translate, "Translate", 'W' },
		GizmoOperationInfo{ GizmoOperation::Rotate, "Rotate", 'E' },
		GizmoOperationInfo{ GizmoOperation::Scale, "Scale", 'R' }
	};

	// Typed snapshot of the validated automation preferences for frame-time editor consumers.
	struct ViewportOptions
	{
		GizmoOperation Operation;
		GizmoSpace Space;
		bool Snap, VertexSnap;
		float TranslationStep, RotationStep, ScaleStep;
		bool Grid, Colliders, LightGizmos, CameraGizmos, AudioGizmos, ParticleGizmos;
		bool UIBounds, DebugLines, SelectionOutline, SelectionBounds;
		float StepFor(GizmoOperation operation) const;
	};

	// Transient editor view preferences, validated by the same schema for UI and automation.
	class ViewportSettings
	{
	public:
		ViewportSettings();
		static nlohmann::json Schema();
		const nlohmann::json& Get() const { return m_Value; }
		ViewportOptions Options() const;
		bool Apply(const nlohmann::json& patch, std::string& error);

	private:
		nlohmann::json m_Value;
	};

}
