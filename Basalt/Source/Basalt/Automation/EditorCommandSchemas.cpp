#include "Basalt/Automation/EditorCommandSchemas.h"

#include "Basalt/Automation/CommandSchema.h"

namespace Basalt::EditorCommandSchemas {

	namespace Schema = CommandSchema;

	nlohmann::json Select()
	{
		return Schema::Object({ { "entity", Schema::Nullable(Schema::Entity("ID, name or null")) } });
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
