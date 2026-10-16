#pragma once

#include "Basalt/Core/UUID.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Basalt {

	class PhysicsLayers;
	class Project;
	class Scene;

	enum class ValidationSeverity : uint8_t
	{
		// The exported game would fail or misbehave: project.export refuses unless validate is false.
		Error,
		// Worth fixing, but the game runs (e.g. a body on an undefined layer plays on Default).
		Warning
	};

	// One problem: where it is and what is wrong.
	struct ValidationIssue
	{
		ValidationSeverity Severity = ValidationSeverity::Error;
		// Project-relative file the problem is in ("Project.bproject" for project settings).
		std::string File;
		std::string Message;
	};

	// What ValidateProject found. Issues are capped at MaxIssues (errors displace warnings, so the list always
	// shows errors when there are any); the counts include every problem.
	class ValidationReport
	{
	public:
		static constexpr size_t MaxIssues = 200;

		void Add(ValidationSeverity severity, std::string file, std::string message);

		const std::vector<ValidationIssue>& GetIssues() const { return m_Issues; }
		size_t GetErrorCount() const { return m_ErrorCount; }
		size_t GetWarningCount() const { return m_WarningCount; }
		// Problems beyond MaxIssues, counted but not listed.
		size_t GetOmittedCount() const { return m_ErrorCount + m_WarningCount - m_Issues.size(); }
		bool IsValid() const { return m_ErrorCount == 0; }

	private:
		std::vector<ValidationIssue> m_Issues;
		size_t m_ErrorCount = 0;
		size_t m_WarningCount = 0;
	};

	// Checks that a project would export and start: project settings, the start scene, every scene and prefab
	// (they load, and every asset path they reference exists inside Assets/ with the exact letter case, which
	// case-sensitive file systems need), and every Lua script under Assets/ (it compiles; a script a Script
	// component uses must also run its top level and return a class table). Asset references are recognised by
	// file extension: a component string such as "Assets/Textures/Wall.png" or "builtin://Cube" is checked (an
	// error when broken), a bare "Wall.png" without a folder only warns, since it may be plain text. Files are
	// checked for existence, not decoded. Headless; reads files but changes nothing.
	ValidationReport ValidateProject(const Project& project);

	// A RigidBody or CharacterController that names a layer the project does not define (it plays on Default).
	struct UnknownPhysicsLayerUse
	{
		UUID Entity = 0;
		std::string EntityName;
		std::string Layer;
	};

	// Uses of undefined layers, in registry order, rigid bodies first. A character controller replaces the
	// entity's RigidBody, so only the controller's layer counts there.
	std::vector<UnknownPhysicsLayerUse> FindUnknownPhysicsLayers(Scene& scene, const PhysicsLayers& layers);

}
