#include "EditorLayer.h"

#include "Basalt/Core/CommandLine.h"
#include "Basalt/Core/EntryPoint.h"

namespace Basalt {

	class EditorApplication : public Application
	{
	public:
		EditorApplication(const ApplicationSpecification& specification, const EditorOptions& options)
			: Application(specification)
		{
			if (IsInitialized())
				PushLayer(new EditorLayer(options));
		}
	};

	// BasaltEditor [--project <dir>] [--port <n> | --no-server]
	Application* CreateApplication(ApplicationCommandLineArgs args)
	{
		const CommandLine commandLine(args.Count, args.Args, { "project", "port" });
		for (const std::string& error : commandLine.GetErrors())
			BS_CORE_ERROR("{}", error);

		EditorOptions options;
		options.ProjectPath = commandLine.GetValue("project").value_or("");
		if (!commandLine.GetPositional().empty() && options.ProjectPath.empty())
			options.ProjectPath = commandLine.GetPositional().front();
		const int64_t port = commandLine.GetInteger("port").value_or(AutomationServer::DefaultPort);
		if (port < 0 || port > 65535)
		{
			BS_CORE_ERROR("--port must be in [0, 65535]");
			return nullptr;
		}
		options.AutomationPort = commandLine.HasFlag("no-server") ? 0 : static_cast<uint16_t>(port);

		ApplicationSpecification specification;
		specification.Name = "Basalt Editor";
		specification.CommandLineArgs = args;
		specification.EnableImGui = true;
		specification.StartMaximized = true;
		return new EditorApplication(specification, options);
	}

}
