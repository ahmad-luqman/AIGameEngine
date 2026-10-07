#include "Basalt/Core/CommandLine.h"
#include "Basalt/Core/EntryPoint.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Project/Project.h"

#include "RuntimeLayer.h"

namespace Basalt {

	class RuntimeApplication : public Application
	{
	public:
		RuntimeApplication(const ApplicationSpecification& specification, const RuntimeOptions& options)
			: Application(specification)
		{
			if (IsInitialized())
				PushLayer(new RuntimeLayer(options));
		}
	};

	Application* CreateApplication(ApplicationCommandLineArgs args)
	{
		const CommandLine commandLine(args.Count, args.Args, { "project", "scene", "frames", "screenshot", "width", "height", "debug-view" });
		for (const std::string& error : commandLine.GetErrors())
			BS_CORE_ERROR("{}", error);

		RuntimeOptions options;
		// An exported game ships its project file next to the executable.
		std::filesystem::path projectPath = commandLine.GetValue("project").value_or(FileSystem::GetExecutableDirectory().string());
		std::string error;
		Ref<Project> project = Project::Load(projectPath, error);
		if (!project)
		{
			BS_CORE_CRITICAL("Cannot load project: {}", error);
			return nullptr;
		}
		Project::SetActive(project);

		options.ScenePath = commandLine.GetValue("scene").value_or(project->GetConfig().StartScene);
		if (commandLine.GetValue("frames") && !commandLine.GetInteger("frames"))
			BS_CORE_WARN("--frames must be an integer");
		options.MaxFrames = static_cast<uint64_t>(std::max<int64_t>(commandLine.GetInteger("frames").value_or(0), 0));
		options.ScreenshotPath = commandLine.GetValue("screenshot").value_or("");
		const std::string debugView = commandLine.GetValue("debug-view").value_or("None");
		if (debugView == "SSAO")
			options.DebugView = RendererDebugView::SSAO;
		else if (debugView == "Normals")
			options.DebugView = RendererDebugView::Normals;
		else if (debugView == "Depth")
			options.DebugView = RendererDebugView::Depth;
		else if (debugView != "None")
			BS_CORE_WARN("Unknown --debug-view '{}' (valid: None, SSAO, Normals, Depth)", debugView);

		ApplicationSpecification specification;
		specification.Name = project->GetConfig().Name;
		specification.CommandLineArgs = args;
		auto readSize = [&](const char* name, uint32_t fallback) {
			if (!commandLine.GetValue(name))
				return fallback;
			const auto value = commandLine.GetInteger(name);
			if (!value || *value < 64 || *value > 16384)
			{
				BS_CORE_WARN("--{} must be an integer in [64, 16384]; using {}", name, fallback);
				return fallback;
			}
			return static_cast<uint32_t>(*value);
		};
		specification.WindowWidth = readSize("width", project->GetConfig().WindowWidth);
		specification.WindowHeight = readSize("height", project->GetConfig().WindowHeight);
		specification.VSync = !commandLine.HasFlag("no-vsync");
		specification.Headless = commandLine.HasFlag("headless");
		// Deterministic frames for automated runs (screenshots, tests).
		if (options.MaxFrames > 0)
			specification.FixedTimestep = 1.0f / 60.0f;
		return new RuntimeApplication(specification, options);
	}

}
