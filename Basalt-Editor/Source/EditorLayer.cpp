#include "EditorLayer.h"

#include "EditorColors.h"
#include "UIRectHandles.h"
#include "Panels/JsonWidgets.h"
#include "Panels/PreviewImage.h"

#include "Basalt/Animation/AnimatedModel.h"
#include "Basalt/Animation/AnimationPose.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Asset/MaterialAsset.h"
#include "Basalt/Automation/EditorCommandSchemas.h"
#include "Basalt/Automation/EditorSelection.h"
#include "Basalt/Core/Application.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Input.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/MemoryTracker.h"
#include "Basalt/Core/Window.h"
#include "Basalt/ImGui/ImGuiLayer.h"
#include "Basalt/ImGui/StatisticsOverlay.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Renderer/DebugDraw.h"
#include "Basalt/Renderer/GraphicsDevice.h"
#include "Basalt/Renderer/PlayStatistics.h"
#include "Basalt/Renderer/RenderMath.h"
#include "Basalt/Renderer/RenderUtils.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scene/SceneSerializer.h"
#include "Basalt/Scene/VertexSnap.h"
#include "Basalt/UI/GameUI.h"
#include "Basalt/UI/UILayout.h"
#include "Basalt/UI/UIRules.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <nvrhi/utils.h>
// ImGuizmo.h relies on imgui.h being included first.
#include <ImGuizmo.h>

#include <algorithm>
#include <cctype>
#include <future>
#include <limits>
#include <ranges>

namespace Basalt {

	namespace {

		const char* SelectionModeFromModifiers()
		{
			const auto& io = ImGui::GetIO();
			if (io.KeyCtrl || io.KeySuper)
				return "toggle";
			return io.KeyShift ? "add" : "replace";
		}

		void ResetGizmoInteraction()
		{
			// ImGuizmo processes mouse-up only in Manipulate, which may no longer be eligible.
			ImGuizmo::Enable(false);
			ImGuizmo::Enable(true);
		}

		const char* GizmoTransactionName(GizmoOperation operation)
		{
			switch (operation)
			{
				case GizmoOperation::Rotate:
					return "Rotate Selection";
				case GizmoOperation::Scale:
					return "Scale Selection";
				default:
					return "Move Selection";
			}
		}

		// How often the editor scans the project's Assets/ folder for changed files.
		constexpr std::chrono::milliseconds FileWatchInterval{ 500 };
		// Preferences are written this long after their last change, so a slider drag or wheel scroll writes once it
		// settles.
		constexpr float PreferencesSaveDelay = 1.0f;

		const glm::vec4 s_SelectionColor(1.0f, 0.75f, 0.1f, 1.0f);
		const glm::vec4 s_ColliderColor(0.2f, 1.0f, 0.4f, 0.8f);
		const glm::vec4 s_JointColor(1.0f, 0.6f, 0.1f, 1.0f);
		const glm::vec4 s_LightColor(1.0f, 0.9f, 0.4f, 0.8f);
		const glm::vec4 s_CameraColor(0.7f, 0.7f, 1.0f, 0.8f);

		// Viewports and captures use the Play preferences or, outside Play, the project defaults.
		// The Game view renders at the project's window size: every allowed size must be a valid render target.
		static_assert(ProjectConfig::MaxWindowSize <= MaxRenderTargetSize);

		// Sets the scene's debug view for one render and restores the previous one afterwards, even on an early return.
		class ScopedDebugView
		{
		public:
			ScopedDebugView(Scene& scene, RendererDebugView view)
				: m_Scene(scene)
				, m_Previous(scene.GetRendererSettings().DebugView)
			{
				scene.GetRendererSettings().DebugView = view;
			}
			~ScopedDebugView() { m_Scene.GetRendererSettings().DebugView = m_Previous; }
			ScopedDebugView(const ScopedDebugView&) = delete;
			ScopedDebugView& operator=(const ScopedDebugView&) = delete;

		private:
			Scene& m_Scene;
			RendererDebugView m_Previous;
		};

		float ActiveRenderScale(const Scene* scene)
		{
			return (scene ? scene->GetEffectiveGraphicsSettings() : Project::GetActiveGraphicsSettings()).RenderScale;
		}

		// Captures use the full effective scale instead of GPU timing feedback, so images are repeatable.
		DynamicResolutionSettings ActiveDynamicResolution(const Scene* scene)
		{
			return (scene ? scene->GetEffectiveGraphicsSettings() : Project::GetActiveGraphicsSettings()).DynamicResolution;
		}

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

		// A circle through center + (u cos a + v sin a) * radius (an ellipse when u and v differ in length).
		void DrawCircle(const glm::vec3& center, const glm::vec3& u, const glm::vec3& v, float radius, const glm::vec4& color)
		{
			constexpr int Segments = 24;
			glm::vec3 previous = center + u * radius;
			for (int i = 1; i <= Segments; i++)
			{
				const float angle = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(Segments);
				const glm::vec3 point = center + (u * std::cos(angle) + v * std::sin(angle)) * radius;
				DebugDraw::Line(previous, point, color);
				previous = point;
			}
		}

