#include "Basalt/Automation/EditorCommandSchemas.h"

#include "Basalt/Automation/CommandSchema.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

namespace Basalt::EditorCommandSchemas {

	namespace Schema = CommandSchema;

	nlohmann::json Pick()
	{
		return Schema::Object({ { "x", Schema::Integer("Integer viewport output pixel, left origin", 0, 16383) }, { "y", Schema::Integer("Integer viewport output pixel, top origin", 0, 16383) }, { "select", Schema::Boolean("Select the hit entity (default true); UI rects are picked outside Play only with editor.viewport uiBounds=true") } }, { "x", "y" });
	}

	nlohmann::json VertexSnap()
	{
		return Schema::Object({ { "source", Schema::Vector("Logical viewport coordinates [x,y], may be fractional, on the selected mesh", 2) }, { "target", Schema::Vector("Logical viewport coordinates [x,y], may be fractional, on another mesh", 2) } }, { "source", "target" });
	}

	namespace {
		nlohmann::json SelectionModeSchema()
		{
			auto mode = Schema::String("Selection operation (default replace)");
			mode["enum"] = { "replace", "add", "toggle" };
			return mode;
		}
	}

	nlohmann::json Select()
	{
		return Schema::Object({ { "entity", Schema::Nullable(Schema::Entity("ID, name or null")) }, { "entities", { { "type", "array" }, { "items", Schema::Entity("ID or name") }, { "maxItems", 10000 } } }, { "mode", SelectionModeSchema() } });
	}

	nlohmann::json Transform()
	{
		return Schema::Object({ { "delta", Schema::Vector("Column-major world-space delta matrix; 16 finite floats", 16) } }, { "delta" });
	}

	nlohmann::json ComponentSet()
	{
		return Schema::Object({ { "component", Schema::String("Shared component name", true) }, { "data", { { "type", "object" }, { "description", "Fields to merge into each selected component" } } } }, { "component", "data" });
	}

	nlohmann::json BoxSelect()
	{
		return Schema::Object({ { "start", Schema::Vector("Logical viewport [x,y], top-left origin", 2) }, { "end", Schema::Vector("Logical viewport [x,y], top-left origin", 2) }, { "mode", SelectionModeSchema() } }, { "start", "end" });
	}

	nlohmann::json Transaction()
	{
		auto action = Schema::String("begin, commit or cancel; nesting is rejected");
		action["enum"] = { "begin", "commit", "cancel" };
		return Schema::Object({ { "action", action }, { "name", Schema::String("Undo label for begin", true) } }, { "action" });
	}

	nlohmann::json Camera()
	{
		return Schema::Object({ { "position", Schema::Vector("[x, y, z]", 3) }, { "target", Schema::Vector("[x, y, z]", 3) } });
	}

	nlohmann::json Undo()
	{
		return Schema::Object();
	}

	nlohmann::json Redo()
	{
		return Schema::Object();
	}

	nlohmann::json WindowScreenshot()
	{
		return Schema::Object({ { "path", Schema::String("string, .png", true) } }, { "path" });
	}

	nlohmann::json Screenshot()
	{
		return Schema::Object({ { "path", Schema::String("string, .png", true) }, { "width", Schema::Integer("integer", 16, 8192) }, { "height", Schema::Integer("integer", 16, 8192) } }, { "path" });
	}

	nlohmann::json Panel()
	{
		auto name = Schema::String("Editor window title");
		name["enum"] = nlohmann::json::array();
		for (const PanelInfo& panel : Panels)
			name["enum"].push_back(panel.Title);
		return Schema::Object({ { "name", name }, { "open", Schema::Boolean("Open or close the window (only Project Settings can be closed)") }, { "focus", Schema::Boolean("Bring the window (or its tab) to the front") } }, { "name" });
	}

	bool IsClosablePanel(const std::string& title)
	{
		return std::any_of(std::begin(Panels), std::end(Panels), [&](const PanelInfo& panel) { return panel.Closable && title == panel.Title; });
	}

	nlohmann::json Thumbnail()
	{
		return Schema::Object({ { "path", Schema::String("Project-relative image, .hdr, .gltf/.glb, .bprefab or .bmaterial", true) }, { "output", Schema::String("Also write the preview to this .png", true) }, { "refresh", Schema::Boolean("Make it again even when the cache is current") } }, { "path" });
	}

	nlohmann::json MaterialPreview()
	{
		return Schema::Object({ { "entity", Schema::Entity("ID or name of an entity with a Material component") }, { "output", Schema::String(".png to write", true) } }, { "entity", "output" });
	}

	nlohmann::json MaterialEditor()
	{
		return Schema::Object({ { "path", Schema::String("Project-relative .bmaterial to open, optional", true) }, { "close", Schema::Boolean("Close the window, discarding unsaved edits") } });
	}

}
