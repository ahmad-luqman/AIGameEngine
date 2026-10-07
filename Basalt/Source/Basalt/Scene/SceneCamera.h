#pragma once

#include <glm/glm.hpp>

namespace Basalt {

	// Projection settings of a CameraComponent. The view matrix comes from the entity's world transform.
	// Projections use nvrhi's (D3D-style) clip space: depth range [0, 1], +Y up. nvrhi's Vulkan backend
	// flips the viewport, so no Y flip belongs in the projection matrix.
	class SceneCamera
	{
	public:
		enum class ProjectionType
		{
			Perspective = 0,
			Orthographic = 1
		};

		SceneCamera() { RecalculateProjection(); }

		void SetPerspective(float verticalFov, float nearClip, float farClip);
		void SetOrthographic(float size, float nearClip, float farClip);
		void SetViewportSize(uint32_t width, uint32_t height);

		const glm::mat4& GetProjection() const { return m_Projection; }

		ProjectionType GetProjectionType() const { return m_ProjectionType; }
		void SetProjectionType(ProjectionType type)
		{
			m_ProjectionType = type;
			RecalculateProjection();
		}

		// Vertical field of view in radians.
		float GetPerspectiveVerticalFov() const { return m_PerspectiveFov; }
		void SetPerspectiveVerticalFov(float fov)
		{
			m_PerspectiveFov = fov;
			RecalculateProjection();
		}
		float GetPerspectiveNearClip() const { return m_PerspectiveNear; }
		void SetPerspectiveNearClip(float nearClip)
		{
			m_PerspectiveNear = nearClip;
			RecalculateProjection();
		}
		float GetPerspectiveFarClip() const { return m_PerspectiveFar; }
		void SetPerspectiveFarClip(float farClip)
		{
			m_PerspectiveFar = farClip;
			RecalculateProjection();
		}

		// Height of the view volume in world units.
		float GetOrthographicSize() const { return m_OrthographicSize; }
		void SetOrthographicSize(float size)
		{
			m_OrthographicSize = size;
			RecalculateProjection();
		}
		float GetOrthographicNearClip() const { return m_OrthographicNear; }
		void SetOrthographicNearClip(float nearClip)
		{
			m_OrthographicNear = nearClip;
			RecalculateProjection();
		}
		float GetOrthographicFarClip() const { return m_OrthographicFar; }
		void SetOrthographicFarClip(float farClip)
		{
			m_OrthographicFar = farClip;
			RecalculateProjection();
		}

		float GetAspectRatio() const { return m_AspectRatio; }

	private:
		void RecalculateProjection();

	private:
		ProjectionType m_ProjectionType = ProjectionType::Perspective;
		glm::mat4 m_Projection = glm::mat4(1.0f);

		float m_PerspectiveFov = glm::radians(50.0f);
		float m_PerspectiveNear = 0.1f;
		float m_PerspectiveFar = 1000.0f;

		float m_OrthographicSize = 10.0f;
		float m_OrthographicNear = -100.0f;
		float m_OrthographicFar = 100.0f;

		float m_AspectRatio = 16.0f / 9.0f;
	};

}