		// The rim of a cone joint's limit, 0.5 m along its axis, with two lines back to the apex. Wide cones
		// are skipped: the rim's radius grows with tan(halfAngle), which runs off toward infinity near 90 degrees.
		void DrawConeRim(const glm::vec3& apex, const glm::vec3& axis, float halfAngle, const glm::vec4& color)
		{
			if (halfAngle >= glm::radians(85.0f))
				return;
			const glm::vec3 reference = std::abs(axis.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
			const glm::vec3 u = glm::normalize(glm::cross(axis, reference));
			const glm::vec3 v = glm::cross(axis, u);
			const glm::vec3 center = apex + axis * 0.5f;
			const float radius = 0.5f * std::tan(halfAngle);
			DrawCircle(center, u, v, radius, color);
			DebugDraw::Line(apex, center + u * radius, color);
			DebugDraw::Line(apex, center - u * radius, color);
		}

		// The selected emitter's shape in its local space (scaled by the transform). Box adds an arrow along +Y;
		// Cone draws its base disc and the rim its directions reach one local unit out.
		void DrawParticleEmitter(const glm::mat4& world, const ParticleEmitterComponent& emitter, const glm::vec4& color)
		{
			const glm::vec3 origin(world[3]);
			const glm::vec3 x(world[0]);
			const glm::vec3 y(world[1]);
			const glm::vec3 z(world[2]);
			switch (emitter.Shape)
			{
				case ParticleEmitterShape::Point:
					DebugDraw::Sphere(origin, 0.1f, color, 12);
					break;
				case ParticleEmitterShape::Sphere:
				{
					const float scale = std::max({ glm::length(x), glm::length(y), glm::length(z) });
					DebugDraw::Sphere(origin, emitter.ShapeRadius * scale, color);
					break;
				}
				case ParticleEmitterShape::Box:
					DebugDraw::Box(world, emitter.BoxExtents, color);
					DebugDraw::Arrow(origin, origin + y * (emitter.BoxExtents.y + 1.0f), color);
					break;
				case ParticleEmitterShape::Cone:
				{
					// The base disc, and where directions at the cone's angle reach one unit from it: a rim
					// cos(angle) up and sin(angle) out, which stays bounded for any angle up to 180 degrees.
					const float angle = glm::radians(emitter.ConeAngle);
					const glm::vec3 rimCenter = origin + y * std::cos(angle);
					const float rimRadius = emitter.ShapeRadius + std::sin(angle);
					DrawCircle(origin, x, z, emitter.ShapeRadius, color);
					DrawCircle(rimCenter, x, z, rimRadius, color);
					for (const glm::vec3& side : { x, z, -x, -z })
						DebugDraw::Line(origin + side * emitter.ShapeRadius, rimCenter + side * rimRadius, color);
					break;
				}
			}
		}

		// Draws a joint from its body's world transform: the anchor, the axis (hinge rotation, slider
		// direction, cone or twist axis), the six-DOF secondary axis, the cone's rim and a line to the
		// connected entity (or the distance joint's world anchor).
		void DrawJoint(Scene& scene, const JointComponent& joint, const glm::mat4& bodyWorld, const glm::vec4& color)
		{
			glm::vec3 translation;
			glm::quat rotation;
			glm::vec3 scale;
			if (!Math::DecomposeTransform(bodyWorld, translation, rotation, scale))
				return;
			const glm::vec3 anchor = glm::vec3(bodyWorld * glm::vec4(joint.Anchor, 1.0f));
			DebugDraw::Sphere(anchor, 0.08f, color, 12);

			// Axes turn with the body's rotation only, as in physics.
			const bool hasAxis = joint.Type == JointType::Hinge || joint.Type == JointType::Slider || joint.Type == JointType::Cone || joint.Type == JointType::SixDOF;
			if (hasAxis && glm::length(joint.Axis) > 1e-6f)
			{
				const glm::vec3 axis = glm::normalize(rotation * joint.Axis);
				DebugDraw::Arrow(anchor - axis * 0.5f, anchor + axis * 0.5f, color);
				if (joint.Type == JointType::SixDOF)
				{
					glm::vec3 secondary = rotation * joint.SecondaryAxis;
					secondary -= glm::dot(secondary, axis) * axis;
					if (glm::length(secondary) > 1e-4f)
						DebugDraw::Arrow(anchor, anchor + glm::normalize(secondary) * 0.3f, color);
				}
				if (joint.Type == JointType::Cone && joint.UseLimits)
					DrawConeRim(anchor, axis, glm::radians(std::clamp(joint.LimitMax, 0.0f, 180.0f)), color);
			}

			if (Entity connected = joint.ConnectedEntity != 0 ? scene.GetEntityByUUID(joint.ConnectedEntity) : Entity{})
			{
				const glm::mat4 connectedWorld = scene.GetWorldTransform(connected);
				const glm::vec3 target = joint.Type == JointType::Distance ? glm::vec3(connectedWorld * glm::vec4(joint.ConnectedAnchor, 1.0f)) : glm::vec3(connectedWorld[3]);
				DebugDraw::Line(anchor, target, color);
			}
			else if (joint.Type == JointType::Distance)
			{
				DebugDraw::Line(anchor, joint.ConnectedAnchor, color);
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
		, m_MaterialEditorPanel(m_Context)
		, m_ConsolePanel(m_Context)
		, m_ProjectSettingsPanel(m_Context)
	{
	}

	EditorLayer::~EditorLayer() = default;

	void EditorLayer::OnAttach()
	{
		Application& application = Application::Get();
		GraphicsDevice* device = application.GetGraphicsDevice();
		m_Renderer = CreateScope<SceneRenderer>(device->GetDevice(), SceneRendererSpecification{ .GpuTimers = device->SupportsTimerQueries(), .Picking = true, .AsyncLoading = true });
		if (!m_Renderer->Initialize())
		{
			BS_CORE_CRITICAL("Editor: renderer initialization failed");
			application.Close();
			return;
		}
		m_CommandList = device->GetDevice()->createCommandList();
		LoadPreferences();
		ApplyPreferences();

		m_Session.HostName = "editor";
		m_Session.OnAssetsReloaded = [this](const AssetReloadReport& report) {
			if (m_Thumbnails)
				m_Thumbnails->OnFilesChanged(report.Changed);
			m_MaterialEditorPanel.OnFilesChanged(report.Changed);
			if (!report.Scripts.Failed.empty())
				m_Context.SetStatus("Hot reload of " + report.Scripts.Failed.front().Path + " failed: see Console", true);
			else if (!report.Scripts.InstanceErrors.empty())
				m_Context.SetStatus("Hot reloaded, but a script raised an error: see Console", true);
			else if (!report.Scripts.Reloaded.empty())
				m_Context.SetStatus(fmt::format("Hot reloaded {} script(s) ({} instance(s))", report.Scripts.Reloaded.size(), report.Scripts.Instances), false);
			else if (!report.Assets.empty())
				m_Context.SetStatus(fmt::format("Reloading {} changed asset(s)", report.Assets.size()), false);
		};
		m_Session.OnAssetMoved = [this](const std::string& from, const std::string& to) { m_MaterialEditorPanel.OnAssetMoved(from, to); };
		m_Session.OnSceneChanged = [this]() { OnSceneChanged(); };
		// Scripts' OnCreate reads the Game view (its resolution even before the tab is first drawn), not the scene
		// view's layout or the headless default.
		m_Session.BeforeRuntimeStart = [this]() { ApplyGameView(); };
		m_Session.Screenshot = [this](const std::filesystem::path& path, uint32_t width, uint32_t height, std::string& outError) {
			return RenderToFile(path, width, height, CaptureView::GameCamera, outError);
		};
		m_Session.RenderStats = [this](nlohmann::json& outStats, std::string& outError) {
			if (!m_Renderer)
			{
				outError = "the editor's renderer is not available";
				return false;
			}
			outStats = GetRenderStats(*m_Renderer);
			return true;
		};
		RegisterEditorCommands();
		m_ContentBrowserPanel.OnOpenAsset = [this](const std::string& path) { OpenAsset(path); };
		m_Thumbnails = CreateScope<ThumbnailManager>(device->GetDevice());
		m_ContentBrowserPanel.SetThumbnails(m_Thumbnails.get());
		m_MaterialEditorPanel.SetThumbnails(m_Thumbnails.get());
		m_InspectorPanel.DrawComponentPreview = [this](Entity entity, const std::string& component) {
			if (component == "Material")
				DrawMaterialPreview(entity);
		};

		if (!m_Options.ProjectPath.empty())
			m_Context.Execute("project.open", { { "path", m_Options.ProjectPath } });
		if (!Project::GetActive())
			m_Prompt = PromptKind::OpenProject;
		m_History.Reset(SceneSerializer::SerializeScene(*m_Session.GetEditScene()));

		if (m_Options.AutomationPort != 0)
		{
			std::string error;
			if (!m_Server.StartWithFactory(m_Options.AutomationPort, [this]() {
				auto connection = std::make_shared<AutomationConnection>();
				return AutomationServer::ClientHandler{
					[this, connection](const std::string& request) { return HandleAutomationRequest(connection, request); },
					[connection]() { return connection->TakePendingEvents(); },
				}; }, error))
				BS_CORE_ERROR("Editor: automation server not started: {}", error);
		}

		ImGuiIO& io = ImGui::GetIO();
		io.ConfigWindowsMoveFromTitleBarOnly = true;
	}

	void EditorLayer::OnDetach()
	{
		m_Server.Stop();
		m_Session.StopPlay();
		m_Session.StopFileWatchPolling();
		m_Session.OnAssetsReloaded = nullptr;
		m_Session.OnAssetMoved = nullptr;
		SavePreferences(true);
		if (Application::Get().GetGraphicsDevice())
			Application::Get().GetGraphicsDevice()->WaitIdle();
		m_UIFrame.Clear();
		m_GameUIFrame.Clear();
		m_ContentBrowserPanel.SetThumbnails(nullptr);
		m_MaterialEditorPanel.SetThumbnails(nullptr);
		m_InspectorPanel.DrawComponentPreview = nullptr;
		m_Thumbnails.reset();
		m_GameRenderer.reset();
		m_CaptureRenderer.reset();
		m_Renderer.reset();
		m_CommandList = nullptr;
	}

	std::string EditorLayer::HandleAutomationRequest(const std::shared_ptr<AutomationConnection>& connection, const std::string& request)
	{
		// Runs on the server thread: hand the request to the main thread and wait for the answer.
		auto promise = std::make_shared<std::promise<std::string>>();
		std::future<std::string> future = promise->get_future();
		Application::Get().SubmitToMainThread([this, connection, request, promise]() {
			if (m_GizmoWasUsing)
				FinishGizmo(false);
			const std::string response = connection->HandleRequest(m_Commands, m_Session, request);
			promise->set_value(response);
		});
		while (future.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
		{
			if (m_Server.IsStopping())
				return connection->ShutdownResponse(request);
		}
		return future.get();
	}

	void EditorLayer::RegisterEditorCommands()
	{
		m_Commands.Register({ "editor.viewport", "Gets or patches transient gizmo and overlay preferences.", ViewportSettings::Schema(), [this](AutomationSession&, const nlohmann::json& params) {
								 std::string error;
								 if (!m_ViewportSettings.Apply(params, error))
									 throw CommandError(error);
								 auto result = m_ViewportSettings.Get();
								 result["logicalWidth"] = m_ViewportSize.x;
								 result["logicalHeight"] = m_ViewportSize.y;
								 result["width"] = m_Renderer ? m_Renderer->GetViewportWidth() : 0;
								 result["height"] = m_Renderer ? m_Renderer->GetViewportHeight() : 0;
								 result["renderWidth"] = m_Renderer ? m_Renderer->GetRenderWidth() : 0;
								 result["renderHeight"] = m_Renderer ? m_Renderer->GetRenderHeight() : 0;
								 return result;
							 } });
		m_Commands.Register({ "editor.pick", "Picks UI rects outside Play when UI Bounds is enabled, then mesh surfaces and enabled light/camera/audio helpers; background is 0. Selects unless select=false.", EditorCommandSchemas::Pick(), [this](AutomationSession& session, const nlohmann::json& params) {
								 if (!m_Renderer)
									 throw CommandError("picking requires a scene viewport");
								 const UUID id = PickViewport(params["x"].get<uint32_t>(), params["y"].get<uint32_t>());
								 if (params.value("select", true))
									 session.SetSelection(id);
								 return nlohmann::json{ { "entity", static_cast<uint64_t>(id) } };
							 } });
		m_Commands.Register({ "editor.vertex_snap", "Moves the selected entity from a source vertex to the nearest target vertex within 16 viewport pixels; excludes its descendants.", EditorCommandSchemas::VertexSnap(), [this](AutomationSession& session, const nlohmann::json& params) {
								 if (session.IsPlaying())
									 throw CommandError("vertex snapping is only available in edit mode");
								 if (session.GetSelections().size() > 1)
									 throw CommandError("vertex snapping requires exactly one selected entity");
								 Scene& scene = *session.GetScene();
								 Entity entity = scene.GetEntityByUUID(session.GetSelection());
								 if (!entity || entity.HasComponent<UIRectComponent>() || (!entity.HasComponent<MeshComponent>() && !entity.HasComponent<AnimatorComponent>()))
									 throw CommandError("select a mesh entity before vertex snapping");
								 const glm::mat4 viewProjection = m_Camera.GetProjection() * m_Camera.GetView();
								 const glm::vec2 from(params["source"][0].get<float>(), params["source"][1].get<float>());
								 const glm::vec2 to(params["target"][0].get<float>(), params["target"][1].get<float>());
								 const auto inBounds = [this](const glm::vec2& point) { return point.x >= 0 && point.y >= 0 && point.x < m_ViewportSize.x && point.y < m_ViewportSize.y; };
								 if (!inBounds(from) || !inBounds(to))
									 throw CommandError("vertex snap coordinates must be inside the logical viewport");
								 bool hasGeometry = false;
								 if (const auto* mesh = entity.TryGetComponent<MeshComponent>())
									 if (const auto asset = AssetManager::GetMesh(mesh->Mesh))
										 hasGeometry = mesh->MeshIndex < asset->Meshes.size();
								 ForEachAnimatedMesh(entity, [&](const Ref<MeshSource>&, const MeshNode&, const glm::mat4&, const std::vector<glm::mat4>&, std::span<const float>) { hasGeometry = true; });
								 if (!hasGeometry)
									 throw CommandError("selected mesh asset is unavailable or its mesh index is invalid");
								 const auto source = FindViewportVertex(entity, viewProjection, m_ViewportSize, from);
								 if (!source)
									 throw CommandError("no source vertex within 16 pixels; check the selected mesh asset and its visibility");
								 const auto target = FindVertexSnapTarget(scene, entity, viewProjection, m_ViewportSize, to);
								 if (!target)
									 throw CommandError("no target vertex within 16 pixels on another loaded mesh");
								 const auto translation = VertexSnapTranslation(scene, entity, source->World, target->World);
								 if (!translation)
									 throw CommandError("vertex snap requires a finite, invertible parent transform");
								 return m_Commands.Execute(session, "component.set", { { "entity", static_cast<uint64_t>(entity.GetUUID()) }, { "component", "Transform" }, { "data", { { "Translation", { translation->x, translation->y, translation->z } } } } });
							 } });

		RegisterEditorSelectionCommands(m_Commands);
		m_Commands.Register({ "editor.history", "Returns named undo/redo entries, cursor, active transaction and retained patch bytes.", EditorCommandSchemas::Undo(), [this](AutomationSession&, const nlohmann::json&) { return m_History.Describe(); } });
		m_Commands.Register({ "editor.transaction", "Begins, commits or cancels one named scene edit. Finish before saving, replacing the scene or playing.", EditorCommandSchemas::Transaction(), [this](AutomationSession& session, const nlohmann::json& params) {
								 if (session.IsPlaying())
									 throw CommandError("transactions are only available in edit mode");
								 const std::string action = params["action"].get<std::string>();
								 if (action == "begin")
								 {
									 if (!params.contains("name"))
										 throw CommandError("begin requires a name");
									 if (!m_Context.BeginTransaction(params["name"].get<std::string>()))
										 throw CommandError("a transaction is already active: " + m_Context.TransactionError());
								 }
								 else
								 {
									 if (!m_History.IsActive())
										 throw CommandError("no active transaction");
									 if (action == "commit")
									 {
										 if (!m_Context.EndTransaction())
											 throw CommandError(m_Context.StatusMessage);
									 }
									 else if (!m_Context.CancelTransaction([this](auto& state, std::string& error) { return RestoreState(state, error); }))
										 throw CommandError(m_History.GetError());
								 }
								 return m_History.Describe();
							 },
							  CommandPolicy::HistoryControl });
		m_Commands.Register({ "editor.box_select", "Selects entities whose world bounds intersect a viewport rectangle, including occluded entities; UI rects use hierarchy selection.", EditorCommandSchemas::BoxSelect(), [this](AutomationSession& session, const nlohmann::json& params) {
								 const auto read = [&](const char* key) {
									 glm::vec2 point;
									 if (!JsonToFloat(params[key][0], point.x) || !JsonToFloat(params[key][1], point.y) || point.x < 0 || point.y < 0 || point.x > m_ViewportSize.x || point.y > m_ViewportSize.y)
										 throw CommandError("rectangle outside logical viewport");
									 return point;
								 };
								 const auto ids = SelectViewportBox(*session.GetScene(), m_Camera.GetProjection() * m_Camera.GetView(), m_ViewportSize, read("start"), read("end"));
								 return ApplySelection(session, ids, params.value("mode", std::string("replace")));
							 } });

		m_Commands.Register({ "editor.camera", "Moves the editor camera (position + target), or returns it when called without params.", EditorCommandSchemas::Camera(), [this](AutomationSession&, const nlohmann::json& params) {
								 auto readVector = [](const nlohmann::json& value) {
									 return glm::vec3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
								 };
								 if (params.contains("position") || params.contains("target"))
								 {
									 const glm::vec3 position = params.contains("position") ? readVector(params["position"]) : m_Camera.GetPosition();
									 const glm::vec3 target = params.contains("target") ? readVector(params["target"]) : m_Camera.GetFocalPoint();
									 CancelVertexSnap();
									 m_Camera.LookAt(position, target);
								 }
								 const glm::vec3 position = m_Camera.GetPosition();
								 const glm::vec3 target = m_Camera.GetFocalPoint();
								 return nlohmann::json{ { "position", { position.x, position.y, position.z } }, { "target", { target.x, target.y, target.z } } };
							 } });

		m_Commands.Register({ "editor.undo", "Undoes the last edit to the scene (edit mode only).", EditorCommandSchemas::Undo(), [this](AutomationSession& session, const nlohmann::json&) {
								 if (session.IsPlaying())
									 throw CommandError("undo is not available while playing");
								 if (m_History.IsActive())
									 throw CommandError(m_Context.TransactionError());
								 if (!m_History.CanUndo())
									 throw CommandError("nothing to undo");
								 if (!Undo())
									 throw CommandError(m_History.GetError());
								 return m_History.Describe();
							 },
							  CommandPolicy::HistoryControl });

		m_Commands.Register({ "editor.redo", "Redoes the last undone edit (edit mode only).", EditorCommandSchemas::Redo(), [this](AutomationSession& session, const nlohmann::json&) {
								 if (session.IsPlaying())
									 throw CommandError("redo is not available while playing");
								 if (m_History.IsActive())
									 throw CommandError(m_Context.TransactionError());
								 if (!m_History.CanRedo())
									 throw CommandError("nothing to redo");
								 if (!Redo())
									 throw CommandError(m_History.GetError());
								 return m_History.Describe();
							 },
							  CommandPolicy::HistoryControl });

		m_Commands.Register({ "editor.window_screenshot", "Captures the whole editor window (all panels) to a PNG at the end of the current frame.", EditorCommandSchemas::WindowScreenshot(), [](AutomationSession&, const nlohmann::json& params) {
								 const std::filesystem::path path = params["path"].get<std::string>();
								 Application::Get().GetGraphicsDevice()->RequestBackBufferCapture([path](const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height) {
									 std::string error;
									 if (!WritePng(path, pixels, width, height, error))
										 BS_CORE_ERROR("Window screenshot: {}", error);
								 });
								 return nlohmann::json{ { "path", path.string() }, { "note", "written at the end of the current frame" } };
							 } });

		m_Commands.Register({ "editor.game_view", "Gets or changes the Game view: the resolution it renders at (the project's window size unless overridden for this session), focus, stats (session-only play overlay), and with path saves what it shows (primary camera at that resolution, game UI, never a debug view; exposure reset as for screenshots) to a PNG. Returns {stats, statsVisible, frameMs (mean host wall time or null before a completed frame), physicsBodies (null without a physics world), luaBytes (process), renderStats (Game renderer or null), imguiGpu (null without an initialized profiler; otherwise includes timers/stalled/disabledReason), visible, mode, status (rendered, notRendered, noCamera, rendererUnavailable, renderFailed: the last frame's), width, height, override, hasCamera, imageRect and panelRect (logical units, relative to the editor window, as last drawn), windowSize, renderWidth, renderHeight, cursorMode (what the game asks for: Normal, Hidden or Locked), cursorLocked (whether the window holds the cursor for the game now), cursorReleased (Escape gave the cursor back to the editor until the Game view is clicked)}.", GameViewSettings::Schema(), [this](AutomationSession& session, const nlohmann::json& params) {
								 std::string error;
								 if (!m_GameView.Apply(params, error))
									 throw CommandError(error);
								 if (params.value("focus", false))
									 m_PendingFocus.emplace_back(EditorCommandSchemas::GameTitle);
								 nlohmann::json result = GetGameViewState();
								 if (const auto path = params.find("path"); path != params.end())
								 {
									 // Through the capture renderer, so the live view keeps its exposure and dynamic resolution.
									 const glm::uvec2 size = GetGameResolution();
									 if (!RenderToFile(path->get<std::string>(), size.x, size.y, CaptureView::GameView, error))
										 throw CommandError(error);
									 result["path"] = *path;
								 }
								 return result;
							 } });

		m_Commands.Register({ "editor.thumbnail", "Makes (or reads from the cache) the content-browser preview of an image, .hdr environment, model, prefab or material file and returns {kind, width, height, cached, cachePath, details, warnings when it could not be cached}; output also writes it to a PNG, refresh makes it again (re-reading the file and the files it uses).", EditorCommandSchemas::Thumbnail(), [this](AutomationSession&, const nlohmann::json& params) {
								 const std::string path = params["path"].get<std::string>();
								 std::string error;
								 const auto generated = m_Thumbnails->Generate(path, params.value("refresh", false), error);
								 if (!generated)
									 throw CommandError(error);
								 nlohmann::json result{ { "kind", ThumbnailKindName(generated->Kind) }, { "width", generated->Image.Width }, { "height", generated->Image.Height }, { "cached", generated->Cached }, { "cachePath", FileSystem::PathToUtf8(generated->CachePath) }, { "details", generated->Details } };
								 if (!generated->CacheWarning.empty())
									 result["warnings"] = nlohmann::json::array({ "not cached: " + generated->CacheWarning });
								 if (const auto output = params.find("output"); output != params.end())
								 {
									 if (!WritePng(output->get<std::string>(), generated->Image.Pixels, generated->Image.Width, generated->Image.Height, error))
										 throw CommandError(error);
									 result["output"] = *output;
								 }
								 return result;
							 } });

		m_Commands.Register({ "editor.material_preview", "Renders an entity's Material component on a sphere (the inspector's preview) to a PNG.", EditorCommandSchemas::MaterialPreview(), [this](AutomationSession& session, const nlohmann::json& params) {
								 const UUID id = m_Commands.Execute(session, "entity.get", { { "entity", params["entity"] } })["ID"].get<uint64_t>();
								 const Entity entity = session.GetScene()->GetEntityByUUID(id);
								 if (!entity.HasComponent<MaterialComponent>())
									 throw CommandError("'" + entity.GetName() + "' has no Material component");
								 std::string error;
								 const auto image = m_Thumbnails->RenderMaterial(entity.GetComponent<MaterialComponent>(), error);
								 if (!image)
									 throw CommandError(error);
								 if (!WritePng(params["output"].get<std::string>(), image->Pixels, image->Width, image->Height, error))
									 throw CommandError(error);
								 return nlohmann::json{ { "output", params["output"] }, { "width", image->Width }, { "height", image->Height } };
							 } });

		m_Commands.Register({ "editor.material_editor", "Opens a material file in the Material Editor window (as double-clicking it in the Content Browser does); with close, first closes the window, discarding unsaved edits (both may be given). Opening fails while another material has unsaved edits. Returns {open, path, dirty, valid, problem, externalChange, error, warnings}: valid says whether the edited values form a material (problem says why not), externalChange that the file changed on disk under unsaved edits, error is the window's message (a failed save, an unreadable change on disk) and warnings what loading or the last save reported.", EditorCommandSchemas::MaterialEditor(), [this](AutomationSession&, const nlohmann::json& params) {
								 std::string error;
								 if (params.value("close", false))
									 m_MaterialEditorPanel.Reset();
								 if (const auto path = params.find("path"); path != params.end())
								 {
									 const std::string requested = path->get<std::string>();
									 // Project-relative like other commands' paths (an absolute path inside the project is accepted).
									 const std::filesystem::path given = FileSystem::PathFromUtf8(requested);
									 const std::string relative = FileSystem::NormalizeGeneric(given.is_absolute() ? Project::MakeRelative(given) : requested);
									 if (!relative.starts_with("Assets/") || !MaterialAsset::IsMaterialFile(relative))
										 throw CommandError("'" + requested + "' is not a .bmaterial file under Assets/");
									 if (!m_MaterialEditorPanel.TryOpen(relative, error))
										 throw CommandError(error);
								 }
								 const MaterialAssetDraft& draft = m_MaterialEditorPanel.GetDraft();
								 std::string problem;
								 const bool valid = draft.IsOpen() && draft.Parse(problem).has_value();
								 nlohmann::json warnings = m_MaterialEditorPanel.GetWarning().empty() ? nlohmann::json(draft.GetWarnings()) : nlohmann::json::array({ m_MaterialEditorPanel.GetWarning() });
								 return nlohmann::json{ { "open", m_MaterialEditorPanel.IsOpen() }, { "path", draft.GetPath() }, { "dirty", draft.IsDirty() }, { "valid", valid }, { "problem", valid ? std::string() : problem }, { "externalChange", draft.HasExternalChange() }, { "error", m_MaterialEditorPanel.GetError() }, { "warnings", warnings } };
							 } });

		m_Commands.Register({ "editor.panel", "Opens, closes or focuses an editor window (e.g. focus the Game view, open Project Settings) and returns {name, open}.", EditorCommandSchemas::Panel(), [this](AutomationSession&, const nlohmann::json& params) {
								 const std::string name = params["name"].get<std::string>();
								 static_assert(EditorCommandSchemas::ProjectSettingsIsTheOnlyClosablePanel(), "a new closable panel needs its own open flag here");
								 const bool closable = EditorCommandSchemas::IsClosablePanel(name);
								 if (const auto open = params.find("open"); open != params.end())
								 {
									 if (closable)
										 m_ProjectSettingsOpen = open->get<bool>();
									 else if (!open->get<bool>())
										 throw CommandError("the " + name + " window cannot be closed");
								 }
								 const bool isOpen = !closable || m_ProjectSettingsOpen;
								 if (params.value("focus", false))
								 {
									 if (!isOpen)
										 throw CommandError("open the " + name + " window before focusing it");
									 m_PendingFocus.push_back(name);
								 }
								 return nlohmann::json{ { "name", name }, { "open", isOpen } };
							 } });

		m_Commands.Register({ "editor.preferences", "Gets or patches the user's editor preferences, kept across sessions in the user data directory: cameraSpeed (fly speed multiplier), theme (Dark or Light), recentProjects (most recent first; [] clears) and autoReload (reload files changed on disk and hot reload scripts in Play). Returns them with the files they are stored in.", EditorPreferences::Schema(), [this](AutomationSession&, const nlohmann::json& params) {
								 std::string error;
								 if (!m_Preferences.Apply(params, error))
									 throw CommandError(error);
								 if (!params.empty())
								 {
									 ApplyPreferences();
									 // Written PreferencesSaveDelay after the last patch (a slider drag patches every frame) and on exit.
									 MarkPreferencesDirty();
								 }
								 nlohmann::json result = m_Preferences.ToJson();
								 result["file"] = FileSystem::PathToUtf8(m_PreferencesFile);
								 result["layoutFile"] = FileSystem::PathToUtf8(m_LayoutFile);
								 // Without a user data directory (or a readable file) they last for this session only.
								 result["persistent"] = !m_PreferencesFile.empty();
								 result["autoReloadActive"] = m_Session.IsFileWatchPolling();
								 if (!m_PreferencesSaveError.empty())
									 result["saveError"] = m_PreferencesSaveError;
								 return result;
							 } });

		m_Commands.Register({ "editor.screenshot", "Renders what the scene view shows (editor camera, editor overlays, selection outline, and the UI components outside Play; without script debug lines) to a PNG.", EditorCommandSchemas::Screenshot(), [this](AutomationSession&, const nlohmann::json& params) {
								 const uint32_t width = params.value("width", 1280u);
								 const uint32_t height = params.value("height", 720u);
								 std::string error;
								 if (!RenderToFile(params["path"].get<std::string>(), width, height, CaptureView::SceneView, error))
									 throw CommandError(error);
								 return nlohmann::json{ { "path", params["path"] } };
							 } });
	}

	void EditorLayer::LoadPreferences()
	{
		std::string error;
		const auto directory = EditorPreferences::GetDirectory(error);
		if (!directory)
		{
			BS_CORE_WARN("Editor: preferences are kept for this session only ({}); the window layout stays in ./imgui.ini", error);
			return;
		}
		const std::filesystem::path file = *directory / EditorPreferences::FileName;
		EditorPreferences::LoadStatus status = EditorPreferences::LoadStatus::Missing;
		m_Preferences = EditorPreferences::Load(file, status, error);
		if (status == EditorPreferences::LoadStatus::Unreadable)
		{
			// Perhaps only for now: not overwritten (nor set aside) by this session.
			BS_CORE_WARN("Editor: {}; using the default preferences for this session only (the window layout stays in ./imgui.ini)", error);
			return;
		}
		if (status == EditorPreferences::LoadStatus::Invalid)
		{
			// Set aside rather than overwritten by the defaults at the next save.
			std::string moveError;
			const auto backup = EditorPreferences::SetAside(file, moveError);
			if (!backup)
			{
				BS_CORE_WARN("Editor: {}; using the default preferences for this session only ({}; the window layout stays in ./imgui.ini)", error, moveError);
				return;
			}
			BS_CORE_WARN("Editor: {}; using the default preferences (the file was moved to '{}')", error, FileSystem::PathToUtf8(*backup));
		}
		m_PreferencesFile = file;

		// ImGui writes the layout file but does not create its folder: without the folder the layout stays in ./imgui.ini.
		std::error_code createError;
		std::filesystem::create_directories(*directory, createError);
		std::error_code statusError;
		if (!std::filesystem::is_directory(*directory, statusError))
		{
			BS_CORE_WARN("Editor: cannot create '{}' ({}); the window layout stays in ./imgui.ini", FileSystem::PathToUtf8(*directory), (createError ? createError : statusError).message());
			return;
		}
		const std::filesystem::path layout = *directory / EditorPreferences::LayoutFileName;
		// Earlier versions kept the layout in the working directory: copied over once. When that fails, ./imgui.ini stays
		// in use rather than starting from an empty layout.
		const std::filesystem::path legacyLayout = EditorPreferences::LayoutFileName;
		std::error_code fsError;
		const bool hasLayout = std::filesystem::exists(layout, fsError);
		if (fsError)
		{
			BS_CORE_WARN("Editor: cannot read '{}' ({}); the window layout stays in ./imgui.ini", FileSystem::PathToUtf8(layout), fsError.message());
			return;
		}
		if (!hasLayout && std::filesystem::is_regular_file(legacyLayout, fsError))
		{
			std::filesystem::copy_file(legacyLayout, layout, fsError);
			if (fsError)
			{
				BS_CORE_WARN("Editor: cannot copy the window layout from ./imgui.ini ({}); it stays there", fsError.message());
				return;
			}
		}
		if (ImGuiLayer* imgui = Application::Get().GetImGuiLayer())
		{
			imgui->SetIniFilename(FileSystem::PathToUtf8(layout));
			m_LayoutFile = layout;
		}
	}

	void EditorLayer::ApplyPreferences()
	{
		m_Camera.SetSpeedMultiplier(m_Preferences.GetCameraSpeed());
		const EditorTheme theme = m_Preferences.GetTheme();
		if (ImGui::GetCurrentContext())
		{
			if (theme == EditorTheme::Light)
				ImGuiLayer::SetLightThemeColors();
			else
				ImGuiLayer::SetDarkThemeColors();
		}
		EditorColors::SetTheme(theme);
		const bool autoReload = m_Preferences.GetAutoReload();
		if (autoReload != m_Session.IsFileWatchPolling())
		{
			if (!autoReload)
				m_Session.StopFileWatchPolling();
			else if (!m_Session.StartFileWatchPolling(FileWatchInterval))
				m_Context.SetStatus("Changed files are not reloaded automatically: the file watcher could not start (see Console); use View > Reload Changed Files Now", true);
		}
	}

	void EditorLayer::MarkPreferencesDirty()
	{
		m_PreferencesDirty = true;
		m_PreferencesChanged.Reset();
	}

	void EditorLayer::SavePreferences(bool now)
	{
		if (!m_PreferencesDirty || m_PreferencesFile.empty())
			return;
		if (!now && m_PreferencesChanged.Elapsed() < PreferencesSaveDelay)
			return;
		std::string error;
		if (m_Preferences.Save(m_PreferencesFile, error))
		{
			m_PreferencesDirty = false;
			m_PreferencesSaveError.clear();
			return;
		}
		// Still dirty: tried again PreferencesSaveDelay later (and on exit). Warned about once, and always on exit.
		m_PreferencesChanged.Reset();
		if (now || m_PreferencesSaveError.empty())
			BS_CORE_WARN("Editor: cannot save the preferences: {}", error);
		m_PreferencesSaveError = error;
	}

	void EditorLayer::UpdateFileWatching()
	{
		// Polling follows project.open/create from any source (menu, prompt, automation).
		if (const std::vector<FileChange> changes = m_Session.PollFileChanges(); !changes.empty())
			m_Session.ReloadAssets(changes);
	}

	void EditorLayer::UpdatePreferences()
	{
		const Ref<Project>& project = Project::GetActive();
		if (project && project != m_RecentProject.lock())
		{
			m_RecentProject = project;
			m_Preferences.AddRecentProject(project->GetDirectory());
			MarkPreferencesDirty();
		}
		// The wheel changes the camera's speed while flying.
		if (m_Camera.GetSpeedMultiplier() != m_Preferences.GetCameraSpeed())
		{
			m_Preferences.SetCameraSpeed(m_Camera.GetSpeedMultiplier());
			MarkPreferencesDirty();
		}
		SavePreferences(false);
	}

	bool EditorLayer::OpenRecentProject(const std::string& directory)
	{
		Stop();
		return m_Context.Execute("project.open", { { "path", directory } }).has_value();
	}

	namespace {

		bool RecentProjectExists(const std::string& directory)
		{
			std::error_code error;
			return std::filesystem::is_directory(FileSystem::PathFromUtf8(directory), error);
		}

	}

	void EditorLayer::DrawRecentProjectsMenu()
	{
		const std::vector<std::string>& recent = m_Preferences.GetRecentProjects();
		if (recent.empty())
			ImGui::TextDisabled("No recent projects");
		std::string chosen;
		for (const std::string& directory : recent)
		{
			if (ImGui::MenuItem(directory.c_str(), nullptr, false, RecentProjectExists(directory)))
				chosen = directory;
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Clear Recent Projects", nullptr, false, !recent.empty()))
			m_Context.Execute("editor.preferences", { { "recentProjects", nlohmann::json::array() } });
		// After the loop, which iterates the list: opening a project leads to changes of it.
		if (!chosen.empty())
			OpenRecentProject(chosen);
	}

	void EditorLayer::DrawRecentProjectsList()
	{
		std::string opened;
		for (const std::string& directory : m_Preferences.GetRecentProjects())
		{
			ImGui::BeginDisabled(!RecentProjectExists(directory));
			if (ImGui::Selectable(directory.c_str(), m_PromptPath == directory, ImGuiSelectableFlags_AllowDoubleClick))
			{
				m_PromptPath = directory;
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					opened = directory;
			}
			ImGui::EndDisabled();
		}
		// Closed only when it opened: a failure stays in the prompt (and the status bar says why).
		if (!opened.empty() && OpenRecentProject(opened))
		{
			m_Prompt = PromptKind::None;
			ImGui::CloseCurrentPopup();
		}
	}

	void EditorLayer::OnSceneChanged()
	{
		m_Context.OnSceneChanged();
		if (m_Renderer)
			m_Renderer->InvalidatePicking();
		CancelVertexSnap();
		m_BoxStart.reset();
		m_GizmoCancelled = m_GizmoWasUsing;
		m_UIRectDrag.reset();
		m_History.Reset(SceneSerializer::SerializeScene(*m_Session.GetEditScene()));
		m_Paused = false;
	}

	bool EditorLayer::PrepareCaptureRenderer(uint32_t width, uint32_t height, float scale, std::string& error)
	{
		if (!m_CaptureRenderer)
		{
			m_CaptureRenderer = CreateScope<SceneRenderer>(Application::Get().GetGraphicsDevice()->GetDevice(), SceneRendererSpecification{ .Picking = true });
			if (!m_CaptureRenderer->Initialize())
			{
				m_CaptureRenderer.reset();
				error = "capture renderer initialization failed";
				return false;
			}
		}
		m_CaptureRenderer->SetRenderScale(scale);
		m_CaptureRenderer->SetViewportSize(width, height);
		return true;
	}

	bool EditorLayer::RenderToFile(const std::filesystem::path& path, uint32_t width, uint32_t height, CaptureView view, std::string& outError)
	{
		Scene& scene = *m_Session.GetScene();
		std::optional<RenderCamera> camera;
		if (view != CaptureView::SceneView)
			camera = GetPrimaryRenderCamera(scene, width, height);
		if (!camera && view == CaptureView::GameView)
		{
			outError = "the scene has no primary camera";
			return false;
		}
		if (!camera)
		{
			EditorCamera editorCamera = m_Camera;
			editorCamera.SetViewportSize(width, height);
			camera = editorCamera.GetRenderCamera();
		}

		if (!PrepareCaptureRenderer(width, height, ActiveRenderScale(&scene), outError))
			return false;

		// The Game view never shows the editor's debug view; the other captures show the scene view's.
		const RendererDebugView debugView = view == CaptureView::GameView ? RendererDebugView::None : m_DebugView;
		{
			const ScopedDebugView scopedDebugView(scene, debugView);
			// Debug lines belong to the viewport frame; the scene view's capture adds its editor helpers.
			const DebugLineStash stash;
			if (view == CaptureView::SceneView)
				DrawEditorOverlays();
			m_CaptureRenderer->SetSelections(view == CaptureView::SceneView && m_ViewportSettings.Options().SelectionOutline ? m_Session.GetSelections() : std::vector<UUID>{});
			m_CaptureRenderer->ResetExposure();
			m_CaptureRenderer->Render(scene, *camera, 0.0f);
		}
		if (m_CaptureRenderer->GetStatistics().Failed)
		{
			outError = "capture render failed; see the renderer error in the log";
			return false;
		}
		// Game UI as the view shows it: the scene view lays out UI components outside Play only.
		if (debugView == RendererDebugView::None && (view != CaptureView::SceneView || !IsGameRunning()))
			DrawGameUIOverCapture(scene, *m_CaptureRenderer, width, height);
		return SaveScreenshot(*m_CaptureRenderer, path, outError);
	}

	void EditorLayer::Play(bool simulate)
	{
		if (m_Session.IsPlaying())
			return;
		if (m_GizmoWasUsing)
			FinishGizmo(false);
		m_Context.FinishFieldEdit();
		m_Context.CommitHistory();
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

	bool EditorLayer::RestoreState(nlohmann::json& state, std::string& error)
	{
		CancelVertexSnap();
		if (m_Renderer)
			m_Renderer->InvalidatePicking();
		Ref<Scene> scene = CreateRef<Scene>();
		if (!SceneSerializer::DeserializeScene(*scene, state, error))
			return false;
		// Quaternion/Euler conversion can normalize values; history must retain the restored baseline.
		state = SceneSerializer::SerializeScene(*scene);
		const auto selection = m_Session.GetSelections();
		const std::string path = m_Session.GetScenePath();
		// SetEditScene resets the history through OnSceneChanged; keep the current one instead.
		auto callback = std::move(m_Session.OnSceneChanged);
		m_Session.OnSceneChanged = nullptr;
		m_Session.SetEditScene(scene, path);
		m_Session.OnSceneChanged = std::move(callback);
		m_Session.MarkDirty();
		m_Session.SetSelections(selection);
		return true;
	}

	bool EditorLayer::Undo()
	{
		m_Context.FinishFieldEdit();
		m_Context.CommitHistory();
		if (m_History.IsActive())
			m_Context.SetStatus(m_Context.TransactionError(), true);
		if (m_Session.IsPlaying() || !m_History.CanUndo())
			return false;
		const bool success = m_History.Undo([this](auto& state, std::string& error) { return RestoreState(state, error); });
		if (!success)
			m_Context.SetStatus(m_History.GetError(), true);
		return success;
	}

	bool EditorLayer::Redo()
	{
		m_Context.FinishFieldEdit();
		m_Context.CommitHistory();
		if (m_History.IsActive())
			m_Context.SetStatus(m_Context.TransactionError(), true);
		if (m_Session.IsPlaying() || !m_History.CanRedo())
			return false;
		const bool success = m_History.Redo([this](auto& state, std::string& error) { return RestoreState(state, error); });
		if (!success)
			m_Context.SetStatus(m_History.GetError(), true);
		return success;
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
		AABB bounds = AnimatedModelBounds(entity);
		if (const auto* mesh = entity.TryGetComponent<MeshComponent>())
		{
			if (Ref<MeshSource> source = AssetManager::GetMesh(mesh->Mesh); source && mesh->MeshIndex < source->Meshes.size())
				bounds.Expand(source->Meshes[mesh->MeshIndex].Bounds.Transformed(world));
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
		else if (MaterialAsset::IsMaterialFile(path))
			m_MaterialEditorPanel.Open(path);
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
		else if (MaterialAsset::IsMaterialFile(path))
		{
			if (!m_Session.GetSelection().IsValid())
			{
				m_Context.SetStatus("Select an entity to give it the material '" + path + "'", true);
				return;
			}
			result = m_Context.Execute("component.set", { { "entity", static_cast<uint64_t>(m_Session.GetSelection()) }, { "component", "Material" }, { "data", { { "Material", path } } } });
		}
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
		m_RenderDeltaTime = ts;
		// Changed files are reloaded before anything uses them this frame (scripts before the scene updates).
		UpdateFileWatching();
		UpdatePreferences();
		HandleShortcuts();
		m_Camera.SetViewportSize(static_cast<uint32_t>(m_ViewportSize.x), static_cast<uint32_t>(m_ViewportSize.y));
		if (!m_VertexSnapDrag && !m_GizmoWasUsing && !m_BoxStart)
			m_Camera.OnUpdate(ts, m_ViewportHovered, m_ViewportFocused);

		// However Play starts or stops (toolbar, shortcut, automation), the game takes the front tab while it runs.
		if (const bool running = IsGameRunning(); running != m_GameWasRunning)
		{
			if (running)
			{
				m_ReturnToViewport = m_ViewportVisible;
				m_PendingFocus.emplace_back(EditorCommandSchemas::GameTitle);
			}
			else if (m_ReturnToViewport)
				m_PendingFocus.emplace_back(EditorCommandSchemas::ViewportTitle);
			m_GameWasRunning = running;
		}

		// The game's cursor and view (geometry, resolution, routing) are decided before the scene updates rather than
		// after the previous one: focus and hover come from the last drawn frame, the mouse position is the current one.
		UpdateGameCursor();
		if (IsGameRunning())
			ApplyGameView();
		if (m_Session.IsPlaying())
		{
			Scene& scene = *m_Session.GetScene();
			m_Session.RecordViewChange();
			if (!m_Paused || scene.IsPaused())
				scene.OnUpdate(ts);
			if (scene.IsQuitRequested())
			{
				Stop();
				m_Context.SetStatus("The game requested to quit; play mode stopped", false);
			}
		}
		if (!m_Session.IsPlaying())
			m_Session.GetScene()->UpdateParticlePreview(ts);
		UpdateWindowTitle();
	}

	void EditorLayer::DrawEditorOverlays()
	{
		Scene& scene = *m_Session.GetScene();
		const auto options = m_ViewportSettings.Options();
		if (options.Grid)
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
			const bool selected = m_Session.IsSelected(entity.GetUUID());

			if (selected && options.AudioGizmos)
				if (const auto* zone = entity.TryGetComponent<AudioReverbZoneComponent>(); zone && zone->Enabled && std::isfinite(zone->Radius) && zone->Radius > 0.0f)
				{
					DebugDraw::Sphere(position, zone->Radius, s_SelectionColor);
					if (std::isfinite(zone->BlendDistance) && zone->BlendDistance > 0.0f && zone->BlendDistance < zone->Radius)
						DebugDraw::Sphere(position, zone->Radius - zone->BlendDistance, glm::vec4(0.3f, 0.7f, 1.0f, 0.5f));
				}
			if (options.LightGizmos)
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
			}
			if (options.CameraGizmos)
			{
				if (const auto* camera = entity.TryGetComponent<CameraComponent>())
					DrawCameraFrustum(Math::ComposeTransform(position, glm::quat_cast(glm::mat3(glm::normalize(glm::vec3(world[0])), glm::normalize(glm::vec3(world[1])), glm::normalize(glm::vec3(world[2])))), glm::vec3(1.0f)), *camera, selected ? s_SelectionColor : s_CameraColor);
			}

			if (options.Colliders)
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
				// Mesh colliders: the bounds of the mesh they collide by (drawing every triangle would bury the scene).
				if (const auto* meshCollider = entity.TryGetComponent<MeshColliderComponent>())
				{
					const auto* mesh = entity.TryGetComponent<MeshComponent>();
					const std::string& key = !meshCollider->Mesh.empty() || !mesh ? meshCollider->Mesh : mesh->Mesh;
					const uint32_t meshIndex = !meshCollider->Mesh.empty() || !mesh ? meshCollider->MeshIndex : mesh->MeshIndex;
					if (Ref<MeshSource> source = key.empty() ? nullptr : AssetManager::GetMesh(key); source && meshIndex < source->Meshes.size())
						DebugDraw::Box(source->Meshes[meshIndex].Bounds.Transformed(world), color);
				}
				// Joints: anchor, axes, cone rim and a line to what it is attached to. A joint entity draws at the
				// body it moves, whose local space holds its anchor and axes.
				if (const auto* joint = entity.TryGetComponent<JointComponent>())
				{
					if (Entity body = joint->BodyEntity != 0 ? scene.GetEntityByUUID(joint->BodyEntity) : entity)
						DrawJoint(scene, *joint, body == entity ? world : scene.GetWorldTransform(body), selected ? s_SelectionColor : s_JointColor);
				}
			}

			if (options.AudioGizmos && entity.HasComponent<AudioSourceComponent>())
				DebugDraw::Sphere(position, 0.2f, selected ? s_SelectionColor : s_CameraColor, 12);
			if (selected && options.SelectionBounds)
			{
				if (const auto* mesh = entity.TryGetComponent<MeshComponent>())
					if (const auto source = AssetManager::GetMesh(mesh->Mesh); source && mesh->MeshIndex < source->Meshes.size())
						DebugDraw::Box(source->Meshes[mesh->MeshIndex].Bounds.Transformed(world), s_SelectionColor);
				const auto bounds = AnimatedModelBounds(entity);
				if (bounds.IsValid())
					DebugDraw::Box(bounds, s_SelectionColor);
			}
			if (selected && options.ParticleGizmos)
				if (const auto* emitter = entity.TryGetComponent<ParticleEmitterComponent>())
					DrawParticleEmitter(world, *emitter, s_SelectionColor);
		}
	}

	void EditorLayer::OnRender()
	{
		GraphicsDevice* device = Application::Get().GetGraphicsDevice();
		if (!m_Renderer || !device)
			return;

		Scene& scene = *m_Session.GetScene();
		const bool playing = IsGameRunning();
		if (playing)
		{
			if (!m_PrePlayVSync)
				m_PrePlayVSync = device->IsVSync();
			device->SetVSync(scene.GetGraphicsSettings().VSync);
		}
		else if (m_PrePlayVSync)
		{
			device->SetVSync(*m_PrePlayVSync);
			m_PrePlayVSync.reset();
		}
		// Cameras follow the game's aspect ratio whichever views are shown: scripts read it, and the camera
		// gizmo shows the frustum the game will see.
		const glm::uvec2 gameSize = GetGameResolution();
		scene.OnViewportResize(gameSize.x, gameSize.y);

		// Lines from scripts go to the Game view, and to the scene view when its Debug Lines option is on; editor
		// helpers only to the scene view. A hidden tab renders nothing.
		const std::vector<DebugLineVertex> scriptLines = DebugDraw::TakeLines();
		if (m_ViewportVisible)
			RenderSceneView(scene, scriptLines);
		if (m_GameViewVisible)
			RenderGameView(scene, scriptLines);
		else
			m_GameViewStatus = GameViewStatus::NotRendered;
		DebugDraw::Clear();

		// UI components show in the scene view outside Play, so layouts can be authored without playing (built
		// empty in debug views, so the UI bounds overlay still gets this frame's canvas). In Play the game UI,
		// including the scripts' immediate-mode UI, belongs to the Game view.
		m_UIFrame.Build(m_DebugView == RendererDebugView::None && !playing ? &scene : nullptr, m_ViewportSize, false);
		// As in the runtime, the game UI shows with or without a camera (a menu scene may have none).
		m_GameUIFrame.Build(&scene, glm::max(m_GameImageSize, glm::vec2(1.0f)), playing);

		// Previews render on their own command lists, before this frame's list opens.
		m_Thumbnails->Update();

		// The dockspace covers the window, but clear the back buffer so no stale content shows through.
		m_CommandList->open();
		m_UIFrame.Prepare([&](const std::string& path) { return m_Renderer->GetUITexture(path, m_CommandList); });
		if (m_GameRenderer)
			m_GameUIFrame.Prepare([&](const std::string& path) { return m_GameRenderer->GetUITexture(path, m_CommandList); });

		nvrhi::utils::ClearColorAttachment(m_CommandList, device->GetCurrentFramebuffer(), 0, nvrhi::Color(0.08f, 0.08f, 0.09f, 1.0f));
		m_CommandList->close();
		device->GetDevice()->executeCommandList(m_CommandList);
	}

	void EditorLayer::RenderSceneView(Scene& scene, const std::vector<DebugLineVertex>& scriptLines)
	{
		const uint32_t width = std::max(1u, static_cast<uint32_t>(m_ViewportSize.x));
		const uint32_t height = std::max(1u, static_cast<uint32_t>(m_ViewportSize.y));
		m_Renderer->SetRenderScale(ActiveRenderScale(&scene));
		m_Renderer->SetDynamicResolution(ActiveDynamicResolution(&scene));
		m_Renderer->SetViewportSize(width, height);
		scene.GetRendererSettings().DebugView = m_DebugView;
		// The script lines switch is independent of the editor helpers'.
		if (m_ViewportSettings.Options().DebugLines)
			DebugDraw::AddLines(scriptLines);
		DrawEditorOverlays();
		m_Renderer->SetSelections(m_ViewportSettings.Options().SelectionOutline ? m_Session.GetSelections() : std::vector<UUID>{});
		m_Renderer->Render(scene, m_Camera.GetRenderCamera(), m_RenderDeltaTime);
	}

	void EditorLayer::RenderGameView(Scene& scene, const std::vector<DebugLineVertex>& scriptLines)
	{
		GraphicsDevice* device = Application::Get().GetGraphicsDevice();
		if (!m_GameRenderer)
		{
			// Reported once; the Game view then shows the failure instead of retrying every frame.
			if (m_GameRendererFailed)
			{
				m_GameViewStatus = GameViewStatus::RendererUnavailable;
				return;
			}
			// Timers drive dynamic resolution, which the Game view runs as the game would.
			auto renderer = CreateScope<SceneRenderer>(device->GetDevice(), SceneRendererSpecification{ .GpuTimers = device->SupportsTimerQueries(), .AsyncLoading = true });
			if (!renderer->Initialize())
			{
				BS_CORE_ERROR("Editor: the Game view renderer failed to initialize (see the renderer error above)");
				m_GameRendererFailed = true;
				m_GameViewStatus = GameViewStatus::RendererUnavailable;
				return;
			}
			m_GameRenderer = std::move(renderer);
		}
		const glm::uvec2 size = GetGameResolution();
		m_GameRenderer->SetRenderScale(ActiveRenderScale(&scene));
		m_GameRenderer->SetDynamicResolution(ActiveDynamicResolution(&scene));
		m_GameRenderer->SetViewportSize(size.x, size.y);
		const std::optional<RenderCamera> camera = GetPrimaryRenderCamera(scene, size.x, size.y);
		if (!camera)
		{
			m_GameViewStatus = GameViewStatus::NoCamera;
			return;
		}
		DebugDraw::AddLines(scriptLines);
		{
			// What the game shows: the editor's debug view is a scene-view aid.
			const ScopedDebugView scopedDebugView(scene, RendererDebugView::None);
			m_GameRenderer->Render(scene, *camera, m_RenderDeltaTime);
		}
		m_GameViewStatus = m_GameRenderer->GetStatistics().Failed ? GameViewStatus::RenderFailed : GameViewStatus::Rendered;
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
			m_Context.Execute("editor.duplicate");
			m_Context.CommitHistory();
		}
		if (!m_VertexSnapDrag && !control && !flying && !ImGuizmo::IsUsing() && !ImGui::IsAnyItemActive() && (m_ViewportFocused || m_ViewportHovered))
		{
			if (!m_Session.IsPlaying())
			{
				for (const auto& operation : GizmoOperations)
					if (Input::IsKeyPressed(static_cast<KeyCode>(operation.Shortcut)))
						m_Context.Execute("editor.viewport", { { "operation", operation.Name } });
				if (Input::IsKeyPressed(KeyCode::F))
					FocusSelection();
			}
			if (Input::IsKeyPressed(KeyCode::Delete) && m_Session.GetSelection().IsValid())
			{
				m_Context.Execute("editor.delete");
				m_Context.CommitHistory();
			}
		}
	}

	UUID EditorLayer::PickViewport(uint32_t x, uint32_t y)
	{
		const uint32_t width = m_Renderer->GetViewportWidth();
		const uint32_t height = m_Renderer->GetViewportHeight();
		if (width == 0 || height == 0)
			throw CommandError("picking unavailable: viewport has not been sized");
		if (x >= width || y >= height)
			throw CommandError("pick coordinates outside " + std::to_string(width) + "x" + std::to_string(height) + " output viewport");
		if (!IsGameRunning() && m_ViewportSettings.Options().UIBounds)
		{
			const glm::vec2 canvas = GameUI::GetCanvasSize({ static_cast<float>(width), static_cast<float>(height) });
			const glm::vec2 point = glm::vec2(x, y) * canvas / glm::vec2(width, height);
			const auto items = UILayout::Layout(*m_Session.GetScene(), canvas);
			for (const auto& item : std::views::reverse(items))
				if (item.Contains(point))
					return item.Owner.GetUUID();
		}
		std::string error;
		if (!PrepareCaptureRenderer(width, height, m_Renderer->GetCurrentRenderScale(), error))
			throw CommandError(error);
		Scene& scene = *m_Session.GetScene();
		EditorCamera camera = m_Camera;
		camera.SetViewportSize(width, height);
		// Commands may run before BeginFrame, including while minimized. Refresh owned capture targets
		// without touching the viewport texture already referenced by ImGui or consuming pending lines.
		{
			const DebugLineStash stash;
			m_CaptureRenderer->SetSelections({});
			m_CaptureRenderer->ResetExposure();
			m_CaptureRenderer->Render(scene, camera.GetRenderCamera(), 0.0f);
		}
		if (m_CaptureRenderer->GetStatistics().Failed)
			throw CommandError("picking render failed; see Console");
		const auto hit = m_CaptureRenderer->PickSurface(x, y);
		if (!hit)
			throw CommandError("picking readback failed; see Console");
		if (hit->EntityId.IsValid() && !scene.GetEntityByUUID(hit->EntityId))
			throw CommandError("picked entity no longer exists; retry with the current scene");

		const glm::vec2 ndc(hit->UV.x * 2 - 1, 1 - hit->UV.y * 2);
		const glm::mat4 inverse = glm::inverse(camera.GetProjection() * camera.GetView());
		const auto unproject = [&](float depth) {
			const glm::vec4 point = inverse * glm::vec4(ndc, depth, 1);
			return glm::vec3(point) / point.w;
		};
		const glm::vec3 origin = unproject(0);
		const glm::vec3 direction = glm::normalize(unproject(1) - origin);
		UUID best = hit->EntityId;
		float bestDistance = glm::length(unproject(hit->Depth) - origin);
		const auto options = m_ViewportSettings.Options();
		for (Entity entity : scene.GetAllEntitiesOrdered())
		{
			const bool light = options.LightGizmos && (entity.HasComponent<PointLightComponent>() || entity.HasComponent<SpotLightComponent>() || entity.HasComponent<DirectionalLightComponent>());
			const bool cameraHelper = options.CameraGizmos && entity.HasComponent<CameraComponent>();
			const bool audio = options.AudioGizmos && entity.HasComponent<AudioSourceComponent>();
			if (entity.HasComponent<MeshComponent>() || entity.HasComponent<AnimatorComponent>() || !(light || cameraHelper || audio))
				continue;
			const glm::vec3 position(scene.GetWorldTransform(entity)[3]);
			AABB bounds;
			bounds.Expand(position - glm::vec3(0.25f));
			bounds.Expand(position + glm::vec3(0.25f));
			if (auto distance = IntersectRay(origin, direction, bounds); distance && *distance < bestDistance)
			{
				bestDistance = *distance;
				best = entity.GetUUID();
			}
		}
		return best;
	}

	void EditorLayer::HandleViewportClick(const glm::vec2& mouse)
	{
		if (!m_Renderer)
			return;
		const uint32_t x = static_cast<uint32_t>(mouse.x * static_cast<float>(m_Renderer->GetViewportWidth()) / m_ViewportSize.x);
		const uint32_t y = static_cast<uint32_t>(mouse.y * static_cast<float>(m_Renderer->GetViewportHeight()) / m_ViewportSize.y);
		if (const auto result = m_Context.Execute("editor.pick", { { "x", x }, { "y", y }, { "select", false } }))
		{
			const uint64_t id = (*result)["entity"].get<uint64_t>();
			m_Context.Execute("editor.select", { { "entity", id ? nlohmann::json(id) : nlohmann::json() }, { "mode", SelectionModeFromModifiers() } });
		}
	}

	void EditorLayer::OnImGuiRender()
	{
		m_Session.PruneSelection();
		// Leave room for the status bar at the bottom of the window.
		const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
		BuildDefaultLayout(dockspace);
		DrawMenuBar();
		DrawViewport();
		DrawGameView();
		// Mouse and keyboard reach editor shortcuts only over (or with focus in) a view.
		Application::Get().GetImGuiLayer()->SetBlockEvents(!m_ViewportHovered && !m_ViewportFocused && !m_GameViewHovered && !m_GameViewFocused);
		// The game's widgets take the keyboard while the Game view has focus; ImGui's own keyboard navigation would
		// act on the toolbar at the same time (Space pressing Stop), so it is off then.
		ImGuiIO& io = ImGui::GetIO();
		if (IsGameRunning() && m_GameViewFocused)
			io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
		else
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		m_HierarchyPanel.OnImGuiRender();
		m_InspectorPanel.OnImGuiRender();
		m_ContentBrowserPanel.OnImGuiRender();
		m_MaterialEditorPanel.OnImGuiRender();
		m_ConsolePanel.OnImGuiRender();
		DrawSceneSettingsPanel();
		DockNextToOnFirstUse(EditorCommandSchemas::ProjectSettingsTitle, EditorCommandSchemas::InspectorTitle);
		m_ProjectSettingsPanel.OnImGuiRender(m_ProjectSettingsOpen);
		DrawStatusBar();
		DrawModals();
		for (const std::string& name : m_PendingFocus)
			ImGui::SetWindowFocus(name.c_str());
		m_PendingFocus.clear();
		if (!ImGui::IsAnyItemActive())
			m_Context.FinishFieldEdit();
	}

	bool EditorLayer::DockNextToOnFirstUse(const char* window, const char* neighbour)
	{
		if (ImGui::FindWindowByName(window) || ImGui::FindWindowSettingsByID(ImHashStr(window)))
			return false;
		const ImGuiWindow* other = ImGui::FindWindowByName(neighbour);
		if (!other || other->DockId == 0)
			return false;
		ImGui::SetNextWindowDockID(other->DockId, ImGuiCond_FirstUseEver);
		return true;
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

		ImGui::DockBuilderDockWindow(EditorCommandSchemas::HierarchyTitle, left);
		ImGui::DockBuilderDockWindow(EditorCommandSchemas::InspectorTitle, rightTop);
		ImGui::DockBuilderDockWindow(EditorCommandSchemas::SceneTitle, rightBottom);
		ImGui::DockBuilderDockWindow(EditorCommandSchemas::ContentBrowserTitle, bottom);
		ImGui::DockBuilderDockWindow(EditorCommandSchemas::ConsoleTitle, bottom);
		ImGui::DockBuilderDockWindow(EditorCommandSchemas::GameTitle, center);
		// Docked last and focused, so the scene view starts in front.
		ImGui::DockBuilderDockWindow(EditorCommandSchemas::ViewportTitle, center);
		ImGui::DockBuilderFinish(dockspaceId);
		m_PendingFocus.emplace_back(EditorCommandSchemas::ViewportTitle);
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
			if (ImGui::BeginMenu("Open Recent Project"))
			{
				DrawRecentProjectsMenu();
				ImGui::EndMenu();
			}
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
			if (m_History.IsActive())
			{
				ImGui::TextDisabled("Transaction: %s", m_History.TransactionName().c_str());
				if (ImGui::MenuItem("Commit Transaction"))
					m_Context.Execute("editor.transaction", { { "action", "commit" } });
				if (ImGui::MenuItem("Cancel Transaction"))
					m_Context.Execute("editor.transaction", { { "action", "cancel" } });
				ImGui::Separator();
			}
			if (ImGui::MenuItem(("Undo " + m_History.UndoName()).c_str(), "Ctrl+Z", false, m_History.CanUndo() && !m_Session.IsPlaying()))
				Undo();
			if (ImGui::MenuItem(("Redo " + m_History.RedoName()).c_str(), "Ctrl+Y", false, m_History.CanRedo() && !m_Session.IsPlaying()))
				Redo();
			ImGui::Separator();
			ImGui::MenuItem("Project Settings...", nullptr, &m_ProjectSettingsOpen, static_cast<bool>(Project::GetActive()));
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("View"))
		{
			DrawOverlayMenu();
			if (ImGui::BeginMenu("Theme"))
			{
				for (const EditorTheme theme : { EditorTheme::Dark, EditorTheme::Light })
				{
					if (ImGui::MenuItem(EditorPreferences::ThemeName(theme), nullptr, m_Preferences.GetTheme() == theme) && m_Preferences.GetTheme() != theme)
						m_Context.Execute("editor.preferences", { { "theme", EditorPreferences::ThemeName(theme) } });
				}
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Reload Changed Files Automatically", nullptr, m_Preferences.GetAutoReload()))
				m_Context.Execute("editor.preferences", { { "autoReload", !m_Preferences.GetAutoReload() } });
			if (ImGui::MenuItem("Reload Changed Files Now", nullptr, false, static_cast<bool>(Project::GetActive())))
				m_Context.Execute("asset.refresh");
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
			// A project setting (it ships with the game), so it goes through project.set like any other.
			if (ImGui::BeginMenu("Render Scale", Project::GetActive() && !m_Session.IsPlaying()))
			{
				const float current = ActiveRenderScale(m_Session.GetScene().get());
				for (const float scale : { 0.5f, 0.75f, 1.0f, 1.5f, 2.0f })
				{
					const std::string label = std::to_string(static_cast<int>(scale * 100.0f)) + "%";
					if (ImGui::MenuItem(label.c_str(), nullptr, current == scale) && current != scale)
						m_Context.Execute("project.set", { { "renderScale", scale } });
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Dynamic Resolution", Project::GetActive() && !m_Session.IsPlaying()))
			{
				const DynamicResolutionSettings current = ActiveDynamicResolution(m_Session.GetScene().get());
				// With a tolerance, so values that went through JSON still tick their preset (a hand-edited 59.94
				// ticks none).
				auto matches = [](float a, float b) { return std::abs(a - b) < 1e-3f; };
				if (ImGui::MenuItem("Enabled", nullptr, current.Enabled))
					m_Context.Execute("project.set", { { "dynamicResolution", !current.Enabled } });
				// Why an enabled dynamic resolution does nothing (no GPU timers, or nothing to adjust).
				if (m_Renderer && current.Enabled && !m_Renderer->IsDynamicResolutionActive())
					ImGui::TextDisabled("%s", DescribeDynamicResolutionState(m_Renderer->GetDynamicResolutionState(), m_Renderer->GetDynamicResolution(), m_Renderer->GetRenderScale()).c_str());
				ImGui::Separator();
				ImGui::TextDisabled("Target frame rate");
				for (const float rate : { 30.0f, 60.0f, 120.0f, 144.0f })
				{
					const std::string label = std::to_string(static_cast<int>(rate)) + " FPS";
					const bool selected = matches(current.TargetFrameRate, rate);
					if (ImGui::MenuItem(label.c_str(), nullptr, selected) && !selected)
						m_Context.Execute("project.set", { { "targetFrameRate", rate } });
				}
				ImGui::Separator();
				ImGui::TextDisabled("Lowest render scale");
				for (const float scale : { 0.25f, 0.5f, 0.75f })
				{
					const std::string label = std::to_string(static_cast<int>(scale * 100.0f)) + "%";
					const bool selected = matches(current.MinScale, scale);
					if (ImGui::MenuItem(label.c_str(), nullptr, selected) && !selected)
						m_Context.Execute("project.set", { { "minRenderScale", scale } });
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
			ImGui::TextDisabled("Viewport: RMB+WASD fly (RMB+wheel: speed), Alt+LMB orbit, MMB pan, wheel zoom, F focus");
			ImGui::TextDisabled("Gizmo: Q none, W move, E rotate, R scale, Ctrl snaps; V+drag vertex to vertex");
			ImGui::TextDisabled("UI Bounds: W drag rect to move, edge/corner handles to resize; E rotate, R scale; Esc cancels");
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
				m_Session.SetPaused(m_Paused);
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(!m_Paused);
			if (ImGui::Button("Step", ImVec2(buttonWidth, 0.0f)))
				m_Session.GetScene()->Step(1);
			ImGui::EndDisabled();
		}
	}

	void EditorLayer::DrawOverlayMenu()
	{
		const auto& options = m_ViewportSettings.Get();
		for (const auto& [key, label] : std::initializer_list<std::pair<const char*, const char*>>{
				 { "grid", "Grid" }, { "colliders", "Colliders" }, { "lightGizmos", "Light Gizmos" }, { "cameraGizmos", "Camera Gizmos" }, { "audioGizmos", "Audio Gizmos" }, { "particleGizmos", "Particle Gizmos" }, { "selectionBounds", "Selection Bounds" }, { "uiBounds", "UI Bounds" }, { "debugLines", "Debug Lines" }, { "selectionOutline", "Selection Outline" } })
		{
			bool enabled = options[key].get<bool>();
			if (ImGui::MenuItem(label, nullptr, &enabled))
				m_Context.Execute("editor.viewport", { { key, enabled } });
		}
	}

	void EditorLayer::DrawViewportOptions()
	{
		const auto& options = m_ViewportSettings.Get();
		ImGui::BeginDisabled(m_Session.IsPlaying() || ImGuizmo::IsUsing());
		for (const auto& operation : GizmoOperations)
		{
			if (ImGui::RadioButton(operation.Name, options["operation"] == operation.Name))
				m_Context.Execute("editor.viewport", { { "operation", operation.Name } });
			ImGui::SameLine();
		}
		if (ImGui::Button(options["space"].get<std::string>().c_str()))
			m_Context.Execute("editor.viewport", { { "space", options["space"] == "Local" ? "World" : "Local" } });
		ImGui::SameLine();
		if (ImGui::Button("Snap"))
			ImGui::OpenPopup("SnapOptions");
		if (ImGui::BeginPopup("SnapOptions"))
		{
			for (const char* key : { "snap", "vertexSnap" })
			{
				bool value = options[key].get<bool>();
				if (ImGui::Checkbox(std::string(key) == "snap" ? "Step snapping" : "Vertex snapping", &value))
					m_Context.Execute("editor.viewport", { { key, value } });
			}
			for (const char* key : { "translationStep", "rotationStep", "scaleStep" })
			{
				float value = options[key].get<float>();
				if (ImGui::InputFloat(std::string(key) == "translationStep" ? "Move step" : std::string(key) == "rotationStep" ? "Rotate step (degrees)"
																															   : "Scale step",
									  &value, 0, 0, "%.4f", ImGuiInputTextFlags_EnterReturnsTrue))
					m_Context.Execute("editor.viewport", { { key, value } });
			}
			ImGui::TextDisabled("Ctrl: step snap. V + drag: vertex to vertex.");
			ImGui::TextDisabled("Scale always uses local axes.");
			ImGui::TextDisabled("Vertex snapping overrides Q/W/E/R; clicking away selects.");
			ImGui::EndPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Overlays"))
			ImGui::OpenPopup("ViewportOverlays");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(110.0f);
		float speed = m_Preferences.GetCameraSpeed();
		if (ImGui::SliderFloat("##CameraSpeed", &speed, EditorPreferences::MinCameraSpeed, EditorPreferences::MaxCameraSpeed, "Speed %.2fx", ImGuiSliderFlags_Logarithmic))
			m_Context.Execute("editor.preferences", { { "cameraSpeed", speed } });
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Fly speed of the editor camera (RMB + wheel while flying)");
		if (ImGui::BeginPopup("ViewportOverlays"))
		{
			DrawOverlayMenu();
			ImGui::EndPopup();
		}
	}

	void EditorLayer::FinishGizmo(bool cancel)
	{
		if (m_GizmoWasUsing && !m_GizmoCancelled)
			m_Context.FinishGizmoTransaction(cancel, [this](auto& state, std::string& error) { return RestoreState(state, error); });
		ResetGizmoInteraction();
		m_UIRectDrag.reset();
		m_GizmoWasUsing = false;
		m_GizmoCancelled = ImGui::IsMouseDown(ImGuiMouseButton_Left);
	}

	void EditorLayer::DrawViewport()
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		m_ViewportVisible = ImGui::Begin(EditorCommandSchemas::ViewportTitle);
		if (!m_ViewportVisible)
		{
			// Behind the Game tab (or collapsed): end any drag, keep the last size for picking.
			m_ViewportHovered = m_ViewportFocused = false;
			CancelVertexSnap();
			m_BoxStart.reset();
			if (m_GizmoWasUsing)
				FinishGizmo(true);
			m_GizmoWasUsing = false;
			ImGui::End();
			ImGui::PopStyleVar();
			return;
		}
		DrawToolbar();
		DrawViewportOptions();

		const ImVec2 available = ImGui::GetContentRegionAvail();
		if (m_ViewportSize != glm::vec2(std::max(available.x, 16.0f), std::max(available.y, 16.0f)))
		{
			CancelVertexSnap();
			m_BoxStart.reset();
			if (m_GizmoWasUsing)
				FinishGizmo(true);
		}
		m_ViewportSize = { std::max(available.x, 16.0f), std::max(available.y, 16.0f) };
		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		m_ViewportMin = { cursor.x, cursor.y };
		m_ViewportFocused = ImGui::IsWindowFocused();
		m_ViewportHovered = false;

		if (m_Renderer && m_Renderer->GetOutputTexture())
		{
			ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(m_Renderer->GetOutputTexture())), available);
			m_ViewportHovered = ImGui::IsItemHovered();
		}
		else
			ImGui::Dummy(available);
		// Outside Play the scene view lays out UI components (for authoring); in Play the Game view hosts the UI.
		if (!IsGameRunning())
		{
			GameUI::SetViewportSize({ available.x, available.y });
			GameUI::SetViewportOrigin({ cursor.x, cursor.y });
			GameUI::SetResolution(std::nullopt);
			GameUI::SetPointerEnabled(m_ViewportHovered);
			GameUI::SetNavigationEnabled(m_ViewportFocused);
		}
		ImGuiIO& io = ImGui::GetIO();
		m_UIFrame.Draw(ImGui::GetWindowDrawList(), { cursor.x, cursor.y }, { available.x, available.y });
		if (m_Renderer && m_Renderer->GetStatistics().Failed)
		{
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			drawList->AddRectFilled(cursor, ImVec2(cursor.x + available.x, cursor.y + 28.0f), IM_COL32(100, 15, 15, 235));
			drawList->AddText(ImVec2(cursor.x + 8.0f, cursor.y + 6.0f), IM_COL32_WHITE, "Rendering failed. See Console.");
		}
		if (m_ViewportSettings.Options().UIBounds && !IsGameRunning())
			DrawUIBounds({ cursor.x, cursor.y }, { available.x, available.y });

		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(AssetPayloadType))
				HandleAssetDrop(static_cast<const char*>(payload->Data));
			ImGui::EndDragDropTarget();
		}

		if (m_GizmoWasUsing && (m_Session.IsPlaying() || m_ViewportSettings.Options().Operation == GizmoOperation::None || m_GizmoSelection != m_Session.GetSelections() || Input::IsKeyPressed(KeyCode::Escape) || (m_UIRectDrag && (!m_ViewportSettings.Options().UIBounds || m_UIRectDrag->Operation != m_ViewportSettings.Options().Operation))))
			FinishGizmo(true);
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
			m_GizmoCancelled = false;
		Scene& scene = *m_Session.GetScene();
		Entity selected = scene.GetEntityByUUID(m_Session.GetSelection());
		const auto options = m_ViewportSettings.Options();
		int gizmoOperation = 0;
		switch (options.Operation)
		{
			case GizmoOperation::Translate:
				gizmoOperation = ImGuizmo::TRANSLATE;
				break;
			case GizmoOperation::Rotate:
				gizmoOperation = ImGuizmo::ROTATE;
				break;
			case GizmoOperation::Scale:
				gizmoOperation = ImGuizmo::SCALE;
				break;
			case GizmoOperation::None:
				break;
		}
		const bool vertexSnap = m_Session.GetSelections().size() == 1 && !ImGuizmo::IsUsing() && !m_Session.IsPlaying() && !io.WantTextInput && !ImGui::IsAnyItemActive() && (options.VertexSnap || Input::IsKeyDown(KeyCode::V));
		bool gizmoActive = HandleUIRectHandles(selected, { cursor.x, cursor.y }, { available.x, available.y });
		// Cancelling a gesture restores a new scene instance.
		selected = m_Session.GetScene()->GetEntityByUUID(m_Session.GetSelection());
		// UI rects are placed by their UIRect; selected world entities share one pivot.
		if (!m_Context.HasAutomationTransaction() && !m_GizmoCancelled && !vertexSnap && selected && gizmoOperation != 0 && !m_Session.IsPlaying() && !selected.HasComponent<UIRectComponent>())
		{
			ImGuizmo::SetOrthographic(false);
			ImGuizmo::SetDrawlist();
			ImGuizmo::SetRect(cursor.x, cursor.y, available.x, available.y);
			const glm::mat4 view = m_Camera.GetView();
			const glm::mat4 projection = m_Camera.GetProjection();
			glm::mat4 world = m_GizmoWasUsing ? m_GizmoPivot : SelectionPivot(scene, m_Session.GetSelections(), options.Space == GizmoSpace::Local || options.Operation == GizmoOperation::Scale);
			const glm::mat4 previous = world;

			const bool snap = options.Snap || io.KeyCtrl || io.KeySuper;
			const float snapValue = options.StepFor(options.Operation);
			const float snapValues[3] = { snapValue, snapValue, snapValue };
			const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection), static_cast<ImGuizmo::OPERATION>(gizmoOperation), options.Space == GizmoSpace::World ? ImGuizmo::WORLD : ImGuizmo::LOCAL, glm::value_ptr(world), nullptr, snap ? snapValues : nullptr);
			// Pressing a handle starts IsUsing before Manipulate reports the first changed matrix.
			if (ImGuizmo::IsUsing() && !m_GizmoWasUsing && !m_GizmoCancelled)
			{
				m_Context.FinishFieldEdit();
				m_GizmoCancelled = !m_Context.BeginTransaction(GizmoTransactionName(options.Operation), EditorContext::TransactionKind::Gizmo);
				m_GizmoSelection = m_Session.GetSelections();
				m_GizmoPivot = previous;
				if (m_GizmoCancelled)
				{
					ResetGizmoInteraction();
				}
			}
			if (changed && !m_GizmoCancelled)
			{
				const glm::mat4 delta = SelectionDelta(previous, world);
				auto matrix = nlohmann::json::array();
				for (int c = 0; c < 4; ++c)
					for (int r = 0; r < 4; ++r)
						matrix.push_back(delta[c][r]);
				if (m_Context.Execute("editor.transform", { { "delta", matrix } }))
					m_GizmoPivot = world;
			}
			gizmoActive = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
		}
		const bool using_ = ImGuizmo::IsUsing() || m_UIRectDrag.has_value();
		if (m_GizmoWasUsing && !using_)
			FinishGizmo(false);
		m_GizmoWasUsing = using_;

		HandleVertexSnapDrag(selected, vertexSnap, { cursor.x, cursor.y });

		if (!m_VertexSnapDrag && m_ViewportHovered && !gizmoActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt)
			m_BoxStart = glm::vec2(io.MousePos.x - cursor.x, io.MousePos.y - cursor.y);
		if (m_BoxStart)
		{
			const glm::vec2 local = glm::clamp(glm::vec2(io.MousePos.x - cursor.x, io.MousePos.y - cursor.y), glm::vec2(0), m_ViewportSize);
			const bool dragged = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f);
			if (dragged)
			{
				const glm::vec2 low = glm::min(*m_BoxStart, local), high = glm::max(*m_BoxStart, local);
				const ImVec2 rectangleMin(cursor.x + low.x, cursor.y + low.y), rectangleMax(cursor.x + high.x, cursor.y + high.y);
				ImGui::GetWindowDrawList()->AddRectFilled(rectangleMin, rectangleMax, IM_COL32(255, 192, 26, 30));
				ImGui::GetWindowDrawList()->AddRect(rectangleMin, rectangleMax, IM_COL32(255, 192, 26, 255));
			}
			if (Input::IsKeyPressed(KeyCode::Escape) || (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Left)))
				m_BoxStart.reset();
			else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			{
				if (glm::length(local - *m_BoxStart) >= 4.0f)
					m_Context.Execute("editor.box_select", { { "start", { m_BoxStart->x, m_BoxStart->y } }, { "end", { local.x, local.y } }, { "mode", SelectionModeFromModifiers() } });
				else if (m_ViewportHovered)
					HandleViewportClick(local);
				m_BoxStart.reset();
			}
		}

