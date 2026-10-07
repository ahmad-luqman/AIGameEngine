#include "EditorCamera.h"

#include "Basalt/Core/Input.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace Basalt {

	EditorCamera::EditorCamera()
	{
		LookAt({ 6.0f, 5.0f, 10.0f }, { 0.0f, 0.5f, 0.0f });
	}

	void EditorCamera::SetViewportSize(uint32_t width, uint32_t height)
	{
		if (width > 0 && height > 0)
			m_AspectRatio = static_cast<float>(width) / static_cast<float>(height);
	}

	glm::quat EditorCamera::GetOrientation() const
	{
		// Yaw about world Y applied after pitch about X; LookAt inverts exactly this.
		return glm::angleAxis(-m_Yaw, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(-m_Pitch, glm::vec3(1.0f, 0.0f, 0.0f));
	}

	glm::vec3 EditorCamera::GetForward() const
	{
		return (GetOrientation() * glm::vec3(0.0f, 0.0f, -1.0f));
	}

	glm::vec3 EditorCamera::GetPosition() const
	{
		return m_FocalPoint - GetForward() * m_Distance;
	}

	glm::mat4 EditorCamera::GetView() const
	{
		const glm::mat4 transform = glm::translate(glm::mat4(1.0f), GetPosition()) * glm::mat4_cast(GetOrientation());
		return glm::inverse(transform);
	}

	glm::mat4 EditorCamera::GetProjection() const
	{
		return glm::perspective(m_VerticalFov, m_AspectRatio, m_Near, m_Far);
	}

	RenderCamera EditorCamera::GetRenderCamera() const
	{
		RenderCamera camera;
		camera.View = GetView();
		camera.Projection = GetProjection();
		camera.Position = GetPosition();
		camera.Near = m_Near;
		camera.Far = m_Far;
		return camera;
	}

	void EditorCamera::LookAt(const glm::vec3& position, const glm::vec3& target)
	{
		glm::vec3 direction = target - position;
		const float length = glm::length(direction);
		if (length < 1e-4f)
			return;
		direction /= length;
		m_FocalPoint = target;
		m_Distance = length;
		m_Pitch = std::asin(std::clamp(-direction.y, -1.0f, 1.0f));
		m_Yaw = std::atan2(direction.x, -direction.z);
	}

	void EditorCamera::Focus(const glm::vec3& center, float radius)
	{
		m_FocalPoint = center;
		m_Distance = std::max(radius, 0.25f) / std::sin(m_VerticalFov * 0.5f) * 1.2f;
	}

	void EditorCamera::OnUpdate(Timestep ts, bool hovered, bool focused)
	{
		const glm::vec2 delta = Input::GetMouseDelta() * 0.003f;
		const bool right = Input::IsMouseButtonDown(MouseCode::Right);
		const bool middle = Input::IsMouseButtonDown(MouseCode::Middle);
		const bool orbit = Input::IsMouseButtonDown(MouseCode::Left) && (Input::IsKeyDown(KeyCode::LeftAlt) || Input::IsKeyDown(KeyCode::RightAlt));

		// Drags that start in the viewport keep working when the cursor leaves it.
		if (!m_Dragging && hovered && (right || middle || orbit))
			m_Dragging = true;
		if (!right && !middle && !orbit)
			m_Dragging = false;
		if (!m_Dragging && !hovered)
			return;

		if (right)
		{
			// Fly mode: rotate around the eye, move with WASD/QE.
			const glm::vec3 position = GetPosition();
			m_Yaw += delta.x;
			m_Pitch = std::clamp(m_Pitch + delta.y, -1.55f, 1.55f);
			glm::vec3 move(0.0f);
			const glm::quat orientation = GetOrientation();
			if (Input::IsKeyDown(KeyCode::W))
				move += (orientation * glm::vec3(0.0f, 0.0f, -1.0f));
			if (Input::IsKeyDown(KeyCode::S))
				move -= (orientation * glm::vec3(0.0f, 0.0f, -1.0f));
			if (Input::IsKeyDown(KeyCode::D))
				move += (orientation * glm::vec3(1.0f, 0.0f, 0.0f));
			if (Input::IsKeyDown(KeyCode::A))
				move -= (orientation * glm::vec3(1.0f, 0.0f, 0.0f));
			if (Input::IsKeyDown(KeyCode::E))
				move += glm::vec3(0.0f, 1.0f, 0.0f);
			if (Input::IsKeyDown(KeyCode::Q))
				move -= glm::vec3(0.0f, 1.0f, 0.0f);
			const float speed = (Input::IsKeyDown(KeyCode::LeftShift) ? 20.0f : 5.0f) * std::max(1.0f, m_Distance * 0.2f);
			const glm::vec3 newPosition = position + move * speed * ts.GetSeconds();
			m_FocalPoint = newPosition + GetForward() * m_Distance;
		}
		else if (orbit)
		{
			m_Yaw += delta.x;
			m_Pitch = std::clamp(m_Pitch + delta.y, -1.55f, 1.55f);
		}
		else if (middle)
		{
			const glm::quat orientation = GetOrientation();
			const float panSpeed = m_Distance * 0.6f;
			m_FocalPoint -= (orientation * glm::vec3(1.0f, 0.0f, 0.0f)) * delta.x * panSpeed;
			m_FocalPoint += (orientation * glm::vec3(0.0f, 1.0f, 0.0f)) * delta.y * panSpeed;
		}

		const float scroll = Input::GetMouseScroll().y;
		if (hovered && scroll != 0.0f)
		{
			const float zoom = std::max(m_Distance * 0.15f, 0.05f) * scroll;
			m_Distance -= zoom;
			if (m_Distance < 0.25f)
			{
				// Push the focal point forward instead of passing through it.
				m_FocalPoint += GetForward() * (0.25f - m_Distance);
				m_Distance = 0.25f;
			}
		}
	}

}
