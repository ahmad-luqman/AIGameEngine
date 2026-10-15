#include "Basalt/Automation/EditorCommandSchemas.h"

#include "Basalt/Automation/CommandSchema.h"

namespace Basalt::EditorCommandSchemas {

	namespace Schema = CommandSchema;

	nlohmann::json Pick()
	{
		return Schema::Object({ { "x", Schema::Integer("Integer viewport output pixel, left origin", 0, 16383) }, { "y", Schema::Integer("Integer viewport output pixel, top origin", 0, 16383) }, { "select", Schema::Boolean("Select the hit entity (default true)") } }, { "x", "y" });
	}

	nlohmann::json VertexSnap()
	{
		return Schema::Object({ { "source", Schema::Vector("Logical viewport coordinates [x,y], may be fractional, on the selected mesh", 2) }, { "target", Schema::Vector("Logical viewport coordinates [x,y], may be fractional, on another mesh", 2) } }, { "source", "target" });
	}

	nlohmann::json Select()
	{
		auto mode = Schema::String("Selection operation (default replace)");
		mode["enum"] = { "replace", "add", "toggle" };
		return Schema::Object({ { "entity", Schema::Nullable(Schema::Entity("ID, name or null")) }, { "entities", { { "type", "array" }, { "items", Schema::Entity("ID or name") }, { "maxItems", 10000 } } }, { "mode", mode } });
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
		return Schema::Object({ { "start", Schema::Vector("Logical viewport [x,y], top-left origin", 2) }, { "end", Schema::Vector("Logical viewport [x,y], top-left origin", 2) }, { "mode", Select()["properties"]["mode"] } }, { "start", "end" });
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

}