		ImGui::End();
		ImGui::PopStyleVar();
	}

	void EditorLayer::HandleVertexSnapDrag(Entity selected, bool enabled, const glm::vec2& origin)
	{
		const auto& io = ImGui::GetIO();
		const glm::vec2 local(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
		const glm::mat4 viewProjection = m_Camera.GetProjection() * m_Camera.GetView();
		Scene& scene = *m_Session.GetScene();
		// Vertex snapping commits one command on release, so cancel/focus loss cannot leave half an edit.
		if (!enabled || !selected || (m_VertexSnapDrag && selected.GetUUID() != m_VertexSnapDrag->EntityId) || !m_ViewportHovered || Input::IsKeyPressed(KeyCode::Escape))
			CancelVertexSnap();
		if (enabled && selected && m_ViewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt)
		{
			if (FindViewportVertex(selected, viewProjection, m_ViewportSize, local))
			{
				m_VertexSnapDrag = VertexSnapDrag{ selected.GetUUID(), local };
			}
		}
		if (enabled && selected && m_ViewportHovered)
		{
			const auto candidate = m_VertexSnapDrag ? FindVertexSnapTarget(scene, selected, viewProjection, m_ViewportSize, local) : FindViewportVertex(selected, viewProjection, m_ViewportSize, local);
			if (candidate)
				ImGui::GetWindowDrawList()->AddCircle(ImVec2(origin.x + candidate->Screen.x, origin.y + candidate->Screen.y), 5.0f, IM_COL32(255, 192, 26, 255), 12, 2.0f);
			if (m_VertexSnapDrag)
				ImGui::GetWindowDrawList()->AddLine(ImVec2(origin.x + m_VertexSnapDrag->Source.x, origin.y + m_VertexSnapDrag->Source.y), io.MousePos, IM_COL32(255, 192, 26, 255));
		}
		if (m_VertexSnapDrag && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			if (m_ViewportHovered)
			{
				m_Context.Execute("editor.vertex_snap", { { "source", { m_VertexSnapDrag->Source.x, m_VertexSnapDrag->Source.y } }, { "target", { io.MousePos.x - origin.x, io.MousePos.y - origin.y } } });
				m_Context.CommitHistory();
			}
			CancelVertexSnap();
		}
		if (m_VertexSnapDrag && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
			CancelVertexSnap();
	}

	void EditorLayer::DrawGameUIOverCapture(Scene& scene, SceneRenderer& renderer, uint32_t width, uint32_t height)
	{
		ImGuiLayer* imgui = Application::Get().GetImGuiLayer();
		GraphicsDevice* device = Application::Get().GetGraphicsDevice();
		if (!imgui || !imgui->IsInitialized() || !device)
			return;
		// UI components always; the scripts' UI.* output while playing (what the Game view shows).
		const glm::vec2 size(static_cast<float>(width), static_cast<float>(height));
		UIFrame frame;
		frame.Build(&scene, size, IsGameRunning());
		if (frame.GetCommands().empty())
			return;
		nvrhi::CommandListHandle commandList = device->GetDevice()->createCommandList();
		commandList->open();
		frame.Prepare([&](const std::string& texture) { return renderer.GetUITexture(texture, commandList); });
		commandList->close();
		device->GetDevice()->executeCommandList(commandList);

		ImDrawList drawList(ImGui::GetDrawListSharedData());
		drawList._ResetForNewFrame();
		drawList.PushTexture(ImGui::GetIO().Fonts->TexRef);
		drawList.PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(size.x, size.y));
		frame.Draw(&drawList, { 0.0f, 0.0f }, size);
		drawList.PopClipRect();
		drawList.PopTexture();
		imgui->RenderDrawList(drawList, size, renderer.GetOverlayFramebuffer());
	}

	void EditorLayer::DrawGameView()
	{
		// A new Game tab joins the scene view's dock node, behind it.
		if (DockNextToOnFirstUse(EditorCommandSchemas::GameTitle, EditorCommandSchemas::ViewportTitle))
			m_PendingFocus.emplace_back(EditorCommandSchemas::ViewportTitle);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		m_GameViewVisible = ImGui::Begin(EditorCommandSchemas::GameTitle);
		m_GameViewHovered = false;
		m_GameViewFocused = m_GameViewVisible && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		if (m_GameViewVisible)
		{
			DrawToolbar();
			DrawGameViewOptions();
			const ImVec2 available = ImGui::GetContentRegionAvail();
			const ImVec2 cursor = ImGui::GetCursorScreenPos();
			const GameViewRect rect = FitLetterbox({ available.x, available.y }, GetGameResolution());
			m_GamePanelMin = { cursor.x, cursor.y };
			m_GamePanelSize = { available.x, available.y };
			m_GameImageMin = glm::vec2(cursor.x, cursor.y) + rect.Origin;
			m_GameImageSize = rect.Size;
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			// Letterbox bars: what the panel shows outside the game's aspect ratio.
			drawList->AddRectFilled(cursor, ImVec2(cursor.x + available.x, cursor.y + available.y), IM_COL32(0, 0, 0, 255));
			const ImVec2 imageMin(m_GameImageMin.x, m_GameImageMin.y);
			const ImVec2 imageSize(std::max(rect.Size.x, 1.0f), std::max(rect.Size.y, 1.0f));
			ImGui::SetCursorScreenPos(imageMin);
			if (m_GameViewStatus == GameViewStatus::Rendered && m_GameRenderer->GetOutputTexture())
				ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(m_GameRenderer->GetOutputTexture())), imageSize);
			else
				ImGui::Dummy(imageSize);
			m_GameViewHovered = ImGui::IsItemHovered();
			// The game UI shows over the image, or over black when nothing rendered (a scene without a camera).
			m_GameUIFrame.Draw(drawList, m_GameImageMin, m_GameImageSize);
			if (IsStatsOverlayActive())
				DrawStatisticsOverlay(drawList, m_GameImageMin, m_GameImageSize, GetPlayStatistics());

			const char* message = nullptr;
			switch (m_GameViewStatus)
			{
				case GameViewStatus::NoCamera:
					message = "No primary camera: add a Camera component with Primary ticked.";
					break;
				case GameViewStatus::RendererUnavailable:
					message = "The Game view renderer failed to initialize. See Console.";
					break;
				case GameViewStatus::RenderFailed:
					message = "Rendering failed. See Console.";
					break;
				case GameViewStatus::Rendered:
					if (!m_GameRenderer->GetOutputTexture())
						message = "The renderer produced no image. See Console.";
					break;
				case GameViewStatus::NotRendered:
					break;
			}
			if (message)
			{
				const ImVec2 textSize = ImGui::CalcTextSize(message);
				drawList->AddText(ImVec2(cursor.x + (available.x - textSize.x) * 0.5f, cursor.y + (available.y - textSize.y) * 0.5f), IM_COL32(230, 120, 120, 255), message);
			}
			// Outside Play the image is a preview of the primary camera; say so, as the game is not running.
			const char* badge = nullptr;
			if (!IsGameRunning())
				badge = m_Session.IsSimulating() ? "Simulating (scripts off)" : "Edit mode";
			else if (m_Paused)
				badge = "Paused";
			if (badge)
			{
				const ImVec2 textSize = ImGui::CalcTextSize(badge);
				// Keep the pause badge from covering the diagnostics at the top of the game image.
				const float badgeY = IsStatsOverlayActive() ? imageMin.y + imageSize.y - textSize.y - 16.0f : imageMin.y + 8.0f;
				const ImVec2 badgeMin(imageMin.x + 8.0f, badgeY);
				drawList->AddRectFilled(badgeMin, ImVec2(badgeMin.x + textSize.x + 12.0f, badgeMin.y + textSize.y + 8.0f), IM_COL32(20, 20, 24, 210), 4.0f);
				drawList->AddText(ImVec2(badgeMin.x + 6.0f, badgeMin.y + 4.0f), IM_COL32(255, 200, 80, 255), badge);
			}
		}
		// While the window holds the cursor, GLFW reports a virtual cursor that wanders over the whole editor: ImGui must
		// ignore the mouse. A hidden cursor is hidden only over the game.
		ImGuiIO& io = ImGui::GetIO();
		if (m_CursorLocked)
			io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
		else
			io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
		if (m_GameCursor.HideOverView && m_GameViewHovered)
			ImGui::SetMouseCursor(ImGuiMouseCursor_None);
		ImGui::End();
		ImGui::PopStyleVar();
	}

	void EditorLayer::UpdateGameCursor()
	{
		GameCursorInput input;
		input.Requested = GetRequestedGameCursor();
		input.ViewFocused = m_GameViewFocused;
		// Hovered on the last drawn frame and under the mouse now, so a click that has already left the image does not
		// take the cursor back.
		input.ViewHovered = m_GameViewHovered && IsInsideRect(Input::GetMousePosition(), m_GameImageMin, m_GameImageSize);
		input.EscapePressed = Input::IsKeyPressed(KeyCode::Escape);
		input.Clicked = Input::IsMouseButtonPressed(MouseCode::Left);
		input.Released = m_GameCursor.Released;
		m_GameCursor = DecideGameCursor(input);
		if (m_GameCursor.ConsumeEscape)
			Input::WithholdFromGame(KeyCode::Escape);
		if (m_GameCursor.ConsumeClick)
			Input::WithholdFromGame(MouseCode::Left);
		Window* window = Application::Get().GetWindow();
		if (window && m_GameCursor.LockWindow != m_CursorLocked)
		{
			window->SetCursorMode(m_GameCursor.LockWindow ? CursorMode::Locked : CursorMode::Normal);
			m_CursorLocked = m_GameCursor.LockWindow;
		}
	}

	void EditorLayer::ApplyGameView()
	{
		const glm::vec2 resolution(GetGameResolution());
		const bool hasImage = m_GameImageSize.x >= 1.0f && m_GameImageSize.y >= 1.0f;
		// Before the Game tab is first drawn, scripts still read the game resolution (an image at its size, at 0,0).
		GameUI::SetViewportSize(hasImage ? m_GameImageSize : resolution);
		GameUI::SetViewportOrigin(hasImage ? m_GameImageMin : glm::vec2(0.0f));
		GameUI::SetResolution(resolution);
		GameViewRoutingInput input;
		input.HasImage = hasImage;
		input.ViewFocused = m_GameViewFocused;
		input.ViewHovered = m_GameViewHovered;
		input.MouseOverImage = IsInsideRect(Input::GetMousePosition(), m_GameImageMin, m_GameImageSize);
		input.CursorLocked = m_CursorLocked;
		const GameViewRouting routing = RouteGameView(input);
		GameUI::SetNavigationEnabled(routing.Focus);
		GameUI::SetPointerEnabled(routing.Pointer);
	}

	const char* EditorLayer::GetSessionModeName() const
	{
		if (IsGameRunning())
			return "Play";
		return m_Session.IsSimulating() ? "Simulate" : "Edit";
	}

	void EditorLayer::DrawGameViewOptions()
	{
		bool showStats = m_GameView.ShowStats();
		if (ImGui::Checkbox("Stats", &showStats))
			m_Context.Execute("editor.game_view", { { "stats", showStats } });
		ImGui::SameLine();
		const glm::uvec2 resolution = GetGameResolution();
		const std::string label = fmt::format("{}x{}{}", resolution.x, resolution.y, m_GameView.GetOverride() ? "" : " (Project)");
		ImGui::SetNextItemWidth(170.0f);
		if (ImGui::BeginCombo("##GameResolution", label.c_str()))
		{
			if (ImGui::Selectable("Project window size", !m_GameView.GetOverride()))
				m_Context.Execute("editor.game_view", { { "resolution", nullptr } });
			for (const glm::uvec2 preset : { glm::uvec2(1280, 720), glm::uvec2(1920, 1080), glm::uvec2(2560, 1440), glm::uvec2(1024, 768), glm::uvec2(1080, 1920), glm::uvec2(720, 1280) })
			{
				const std::string name = fmt::format("{}x{}", preset.x, preset.y);
				if (ImGui::Selectable(name.c_str(), m_GameView.GetOverride() == preset))
					m_Context.Execute("editor.game_view", { { "resolution", { preset.x, preset.y } } });
			}
			ImGui::EndCombo();
		}
		if (ImGui::BeginItemTooltip())
		{
			ImGui::TextUnformatted("The resolution the game renders at. Presets apply to this editor session; the project's\nwindow size (Project Settings) is what the exported game uses.");
			ImGui::EndTooltip();
		}
		if (m_GameViewStatus == GameViewStatus::Rendered)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("render %ux%u", m_GameRenderer->GetRenderWidth(), m_GameRenderer->GetRenderHeight());
		}
	}

	void EditorLayer::DrawMaterialPreview(Entity entity)
	{
		static constexpr float Size = 96.0f;
		const auto& material = entity.GetComponent<MaterialComponent>();
		DrawPreviewSquare(*m_Thumbnails->GetMaterial(std::to_string(static_cast<uint64_t>(entity.GetUUID())), material), Size);
		if (!material.Material.empty())
		{
			ImGui::TextDisabled("Material replaces the values below (they apply while it loads or if it cannot).");
			if (MaterialAsset::IsMaterialFile(material.Material))
			{
				if (ImGui::SmallButton("Edit Material"))
					m_MaterialEditorPanel.Open(material.Material);
				ImGui::SameLine();
			}
		}
		if (ImGui::SmallButton("Save as Material Asset..."))
		{
			const std::string name = MaterialAsset::MakeFileName(entity.GetName());
			m_SaveMaterialPath = "Assets/Materials/" + (name.empty() ? std::string("Material") : name) + std::string(MaterialAsset::Extension);
			ImGui::OpenPopup("Save Material Asset");
		}
		if (ImGui::BeginPopupModal("Save Material Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted("Writes what this entity draws to a material file and uses it.");
			ImGui::SetNextItemWidth(360.0f);
			ImGui::InputText("Path", &m_SaveMaterialPath);
			if (ImGui::Button("Save"))
			{
				// The file is written first; only the component change is a scene edit (and undoable).
				const uint64_t id = static_cast<uint64_t>(entity.GetUUID());
				if (const auto created = m_Context.Execute("material.create", { { "path", m_SaveMaterialPath }, { "entity", id } }))
				{
					// The file exists now: a retry would fail with "already exists", so the prompt closes either way.
					if (m_Context.Execute("component.set", { { "entity", id }, { "component", "Material" }, { "data", { { "Material", m_SaveMaterialPath } } } }))
					{
						m_Context.CommitHistory();
						if (const std::string warnings = EditorContext::JoinWarnings(*created); !warnings.empty())
							m_Context.SetStatus("Saved " + m_SaveMaterialPath + " with warnings: " + warnings, true);
					}
					else
						m_Context.SetStatus("Wrote " + m_SaveMaterialPath + " but could not use it: " + m_Context.StatusMessage, true);
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	const char* EditorLayer::GameViewStatusName(GameViewStatus status)
	{
		switch (status)
		{
			case GameViewStatus::NotRendered:
				return "notRendered";
			case GameViewStatus::Rendered:
				return "rendered";
			case GameViewStatus::NoCamera:
				return "noCamera";
			case GameViewStatus::RendererUnavailable:
				return "rendererUnavailable";
			case GameViewStatus::RenderFailed:
				return "renderFailed";
		}
		return "unknown";
	}

	PlayStatistics EditorLayer::GetPlayStatistics() const
	{
		return CollectPlayStatistics(*m_Session.GetScene(), { .FrameMilliseconds = Application::Get().GetFrameMilliseconds(),
															  .Renderer = m_GameViewStatus == GameViewStatus::Rendered ? m_GameRenderer.get() : nullptr,
															  .ImGui = Application::Get().GetImGuiLayer()->GetProfiler() });
	}

	nlohmann::json EditorLayer::GetGameViewState()
	{
		const glm::uvec2 resolution = GetGameResolution();
		const auto& override = m_GameView.GetOverride();
		const ImGuiViewport* window = ImGui::GetMainViewport();
		const glm::vec2 windowOrigin(window->Pos.x, window->Pos.y);
		const auto rectJson = [&](const glm::vec2& min, const glm::vec2& size) {
			return nlohmann::json{ { "x", min.x - windowOrigin.x }, { "y", min.y - windowOrigin.y }, { "width", size.x }, { "height", size.y } };
		};
		auto result = PlayStatisticsToJson(GetPlayStatistics());
		result.erase("blitGpu");
		result.update({
			{ "visible", m_GameViewVisible },
			{ "stats", m_GameView.ShowStats() },
			{ "statsVisible", IsStatsOverlayActive() },

			{ "mode", GetSessionModeName() },
			{ "status", GameViewStatusName(m_GameViewStatus) },
			{ "width", resolution.x },
			{ "height", resolution.y },
			{ "override", override ? nlohmann::json{ override->x, override->y } : nlohmann::json() },
			{ "hasCamera", static_cast<bool>(m_Session.GetScene()->GetPrimaryCameraEntity()) },
			{ "imageRect", rectJson(m_GameImageMin, m_GameImageSize) },
			{ "panelRect", rectJson(m_GamePanelMin, m_GamePanelSize) },
			{ "windowSize", { window->Size.x, window->Size.y } },
			{ "renderWidth", m_GameRenderer ? m_GameRenderer->GetRenderWidth() : 0 },
			{ "renderHeight", m_GameRenderer ? m_GameRenderer->GetRenderHeight() : 0 },
			{ "cursorMode", std::string(CursorModeToString(GetRequestedGameCursor())) },
			{ "cursorReleased", m_GameCursor.Released },
			{ "cursorLocked", m_CursorLocked },
		});
		return result;
	}

	bool EditorLayer::HandleUIRectHandles(Entity selected, const glm::vec2& origin, const glm::vec2& size)
	{
		const auto options = m_ViewportSettings.Options();
		if (!selected || !selected.HasComponent<UIRectComponent>() || m_Session.IsPlaying() || !options.UIBounds || options.Operation == GizmoOperation::None || m_Context.HasAutomationTransaction() || m_GizmoCancelled)
			return false;
		const UICanvasMapping mapping = UICanvasMapping::ForViewport(origin, size);
		const glm::vec2 mouse = mapping.ToCanvas({ ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y });
		std::optional<UILayoutItem> selectedItem;
		for (const auto& item : UILayout::Layout(*m_Session.GetScene(), m_UIFrame.GetCanvasSize()))
			if (item.Owner == selected)
				selectedItem = item;
		if (!selectedItem)
		{
			if (m_UIRectDrag)
				FinishGizmo(true);
			return false;
		}
		const auto& item = *selectedItem;
		const auto rect = SanitizeUIRect(selected.GetComponent<UIRectComponent>());
		const auto point = [&](const glm::vec2& uv) { return glm::vec2(item.Transform * glm::vec3(item.Rect.Min + item.Rect.Size * uv, 1.0f)); };
		const auto screen = [&](const glm::vec2& uv) {
			const glm::vec2 p = mapping.ToScreen(point(uv));
			return ImVec2(p.x, p.y);
		};
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(ImVec2(origin.x, origin.y), ImVec2(origin.x + size.x, origin.y + size.y), true);
		const ImU32 color = IM_COL32(255, 200, 80, 255);
		const std::array<glm::vec2, 8> handles = { glm::vec2(0, 0), { 0.5f, 0 }, { 1, 0 }, { 1, 0.5f }, { 1, 1 }, { 0.5f, 1 }, { 0, 1 }, { 0, 0.5f } };
		std::optional<glm::vec2> hovered;
		for (const glm::vec2& handle : handles)
		{
			const ImVec2 p = screen(handle);
			drawList->AddRectFilled(ImVec2(p.x - 4, p.y - 4), ImVec2(p.x + 4, p.y + 4), color);
			if (glm::length(mapping.ToScreen(point(handle)) - glm::vec2(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y)) <= 8.0f)
				hovered = handle;
		}
		const bool inside = item.Contains(mouse);
		const bool active = m_UIRectDrag.has_value() || (m_ViewportHovered && (hovered.has_value() || inside));
		if (active)
			ImGui::SetMouseCursor(options.Operation == GizmoOperation::Translate && hovered ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);
		if (!m_UIRectDrag && active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyAlt && !ImGui::IsAnyItemActive())
		{
			const glm::mat3 local = ComputeUITransform(item.Rect, rect);
			if (std::abs(glm::determinant(local)) > 1e-8f && std::abs(glm::determinant(item.Transform)) > 1e-8f)
			{
				m_Context.FinishFieldEdit();
				if (m_Context.BeginTransaction("UI rect gesture", EditorContext::TransactionKind::Gizmo))
				{
					m_UIRectDrag = UIRectDrag{ rect, item.Rect, item.Transform * glm::inverse(local), mouse, hovered.value_or(glm::vec2(0.5f)), options.Operation, hovered.has_value() && options.Operation == GizmoOperation::Translate };
					m_GizmoSelection = m_Session.GetSelections();
					m_GizmoWasUsing = true;
				}
			}
		}
		if (m_UIRectDrag && ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			const auto& drag = *m_UIRectDrag;
			const bool snap = options.Snap || ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
			const UIRectComponent edited = TransformUIRect(drag.Initial, drag.Box, drag.ParentTransform, drag.Start, mouse, drag.Operation, snap ? options.StepFor(drag.Operation) : 0.0f, drag.Resize ? std::optional(drag.Handle) : std::nullopt);
			if (edited != selected.GetComponent<UIRectComponent>())
				if (!m_Context.Execute("component.set", { { "entity", static_cast<uint64_t>(selected.GetUUID()) }, { "component", "UIRect" }, { "data", { { "Position", { edited.Position.x, edited.Position.y } }, { "Size", { edited.Size.x, edited.Size.y } }, { "Rotation", glm::degrees(edited.Rotation) }, { "Scale", { edited.Scale.x, edited.Scale.y } } } } }))
					FinishGizmo(true);
		}
		else if (m_UIRectDrag)
			m_UIRectDrag.reset();
		drawList->PopClipRect();
		return active;
	}

	void EditorLayer::DrawUIBounds(const glm::vec2& origin, const glm::vec2& size)
	{
		Scene& scene = *m_Session.GetScene();
		const glm::vec2 canvas = m_UIFrame.GetCanvasSize();
		const UICanvasMapping mapping = UICanvasMapping::ForViewport(origin, size);
		const auto toScreen = [&](const glm::vec2& point) {
			const glm::vec2 screen = mapping.ToScreen(point);
			return ImVec2(screen.x, screen.y);
		};
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(toScreen({ 0.0f, 0.0f }), toScreen(canvas), true);

		const Entity selected = scene.GetEntityByUUID(m_Session.GetSelection());
		const ImU32 boundsColor = IM_COL32(255, 255, 255, 90);
		const ImU32 selectionColor = ImGui::ColorConvertFloat4ToU32(ImVec4(s_SelectionColor.r, s_SelectionColor.g, s_SelectionColor.b, s_SelectionColor.a));
		std::optional<UILayoutItem> selectedRect;
		for (const UILayoutItem& item : UILayout::Layout(scene, canvas))
		{
			if (item.Owner == selected)
				selectedRect = item;
			else
			{
				const auto point = [&](const glm::vec2& uv) { return toScreen(glm::vec2(item.Transform * glm::vec3(item.Rect.Min + item.Rect.Size * uv, 1.0f))); };
				drawList->AddQuad(point({ 0, 0 }), point({ 1, 0 }), point({ 1, 1 }), point({ 0, 1 }), boundsColor);
			}
		}
		// The selection on top: its rect, its pivot as a dot and its anchors as diamonds on the parent rect.
		const auto* rectComponent = selected ? selected.TryGetComponent<UIRectComponent>() : nullptr;
		if (selectedRect)
		{
			const auto point = [&](const glm::vec2& uv) { return toScreen(glm::vec2(selectedRect->Transform * glm::vec3(selectedRect->Rect.Min + selectedRect->Rect.Size * uv, 1.0f))); };
			drawList->AddQuad(point({ 0, 0 }), point({ 1, 0 }), point({ 1, 1 }), point({ 0, 1 }), selectionColor, 2.0f);
		}
		if (selectedRect && rectComponent)
		{
			const UIRectComponent rect = SanitizeUIRect(*rectComponent);
			drawList->AddCircleFilled(toScreen(glm::vec2(selectedRect->Transform * glm::vec3(selectedRect->Rect.Min + selectedRect->Rect.Size * rect.Pivot, 1.0f))), 4.0f, selectionColor);
			const UIBox parent = UILayout::GetParentRect(scene, selected, canvas);
			const glm::vec2 anchorMin = parent.Min + parent.Size * rect.AnchorMin;
			const glm::vec2 anchorMax = parent.Min + parent.Size * rect.AnchorMax;
			const glm::mat3 parentTransform = selectedRect->Transform * glm::inverse(ComputeUITransform(selectedRect->Rect, rect));
			constexpr float AnchorMarkerRadius = 5.0f;
			for (const glm::vec2& anchor : { anchorMin, glm::vec2(anchorMax.x, anchorMin.y), glm::vec2(anchorMin.x, anchorMax.y), anchorMax })
			{
				const ImVec2 point = toScreen(glm::vec2(parentTransform * glm::vec3(anchor, 1.0f)));
				drawList->AddQuad(ImVec2(point.x, point.y - AnchorMarkerRadius), ImVec2(point.x + AnchorMarkerRadius, point.y), ImVec2(point.x, point.y + AnchorMarkerRadius), ImVec2(point.x - AnchorMarkerRadius, point.y), selectionColor, 1.5f);
			}
		}
		drawList->PopClipRect();
	}

	void EditorLayer::DrawSceneSettingsPanel()
	{
		ImGui::Begin(EditorCommandSchemas::SceneTitle);
		Scene& scene = *m_Session.GetScene();
		const nlohmann::json state = SceneSerializer::SerializeScene(scene);

		for (const char* group : { "Renderer", "Physics" })
		{
			if (!ImGui::CollapsingHeader(group, ImGuiTreeNodeFlags_DefaultOpen))
				continue;
			ImGui::PushID(group);
			nlohmann::json values = state[group];
			static const std::vector<std::string> s_Tonemappers(std::begin(TonemapperNames), std::end(TonemapperNames));
			UUID postProcessId = 0;
			if (std::string_view(group) == "Renderer")
				ResolvePostProcess(scene, &postProcessId);
			if (postProcessId != 0 && std::string_view(group) == "Renderer")
			{
				ImGui::TextDisabled("Post-processing is controlled by the active PostProcess component.");
				for (const char* key : PostProcessOverrideKeys)
					values.erase(key);
			}
			for (auto it = values.begin(); it != values.end(); ++it)
			{
				const JsonFieldResult result = DrawJsonField(it.key(), it.value(), it.key() == "Tonemapper" ? &s_Tonemappers : nullptr);
				m_Context.EditField("scene.settings", { { group, { { it.key(), it.value() } } } }, result.Changed, result.Committed);
			}
			ImGui::PopID();
		}

		if (ImGui::CollapsingHeader("Statistics", ImGuiTreeNodeFlags_DefaultOpen) && m_Renderer)
		{
			const SceneRendererStatistics& stats = m_Renderer->GetStatistics();
			if (stats.Failed)
				ImGui::TextColored(EditorColors::Error(), "Frame failed — see Console. Toggle FSR off/on to retry FSR allocation.");
			ImGui::Text("FSR 1: %s", stats.FSR1Active ? "active" : "inactive");
			ImGui::Text("Entities: %zu", scene.GetEntityCount());
			for (uint8_t i = 0; i < static_cast<uint8_t>(MemoryCategory::Count); ++i)
			{
				const auto category = static_cast<MemoryCategory>(i);
				const auto memory = MemoryTracker::Get(category);
				ImGui::Text("%s: %.2f MiB (peak %.2f)", MemoryTracker::GetName(category), static_cast<double>(memory.Bytes) / 1048576.0, static_cast<double>(memory.PeakBytes) / 1048576.0);
			}
			ImGui::Text("Draw calls: %u (shadow %u)", stats.DrawCalls, stats.ShadowDrawCalls);
			ImGui::Text("Culled submeshes: %u", stats.CulledSubmeshes);
			ImGui::Text("Local shadow maps: %u (skipped lights %u)", stats.LocalShadowMaps, stats.LocalShadowsSkipped);
			ImGui::Text("Lights skipped: %u", stats.LightsSkipped);
			ImGui::Text("Particles: %u (skipped %u)", stats.Particles, stats.ParticlesSkipped);
			ImGui::Text("Lights: %u", stats.Lights);
			if (m_Renderer->IsDynamicResolutionActive())
				ImGui::Text("Resolution: %ux%u (%.0f%% render scale, dynamic up to %.0f%%)", m_Renderer->GetRenderWidth(), m_Renderer->GetRenderHeight(), m_Renderer->GetCurrentRenderScale() * 100.0f, m_Renderer->GetRenderScale() * 100.0f);
			else
				ImGui::Text("Resolution: %ux%u (%.0f%% render scale)", m_Renderer->GetRenderWidth(), m_Renderer->GetRenderHeight(), m_Renderer->GetRenderScale() * 100.0f);
			if (m_Renderer->GetDynamicResolution().Enabled && !m_Renderer->IsDynamicResolutionActive())
				ImGui::TextDisabled("Dynamic resolution %s", DescribeDynamicResolutionState(m_Renderer->GetDynamicResolutionState(), m_Renderer->GetDynamicResolution(), m_Renderer->GetRenderScale()).c_str());
			ImGui::Text("Frame: %.2f ms (%.0f FPS)", 1000.0f / std::max(ImGui::GetIO().Framerate, 1.0f), ImGui::GetIO().Framerate);
			if (GraphicsDevice* device = Application::Get().GetGraphicsDevice())
				ImGui::Text("GPU: %s", device->GetAdapterName().c_str());

			// Scene passes only: the editor UI is drawn after the scene and is not part of the game's cost.
			const GpuTimings& timings = m_Renderer->GetGpuTimings();
			if (timings.SampleCount == 0)
			{
				ImGui::TextDisabled("GPU timings: %s", FormatGpuTimings(timings, m_Renderer->HasGpuTimers()).c_str());
			}
			else if (ImGui::BeginTable("GpuTimings", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
			{
				auto timingRow = [](const char* name, float milliseconds) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(name);
					ImGui::TableNextColumn();
					ImGui::Text("%.2f", milliseconds);
				};
				ImGui::TableSetupColumn("Scene GPU time");
				ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableHeadersRow();
				for (const GpuPassTiming& pass : timings.Passes)
					timingRow(pass.Name.c_str(), pass.Milliseconds);
				timingRow("Total", timings.FrameMilliseconds);
				ImGui::EndTable();
				if (timings.DroppedFrames > 0)
					ImGui::TextDisabled("%llu frames without timer results", static_cast<unsigned long long>(timings.DroppedFrames));
			}
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
		if (m_History.IsActive())
		{
			ImGui::Text("Transaction: %s", m_History.TransactionName().c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("Commit"))
				m_Context.Execute("editor.transaction", { { "action", "commit" } });
			ImGui::SameLine();
			if (ImGui::SmallButton("Cancel"))
				m_Context.Execute("editor.transaction", { { "action", "cancel" } });
			ImGui::SameLine();
		}
		if (!m_Context.StatusMessage.empty() && ImGui::GetTime() - m_Context.StatusTime < 8.0)
			ImGui::TextColored(m_Context.StatusIsError ? EditorColors::Error() : EditorColors::Success(), "%s", m_Context.StatusMessage.c_str());
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
			if (m_Prompt == PromptKind::OpenProject && !m_Preferences.GetRecentProjects().empty())
			{
				ImGui::TextDisabled("Recent projects (double-click to open):");
				DrawRecentProjectsList();
				ImGui::Separator();
			}
			ImGui::SetNextItemWidth(520.0f);
			ImGui::InputText("##Path", &m_PromptPath);
			if (m_Prompt == PromptKind::NewProject)
			{
				ImGui::SetNextItemWidth(520.0f);
				ImGui::InputTextWithHint("##Name", "Project name", &m_PromptName);
			}
			if (m_Prompt == PromptKind::Export)
			{
				ImGui::TextDisabled("The project is validated first; errors stop the export (Project Settings > Validate Project).");
				ImGui::Checkbox("Smoke test: run the exported game headless for", &m_ExportSmokeTest);
				ImGui::SameLine();
				ImGui::SetNextItemWidth(90.0f);
				ImGui::BeginDisabled(!m_ExportSmokeTest);
				ImGui::InputInt("frames", &m_ExportSmokeFrames, 0);
				m_ExportSmokeFrames = std::clamp(m_ExportSmokeFrames, 1, 100000);
				ImGui::EndDisabled();
				ImGui::TextDisabled("The editor waits for the smoke test (at most 60 s).");
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
						if (auto result = m_Context.Execute("project.export", { { "output", m_PromptPath }, { "smokeTestFrames", m_ExportSmokeTest ? m_ExportSmokeFrames : 0 } }))
						{
							const std::string executable = (*result)["executable"].get<std::string>();
							m_Context.SetStatus(m_ExportSmokeTest ? "Exported to " + executable + "; the smoke test passed" : "Exported to " + executable, false);
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
			{
				// Validation and smoke test failures span several lines (the problems, the game's log).
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 640.0f);
				ImGui::TextColored(EditorColors::Error(), "%s", m_Context.StatusMessage.c_str());
				ImGui::PopTextWrapPos();
			}
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
