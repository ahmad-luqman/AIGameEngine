#include "Basalt/Core/CommandLine.h"
#include "Basalt/Core/EntryPoint.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Project/Exporter.h"
#include "Basalt/Project/Packaging.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Renderer/RenderMath.h"

#include "RuntimeLayer.h"

#include <array>
#include <optional>
#include <vector>

namespace Basalt {

	class RuntimeApplication : public Application
	{
	public:
		RuntimeApplication(const ApplicationSpecification& specification, const RuntimeOptions& options)
			: Application(specification)
		{
			if (!IsInitialized())
				return;
			SetWindowIcon();
			PushLayer(new RuntimeLayer(options));
		}

	private:
		// The project icon as the window icon where the window has one (Windows, X11). A bad icon is reported, not fatal:
		// the game still plays.
		void SetWindowIcon()
		{
			const Ref<Project>& project = Project::GetActive();
			Window* window = GetWindow();
			if (!project || !window || !window->SupportsIcon())
				return;
			std::string error;
			std::optional<IconImage> icon;
			if (!LoadProjectIcon(*project, icon, error))
			{
				BS_CORE_ERROR("Window icon: {}", error);
				return;
			}
			if (!icon)
				return;
			// The sizes title bars, task switchers and taskbars use; the window system picks the closest.
			constexpr std::array<uint32_t, 4> Sizes = { 16, 32, 48, 256 };
			std::vector<IconImage> resized;
			resized.reserve(Sizes.size());
			for (uint32_t size : Sizes)
			{
				resized.push_back(ResizeIcon(*icon, size));
				if (!resized.back().IsValid())
				{
					BS_CORE_ERROR("Window icon: cannot scale the icon to {} pixels", size);
					return;
				}
			}
			// Views into resized, which outlives them.
			std::vector<WindowIcon> images;
			images.reserve(resized.size());
			for (const IconImage& image : resized)
				images.push_back({ image.Width, image.Height, image.Pixels });
			window->SetIcon(images);
		}
	};

	namespace {

		// Reads a render scale option into `value` (unchanged when absent); false, after logging, when it is
		// present but not a number in the render scale range.
		bool ReadScaleOption(const CommandLine& commandLine, const char* name, float& value)
		{
			if (!commandLine.GetValue(name))
				return true;
			const auto scale = commandLine.GetFloat(name);
			if (!scale || *scale < MinRenderScale || *scale > MaxRenderScale)
			{
				BS_CORE_CRITICAL("--{} must be a number in [{}, {}]", name, MinRenderScale, MaxRenderScale);
				return false;
			}
			value = *scale;
			return true;
		}

	}

