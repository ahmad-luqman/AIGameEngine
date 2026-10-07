#include "Basalt/Core/EntryPoint.h"
#include "Basalt/Renderer/GraphicsDevice.h"

#include <nvrhi/utils.h>

namespace Basalt {

	// Temporary bring-up layer: clears the back buffer to prove the device/swapchain path.
	class ClearLayer : public Layer
	{
	public:
		ClearLayer()
			: Layer("ClearLayer")
		{
		}

		void OnAttach() override
		{
			m_CommandList = Application::Get().GetGraphicsDevice()->GetDevice()->createCommandList();
		}

		void OnDetach() override { m_CommandList = nullptr; }

		void OnRender() override
		{
			GraphicsDevice* device = Application::Get().GetGraphicsDevice();
			m_CommandList->open();
			nvrhi::utils::ClearColorAttachment(m_CommandList, device->GetCurrentFramebuffer(), 0, nvrhi::Color(0.1f, 0.2f, 0.3f, 1.0f));
			m_CommandList->close();
			device->GetDevice()->executeCommandList(m_CommandList);
		}

	private:
		nvrhi::CommandListHandle m_CommandList;
	};

	class RuntimeApplication : public Application
	{
	public:
		explicit RuntimeApplication(const ApplicationSpecification& specification)
			: Application(specification)
		{
			if (IsInitialized())
				PushLayer(new ClearLayer());
		}
	};

	Application* CreateApplication(ApplicationCommandLineArgs args)
	{
		ApplicationSpecification specification;
		specification.Name = "Basalt Runtime";
		specification.CommandLineArgs = args;
		specification.EnableImGui = true;
		return new RuntimeApplication(specification);
	}

}
