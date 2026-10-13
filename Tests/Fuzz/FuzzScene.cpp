// Scene and prefab JSON (.bscene / .bprefab) as read from disk: parse, deserialize, and lay out the UI.
#include "FuzzCommon.h"

#include <Basalt/Core/JsonUtils.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/Scene/SceneSerializer.h>
#include <Basalt/UI/UILayout.h>

#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	const nlohmann::json json = Basalt::ParseJson(BasaltFuzz::AsText(data, size));
	if (json.is_discarded())
		return 0;

	std::string error;
	{
		Basalt::Scene scene;
		if (Basalt::SceneSerializer::DeserializeScene(scene, json, error))
		{
			// Whatever loaded must serialize and hash again (round trip of accepted input), and lay out its UI.
			Basalt::SceneSerializer::SerializeScene(scene);
			Basalt::SceneSerializer::ComputeStateHash(scene);
			std::vector<Basalt::UIDrawCommand> commands;
			Basalt::UILayout::BuildCommands(scene, { 1920.0f, 1080.0f }, commands);
		}
	}
	if (json.is_object() && json.contains("Entities"))
	{
		Basalt::Scene scene;
		Basalt::SceneSerializer::DeserializeEntityTree(scene, json["Entities"], Basalt::Entity(), error);
	}
	return 0;
}
