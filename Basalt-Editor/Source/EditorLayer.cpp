#include "EditorLayer.h"

#include "Panels/JsonWidgets.h"

#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/Application.h"
#include "Basalt/Core/Input.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/ImGui/ImGuiLayer.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Renderer/DebugDraw.h"
#include "Basalt/Renderer/GameUI.h"
#include "Basalt/Renderer/GraphicsDevice.h"
#include "Basalt/Renderer/RenderUtils.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scene/SceneSerializer.h"

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <nvrhi/utils.h>
// ImGuizmo.h relies on imgui.h being included first.
#include <ImGuizmo.h>

#include <future>
#include <limits>

namespace Basalt {

	namespace {

		const glm::vec4 s_SelectionColor(1.0f, 0.75f, 0.1f, 1.0f);
		const glm::vec4 s_ColliderColor(0.2f, 1.0f, 0.4f, 0.8f);
		const glm::vec4 s_LightColor(1.0f, 0.9f, 0.4f, 0.8f);
		const glm::vec4 s_CameraColor(0.7f, 0.7f, 1.0f, 0.8f);

		// Ray/AABB slab test. Returns the entry distance, or nothing.
		std::optional<float> IntersectRay(const glm::vec3& origin, const glm::vec3& direction, const AABB& box)
		{
			float tMin = 0.0f;
			float tMax = std::numeric_limits<float>::max();
			for (int axis = 0; axis < 3; axis++)
			{
				if (std::abs(direction[axis]) < 1e-8f)
				{
					if (origin[axis] < box.Min[axis] || origin[axis] > box.Max[axis])
						return std::nullopt;
					continue;
				}
				const float inverse = 1.0f / direction[axis];
				float t0 = (box.Min[axis] - origin[axis]) * inverse;
				float t1 = (box.Max[axis] - origin[axis]) * inverse;
				if (t0 > t1)
					std::swap(t0, t1);
				tMin = std::max(tMin, t0);
				tMax = std::min(tMax, t1);
				if (tMin > tMax)
					return std::nullopt;
			}
			return tMin;
		}

		void DrawCameraFrustum(const glm::mat4& world, const CameraComponent& camera, const glm::vec4& color)
		{
			// Unit-depth frustum preview (the far plane would be far too large to draw).
			const float depth = 1.0f;
			float halfHeight;
			float halfWidth;
			const float aspect = camera.Camera.GetAspectRatio();
			if (camera.Camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective)
			{
				halfHeight = std::tan(camera.Camera.GetPerspectiveVerticalFov() * 0.5f) * depth;
				halfWidth = halfHeight * aspect;
			}
			else
			{
				halfHeight = camera.Camera.GetOrthographicSize() * 0.5f;
				halfWidth = halfHeight * aspect;
			}
			const glm::vec3 origin(world[3]);
			glm::vec3 corners[4];
			const glm::vec2 signs[4] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
			for (int i = 0; i < 4; i++)
				corners[i] = glm::vec3(world * glm::vec4(signs[i].x * halfWidth, signs[i].y * halfHeight, -depth, 1.0f));
			for (int i = 0; i < 4; i++)
			{
				DebugDraw::Line(origin, corners[i], color);
				DebugDraw::Line(corners[i], corners[(i + 1) % 4], color);
			}
		}

	}

	EditorLayer::EditorLayer(EditorOptions options)
		: Layer("EditorLayer")
		, m_Options(std::move(options))
		, m_Context(m_Commands, m_Session, m_History)
		, m_HierarchyPanel(m_Context)
		, m_InspectorPanel(m_Context)
		, m_ContentBrowserPanel(m_Context)
		, m_ConsolePanel(m_Context)
	{
	}

	EditorLayer::~EditorLayer() = default;

	void EditorLayer::OnAttach()
	{
		Application& application = Application::Get();
		GraphicsDevice* device = application.GetGraphicsDevice();
		m_Renderer = CreateScope<SceneRenderer>(device->GetDevice());
		if (!m_Renderer->Initialize())
		{
			BS_CORE_CRITICAL("Editor: renderer initialization failed");
			application.Close();
			return;
		}
		m_CommandList = device->GetDevice()->createCommandList();

		m_Session.HostName = "editor";
		m_Session.OnSceneChanged = [this]() { OnSceneChanged(); };
		m_Session.Screenshot = [this](const std::filesystem::path& path, uint32_t width, uint32_t height, std::string& outError) {
			return RenderToFile(path, width, height, false, outError);
		};
		RegisterEditorCommands();
		m_ContentBrowserPanel.OnOpenAsset = [this](const std::string& path) { OpenAsset(path); };

		if (!m_Options.ProjectPath.empty())
			m_Context.Execute("project.open", { { "path", m_Options.ProjectPath } });
		if (!Project::GetActive())
			m_Prompt = PromptKind::OpenProject;
		m_History.Reset(SceneSerializer::SerializeScene(*m_Session.GetEditScene()));

		if (m_Options.AutomationPort != 0)
		{
			std::string error;
			if (!m_Server.Start(m_Options.AutomationPort, [this](const std::string& request) { return HandleAutomationRequest(request); }, error))
				BS_CORE_ERROR("Editor: automation server not started: {}", error);
		}

		ImGuiIO& io = ImGui::GetIO();
		io.ConfigWindowsMoveFromTitleBarOnly = true;
	}

	void EditorLayer::OnDetach()
	{
		m_Server.Stop();
		m_Session.StopPlay();
		if (Application::Get().GetGraphicsDevice())
			Application::Get().GetGraphicsDevice()->WaitIdle();
		m_CaptureRenderer.reset();
		m_Renderer.reset();
		m_CommandList = nullptr;
	}

