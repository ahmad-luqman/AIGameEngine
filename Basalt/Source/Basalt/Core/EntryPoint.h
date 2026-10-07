#pragma once

// Include exactly once, in the translation unit that defines Basalt::CreateApplication.

#include "Basalt/Core/Application.h"
#include "Basalt/Core/Log.h"

int main(int argc, char** argv)
{
	Basalt::Log::Init();

	int exitCode = 1;
	{
		Basalt::Application* application = Basalt::CreateApplication({ argc, argv });
		if (application)
		{
			exitCode = application->Run();
			delete application;
		}
	}

	Basalt::Log::Shutdown();
	return exitCode;
}
