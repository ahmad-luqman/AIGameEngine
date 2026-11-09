#include "EditorLayer.h"

#include "Breccia/Core/CommandLine.h"
#include "Breccia/Core/EntryPoint.h"

namespace Breccia {

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

	// BrecciaEditor [--project <dir>] [--port <n> | --no-server]
	Application* CreateApplication(const ApplicationCommandLineArgs& args)
	{
		const CommandLine commandLine(args.Arguments, { "project", "port", "gpu" });
		for (const std::string& error : commandLine.GetErrors())
			BR_CORE_ERROR("{}", error);

		EditorOptions options;
		options.ProjectPath = commandLine.GetValue("project").value_or("");
		if (!commandLine.GetPositional().empty() && options.ProjectPath.empty())
			options.ProjectPath = commandLine.GetPositional().front();
		const int64_t port = commandLine.GetInteger("port").value_or(AutomationServer::DefaultPort);
		if (port < 0 || port > 65535)
		{
			BR_CORE_ERROR("--port must be in [0, 65535]");
			return nullptr;
		}
		options.AutomationPort = commandLine.HasFlag("no-server") ? 0 : static_cast<uint16_t>(port);

		ApplicationSpecification specification;
		specification.Name = "Breccia Editor";
		specification.GpuSelector = commandLine.GetValue("gpu").value_or("");
		specification.EnableImGui = true;
		specification.StartMaximized = true;
		return new EditorApplication(specification, options);
	}

}
