#pragma once

// Include exactly once, in the translation unit that defines Basalt::CreateApplication.

#include "Basalt/Core/Application.h"
#include "Basalt/Core/CrashReporter.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"

int main(int argc, char** argv)
{
	Basalt::Log::Init();
	// Before anything that can crash. Reports are named after the executable (an exported game's is the game's name).
	Basalt::CrashReporter::Install({ Basalt::FileSystem::PathToUtf8(Basalt::FileSystem::GetExecutablePath().stem()) });

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