	std::string EditorLayer::HandleAutomationRequest(const std::string& request)
	{
		// Runs on the server thread: hand the request to the main thread and wait for the answer.
		const nlohmann::json parsed = ParseJson(request);
		if (parsed.is_discarded())
			return R"({"ok":false,"error":"invalid JSON"})";

		auto promise = std::make_shared<std::promise<std::string>>();
		std::future<std::string> future = promise->get_future();
		Application::Get().SubmitToMainThread([this, parsed, promise]() {
			const nlohmann::json response = m_Commands.HandleRequest(m_Session, parsed);
			m_Context.CommitHistory();
			promise->set_value(response.dump());
		});
		while (future.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
		{
			if (m_Server.IsStopping())
				return R"({"ok":false,"error":"editor is shutting down"})";
		}
		return future.get();
	}

	void EditorLayer::RegisterEditorCommands()
	{
		m_Commands.Register({ "editor.select", "Selects an entity in the editor (null clears the selection).", { { "entity", "ID, name or null" } }, [this](AutomationSession& session, const nlohmann::json& params) {
								 const nlohmann::json& reference = params.contains("entity") ? params["entity"] : nlohmann::json();
								 if (reference.is_null())
								 {
									 session.SetSelection(0);
									 return nlohmann::json::object();
								 }
								 const nlohmann::json entity = m_Commands.Execute(session, "entity.get", { { "entity", reference } });
								 session.SetSelection(entity["ID"].get<uint64_t>());
								 return nlohmann::json{ { "selected", entity["ID"] } };
							 } });

		m_Commands.Register({ "editor.camera", "Moves the editor camera (position + target), or returns it when called without params.", { { "position", "[x, y, z]" }, { "target", "[x, y, z]" } }, [this](AutomationSession&, const nlohmann::json& params) {
								 auto readVector = [](const nlohmann::json& value, const char* name) {
									 if (!value.is_array() || value.size() != 3 || !value[0].is_number() || !value[1].is_number() || !value[2].is_number())
										 throw CommandError(std::string("'") + name + "' must be [x, y, z]");
									 return glm::vec3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
								 };
								 if (params.contains("position") || params.contains("target"))
								 {
									 const glm::vec3 position = params.contains("position") ? readVector(params["position"], "position") : m_Camera.GetPosition();
									 const glm::vec3 target = params.contains("target") ? readVector(params["target"], "target") : m_Camera.GetFocalPoint();
									 m_Camera.LookAt(position, target);
								 }
								 const glm::vec3 position = m_Camera.GetPosition();
								 const glm::vec3 target = m_Camera.GetFocalPoint();
								 return nlohmann::json{ { "position", { position.x, position.y, position.z } }, { "target", { target.x, target.y, target.z } } };
							 } });

		m_Commands.Register({ "editor.undo", "Undoes the last edit to the scene (edit mode only).", {}, [this](AutomationSession& session, const nlohmann::json&) {
								 if (session.IsPlaying())
									 throw CommandError("undo is not available while playing");
								 if (!m_History.CanUndo())
									 throw CommandError("nothing to undo");
								 Undo();
								 return nlohmann::json{ { "canUndo", m_History.CanUndo() }, { "canRedo", m_History.CanRedo() } };
							 } });

		m_Commands.Register({ "editor.redo", "Redoes the last undone edit (edit mode only).", {}, [this](AutomationSession& session, const nlohmann::json&) {
								 if (session.IsPlaying())
									 throw CommandError("redo is not available while playing");
								 if (!m_History.CanRedo())
									 throw CommandError("nothing to redo");
								 Redo();
								 return nlohmann::json{ { "canUndo", m_History.CanUndo() }, { "canRedo", m_History.CanRedo() } };
							 } });

		m_Commands.Register({ "editor.window_screenshot", "Captures the whole editor window (all panels) to a PNG at the end of the current frame.", { { "path", "string, .png" } }, [](AutomationSession&, const nlohmann::json& params) {
								 if (!params.contains("path") || !params["path"].is_string())
									 throw CommandError("missing parameter 'path'");
								 const std::filesystem::path path = params["path"].get<std::string>();
								 Application::Get().GetGraphicsDevice()->RequestBackBufferCapture([path](const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height) {
									 std::string error;
									 if (!WritePng(path, pixels, width, height, error))
										 BS_CORE_ERROR("Window screenshot: {}", error);
								 });
								 return nlohmann::json{ { "path", path.string() }, { "note", "written at the end of the current frame" } };
							 } });

		m_Commands.Register({ "editor.screenshot", "Renders the scene from the editor camera (with editor overlays) to a PNG.", { { "path", "string, .png" }, { "width", "integer" }, { "height", "integer" } }, [this](AutomationSession&, const nlohmann::json& params) {
								 if (!params.contains("path") || !params["path"].is_string())
									 throw CommandError("missing parameter 'path'");
								 const uint32_t width = params.value("width", 1280u);
								 const uint32_t height = params.value("height", 720u);
								 if (width < 16 || height < 16 || width > 8192 || height > 8192)
									 throw CommandError("width and height must be in [16, 8192]");
								 std::string error;
								 if (!RenderToFile(params["path"].get<std::string>(), width, height, true, error))
									 throw CommandError(error);
								 return nlohmann::json{ { "path", params["path"] } };
							 } });
	}

	void EditorLayer::OnSceneChanged()
	{
		m_History.Reset(SceneSerializer::SerializeScene(*m_Session.GetEditScene()));
		m_Paused = false;
	}

	bool EditorLayer::RenderToFile(const std::filesystem::path& path, uint32_t width, uint32_t height, bool useEditorCamera, std::string& outError)
	{
		Scene& scene = *m_Session.GetScene();
		std::optional<RenderCamera> camera;
		if (!useEditorCamera)
			camera = GetPrimaryRenderCamera(scene, width, height);
		if (!camera)
		{
			EditorCamera editorCamera = m_Camera;
			editorCamera.SetViewportSize(width, height);
			camera = editorCamera.GetRenderCamera();
		}

		if (!m_CaptureRenderer)
		{
			m_CaptureRenderer = CreateScope<SceneRenderer>(Application::Get().GetGraphicsDevice()->GetDevice());
			if (!m_CaptureRenderer->Initialize())
			{
				m_CaptureRenderer.reset();
				outError = "capture renderer initialization failed";
				return false;
			}
		}
		m_CaptureRenderer->SetViewportSize(width, height);

		// Debug lines belong to the viewport frame; draw editor overlays only for editor-camera shots.
		std::vector<DebugLineVertex> pending = DebugDraw::TakeLines();
		if (useEditorCamera && !m_Session.IsPlaying())
			DrawEditorOverlays();
		m_CaptureRenderer->Render(scene, *camera);
		DebugDraw::Clear();
		for (size_t i = 0; i + 1 < pending.size(); i += 2)
			DebugDraw::Line(pending[i].Position, pending[i + 1].Position, pending[i].Color);
		return SaveScreenshot(*m_CaptureRenderer, path, outError);
	}

	void EditorLayer::Play(bool simulate)
	{
		if (m_Session.IsPlaying())
			return;
		std::string error;
		if (!m_Session.StartPlay(error, simulate))
		{
			m_Context.SetStatus(error, true);
			return;
		}
		m_Paused = false;
		m_Context.SetStatus(simulate ? "Simulating (physics only)" : "Playing", false);
	}

	void EditorLayer::Stop()
	{
		m_Session.StopPlay();
		m_Paused = false;
	}

	void EditorLayer::RestoreState(const nlohmann::json& state)
	{
		Ref<Scene> scene = CreateRef<Scene>();
		std::string error;
		if (!SceneSerializer::DeserializeScene(*scene, state, error))
		{
			m_Context.SetStatus("Undo failed: " + error, true);
			return;
		}
		const UUID selection = m_Session.GetSelection();
		const std::string path = m_Session.GetScenePath();
		// SetEditScene resets the history through OnSceneChanged; keep the current one instead.
		auto callback = std::move(m_Session.OnSceneChanged);
		m_Session.OnSceneChanged = nullptr;
		m_Session.SetEditScene(scene, path);
		m_Session.OnSceneChanged = std::move(callback);
		m_Session.MarkDirty();
		if (scene->GetEntityByUUID(selection))
			m_Session.SetSelection(selection);
	}

	void EditorLayer::Undo()
	{
		if (m_Session.IsPlaying() || !m_History.CanUndo())
			return;
		RestoreState(m_History.Undo());
	}

	void EditorLayer::Redo()
	{
		if (m_Session.IsPlaying() || !m_History.CanRedo())
			return;
		RestoreState(m_History.Redo());
	}

	void EditorLayer::SaveScene()
	{
		if (m_Session.GetScenePath().empty())
		{
			m_Prompt = PromptKind::SaveSceneAs;
			m_PromptPath = "Assets/Scenes/" + m_Session.GetEditScene()->GetName() + ".bscene";
			return;
		}
		if (m_Context.Execute("scene.save"))
			m_Context.SetStatus("Saved " + m_Session.GetScenePath(), false);
	}

	void EditorLayer::FocusSelection()
	{
		Scene& scene = *m_Session.GetScene();
		Entity entity = scene.GetEntityByUUID(m_Session.GetSelection());
		if (!entity)
			return;
		const glm::mat4 world = scene.GetWorldTransform(entity);
		AABB bounds;
		if (const auto* mesh = entity.TryGetComponent<MeshComponent>())
		{
			if (Ref<MeshSource> source = AssetManager::GetMesh(mesh->Mesh); source && mesh->MeshIndex < source->Meshes.size())
				bounds = source->Meshes[mesh->MeshIndex].Bounds.Transformed(world);
		}
		if (bounds.IsValid())
			m_Camera.Focus(bounds.GetCenter(), glm::length(bounds.GetExtents()));
		else
			m_Camera.Focus(glm::vec3(world[3]), 1.0f);
	}

	void EditorLayer::OpenAsset(const std::string& path)
	{
		const std::string extension = std::filesystem::path(path).extension().string();
		if (extension == ".bscene")
		{
			Stop();
			m_Context.Execute("scene.open", { { "path", path } });
		}
		else
		{
			HandleAssetDrop(path);
		}
	}

	void EditorLayer::HandleAssetDrop(const std::string& path)
	{
		const std::string extension = std::filesystem::path(path).extension().string();
		std::optional<nlohmann::json> result;
		if (extension == ".bscene")
		{
			Stop();
			m_Context.Execute("scene.open", { { "path", path } });
			return;
		}
		if (AssetManager::IsMeshFile(path))
			result = m_Context.Execute("asset.import_model", { { "path", path } });
		else if (extension == ".bprefab")
			result = m_Context.Execute("prefab.instantiate", { { "path", path } });
		else if (extension == ".lua" && m_Session.GetSelection().IsValid())
			result = m_Context.Execute("component.set", { { "entity", static_cast<uint64_t>(m_Session.GetSelection()) }, { "component", "Script" }, { "data", { { "Script", path } } } });
		else if (extension == ".hdr")
			result = m_Context.Execute("entity.create", { { "name", "Sky" }, { "components", { { "SkyLight", { { "EnvironmentMap", path } } } } } });
		else
			m_Context.SetStatus("Don't know how to place '" + path + "' in the scene", true);

		if (result)
		{
			if (result->contains("id"))
				m_Session.SetSelection((*result)["id"].get<uint64_t>());
			m_Context.CommitHistory();
		}
	}

	void EditorLayer::OnUpdate(Timestep ts)
	{
		HandleShortcuts();
		m_Camera.SetViewportSize(static_cast<uint32_t>(m_ViewportSize.x), static_cast<uint32_t>(m_ViewportSize.y));
		m_Camera.OnUpdate(ts, m_ViewportHovered, m_ViewportFocused);

		if (m_Session.IsPlaying())
		{
			Scene& scene = *m_Session.GetScene();
			if (!m_Paused || scene.IsPaused())
				scene.OnUpdate(ts);
			if (scene.IsQuitRequested())
			{
				Stop();
				m_Context.SetStatus("The game requested to quit; play mode stopped", false);
			}
		}
		UpdateWindowTitle();
	}

	void EditorLayer::DrawEditorOverlays()
	{
		Scene& scene = *m_Session.GetScene();
		if (m_ShowGrid)
		{
			const int extent = 20;
			for (int i = -extent; i <= extent; i++)
			{
				const float t = static_cast<float>(i);
				const float alpha = (i % 10 == 0) ? 0.45f : 0.18f;
				const glm::vec4 colorX = i == 0 ? glm::vec4(0.9f, 0.25f, 0.25f, 0.8f) : glm::vec4(0.6f, 0.6f, 0.6f, alpha);
				const glm::vec4 colorZ = i == 0 ? glm::vec4(0.25f, 0.45f, 0.95f, 0.8f) : glm::vec4(0.6f, 0.6f, 0.6f, alpha);
				DebugDraw::Line({ -extent, 0.0f, t }, { extent, 0.0f, t }, colorX);
				DebugDraw::Line({ t, 0.0f, -extent }, { t, 0.0f, extent }, colorZ);
			}
		}

		for (Entity entity : scene.GetAllEntitiesOrdered())
		{
			const glm::mat4 world = scene.GetWorldTransform(entity);
			const glm::vec3 position(world[3]);
			const bool selected = entity.GetUUID() == m_Session.GetSelection();

			if (m_ShowLightGizmos)
			{
				if (entity.HasComponent<DirectionalLightComponent>())
					DebugDraw::Arrow(position, position + glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f))) * 1.5f, s_LightColor);
				if (const auto* point = entity.TryGetComponent<PointLightComponent>())
					DebugDraw::Sphere(position, selected ? point->Radius : 0.2f, s_LightColor, selected ? 32 : 12);
				if (const auto* spot = entity.TryGetComponent<SpotLightComponent>())
				{
					const glm::vec3 direction = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
					DebugDraw::Arrow(position, position + direction * (selected ? spot->Range : 1.0f), s_LightColor);
				}
				if (const auto* camera = entity.TryGetComponent<CameraComponent>())
					DrawCameraFrustum(Math::ComposeTransform(position, glm::quat_cast(glm::mat3(glm::normalize(glm::vec3(world[0])), glm::normalize(glm::vec3(world[1])), glm::normalize(glm::vec3(world[2])))), glm::vec3(1.0f)), *camera, selected ? s_SelectionColor : s_CameraColor);
			}