	Application* CreateApplication(ApplicationCommandLineArgs args)
	{
		const CommandLine commandLine(args.Count, args.Args, { "project", "scene", "frames", "screenshot", "width", "height", "debug-view", "gpu", "render-scale", "dynamic-resolution", "min-render-scale" });
		// An option without its value would otherwise run with the default silently (a test passing vacuously).
		for (const std::string& error : commandLine.GetErrors())
			BS_CORE_CRITICAL("{}", error);
		if (!commandLine.GetErrors().empty())
			return nullptr;

		RuntimeOptions options;
		// An exported game ships its project file next to the executable (in Contents/Resources of a macOS bundle).
		std::filesystem::path projectPath = commandLine.GetValue("project").value_or(Exporter::GetExportedProjectDirectory(FileSystem::GetExecutableDirectory()).string());
		std::string error;
		Ref<Project> project = Project::Load(projectPath, error);
		if (!project)
		{
			BS_CORE_CRITICAL("Cannot load project: {}", error);
			return nullptr;
		}
		Project::SetActive(project);

		options.ScenePath = commandLine.GetValue("scene").value_or(project->GetConfig().StartScene);
		// Fatal: a malformed count would otherwise mean "run forever".
		if (commandLine.GetValue("frames") && !commandLine.GetInteger("frames"))
		{
			BS_CORE_CRITICAL("--frames must be an integer");
			return nullptr;
		}
		options.MaxFrames = static_cast<uint64_t>(std::max<int64_t>(commandLine.GetInteger("frames").value_or(0), 0));
		options.ScreenshotPath = commandLine.GetValue("screenshot").value_or("");
		options.RequireValidation = commandLine.HasFlag("require-validation");
		options.ShowStats = commandLine.HasFlag("stats");
		options.FailOnScriptErrors = commandLine.HasFlag("fail-on-script-errors");
		if (options.FailOnScriptErrors && options.MaxFrames == 0)
		{
			BS_CORE_CRITICAL("--fail-on-script-errors needs --frames (an interactive run has no end to report at)");
			return nullptr;
		}
		const std::string debugView = commandLine.GetValue("debug-view").value_or("None");
		if (debugView == "SSAO")
			options.DebugView = RendererDebugView::SSAO;
		else if (debugView == "Normals")
			options.DebugView = RendererDebugView::Normals;
		else if (debugView == "Depth")
			options.DebugView = RendererDebugView::Depth;
		else if (debugView != "None")
		{
			// An error, not a warning: a typo would otherwise silently test the lit image instead.
			BS_CORE_CRITICAL("Unknown --debug-view '{}' (valid: None, SSAO, Normals, Depth)", debugView);
			return nullptr;
		}
		options.RenderScale = project->GetConfig().Graphics.RenderScale;
		if (!ReadScaleOption(commandLine, "render-scale", options.RenderScale))
			return nullptr;
		if (commandLine.GetValue("render-scale"))
			options.Overrides.RenderScale = options.RenderScale;
		if (commandLine.HasFlag("no-vsync"))
			options.Overrides.VSync = false;
		options.DynamicResolution = project->GetConfig().Graphics.DynamicResolution;
		if (commandLine.GetValue("dynamic-resolution"))
		{
			// Above the project range on purpose: an unreachable target pins the lowest scale, which the GPU
			// tests rely on. 0 turns dynamic resolution off.
			constexpr float MaxCommandLineFrameRate = 1e6f;
			const auto frameRate = commandLine.GetFloat("dynamic-resolution");
			if (!frameRate || (*frameRate != 0.0f && (*frameRate < MinTargetFrameRate || *frameRate > MaxCommandLineFrameRate)))
			{
				BS_CORE_CRITICAL("--dynamic-resolution must be 0 (off) or a frame rate in [{}, {}]", MinTargetFrameRate, MaxCommandLineFrameRate);
				return nullptr;
			}
			options.DynamicResolution.Enabled = *frameRate != 0.0f;
			options.RequireDynamicResolution = options.DynamicResolution.Enabled;
			options.Overrides.DynamicResolution = options.DynamicResolution.Enabled;
			if (options.DynamicResolution.Enabled)
			{
				options.DynamicResolution.TargetFrameRate = *frameRate;
				options.Overrides.TargetFrameRate = frameRate;
			}
		}
		if (!ReadScaleOption(commandLine, "min-render-scale", options.DynamicResolution.MinScale))
			return nullptr;
		if (commandLine.GetValue("min-render-scale"))
			options.Overrides.MinRenderScale = options.DynamicResolution.MinScale;
		if (commandLine.GetValue("min-render-scale") && !options.DynamicResolution.Enabled)
			BS_CORE_WARN("--min-render-scale has no effect: dynamic resolution is off (enable it in the project or with --dynamic-resolution <fps>)");

		ApplicationSpecification specification;
		specification.Name = project->GetConfig().Name;
		specification.CommandLineArgs = args;
		specification.GpuSelector = commandLine.GetValue("gpu").value_or("");
		auto readSize = [&](const char* name, uint32_t fallback) {
			if (!commandLine.GetValue(name))
				return fallback;
			const auto value = commandLine.GetInteger(name);
			if (!value || *value < ProjectConfig::MinWindowSize || *value > ProjectConfig::MaxWindowSize)
			{
				BS_CORE_WARN("--{} must be an integer in [{}, {}]; using {}", name, ProjectConfig::MinWindowSize, ProjectConfig::MaxWindowSize, fallback);
				return fallback;
			}
			return static_cast<uint32_t>(*value);
		};
		specification.WindowWidth = readSize("width", project->GetConfig().WindowWidth);
		specification.WindowHeight = readSize("height", project->GetConfig().WindowHeight);
		specification.VSync = project->GetConfig().Graphics.VSync && !commandLine.HasFlag("no-vsync");
		options.VSync = specification.VSync;
		specification.Headless = commandLine.HasFlag("headless");
		specification.Offscreen = commandLine.HasFlag("offscreen");
		if (specification.Headless && specification.Offscreen)
		{
			BS_CORE_CRITICAL("--headless (no GPU) and --offscreen (GPU, no window) cannot be combined");
			return nullptr;
		}
		// ImGui draws the game's screen-space UI (UI.Text / UI.Rect); there are no ImGui windows.
		specification.EnableImGui = true;
		// Deterministic frames for automated runs (screenshots, tests).
		if (options.MaxFrames > 0)
			specification.FixedTimestep = 1.0f / 60.0f;
		if (specification.Headless)
		{
			if (options.ShowStats)
			{
				BS_CORE_ERROR("--stats needs a GPU and cannot be combined with --headless");
				return nullptr;
			}
			// Without a GPU nothing renders: the application loop itself enforces the frame cap.
			specification.MaxFrames = options.MaxFrames;
			if (!options.ScreenshotPath.empty())
			{
				BS_CORE_ERROR("--screenshot needs a GPU and cannot be combined with --headless");
				return nullptr;
			}
			if (options.RequireDynamicResolution)
			{
				BS_CORE_ERROR("--dynamic-resolution needs a GPU and cannot be combined with --headless");
				return nullptr;
			}
		}
		return new RuntimeApplication(specification, options);
	}

}
