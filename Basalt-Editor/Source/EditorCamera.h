#pragma once

#include "Basalt/Core/Timestep.h"
#include "Basalt/Renderer/SceneRenderer.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Basalt {

	// Orbit/fly camera for the editor viewport.
	//   Right mouse drag: look around; with WASD/QE: fly (Shift = fast)
	//   Alt + left drag: orbit around the focal point
	//   Middle drag: pan; mouse wheel: dolly towards the focal point
	class EditorCamera
	{
	public:
		EditorCamera();

		// Processes input when the viewport is hovered/focused.
		void OnUpdate(Timestep ts, bool hovered, bool focused);
		void SetViewportSize(uint32_t width, uint32_t height);

		// Places the camera at position looking at target.
		void LookAt(const glm::vec3& position, const glm::vec3& target);
		// Frames a sphere around center.
		void Focus(const glm::vec3& center, float radius);

		RenderCamera GetRenderCamera() const;
		glm::mat4 GetView() const;
		glm::mat4 GetProjection() const;
		glm::vec3 GetPosition() const;
		glm::vec3 GetForward() const;
		const glm::vec3& GetFocalPoint() const { return m_FocalPoint; }
		float GetDistance() const { return m_Distance; }

	private:
		glm::quat GetOrientation() const;

	private:
		glm::vec3 m_FocalPoint = { 0.0f, 0.0f, 0.0f };
		float m_Distance = 10.0f;
		float m_Pitch = 0.35f;
		float m_Yaw = 0.0f;
		float m_VerticalFov = glm::radians(50.0f);
		float m_Near = 0.05f;
		float m_Far = 2000.0f;
		float m_AspectRatio = 16.0f / 9.0f;
		bool m_Dragging = false;
	};

}