			if (m_ShowColliders || selected)
			{
				const glm::vec4 color = selected ? s_SelectionColor : s_ColliderColor;
				glm::vec3 translation;
				glm::quat rotation;
				glm::vec3 scale;
				Math::DecomposeTransform(world, translation, rotation, scale);
				const glm::mat4 unscaled = Math::ComposeTransform(translation, rotation, glm::vec3(1.0f));
				if (const auto* box = entity.TryGetComponent<BoxColliderComponent>())
					DebugDraw::Box(unscaled * glm::translate(glm::mat4(1.0f), box->Offset * scale), box->HalfExtents * glm::abs(scale), color);
				if (const auto* sphere = entity.TryGetComponent<SphereColliderComponent>())
					DebugDraw::Sphere(glm::vec3(unscaled * glm::vec4(sphere->Offset * scale, 1.0f)), sphere->Radius * std::max({ std::abs(scale.x), std::abs(scale.y), std::abs(scale.z) }), color);
				if (const auto* capsule = entity.TryGetComponent<CapsuleColliderComponent>())
				{
					const glm::vec3 center = glm::vec3(unscaled * glm::vec4(capsule->Offset * scale, 1.0f));
					const glm::vec3 up = glm::normalize(glm::vec3(unscaled * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)));
					const float radius = capsule->Radius * std::max(std::abs(scale.x), std::abs(scale.z));
					const float halfHeight = capsule->HalfHeight * std::abs(scale.y);
					DebugDraw::Sphere(center + up * halfHeight, radius, color, 16);
					DebugDraw::Sphere(center - up * halfHeight, radius, color, 16);
				}
			}

			if (selected)
			{
				if (const auto* mesh = entity.TryGetComponent<MeshComponent>())
				{
					if (Ref<MeshSource> source = AssetManager::GetMesh(mesh->Mesh); source && mesh->MeshIndex < source->Meshes.size())
						DebugDraw::Box(source->Meshes[mesh->MeshIndex].Bounds.Transformed(world), s_SelectionColor);
				}
			}
		}
	}

	void EditorLayer::OnRender()
	{
		GraphicsDevice* device = Application::Get().GetGraphicsDevice();
		if (!m_Renderer || !device)
			return;

		const uint32_t width = std::max(1u, static_cast<uint32_t>(m_ViewportSize.x));
		const uint32_t height = std::max(1u, static_cast<uint32_t>(m_ViewportSize.y));
		m_Renderer->SetViewportSize(width, height);
		Scene& scene = *m_Session.GetScene();
		scene.OnViewportResize(width, height);
		scene.GetRendererSettings().DebugView = m_DebugView;

		std::optional<RenderCamera> camera;
		if (m_Session.IsPlaying() && !m_Session.IsSimulating())
			camera = GetPrimaryRenderCamera(scene, width, height);
		if (!camera)
		{
			camera = m_Camera.GetRenderCamera();
			if (!m_Session.IsPlaying() || m_Session.IsSimulating())
				DrawEditorOverlays();
		}
		m_Renderer->Render(scene, *camera);

		// The dockspace covers the window, but clear the back buffer so no stale content shows through.
		m_CommandList->open();
		nvrhi::utils::ClearColorAttachment(m_CommandList, device->GetCurrentFramebuffer(), 0, nvrhi::Color(0.08f, 0.08f, 0.09f, 1.0f));
		m_CommandList->close();
		device->GetDevice()->executeCommandList(m_CommandList);
	}

	void EditorLayer::HandleShortcuts()
	{
		const ImGuiIO& io = ImGui::GetIO();
		if (io.WantTextInput)
			return;
		const bool control = Input::IsKeyDown(KeyCode::LeftControl) || Input::IsKeyDown(KeyCode::RightControl) || Input::IsKeyDown(KeyCode::LeftSuper) || Input::IsKeyDown(KeyCode::RightSuper);
		const bool shift = Input::IsKeyDown(KeyCode::LeftShift) || Input::IsKeyDown(KeyCode::RightShift);
		const bool flying = Input::IsMouseButtonDown(MouseCode::Right);

		if (control && Input::IsKeyPressed(KeyCode::S))
			SaveScene();
		if (control && Input::IsKeyPressed(KeyCode::Z))
			shift ? Redo() : Undo();
		if (control && Input::IsKeyPressed(KeyCode::Y))
			Redo();
		if (control && Input::IsKeyPressed(KeyCode::P))
			m_Session.IsPlaying() ? Stop() : Play(false);
		if (control && Input::IsKeyPressed(KeyCode::D) && m_Session.GetSelection().IsValid())
		{
			if (auto result = m_Context.Execute("entity.duplicate", { { "entity", static_cast<uint64_t>(m_Session.GetSelection()) } }))
			{
				m_Session.SetSelection((*result)["id"].get<uint64_t>());
				m_Context.CommitHistory();
			}
		}
		if (!control && !flying && (m_ViewportFocused || m_ViewportHovered))
		{
			if (Input::IsKeyPressed(KeyCode::Q))
				m_GizmoOperation = 0;
			if (Input::IsKeyPressed(KeyCode::W))
				m_GizmoOperation = ImGuizmo::TRANSLATE;
			if (Input::IsKeyPressed(KeyCode::E))
				m_GizmoOperation = ImGuizmo::ROTATE;
			if (Input::IsKeyPressed(KeyCode::R))
				m_GizmoOperation = ImGuizmo::SCALE;
			if (Input::IsKeyPressed(KeyCode::F))
				FocusSelection();
			if (Input::IsKeyPressed(KeyCode::Delete) && m_Session.GetSelection().IsValid())
			{
				if (m_Context.Execute("entity.destroy", { { "entity", static_cast<uint64_t>(m_Session.GetSelection()) } }))
				{
					m_Session.SetSelection(0);
					m_Context.CommitHistory();
				}
			}
		}
	}

	void EditorLayer::HandleViewportClick(const glm::vec2& mouse)
	{
		Scene& scene = *m_Session.GetScene();
		const glm::vec2 ndc((mouse.x / m_ViewportSize.x) * 2.0f - 1.0f, 1.0f - (mouse.y / m_ViewportSize.y) * 2.0f);
		const glm::mat4 inverse = glm::inverse(m_Camera.GetProjection() * m_Camera.GetView());
		glm::vec4 nearPoint = inverse * glm::vec4(ndc, 0.0f, 1.0f);
		glm::vec4 farPoint = inverse * glm::vec4(ndc, 1.0f, 1.0f);
		const glm::vec3 origin = glm::vec3(nearPoint) / nearPoint.w;
		const glm::vec3 direction = glm::normalize(glm::vec3(farPoint) / farPoint.w - origin);

		UUID best = 0;
		float bestDistance = std::numeric_limits<float>::max();
		for (Entity entity : scene.GetAllEntitiesOrdered())
		{
			const glm::mat4 world = scene.GetWorldTransform(entity);
			AABB bounds;
			if (const auto* mesh = entity.TryGetComponent<MeshComponent>())
			{
				if (Ref<MeshSource> source = AssetManager::GetMesh(mesh->Mesh); source && mesh->MeshIndex < source->Meshes.size())
					bounds = source->Meshes[mesh->MeshIndex].Bounds.Transformed(world);
			}
			else if (entity.HasComponent<CameraComponent>() || entity.HasComponent<PointLightComponent>() || entity.HasComponent<SpotLightComponent>() || entity.HasComponent<DirectionalLightComponent>() || entity.HasComponent<AudioSourceComponent>())
			{
				const glm::vec3 position(world[3]);
				bounds.Expand(position - glm::vec3(0.25f));
				bounds.Expand(position + glm::vec3(0.25f));
			}
			if (!bounds.IsValid())
				continue;
			if (auto distance = IntersectRay(origin, direction, bounds); distance && *distance < bestDistance)
			{
				bestDistance = *distance;
				best = entity.GetUUID();
			}
		}
		m_Session.SetSelection(best);
	}

	void EditorLayer::OnImGuiRender()
	{
		// Leave room for the status bar at the bottom of the window.
		const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
		BuildDefaultLayout(dockspace);
		DrawMenuBar();
		DrawViewport();
		m_HierarchyPanel.OnImGuiRender();
		m_InspectorPanel.OnImGuiRender();
		m_ContentBrowserPanel.OnImGuiRender();
		m_ConsolePanel.OnImGuiRender();
		DrawSceneSettingsPanel();
		DrawStatusBar();
		DrawModals();
	}

	void EditorLayer::BuildDefaultLayout(unsigned int dockspaceId)
	{
		// Only when no saved layout exists (first run, or imgui.ini deleted).
		ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspaceId);
		if (!root || root->IsSplitNode() || root->Windows.Size > 0)
			return;

		ImGui::DockBuilderRemoveNode(dockspaceId);
		ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

		ImGuiID center = dockspaceId;
		const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
		const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
		const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
		ImGuiID rightBottom = 0;
		const ImGuiID rightTop = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.6f, nullptr, &rightBottom);

		ImGui::DockBuilderDockWindow("Hierarchy", left);
		ImGui::DockBuilderDockWindow("Inspector", rightTop);
		ImGui::DockBuilderDockWindow("Scene", rightBottom);
		ImGui::DockBuilderDockWindow("Content Browser", bottom);
		ImGui::DockBuilderDockWindow("Console", bottom);
		ImGui::DockBuilderDockWindow("Viewport", center);
		ImGui::DockBuilderFinish(dockspaceId);
	}

	void EditorLayer::DrawMenuBar()
	{
		if (!ImGui::BeginMainMenuBar())
			return;

		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("New Scene"))
			{
				Stop();
				m_Context.Execute("scene.new", { { "name", "Untitled" } });
			}
			if (ImGui::MenuItem("Open Scene..."))
			{
				m_Prompt = PromptKind::OpenScene;
				m_PromptPath = "Assets/Scenes/";
			}
			if (ImGui::MenuItem("Save Scene", "Ctrl+S"))
				SaveScene();
			if (ImGui::MenuItem("Save Scene As..."))
			{
				m_Prompt = PromptKind::SaveSceneAs;
				m_PromptPath = m_Session.GetScenePath().empty() ? "Assets/Scenes/Untitled.bscene" : m_Session.GetScenePath();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("New Project..."))
				m_Prompt = PromptKind::NewProject;
			if (ImGui::MenuItem("Open Project..."))
				m_Prompt = PromptKind::OpenProject;
			if (ImGui::MenuItem("Set Current Scene as Start Scene", nullptr, false, !m_Session.GetScenePath().empty()))
				m_Context.Execute("project.set", { { "startScene", m_Session.GetScenePath() } });
			if (ImGui::MenuItem("Export Game...", nullptr, false, static_cast<bool>(Project::GetActive())))
				m_Prompt = PromptKind::Export;
			ImGui::Separator();
			if (ImGui::MenuItem("Exit"))
				Application::Get().Close();
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Edit"))
		{
			if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_History.CanUndo() && !m_Session.IsPlaying()))
				Undo();
			if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_History.CanRedo() && !m_Session.IsPlaying()))
				Redo();
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("View"))
		{
			ImGui::MenuItem("Grid", nullptr, &m_ShowGrid);
			ImGui::MenuItem("Colliders", nullptr, &m_ShowColliders);
			ImGui::MenuItem("Light & Camera Gizmos", nullptr, &m_ShowLightGizmos);
			if (ImGui::BeginMenu("Debug View"))
			{
				const char* names[] = { "None", "SSAO", "Normals", "Depth" };
				for (int i = 0; i < 4; i++)
				{
					if (ImGui::MenuItem(names[i], nullptr, static_cast<int>(m_DebugView) == i))
						m_DebugView = static_cast<RendererDebugView>(i);
				}
				ImGui::EndMenu();
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Help"))
		{
			if (m_Server.IsRunning())
				ImGui::Text("Automation server: 127.0.0.1:%u", static_cast<unsigned>(m_Server.GetPort()));
			else
				ImGui::TextDisabled("Automation server: off");
			ImGui::TextDisabled("Viewport: RMB+WASD fly, Alt+LMB orbit, MMB pan, wheel zoom, F focus");
			ImGui::TextDisabled("Gizmo: Q none, W move, E rotate, R scale, Ctrl snaps");
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}

	void EditorLayer::DrawToolbar()
	{
		const bool playing = m_Session.IsPlaying();
		const float buttonWidth = 70.0f;
		const float total = playing ? buttonWidth * 3.0f : buttonWidth * 2.0f;
		ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetContentRegionAvail().x - total) * 0.5f));
		if (!playing)
		{
			if (ImGui::Button("Play", ImVec2(buttonWidth, 0.0f)))
				Play(false);
			ImGui::SameLine();
			if (ImGui::Button("Simulate", ImVec2(buttonWidth, 0.0f)))
				Play(true);
		}
		else
		{
			if (ImGui::Button("Stop", ImVec2(buttonWidth, 0.0f)))
				Stop();
			ImGui::SameLine();
			if (ImGui::Button(m_Paused ? "Resume" : "Pause", ImVec2(buttonWidth, 0.0f)))
			{
				m_Paused = !m_Paused;
				m_Session.GetScene()->SetPaused(m_Paused);
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(!m_Paused);
			if (ImGui::Button("Step", ImVec2(buttonWidth, 0.0f)))
				m_Session.GetScene()->Step(1);
			ImGui::EndDisabled();
		}
	}

	void EditorLayer::DrawViewport()
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("Viewport");
		DrawToolbar();

		const ImVec2 available = ImGui::GetContentRegionAvail();
		m_ViewportSize = { std::max(available.x, 16.0f), std::max(available.y, 16.0f) };
		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		m_ViewportMin = { cursor.x, cursor.y };
		m_ViewportFocused = ImGui::IsWindowFocused();
		m_ViewportHovered = ImGui::IsWindowHovered();
		Application::Get().GetImGuiLayer()->SetBlockEvents(!m_ViewportHovered && !m_ViewportFocused);

		if (m_Renderer && m_Renderer->GetOutputTexture())
			ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(m_Renderer->GetOutputTexture())), available);
		GameUI::SetViewportSize({ available.x, available.y });
		if (m_Session.IsPlaying() && !m_Session.IsSimulating())
			GameUI::Draw(ImGui::GetWindowDrawList(), { cursor.x, cursor.y }, { available.x, available.y });

		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(AssetPayloadType))
				HandleAssetDrop(static_cast<const char*>(payload->Data));
			ImGui::EndDragDropTarget();
		}

		// Gizmo for the selected entity (edit mode only: play-mode edits are discarded anyway).
		Scene& scene = *m_Session.GetScene();
		Entity selected = scene.GetEntityByUUID(m_Session.GetSelection());
		bool gizmoActive = false;
		if (selected && m_GizmoOperation != 0 && !m_Session.IsPlaying())
		{
			ImGuizmo::SetOrthographic(false);
			ImGuizmo::SetDrawlist();
			ImGuizmo::SetRect(cursor.x, cursor.y, available.x, available.y);
			const glm::mat4 view = m_Camera.GetView();
			const glm::mat4 projection = m_Camera.GetProjection();
			glm::mat4 world = scene.GetWorldTransform(selected);

			const bool snap = Input::IsKeyDown(KeyCode::LeftControl) || Input::IsKeyDown(KeyCode::LeftSuper);
			const float snapValue = m_GizmoOperation == ImGuizmo::ROTATE ? 15.0f : (m_GizmoOperation == ImGuizmo::SCALE ? 0.1f : 0.5f);
			const float snapValues[3] = { snapValue, snapValue, snapValue };
			if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection), static_cast<ImGuizmo::OPERATION>(m_GizmoOperation), ImGuizmo::LOCAL, glm::value_ptr(world), nullptr, snap ? snapValues : nullptr))
			{
				Entity parent = selected.GetParent();
				const glm::mat4 local = parent ? glm::inverse(scene.GetWorldTransform(parent)) * world : world;
				glm::vec3 translation;
				glm::quat rotation;
				glm::vec3 scale;
				if (Math::DecomposeTransform(local, translation, rotation, scale))
				{
					const glm::vec3 euler = glm::degrees(Math::EulerFromQuat(rotation));
					m_Context.Execute("component.set", { { "entity", static_cast<uint64_t>(selected.GetUUID()) }, { "component", "Transform" }, { "data", { { "Translation", { translation.x, translation.y, translation.z } }, { "Rotation", { euler.x, euler.y, euler.z } }, { "Scale", { scale.x, scale.y, scale.z } } } } });
				}
			}
			gizmoActive = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
		}
		const bool using_ = ImGuizmo::IsUsing();
		if (m_GizmoWasUsing && !using_)
			m_Context.CommitHistory();
		m_GizmoWasUsing = using_;

		// Click to select.
		if (m_ViewportHovered && !gizmoActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyAlt)
		{
			const ImVec2 mouse = ImGui::GetMousePos();
			const glm::vec2 local(mouse.x - cursor.x, mouse.y - cursor.y);
			if (local.x >= 0.0f && local.y >= 0.0f && local.x < available.x && local.y < available.y)
				HandleViewportClick(local);
		}

		ImGui::End();
		ImGui::PopStyleVar();
	}

	void EditorLayer::DrawSceneSettingsPanel()
	{
		ImGui::Begin("Scene");
		Scene& scene = *m_Session.GetScene();
		const nlohmann::json state = SceneSerializer::SerializeScene(scene);

		for (const char* group : { "Renderer", "Physics" })
		{
			if (!ImGui::CollapsingHeader(group, ImGuiTreeNodeFlags_DefaultOpen))
				continue;
			ImGui::PushID(group);
			nlohmann::json values = state[group];
			static const std::vector<std::string> s_Tonemappers = { "None", "Reinhard", "ACES", "AgX" };
			for (auto it = values.begin(); it != values.end(); ++it)
			{
				const JsonFieldResult result = DrawJsonField(it.key(), it.value(), it.key() == "Tonemapper" ? &s_Tonemappers : nullptr);
				if (result.Changed)
					m_Context.Execute("scene.settings", { { group, { { it.key(), it.value() } } } });
				if (result.Committed)
					m_Context.CommitHistory();
			}
			ImGui::PopID();
		}

		if (ImGui::CollapsingHeader("Statistics", ImGuiTreeNodeFlags_DefaultOpen) && m_Renderer)
		{
			const SceneRendererStatistics& stats = m_Renderer->GetStatistics();
			ImGui::Text("Entities: %zu", scene.GetEntityCount());
			ImGui::Text("Draw calls: %u (shadow %u)", stats.DrawCalls, stats.ShadowDrawCalls);
			ImGui::Text("Culled submeshes: %u", stats.CulledSubmeshes);
			ImGui::Text("Lights: %u", stats.Lights);
			ImGui::Text("Frame: %.2f ms (%.0f FPS)", 1000.0f / std::max(ImGui::GetIO().Framerate, 1.0f), ImGui::GetIO().Framerate);
			if (GraphicsDevice* device = Application::Get().GetGraphicsDevice())
				ImGui::Text("GPU: %s", device->GetAdapterName().c_str());
		}
		ImGui::End();
	}

	void EditorLayer::DrawStatusBar()
	{
		ImGuiViewport* viewport = ImGui::GetMainViewport();
		const float height = ImGui::GetFrameHeight();
		ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - height));
		ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, height));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 2.0f));
		ImGui::Begin("##StatusBar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
		if (!m_Context.StatusMessage.empty() && ImGui::GetTime() - m_Context.StatusTime < 8.0)
			ImGui::TextColored(m_Context.StatusIsError ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f) : ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", m_Context.StatusMessage.c_str());
		else if (m_Session.IsPlaying())
			ImGui::TextUnformatted(m_Session.IsSimulating() ? "Simulating" : "Playing");
		else
			ImGui::TextDisabled("Ready");
		ImGui::End();
		ImGui::PopStyleVar();
	}

	void EditorLayer::DrawModals()
	{
		static const char* s_Titles[] = { "", "Open Scene", "Save Scene As", "Open Project", "New Project", "Export Game" };
		if (m_Prompt == PromptKind::None)
			return;
		const char* title = s_Titles[static_cast<int>(m_Prompt)];
		if (!m_PromptOpened)
		{
			ImGui::OpenPopup(title);
			m_PromptOpened = true;
		}

		bool close = false;
		if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			switch (m_Prompt)
			{
				case PromptKind::OpenScene:
				case PromptKind::SaveSceneAs:
					ImGui::TextUnformatted("Project-relative scene path:");
					break;
				case PromptKind::OpenProject:
					ImGui::TextUnformatted("Project directory (or .bproject file):");
					break;
				case PromptKind::NewProject:
					ImGui::TextUnformatted("New project directory:");
					break;
				case PromptKind::Export:
					ImGui::TextUnformatted("Output directory (outside the project):");
					break;
				default:
					break;
			}
			ImGui::SetNextItemWidth(520.0f);
			ImGui::InputText("##Path", &m_PromptPath);
			if (m_Prompt == PromptKind::NewProject)
			{
				ImGui::SetNextItemWidth(520.0f);
				ImGui::InputTextWithHint("##Name", "Project name", &m_PromptName);
			}

			if (ImGui::Button("OK", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Enter))
			{
				bool ok = false;
				switch (m_Prompt)
				{
					case PromptKind::OpenScene:
						Stop();
						ok = m_Context.Execute("scene.open", { { "path", m_PromptPath } }).has_value();
						break;
					case PromptKind::SaveSceneAs:
						ok = m_Context.Execute("scene.save", { { "path", m_PromptPath } }).has_value();
						break;
					case PromptKind::OpenProject:
						Stop();
						ok = m_Context.Execute("project.open", { { "path", m_PromptPath } }).has_value();
						break;
					case PromptKind::NewProject:
						Stop();
						ok = m_Context.Execute("project.create", { { "path", m_PromptPath }, { "name", m_PromptName.empty() ? "Game" : m_PromptName } }).has_value();
						if (ok)
							m_Context.Execute("scene.new", { { "name", "Main" } });
						break;
					case PromptKind::Export:
						if (auto result = m_Context.Execute("project.export", { { "output", m_PromptPath } }))
						{
							m_Context.SetStatus("Exported to " + (*result)["executable"].get<std::string>(), false);
							ok = true;
						}
						break;
					default:
						break;
				}
				close = ok;
			}
			ImGui::SameLine();
			// Opening a project is required to use the editor, so that prompt cannot be cancelled.
			const bool required = m_Prompt == PromptKind::OpenProject && !Project::GetActive();
			if (!required && (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)))
				close = true;
			if (required && ImGui::Button("New Project...", ImVec2(120.0f, 0.0f)))
			{
				ImGui::CloseCurrentPopup();
				m_Prompt = PromptKind::NewProject;
				m_PromptOpened = false;
				ImGui::EndPopup();
				return;
			}
			if (!m_Context.StatusMessage.empty() && m_Context.StatusIsError && ImGui::GetTime() - m_Context.StatusTime < 8.0)
				ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", m_Context.StatusMessage.c_str());
			if (close)
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		if (close)
		{
			m_Prompt = PromptKind::None;
			m_PromptOpened = false;
		}
	}

	void EditorLayer::UpdateWindowTitle()
	{
		Window* window = Application::Get().GetWindow();
		if (!window)
			return;
		const Ref<Project>& project = Project::GetActive();
		std::string title = "Basalt Editor";
		if (project)
			title += " - " + project->GetConfig().Name;
		title += " - " + (m_Session.GetScenePath().empty() ? std::string("Untitled") : m_Session.GetScenePath());
		if (m_Session.IsDirty())
			title += " *";
		if (title != m_WindowTitle)
		{
			m_WindowTitle = title;
			window->SetTitle(title);
		}
	}

}
