#pragma once

// Include exactly once, in the translation unit that defines Breccia::CreateApplication.

#include "Breccia/Core/Application.h"
#include "Breccia/Core/CrashReporter.h"
#include "Breccia/Core/FileSystem.h"
#include "Breccia/Core/Log.h"
#include "Breccia/Core/Platform.h"

int main(int argc, char** argv)
{
	Breccia::Log::Init();
	// Before anything that can crash. Reports are named after the executable (an exported game's is the game's name).
	Breccia::CrashReporter::Install({ Breccia::FileSystem::PathToUtf8(Breccia::FileSystem::GetExecutablePath().stem()) });

	int exitCode = 1;
	{
		Breccia::Application* application = Breccia::CreateApplication({ Breccia::Platform::GetProcessArguments(argc, argv) });
		if (application)
		{
			exitCode = application->Run();
			delete application;
		}
	}

	Breccia::Log::Shutdown();
	return exitCode;
}
