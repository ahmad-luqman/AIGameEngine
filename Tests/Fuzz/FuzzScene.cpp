// Scene and prefab JSON (.bscene / .bprefab) as read from disk: parse, deserialize, lay out the UI and let the
// widgets take a click and a navigation step.
#include "FuzzCommon.h"

#include <Breccia/Core/Input.h>
#include <Breccia/Core/JsonUtils.h>
#include <Breccia/Scene/Entity.h>
#include <Breccia/Scene/Scene.h>
#include <Breccia/Scene/SceneSerializer.h>
#include <Breccia/UI/UIInteraction.h>
#include <Breccia/UI/UILayout.h>

#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BrecciaFuzz::Init();
	const nlohmann::json json = Breccia::ParseJson(BrecciaFuzz::AsText(data, size));
	if (json.is_discarded())
		return 0;

	std::string error;
	{
		Breccia::Scene scene;
		if (Breccia::SceneSerializer::DeserializeScene(scene, json, error))
		{
			// Whatever loaded must serialize and hash again (round trip of accepted input), and lay out its UI.
			Breccia::SceneSerializer::SerializeScene(scene);
			Breccia::SceneSerializer::ComputeStateHash(scene);
			std::vector<Breccia::UIDrawCommand> commands;
			Breccia::UILayout::BuildCommands(scene, { 1920.0f, 1080.0f }, commands);

			Breccia::UIInteraction interaction;
			Breccia::Input::SetMousePosition({ 960.0f, 540.0f });
			Breccia::Input::SetMouseButtonState(Breccia::MouseCode::Left, true);
			Breccia::Input::SetKeyState(Breccia::KeyCode::Tab, true);
			interaction.Update(scene, 1.0f / 60.0f);
			Breccia::Input::EndFrame();
			Breccia::Input::SetMouseButtonState(Breccia::MouseCode::Left, false);
			Breccia::Input::SetKeyState(Breccia::KeyCode::Tab, false);
			Breccia::Input::SetKeyState(Breccia::KeyCode::Right, true);
			interaction.Update(scene, 1.0f / 60.0f);
			Breccia::Input::Reset();
		}
	}
	if (json.is_object() && json.contains("Entities"))
	{
		Breccia::Scene scene;
		Breccia::SceneSerializer::DeserializeEntityTree(scene, json["Entities"], Breccia::Entity(), error);
	}
	return 0;
}
