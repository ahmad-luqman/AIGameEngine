#include "Breccia/Core/CommandLine.h"
#include "Breccia/Core/CrashReporter.h"
#include "Breccia/Core/EntryPoint.h"
#include "Breccia/Core/FileSystem.h"
#include "Breccia/Project/Exporter.h"
#include "Breccia/Project/Packaging.h"
#include "Breccia/Project/Project.h"
#include "Breccia/Renderer/RenderMath.h"
#include "Breccia/Scripting/ScriptEngine.h"

#include "RuntimeLayer.h"

#include <array>
#include <charconv>
#include <optional>
#include <vector>

namespace Breccia {

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
				BR_CORE_ERROR("Window icon: {}", error);
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
					BR_CORE_ERROR("Window icon: cannot scale the icon to {} pixels", size);
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
				BR_CORE_CRITICAL("--{} must be a number in [{}, {}]", name, MinRenderScale, MaxRenderScale);
				return false;
			}
			value = *scale;
			return true;
		}

		// Reads --host <port> and --connect <address[:port]> into options (a missing port is the project's); false, after
		// logging, when they are malformed or both given.
		bool ReadNetworkOptions(const CommandLine& commandLine, const NetworkSettings& settings, RuntimeOptions& options)
		{
			const std::optional<std::string> host = commandLine.GetValue("host");
			const std::optional<std::string> connect = commandLine.GetValue("connect");
			if (host && connect)
			{
				BR_CORE_CRITICAL("--host and --connect cannot be combined (a host plays its own game)");
				return false;
			}
			if (host)
			{
				const std::optional<int64_t> port = commandLine.GetInteger("host");
				if (!port || *port < 0 || *port > 65535)
				{
					BR_CORE_CRITICAL("--host must be a port in [0, 65535] (0 picks a free one), not '{}'", *host);
					return false;
				}
				options.NetworkPort = static_cast<uint16_t>(*port);
				options.Network = NetworkRole::Host;
			}
			else if (connect)
			{
				if (!NetworkSettings::ParseAddress(*connect, settings.Port, options.ConnectAddress, options.NetworkPort))
				{
					BR_CORE_CRITICAL("--connect must be <address> or <address>:<port> with a port in [1, 65535], not '{}'", *connect);
					return false;
				}
				options.Network = NetworkRole::Client;
			}
			return true;
		}

	}

	Application* CreateApplication(const ApplicationCommandLineArgs& args)
	{
		ScriptEngine::DisableInstructionLimit();
		const CommandLine commandLine(args.Arguments, { "project", "scene", "frames", "screenshot", "width", "height", "debug-view", "gpu", "render-scale", "dynamic-resolution", "min-render-scale", "host", "connect" });
		// An option without its value would otherwise run with the default silently (a test passing vacuously).
		for (const std::string& error : commandLine.GetErrors())
			BR_CORE_CRITICAL("{}", error);
		if (!commandLine.GetErrors().empty())
			return nullptr;
		// Fatal: a malformed count would otherwise mean "run forever".
		if (commandLine.GetValue("frames") && !commandLine.GetInteger("frames"))
		{
			BR_CORE_CRITICAL("--frames must be an integer");
			return nullptr;
		}
		// A player sees a message box after a crash or a failed start; automated runs (no window, or a frame count) never
		// block on one. Decided first: the project and its assets load before the rest of the options are read.
		CrashReporter::SetDialogEnabled(!commandLine.HasFlag("headless") && !commandLine.HasFlag("offscreen") && commandLine.GetInteger("frames").value_or(0) <= 0);
		const auto fail = [](const std::string& message) -> Application* {
			BR_CORE_CRITICAL("{}", message);
			CrashReporter::ShowErrorDialog(message);
			return nullptr;
		};

		RuntimeOptions options;
		// An exported game ships its project file next to the executable (in Contents/Resources of a macOS bundle).
		// A path, not path::string(), which throws on Windows for an install folder outside the ANSI code page.
		const std::optional<std::string> projectArgument = commandLine.GetValue("project");
		const std::filesystem::path projectPath = projectArgument ? FileSystem::PathFromUtf8(*projectArgument) : Exporter::GetExportedProjectDirectory(FileSystem::GetExecutableDirectory());
		std::string error;
		// An archived export reads its Assets from Assets.bpak, mounted before anything (the project's asset UUIDs) loads.
		std::error_code directoryError;
		const bool isDirectory = std::filesystem::is_directory(projectPath, directoryError);
		if (directoryError && directoryError != std::errc::no_such_file_or_directory)
			return fail(fmt::format("Cannot inspect '{}': {}", FileSystem::PathToUtf8(projectPath), FileSystem::DescribeError(directoryError)));
		const std::filesystem::path projectDirectory = isDirectory ? projectPath : projectPath.parent_path();
		if (!Exporter::MountExportedArchive(projectDirectory, error))
			return fail("Cannot open the game's assets: " + error);
		Ref<Project> project = Project::Load(projectPath, error);
		if (!project)
			return fail("Cannot load project: " + error);
		// The asset registry only reads UUIDs from an archive mounted at its project's folder; a mismatch (another spelling
		// of the path) would leave every UUID reference unresolved without saying why.
		if (FileSystem::Exists(projectDirectory / Project::ArchiveFileName) && !FileSystem::GetMountedArchive(project->GetDirectory()))
			return fail(fmt::format("The game's {} is not mounted at its project folder '{}'", Project::ArchiveFileName, FileSystem::PathToUtf8(project->GetDirectory())));
		Project::SetActive(project);
		CrashReporter::SetContext("Project: " + project->GetConfig().Name + " " + project->GetConfig().Version);

		options.ScenePath = commandLine.GetValue("scene").value_or(project->GetConfig().StartScene);
		options.MaxFrames = static_cast<uint64_t>(std::max<int64_t>(commandLine.GetInteger("frames").value_or(0), 0));
		options.ScreenshotPath = commandLine.GetValue("screenshot").value_or("");
		options.RequireValidation = commandLine.HasFlag("require-validation");
		options.ShowStats = commandLine.HasFlag("stats");
		options.FailOnScriptErrors = commandLine.HasFlag("fail-on-script-errors");
		if (options.FailOnScriptErrors && options.MaxFrames == 0)
		{
			BR_CORE_CRITICAL("--fail-on-script-errors needs --frames (an interactive run has no end to report at)");
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
			BR_CORE_CRITICAL("Unknown --debug-view '{}' (valid: None, SSAO, Normals, Depth)", debugView);
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
				BR_CORE_CRITICAL("--dynamic-resolution must be 0 (off) or a frame rate in [{}, {}]", MinTargetFrameRate, MaxCommandLineFrameRate);
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
			BR_CORE_WARN("--min-render-scale has no effect: dynamic resolution is off (enable it in the project or with --dynamic-resolution <fps>)");

		if (!ReadNetworkOptions(commandLine, project->GetConfig().Network, options))
			return nullptr;

		ApplicationSpecification specification;
		specification.Name = project->GetConfig().Name;
		specification.GpuSelector = commandLine.GetValue("gpu").value_or("");
		auto readSize = [&](const char* name, uint32_t fallback) {
			if (!commandLine.GetValue(name))
				return fallback;
			const auto value = commandLine.GetInteger(name);
			if (!value || *value < ProjectConfig::MinWindowSize || *value > ProjectConfig::MaxWindowSize)
			{
				BR_CORE_WARN("--{} must be an integer in [{}, {}]; using {}", name, ProjectConfig::MinWindowSize, ProjectConfig::MaxWindowSize, fallback);
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
			BR_CORE_CRITICAL("--headless (no GPU) and --offscreen (GPU, no window) cannot be combined");
			return nullptr;
		}
		// ImGui draws the game's screen-space UI (UI.Text / UI.Rect); there are no ImGui windows.
		specification.EnableImGui = true;
		// Deterministic frames for automated runs (screenshots, tests).
		if (options.MaxFrames > 0)
			specification.FixedTimestep = 1.0f / 60.0f;
		if (specification.Headless)
		{
			// A dedicated server (or a headless client) has no display to wait for: run at the frame rate instead of
			// spinning. Other headless runs (tests) go as fast as they can.
			if (options.Network)
				specification.MinFrameSeconds = specification.FixedTimestep > 0.0f ? specification.FixedTimestep : 1.0f / 60.0f;
			if (options.ShowStats)
			{
				BR_CORE_ERROR("--stats needs a GPU and cannot be combined with --headless");
				return nullptr;
			}
			// Without a GPU nothing renders: the application loop itself enforces the frame cap.
			specification.MaxFrames = options.MaxFrames;
			if (!options.ScreenshotPath.empty())
			{
				BR_CORE_ERROR("--screenshot needs a GPU and cannot be combined with --headless");
				return nullptr;
			}
			if (options.RequireDynamicResolution)
			{
				BR_CORE_ERROR("--dynamic-resolution needs a GPU and cannot be combined with --headless");
				return nullptr;
			}
		}
		return new RuntimeApplication(specification, options);
	}

}
