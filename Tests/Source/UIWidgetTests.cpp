#include <doctest/doctest.h>

#include <Basalt/Core/Input.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Events/KeyEvent.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/Scripting/ScriptEngine.h>
#include <Basalt/UI/GameUI.h>
#include <Basalt/UI/UIInteraction.h>
#include <Basalt/UI/UILayout.h>
#include <Basalt/UI/UIRules.h>
#include <Basalt/UI/UIWidgets.h>

#include "TestUtils.h"
#include "UITestUtils.h"

#include <algorithm>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace Basalt;
using namespace BasaltTest;

namespace {

	// Resets the global input and game UI state the widgets read, on entry and exit, so tests do not leak into
	// each other: no keys or buttons, no gamepads, the mouse far away, the headless 1920x1080 view.
	class WidgetInputScope
	{
	public:
		WidgetInputScope() { Reset(); }
		~WidgetInputScope() { Reset(); }

		WidgetInputScope(const WidgetInputScope&) = delete;
		WidgetInputScope& operator=(const WidgetInputScope&) = delete;

		static void Reset()
		{
			Input::Reset();
			Input::ForgetInjectedGamepads();
			Input::SetMousePosition({ -1000.0f, -1000.0f });
			Input::EndFrame();
			GameUI::SetViewportSize({ 1920.0f, 1080.0f });
			GameUI::SetViewportOrigin({ 0.0f, 0.0f });
			GameUI::SetPointerEnabled(true);
			GameUI::SetNavigationEnabled(true);
			GameUI::BeginFrame();
		}
	};

	void Frame(Scene& scene, int frames = 1, float deltaTime = Step)
	{
		for (int i = 0; i < frames; i++)
		{
			scene.OnUpdate(Timestep(deltaTime));
			Input::EndFrame();
		}
	}

	// A rect pinned to the canvas' top-left corner: its box is exactly {position, size}.
	UIRectComponent PlacedRect(const glm::vec2& position, const glm::vec2& size)
	{
		return { .AnchorMin = { 0.0f, 0.0f }, .AnchorMax = { 0.0f, 0.0f }, .Pivot = { 0.0f, 0.0f }, .Position = position, .Size = size };
	}

	template<typename T>
	Entity AddWidget(Scene& scene, const std::string& name, const glm::vec2& position, const glm::vec2& size, T widget = {})
	{
		Entity entity = scene.CreateEntity(name);
		entity.AddComponent<UIRectComponent>(PlacedRect(position, size));
		entity.AddComponent<T>(std::move(widget));
		return entity;
	}

	void Press(Scene& scene, const glm::vec2& point)
	{
		Input::SetMousePosition(point);
		Input::SetMouseButtonState(MouseCode::Left, true);
		Frame(scene);
	}

	void Release(Scene& scene, const glm::vec2& point)
	{
		Input::SetMousePosition(point);
		Input::SetMouseButtonState(MouseCode::Left, false);
		Frame(scene);
	}

	void Click(Scene& scene, const glm::vec2& point)
	{
		Press(scene, point);
		Release(scene, point);
	}

	// Presses a key for one frame, then releases it for one.
	void Tap(Scene& scene, KeyCode key)
	{
		Input::SetKeyState(key, true);
		Frame(scene);
		Input::SetKeyState(key, false);
		Frame(scene);
	}

	void TapGamepad(Scene& scene, GamepadButton button)
	{
		REQUIRE(Input::SetGamepadButtonState(0, button, true));
		Frame(scene);
		REQUIRE(Input::SetGamepadButtonState(0, button, false));
		Frame(scene);
	}

	UIWidgetID FocusOf(Scene& scene)
	{
		REQUIRE(scene.GetUIInteraction());
		return scene.GetUIInteraction()->GetFocus();
	}

	UIWidgetID IDOf(Entity entity)
	{
		return UIWidgetID::ForEntity(entity.GetUUID());
	}

	UIWidgetState StateOf(Scene& scene, Entity widget)
	{
		const std::optional<UIWidgetInfo> info = UILayout::GetWidget(widget);
		REQUIRE(info);
		return scene.GetUIInteraction()->GetState(IDOf(widget), info->Interactable);
	}

	nlohmann::json Field(Scene& scene, Entity entity, const std::string& name)
	{
		return scene.GetScriptEngine()->GetInstanceField(entity, name).value_or(nlohmann::json());
	}

	// Records every widget callback as "<widget>:<method>[=<value>]" in self.Log.
	constexpr const char* RecorderScript = R"(
		local Recorder = {}
		local function Record(self, method, widget, value)
			local entry = widget.Name .. ":" .. method
			if value ~= nil then entry = entry .. "=" .. tostring(value) end
			table.insert(self.Log, entry)
		end
		function Recorder:OnCreate() self.Log = {} end
		function Recorder:OnClick(widget) Record(self, "OnClick", widget) end
		function Recorder:StartGame(widget) Record(self, "StartGame", widget) end
		function Recorder:OnValueChanged(widget, value) Record(self, "OnValueChanged", widget, value) end
		return Recorder
	)";

	std::vector<std::string> LogOf(Scene& scene, Entity recorder)
	{
		std::vector<std::string> log;
		for (const nlohmann::json& entry : Field(scene, recorder, "Log"))
			log.push_back(entry.get<std::string>());
		return log;
	}

	// Records gamepad connections as "+<index>" / "-<index>" in self.Log.
	constexpr const char* PadsScript = R"(
		local Pads = {}
		function Pads:OnCreate() self.Log = {} end
		function Pads:OnGamepadConnected(index) table.insert(self.Log, "+" .. index) end
		function Pads:OnGamepadDisconnected(index) table.insert(self.Log, "-" .. index) end
		return Pads
	)";

	// An entity with a script written into the project.
	Entity AddScripted(Scene& scene, const TempProject& project, const std::string& name, const std::string& path, const char* source)
	{
		Entity entity = scene.CreateEntity(name);
		entity.AddComponent<ScriptComponent>().Script = project.WriteFile(path, source);
		return entity;
	}

	Entity AddRecorder(Scene& scene, const TempProject& project, const std::string& name)
	{
		Entity entity = scene.CreateEntity(name);
		entity.AddComponent<ScriptComponent>().Script = project.WriteFile("Assets/Scripts/Recorder.lua", RecorderScript);
		return entity;
	}

	std::vector<UIDrawCommand> BuildCommands(Scene& scene)
	{
		std::vector<UIDrawCommand> commands;
		UILayout::BuildCommands(scene, GameUI::GetCanvasSize(), commands);
		return commands;
	}

}

TEST_SUITE("UIWidgets")
{
	TEST_CASE("Widget component rules reject values the widgets cannot use")
	{
		Scene scene;
		Entity entity = scene.CreateEntity();
		const auto expectError = [&](const char* component, const nlohmann::json& data, const char* fragment) {
			std::string error;
			CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, component, data, error));
			CHECK_MESSAGE(Contains(error, fragment), component, " ", data.dump(), " -> ", error);
		};
		expectError("UISlider", { { "Min", 1.0 }, { "Max", 1.0 } }, "Max must be greater than Min");
		expectError("UISlider", { { "Min", 2.0 } }, "Max must be greater than Min");
		expectError("UISlider", { { "Value", 1.5 } }, "Value must be between 0 and 1");
		expectError("UISlider", { { "Step", -0.5 } }, "Step must be between");
		expectError("UISlider", { { "Max", 2e6 } }, "Max must be between");
		expectError("UISlider", { { "HandleWidth", -1.0 } }, "HandleWidth must be between");
		expectError("UISlider", { { "FillColor", { 0.0, 0.0, 2.0, 1.0 } } }, "FillColor.b must be between 0 and 1");
		expectError("UIButton", { { "HighlightTint", { 0.0, 0.0, 0.0, -1.0 } } }, "HighlightTint.a must be between 0 and 1");
		expectError("UIButton", { { "OnClick", "2Fast" } }, "OnClick must be the name of a script method");
		expectError("UIButton", { { "OnClick", "end" } }, "OnClick must be the name of a script method");
		expectError("UIButton", { { "OnClick", "Start Game" } }, "OnClick must be the name of a script method");
		expectError("UIButton", { { "Target", "Menu" } }, "Target");
		expectError("UICheckbox", { { "OnValueChanged", "a-b" } }, "OnValueChanged must be the name of a script method");
		expectError("UICheckbox", { { "CheckColor", { 0.0, 0.0, 0.0 } } }, "array of 4 numbers");
		CHECK_FALSE(entity.HasComponent<UISliderComponent>());
		CHECK_FALSE(entity.HasComponent<UIButtonComponent>());
		CHECK_FALSE(entity.HasComponent<UICheckboxComponent>());

		// Raising Max first, then Min, works within one patch too: the rules see the patched values together.
		std::string error;
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, "UISlider", { { "Min", 10.0 }, { "Max", 20.0 }, { "Value", 15.0 }, { "Step", 0.5 } }, error), error);
		CHECK(entity.GetComponent<UISliderComponent>().Value == 15.0f);
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, "UIButton", { { "OnClick", "" } }, error), error);
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, "UICheckbox", { { "OnValueChanged", "_Toggle2" } }, error), error);

		CHECK(IsValidUICallbackName("OnClick"));
		CHECK(IsValidUICallbackName("_"));
		CHECK_FALSE(IsValidUICallbackName(""));
		CHECK_FALSE(IsValidUICallbackName("while"));
		CHECK_FALSE(IsValidUICallbackName(std::string(MaxUICallbackNameLength + 1, 'a')));
		CHECK(IsValidUICallbackName(std::string(MaxUICallbackNameLength, 'a')));

		// Sanitizing gives what validation accepts: an empty range opens upwards (downwards at the top of the
		// range), Value is clamped into it, and a bad callback name calls nothing.
		UISliderComponent slider{ .Value = 50.0f, .Min = 3.0f, .Max = 3.0f, .Step = -1.0f };
		UISliderComponent repaired = SanitizeUISlider(slider);
		CHECK(ValidateUISlider(repaired).empty());
		CHECK(repaired.Min == 3.0f);
		CHECK(repaired.Max == 4.0f);
		CHECK(repaired.Value == 4.0f);
		CHECK(repaired.Step == 0.0f);
		slider = { .Value = 0.0f, .Min = UIRectComponent::MaxMagnitude, .Max = -UIRectComponent::MaxMagnitude };
		repaired = SanitizeUISlider(slider);
		CHECK(ValidateUISlider(repaired).empty());
		CHECK(repaired.Max == UIRectComponent::MaxMagnitude);
		CHECK(repaired.Min == UIRectComponent::MaxMagnitude - 1.0f);
		UIButtonComponent button{ .OnClick = "not a name" };
		CHECK(SanitizeUIButton(button).OnClick.empty());
	}

	TEST_CASE("Slider values snap to steps and map to the handle's travel")
	{
		CHECK(UIWidgets::SnapSliderValue(0.26f, 0.0f, 1.0f, 0.25f) == 0.25f);
		CHECK(UIWidgets::SnapSliderValue(0.38f, 0.0f, 1.0f, 0.25f) == 0.5f);
		CHECK(UIWidgets::SnapSliderValue(7.0f, 1.0f, 10.0f, 4.0f) == 9.0f);
		// The last step may be shorter: Max stays reachable.
		CHECK(UIWidgets::SnapSliderValue(9.9f, 1.0f, 10.0f, 4.0f) == 10.0f);
		CHECK(UIWidgets::SnapSliderValue(-3.0f, 0.0f, 1.0f, 0.0f) == 0.0f);
		CHECK(UIWidgets::SnapSliderValue(0.3f, 0.0f, 1.0f, 1e-38f) == doctest::Approx(0.3f));
		// Navigation steps from stop to stop, reaching every stop both ways, even when the range is not a whole
		// number of steps (0, 0.3, 0.6, 0.9, 1).
		CHECK(UIWidgets::StepSliderValue(1.0f, -1, 0.0f, 1.0f, 0.3f) == doctest::Approx(0.9f));
		CHECK(UIWidgets::StepSliderValue(0.9f, 1, 0.0f, 1.0f, 0.3f) == 1.0f);
		CHECK(UIWidgets::StepSliderValue(0.9f, -1, 0.0f, 1.0f, 0.3f) == doctest::Approx(0.6f));
		CHECK(UIWidgets::StepSliderValue(0.0f, -1, 0.0f, 1.0f, 0.3f) == 0.0f);
		CHECK(UIWidgets::StepSliderValue(0.0f, 9, 0.0f, 1.0f, 0.3f) == 1.0f);
		// From between stops: to the neighbour of the nearest stop.
		CHECK(UIWidgets::StepSliderValue(0.4f, 1, 0.0f, 1.0f, 0.3f) == doctest::Approx(0.6f));
		// Exact steps (0.1 is not exact in binary) still end on Max.
		CHECK(UIWidgets::StepSliderValue(0.9f, 1, 0.0f, 1.0f, 0.1f) == 1.0f);
		CHECK(UIWidgets::StepSliderValue(2.0f, 3, -10.0f, 10.0f, 2.0f) == 8.0f);
		// Continuous: a twentieth of the range.
		CHECK(UIWidgets::StepSliderValue(5.0f, 1, 0.0f, 10.0f, 0.0f) == 5.5f);
		CHECK(UIWidgets::StepSliderValue(9.9f, 2, 0.0f, 10.0f, 0.0f) == 10.0f);
		// More stops than a float tells apart: steps by value.
		CHECK(UIWidgets::StepSliderValue(0.0f, 1, 0.0f, 1e6f, 1e-3f) == doctest::Approx(1e-3f));
		CHECK(UIWidgets::SliderValueAtFraction(0.5f, 0.0f, 10.0f, 3.0f) == 6.0f);
		CHECK(UIWidgets::SliderValueAtFraction(2.0f, 0.0f, 10.0f, 0.0f) == 10.0f);
		CHECK(UIWidgets::SliderFraction(5.0f, 0.0f, 10.0f) == 0.5f);
		CHECK(UIWidgets::SliderFraction(5.0f, 1.0f, 1.0f) == 0.0f);

		const UIBox rect{ { 100.0f, 50.0f }, { 420.0f, 40.0f } };
		CHECK(UIWidgets::SliderHandleBox(rect, 0.0f, 20.0f) == UIBox{ { 100.0f, 50.0f }, { 20.0f, 40.0f } });
		CHECK(UIWidgets::SliderHandleBox(rect, 1.0f, 20.0f) == UIBox{ { 500.0f, 50.0f }, { 20.0f, 40.0f } });
		CHECK(UIWidgets::SliderFractionAt(rect, 20.0f, 310.0f) == 0.5f);
		CHECK(UIWidgets::SliderFractionAt(rect, 20.0f, 0.0f) == 0.0f);
		CHECK(UIWidgets::SliderFractionAt(rect, 20.0f, 9000.0f) == 1.0f);
		// A handle as wide as the rect cannot travel: the pointer's half decides.
		CHECK(UIWidgets::SliderHandleBox(rect, 0.7f, 1000.0f) == rect);
		CHECK(UIWidgets::SliderFractionAt(rect, 1000.0f, 309.0f) == 0.0f);
		CHECK(UIWidgets::SliderFractionAt(rect, 1000.0f, 311.0f) == 1.0f);

		const UIBox row{ { 10.0f, 20.0f }, { 300.0f, 40.0f } };
		CHECK(UIWidgets::CheckboxBox(row) == UIBox{ { 10.0f, 20.0f }, { 40.0f, 40.0f } });
		CHECK(UIWidgets::CheckboxMark(UIWidgets::CheckboxBox(row)) == UIBox{ { 20.0f, 30.0f }, { 20.0f, 20.0f } });
		CHECK(UIWidgets::CheckboxLabelBox(row) == UIBox{ { 60.0f, 20.0f }, { 250.0f, 40.0f } });
		// A rect narrower than tall: the box is as wide as the rect, and the label has no room.
		const UIBox narrow{ { 0.0f, 0.0f }, { 20.0f, 60.0f } };
		CHECK(UIWidgets::CheckboxBox(narrow) == UIBox{ { 0.0f, 20.0f }, { 20.0f, 20.0f } });
		CHECK(UIWidgets::CheckboxLabelBox(narrow).Size.x == 0.0f);
	}

	TEST_CASE("A button calls its target's method when released over it")
	{
		WidgetInputScope input;
		TempProject project("UIButtonClick");
		Scene scene;
		Entity menu = AddRecorder(scene, project, "Menu");
		Entity play = AddWidget<UIButtonComponent>(scene, "Play", { 100.0f, 100.0f }, { 200.0f, 80.0f }, { .Target = menu.GetUUID(), .OnClick = "StartGame" });
		Entity self = AddWidget<UIButtonComponent>(scene, "Self", { 100.0f, 300.0f }, { 200.0f, 80.0f });
		self.AddComponent<ScriptComponent>().Script = "Assets/Scripts/Recorder.lua";
		AddWidget<UIButtonComponent>(scene, "Silent", { 100.0f, 500.0f }, { 200.0f, 80.0f }, { .Target = menu.GetUUID(), .OnClick = "" });
		scene.OnRuntimeStart();
		REQUIRE(scene.GetUIInteraction());

		Press(scene, { 150.0f, 120.0f });
		CHECK(LogOf(scene, menu).empty());
		CHECK(StateOf(scene, play) == UIWidgetState::Pressed);
		CHECK(scene.GetUIInteraction()->IsPointerOverWidget());
		Release(scene, { 160.0f, 130.0f });
		CHECK(LogOf(scene, menu) == std::vector<std::string>{ "Play:StartGame" });
		// A click does not focus (Space and Enter stay the game's); the pointer still over it highlights it.
		CHECK_FALSE(FocusOf(scene).IsValid());
		CHECK(StateOf(scene, play) == UIWidgetState::Highlighted);

		// Released elsewhere: no click (dragging off a button cancels it). Pressed elsewhere: no click either.
		Press(scene, { 150.0f, 120.0f });
		Release(scene, { 150.0f, 900.0f });
		Press(scene, { 900.0f, 120.0f });
		Release(scene, { 150.0f, 120.0f });
		CHECK(LogOf(scene, menu).size() == 1);
		CHECK(scene.GetUIInteraction()->IsPointerOverWidget());

		// Target 0 is the button itself; an empty method name calls nothing.
		Click(scene, { 150.0f, 320.0f });
		CHECK(LogOf(scene, self) == std::vector<std::string>{ "Self:OnClick" });
		Click(scene, { 150.0f, 520.0f });
		CHECK(LogOf(scene, menu).size() == 1);
		CHECK(scene.GetScriptEngine()->GetErrors().empty());

		// A disabled button neither clicks nor highlights, and it is not "under the pointer" for the game.
		play.GetComponent<UIButtonComponent>().Interactable = false;
		Click(scene, { 150.0f, 120.0f });
		CHECK(LogOf(scene, menu).size() == 1);
		CHECK_FALSE(scene.GetUIInteraction()->IsPointerOverWidget());
		CHECK(StateOf(scene, play) == UIWidgetState::Disabled);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A callback that cannot run is reported once per widget and method")
	{
		WidgetInputScope input;
		TempProject project("UIButtonMissing");
		Scene scene;
		Entity menu = AddRecorder(scene, project, "Menu");
		Entity plain = scene.CreateEntity("Plain");
		AddWidget<UIButtonComponent>(scene, "Typo", { 0.0f, 0.0f }, { 100.0f, 100.0f }, { .Target = menu.GetUUID(), .OnClick = "StartGaem" });
		AddWidget<UIButtonComponent>(scene, "NoScript", { 200.0f, 0.0f }, { 100.0f, 100.0f }, { .Target = plain.GetUUID() });
		AddWidget<UIButtonComponent>(scene, "Gone", { 400.0f, 0.0f }, { 100.0f, 100.0f }, { .Target = UUID(12345) });
		scene.OnRuntimeStart();

		for (int i = 0; i < 2; i++)
		{
			Click(scene, { 50.0f, 50.0f });
			Click(scene, { 250.0f, 50.0f });
			Click(scene, { 450.0f, 50.0f });
		}
		const std::vector<std::string>& errors = scene.GetScriptEngine()->GetErrors();
		REQUIRE(errors.size() == 3);
		CHECK(Contains(errors[0], "UI widget 'Typo' cannot call StartGaem: the script of 'Menu' has no method StartGaem"));
		CHECK(Contains(errors[1], "UI widget 'NoScript' cannot call OnClick: 'Plain' has no running script"));
		CHECK(Contains(errors[2], "UI widget 'Gone' cannot call OnClick: its Target entity 12345 does not exist"));
		CHECK(LogOf(scene, menu).empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Dragging a slider follows the pointer, snaps to steps and reports each change")
	{
		WidgetInputScope input;
		TempProject project("UISliderDrag");
		Scene scene;
		Entity menu = AddRecorder(scene, project, "Menu");
		Entity volume = AddWidget<UISliderComponent>(scene, "Volume", { 100.0f, 100.0f }, { 420.0f, 40.0f }, { .Value = 2.0f, .Min = 0.0f, .Max = 10.0f, .Step = 1.0f, .HandleWidth = 20.0f, .Target = menu.GetUUID() });
		scene.OnRuntimeStart();

		// Handle center travels from x = 110 to x = 510.
		Press(scene, { 312.0f, 120.0f });
		CHECK(volume.GetComponent<UISliderComponent>().Value == 5.0f);
		// Holding still reports nothing; dragging off the rect keeps the drag (and the pressed look).
		Frame(scene, 3);
		Input::SetMousePosition({ 9000.0f, 700.0f });
		Frame(scene);
		CHECK(volume.GetComponent<UISliderComponent>().Value == 10.0f);
		CHECK(StateOf(scene, volume) == UIWidgetState::Pressed);
		Input::SetMousePosition({ 151.0f, 700.0f });
		Frame(scene);
		Release(scene, { 151.0f, 700.0f });
		CHECK(volume.GetComponent<UISliderComponent>().Value == 1.0f);
		CHECK(LogOf(scene, menu) == std::vector<std::string>{ "Volume:OnValueChanged=5.0", "Volume:OnValueChanged=10.0", "Volume:OnValueChanged=1.0" });

		// Setting the value from code is not a user interaction: no callback.
		std::string error;
		REQUIRE(ComponentRegistry::AddOrPatch(volume, "UISlider", { { "Value", 7.0 } }, error));
		Frame(scene);
		CHECK(LogOf(scene, menu).size() == 3);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Clicking a checkbox toggles it; its label sits beside the box")
	{
		WidgetInputScope input;
		TempProject project("UICheckboxToggle");
		Scene scene;
		Entity menu = AddRecorder(scene, project, "Menu");
		Entity music = AddWidget<UICheckboxComponent>(scene, "Music", { 100.0f, 100.0f }, { 300.0f, 40.0f }, { .Target = menu.GetUUID() });
		music.AddComponent<UITextComponent>(UITextComponent{ .Text = "Music" });
		scene.OnRuntimeStart();

		// The label is part of the widget: clicking it toggles too.
		Click(scene, { 300.0f, 120.0f });
		CHECK(music.GetComponent<UICheckboxComponent>().Checked);
		Click(scene, { 110.0f, 110.0f });
		CHECK_FALSE(music.GetComponent<UICheckboxComponent>().Checked);
		CHECK(LogOf(scene, menu) == std::vector<std::string>{ "Music:OnValueChanged=true", "Music:OnValueChanged=false" });

		music.GetComponent<UICheckboxComponent>().Checked = true;
		Input::SetMousePosition({ -100.0f, -100.0f });
		Frame(scene);
		const std::vector<UIDrawCommand> commands = BuildCommands(scene);
		REQUIRE(commands.size() == 3);
		const UICheckboxComponent defaults;
		CHECK(commands[0].Kind == UIDrawKind::Rect);
		CHECK(ResolveUIBox(commands[0], GameUI::GetCanvasSize()) == UIBox{ { 100.0f, 100.0f }, { 40.0f, 40.0f } });
		// Neither hovered nor focused (clicks do not focus): normal.
		CHECK(commands[0].Color == defaults.BoxColor * defaults.NormalTint);
		CHECK(ResolveUIBox(commands[1], GameUI::GetCanvasSize()) == UIBox{ { 110.0f, 110.0f }, { 20.0f, 20.0f } });
		CHECK(commands[1].Color == defaults.CheckColor);
		CHECK(commands[2].Kind == UIDrawKind::Text);
		CHECK(commands[2].Position == glm::vec2(150.0f, 100.0f));
		CHECK(commands[2].Size == glm::vec2(250.0f, 40.0f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Widgets draw in the tint of their state")
	{
		WidgetInputScope input;
		Scene scene;
		const UIButtonComponent tints{ .NormalTint = { 0.5f, 0.5f, 0.5f, 1.0f }, .HighlightTint = { 1.0f, 1.0f, 1.0f, 1.0f }, .PressedTint = { 0.25f, 0.25f, 0.25f, 1.0f }, .DisabledTint = { 1.0f, 1.0f, 1.0f, 0.5f } };
		Entity button = AddWidget<UIButtonComponent>(scene, "Button", { 0.0f, 0.0f }, { 100.0f, 100.0f }, tints);
		button.AddComponent<UIImageComponent>(UIImageComponent{ .Color = { 0.8f, 0.4f, 0.2f, 1.0f } });
		Entity slider = AddWidget<UISliderComponent>(scene, "Slider", { 0.0f, 200.0f }, { 220.0f, 20.0f }, { .Value = 0.25f, .HandleWidth = 20.0f });
		const auto imageColor = [&] { return BuildCommands(scene)[0].Color; };

		// Editing: no interaction, every widget in its normal (or disabled) look.
		CHECK(imageColor() == glm::vec4(0.4f, 0.2f, 0.1f, 1.0f));
		std::vector<UIDrawCommand> commands = BuildCommands(scene);
		REQUIRE(commands.size() == 4);
		const UISliderComponent sliderDefaults;
		CHECK(commands[1].Color == sliderDefaults.TrackColor);
		CHECK(ResolveUIBox(commands[1], GameUI::GetCanvasSize()) == UIBox{ { 0.0f, 200.0f }, { 220.0f, 20.0f } });
		CHECK(commands[2].Color == sliderDefaults.FillColor);
		// The fill reaches the handle's center: a quarter of the way along the 200-unit travel, plus half a handle.
		CHECK(commands[2].Size == glm::vec2(60.0f, 20.0f));
		CHECK(commands[3].Color == sliderDefaults.HandleColor * sliderDefaults.NormalTint);
		CHECK(ResolveUIBox(commands[3], GameUI::GetCanvasSize()) == UIBox{ { 50.0f, 200.0f }, { 20.0f, 20.0f } });

		scene.OnRuntimeStart();
		Frame(scene);
		CHECK(imageColor() == glm::vec4(0.4f, 0.2f, 0.1f, 1.0f));
		Input::SetMousePosition({ 50.0f, 50.0f });
		Frame(scene);
		CHECK(imageColor() == glm::vec4(0.8f, 0.4f, 0.2f, 1.0f));
		Input::SetMouseButtonState(MouseCode::Left, true);
		Frame(scene);
		CHECK(imageColor() == glm::vec4(0.2f, 0.1f, 0.05f, 1.0f));
		// Held, but the pointer moved off: no longer pressed or hovered (and a press does not focus).
		Input::SetMousePosition({ 500.0f, 500.0f });
		Frame(scene);
		CHECK(imageColor() == glm::vec4(0.4f, 0.2f, 0.1f, 1.0f));
		// Focused by navigation: highlighted.
		scene.GetUIInteraction()->SetFocus(IDOf(button));
		Frame(scene);
		CHECK(imageColor() == glm::vec4(0.8f, 0.4f, 0.2f, 1.0f));
		Input::SetMouseButtonState(MouseCode::Left, false);
		Frame(scene);
		button.GetComponent<UIButtonComponent>().Interactable = false;
		Frame(scene);
		CHECK(imageColor() == glm::vec4(0.8f, 0.4f, 0.2f, 0.5f));
		CHECK_FALSE(FocusOf(scene).IsValid());

		// An entity with several widget components acts (and draws) as the first of button, slider, checkbox, and
		// says so once.
		slider.AddComponent<UIButtonComponent>();
		CHECK(UILayout::GetWidgetKind(slider) == UIWidgetKind::Button);
		CHECK(UILayout::CountWidgetComponents(slider) == 2);
		const uint64_t since = Log::GetHistory().GetTotalCount();
		commands = BuildCommands(scene);
		Frame(scene, 2);
		CHECK(commands.size() == 1);
		CHECK(CountLogMessages(since, "'Slider' has several widget components; it acts as its UIButton only") == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Arrows and Tab move the focus between widgets")
	{
		WidgetInputScope input;
		Scene scene;
		// A 2 x 2 grid, then a widget far down and to the right.
		Entity a = AddWidget<UIButtonComponent>(scene, "A", { 100.0f, 100.0f }, { 200.0f, 80.0f });
		Entity b = AddWidget<UIButtonComponent>(scene, "B", { 400.0f, 100.0f }, { 200.0f, 80.0f });
		Entity c = AddWidget<UICheckboxComponent>(scene, "C", { 100.0f, 300.0f }, { 200.0f, 80.0f });
		Entity d = AddWidget<UIButtonComponent>(scene, "D", { 400.0f, 300.0f }, { 200.0f, 80.0f });
		Entity e = AddWidget<UIButtonComponent>(scene, "E", { 1500.0f, 900.0f }, { 200.0f, 80.0f });
		scene.OnRuntimeStart();

		Frame(scene);
		CHECK_FALSE(FocusOf(scene).IsValid());
		// The first move only shows the focus, on the first widget.
		Tap(scene, KeyCode::Down);
		CHECK(FocusOf(scene) == IDOf(a));
		CHECK(StateOf(scene, a) == UIWidgetState::Highlighted);
		Tap(scene, KeyCode::Right);
		CHECK(FocusOf(scene) == IDOf(b));
		Tap(scene, KeyCode::Down);
		CHECK(FocusOf(scene) == IDOf(d));
		Tap(scene, KeyCode::Left);
		CHECK(FocusOf(scene) == IDOf(c));
		Tap(scene, KeyCode::Up);
		CHECK(FocusOf(scene) == IDOf(a));
		// Nothing above: the focus stays.
		Tap(scene, KeyCode::Up);
		CHECK(FocusOf(scene) == IDOf(a));
		// From D, Down reaches the far-right E: nothing else is below.
		scene.GetUIInteraction()->SetFocus(IDOf(d));
		Tap(scene, KeyCode::Down);
		CHECK(FocusOf(scene) == IDOf(e));

		// Tab cycles in draw order and wraps; Shift+Tab goes back.
		Tap(scene, KeyCode::Tab);
		CHECK(FocusOf(scene) == IDOf(a));
		Tap(scene, KeyCode::Tab);
		CHECK(FocusOf(scene) == IDOf(b));
		Input::SetKeyState(KeyCode::LeftShift, true);
		Tap(scene, KeyCode::Tab);
		Tap(scene, KeyCode::Tab);
		Input::SetKeyState(KeyCode::LeftShift, false);
		CHECK(FocusOf(scene) == IDOf(e));

		// Disabled and hidden widgets are skipped, and lose the focus.
		b.GetComponent<UIButtonComponent>().Interactable = false;
		c.GetComponent<UIRectComponent>().Visible = false;
		scene.GetUIInteraction()->SetFocus(IDOf(a));
		Tap(scene, KeyCode::Right);
		CHECK(FocusOf(scene) == IDOf(d));
		Tap(scene, KeyCode::Tab);
		CHECK(FocusOf(scene) == IDOf(e));
		Tap(scene, KeyCode::Tab);
		CHECK(FocusOf(scene) == IDOf(a));
		// A focus set on a hidden widget gets one frame to appear (a widget may be shown as it is focused), then
		// is dropped with a warning: it never was a widget that could take focus.
		const uint64_t since = Log::GetHistory().GetTotalCount();
		scene.GetUIInteraction()->SetFocus(IDOf(c));
		Frame(scene);
		CHECK(FocusOf(scene) == IDOf(c));
		Frame(scene);
		CHECK_FALSE(FocusOf(scene).IsValid());
		CHECK(CountLogMessages(since, "is not a visible, interactable widget; dropped it") == 1);
		// A focused widget that goes away loses the focus at once, without a warning.
		scene.GetUIInteraction()->SetFocus(IDOf(d));
		Frame(scene);
		scene.DestroyEntity(d);
		Frame(scene);
		CHECK_FALSE(FocusOf(scene).IsValid());
		CHECK(CountLogMessages(since, "is not a visible, interactable widget; dropped it") == 1);

		// Navigation off (the editor's view does not have focus): keys do nothing.
		GameUI::SetNavigationEnabled(false);
		Tap(scene, KeyCode::Down);
		Tap(scene, KeyCode::Tab);
		CHECK_FALSE(FocusOf(scene).IsValid());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Sideways distance counts double when choosing the next widget")
	{
		WidgetInputScope input;
		Scene scene;
		// Below Top: Straight is 500 units straight down (score 500); Near is 100 down and 300 to the side
		// (score 100 + 2 x 300 = 700). Counting sideways distance once would pick Near (400).
		Entity top = AddWidget<UIButtonComponent>(scene, "Top", { 100.0f, 100.0f }, { 100.0f, 40.0f });
		Entity straight = AddWidget<UIButtonComponent>(scene, "Straight", { 100.0f, 600.0f }, { 100.0f, 40.0f });
		Entity near = AddWidget<UIButtonComponent>(scene, "Near", { 400.0f, 200.0f }, { 100.0f, 40.0f });
		scene.OnRuntimeStart();
		scene.GetUIInteraction()->SetFocus(IDOf(top));
		Tap(scene, KeyCode::Down);
		CHECK(FocusOf(scene) == IDOf(straight));
		// From Straight, Up: Top (500, score 500) against Near (400 up, 300 sideways: 1000).
		Tap(scene, KeyCode::Up);
		CHECK(FocusOf(scene) == IDOf(top));
		Tap(scene, KeyCode::Right);
		CHECK(FocusOf(scene) == IDOf(near));
		scene.OnRuntimeStop();
	}

	TEST_CASE("A held direction repeats after a delay, at most once per frame")
	{
		WidgetInputScope input;
		Scene scene;
		std::vector<Entity> column;
		column.reserve(12);
		for (int i = 0; i < 12; i++)
			column.push_back(AddWidget<UIButtonComponent>(scene, "Item" + std::to_string(i), { 100.0f, 50.0f + 80.0f * static_cast<float>(i) }, { 200.0f, 60.0f }));
		scene.OnRuntimeStart();
		scene.GetUIInteraction()->SetFocus(IDOf(column[0]));
		const auto focusIndex = [&] {
			const auto it = std::ranges::find_if(column, [&](Entity entity) { return IDOf(entity) == FocusOf(scene); });
			return static_cast<int>(it - column.begin());
		};

		// 1/64 s frames are exact in binary: the move happens on the first frame (t = 0), repeats once
		// t >= RepeatDelay (0.4 s: frame 26, t = 0.40625), then every RepeatInterval (due at t = 0.5: frame 32;
		// due at t = 0.6: frame 39, t = 0.609).
		constexpr float Dt = 1.0f / 64.0f;
		Input::SetKeyState(KeyCode::Down, true);
		Frame(scene, 1, Dt);
		CHECK(focusIndex() == 1);
		Frame(scene, 25, Dt);
		CHECK(focusIndex() == 1);
		Frame(scene, 1, Dt);
		CHECK(focusIndex() == 2);
		Frame(scene, 5, Dt);
		CHECK(focusIndex() == 2);
		Frame(scene, 1, Dt);
		CHECK(focusIndex() == 3);
		Frame(scene, 6, Dt);
		CHECK(focusIndex() == 3);
		Frame(scene, 1, Dt);
		CHECK(focusIndex() == 4);
		// A long frame repeats only once, and the next repeat is an interval after it, not a burst of the
		// repeats the long frame skipped.
		Frame(scene, 1, 2.0f);
		CHECK(focusIndex() == 5);
		Frame(scene, 6, Dt);
		CHECK(focusIndex() == 5);
		Frame(scene, 1, Dt);
		CHECK(focusIndex() == 6);
		// Released and pressed again: moves at once.
		Input::SetKeyState(KeyCode::Down, false);
		Frame(scene, 1, Dt);
		Input::SetKeyState(KeyCode::Down, true);
		Frame(scene, 1, Dt);
		CHECK(focusIndex() == 7);
		Input::SetKeyState(KeyCode::Down, false);
		Frame(scene, 1, Dt);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Keys and gamepads held when the game gets focus back neither navigate nor activate")
	{
		WidgetInputScope input;
		TempProject project("UIHeldIntoFocus");
		Scene scene;
		Entity menu = AddRecorder(scene, project, "Menu");
		Entity top = AddWidget<UIButtonComponent>(scene, "Top", { 100.0f, 100.0f }, { 200.0f, 80.0f }, { .Target = menu.GetUUID() });
		AddWidget<UIButtonComponent>(scene, "Bottom", { 100.0f, 300.0f }, { 200.0f, 80.0f }, { .Target = menu.GetUUID() });
		scene.OnRuntimeStart();
		scene.GetUIInteraction()->SetFocus(IDOf(top));
		// Held in another editor panel, then the Game view gets focus with them still down.
		Input::SetGameFocus(false);
		Input::SetKeyState(KeyCode::Down, true);
		Input::SetKeyState(KeyCode::Enter, true);
		Input::SetGamepadConnected(0, true);
		REQUIRE(Input::SetGamepadButtonState(0, GamepadButton::DPadDown, true));
		REQUIRE(Input::SetGamepadButtonState(0, GamepadButton::A, true));
		Frame(scene);
		Input::SetGameFocus(true);
		// Past the repeat delay: nothing moves and nothing is clicked.
		Frame(scene, 40, 1.0f / 64.0f);
		CHECK(FocusOf(scene) == IDOf(top));
		CHECK(LogOf(scene, menu).empty());
		// Let go and pressed again: the game's.
		Input::SetKeyState(KeyCode::Down, false);
		Frame(scene);
		Input::SetKeyState(KeyCode::Down, true);
		Frame(scene);
		CHECK(FocusOf(scene) != IDOf(top));
		Input::SetKeyState(KeyCode::Down, false);
		Input::SetKeyState(KeyCode::Enter, false);
		Frame(scene);
		// Enter pressed elsewhere on the very frame the game gets focus (the press still reads as new to the host):
		// not the game's.
		Input::SetGameFocus(false);
		Input::SetKeyState(KeyCode::Enter, true);
		Input::SetGameFocus(true);
		Frame(scene);
		CHECK(LogOf(scene, menu).empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("A mouse button held when the game gets the pointer back neither presses nor clicks a widget")
	{
		WidgetInputScope input;
		TempProject project("UIHeldPointer");
		Scene scene;
		Entity menu = AddRecorder(scene, project, "Menu");
		Entity button = AddWidget<UIButtonComponent>(scene, "Button", { 100.0f, 100.0f }, { 200.0f, 80.0f }, { .Target = menu.GetUUID() });
		scene.OnRuntimeStart();
		Input::SetMousePosition({ 200.0f, 140.0f });
		// Pressed while the pointer was the host's (a replay's view without it, then the press, then a view with it, all
		// on one frame) ...
		Input::SetGamePointer(false);
		Input::SetMouseButtonState(MouseCode::Left, true);
		Input::SetGamePointer(true);
		Frame(scene);
		// ... and let go over the button.
		CHECK(StateOf(scene, button) != UIWidgetState::Pressed);
		Frame(scene);
		CHECK(StateOf(scene, button) != UIWidgetState::Pressed);
		Input::SetMouseButtonState(MouseCode::Left, false);
		Frame(scene);
		CHECK(LogOf(scene, menu).empty());
		// A new click is the game's.
		Input::SetMouseButtonState(MouseCode::Left, true);
		Frame(scene);
		Input::SetMouseButtonState(MouseCode::Left, false);
		Frame(scene);
		CHECK(LogOf(scene, menu) == std::vector<std::string>{ "Button:OnClick" });
		scene.OnRuntimeStop();
	}

	TEST_CASE("Enter, Space and gamepad A activate the focused widget")
	{
		WidgetInputScope input;
		TempProject project("UIAccept");
		Scene scene;
		Entity menu = AddRecorder(scene, project, "Menu");
		Entity play = AddWidget<UIButtonComponent>(scene, "Play", { 100.0f, 100.0f }, { 200.0f, 80.0f }, { .Target = menu.GetUUID() });
		Entity music = AddWidget<UICheckboxComponent>(scene, "Music", { 100.0f, 300.0f }, { 200.0f, 40.0f }, { .Target = menu.GetUUID() });
		scene.OnRuntimeStart();

		// Nothing focused: accept does nothing.
		Tap(scene, KeyCode::Enter);
		CHECK(LogOf(scene, menu).empty());
		scene.GetUIInteraction()->SetFocus(IDOf(play));
		Input::SetKeyState(KeyCode::Enter, true);
		Frame(scene);
		CHECK(StateOf(scene, play) == UIWidgetState::Pressed);
		// Activated on the press, once.
		Frame(scene, 3);
		Input::SetKeyState(KeyCode::Enter, false);
		Frame(scene);
		CHECK(StateOf(scene, play) == UIWidgetState::Highlighted);
		Tap(scene, KeyCode::KPEnter);
		CHECK(LogOf(scene, menu) == std::vector<std::string>{ "Play:OnClick", "Play:OnClick" });

		Tap(scene, KeyCode::Down);
		CHECK(FocusOf(scene) == IDOf(music));
		Tap(scene, KeyCode::Space);
		CHECK(music.GetComponent<UICheckboxComponent>().Checked);

		// Gamepads: the D-pad and the left stick navigate, A activates. A stick pushed less than the threshold
		// does nothing.
		Input::SetGamepadConnected(0, true);
		TapGamepad(scene, GamepadButton::A);
		CHECK_FALSE(music.GetComponent<UICheckboxComponent>().Checked);
		TapGamepad(scene, GamepadButton::DPadUp);
		CHECK(FocusOf(scene) == IDOf(play));
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftY, 0.4f));
		Frame(scene);
		CHECK(FocusOf(scene) == IDOf(play));
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftY, 0.6f));
		Frame(scene);
		CHECK(FocusOf(scene) == IDOf(music));
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftY, 0.0f));
		Frame(scene);
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftY, -1.0f));
		Frame(scene);
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftY, 0.0f));
		CHECK(FocusOf(scene) == IDOf(play));
		TapGamepad(scene, GamepadButton::A);
		CHECK(LogOf(scene, menu) == std::vector<std::string>{ "Play:OnClick", "Play:OnClick", "Music:OnValueChanged=true", "Music:OnValueChanged=false", "Play:OnClick" });
		scene.OnRuntimeStop();
	}

	TEST_CASE("A focused slider moves with Left and Right; Up and Down still navigate")
	{
		WidgetInputScope input;
		Scene scene;
		Entity top = AddWidget<UIButtonComponent>(scene, "Top", { 100.0f, 100.0f }, { 400.0f, 40.0f });
		Entity stepped = AddWidget<UISliderComponent>(scene, "Stepped", { 100.0f, 200.0f }, { 400.0f, 40.0f }, { .Value = 4.0f, .Min = 0.0f, .Max = 10.0f, .Step = 2.0f });
		Entity continuous = AddWidget<UISliderComponent>(scene, "Continuous", { 100.0f, 300.0f }, { 400.0f, 40.0f }, { .Value = 0.5f });
		scene.OnRuntimeStart();
		scene.GetUIInteraction()->SetFocus(IDOf(stepped));

		Tap(scene, KeyCode::Right);
		CHECK(stepped.GetComponent<UISliderComponent>().Value == 6.0f);
		Tap(scene, KeyCode::Left);
		Tap(scene, KeyCode::Left);
		Tap(scene, KeyCode::Left);
		Tap(scene, KeyCode::Left);
		CHECK(stepped.GetComponent<UISliderComponent>().Value == 0.0f);
		// Accept does nothing on a slider.
		Tap(scene, KeyCode::Enter);
		CHECK(stepped.GetComponent<UISliderComponent>().Value == 0.0f);
		Tap(scene, KeyCode::Down);
		CHECK(FocusOf(scene) == IDOf(continuous));
		Input::SetGamepadConnected(0, true);
		TapGamepad(scene, GamepadButton::DPadRight);
		CHECK(continuous.GetComponent<UISliderComponent>().Value == doctest::Approx(0.55f));
		Tap(scene, KeyCode::Up);
		Tap(scene, KeyCode::Up);
		CHECK(FocusOf(scene) == IDOf(top));
		scene.OnRuntimeStop();
	}

	TEST_CASE("The host maps the mouse onto the canvas and can turn pointer input off")
	{
		WidgetInputScope input;
		TempProject project("UIPointerMapping");
		Scene scene;
		Entity menu = AddRecorder(scene, project, "Menu");
		Entity button = AddWidget<UIButtonComponent>(scene, "Button", { 1000.0f, 500.0f }, { 100.0f, 100.0f }, { .Target = menu.GetUUID() });
		// A topmost disabled widget covers the button's left half.
		AddWidget<UIButtonComponent>(scene, "Cover", { 1000.0f, 500.0f }, { 50.0f, 100.0f }, { .Interactable = false });
		scene.OnRuntimeStart();

		// The game view is a 960 x 540 panel at (100, 50) in window coordinates: half scale.
		GameUI::SetViewportOrigin({ 100.0f, 50.0f });
		GameUI::SetViewportSize({ 960.0f, 540.0f });
		REQUIRE(GameUI::GetPointerPosition());
		Input::SetMousePosition({ 100.0f + 1075.0f * 0.5f, 50.0f + 550.0f * 0.5f });
		CHECK(*GameUI::GetPointerPosition() == glm::vec2(1075.0f, 550.0f));
		Click(scene, { 100.0f + 1075.0f * 0.5f, 50.0f + 550.0f * 0.5f });
		CHECK(LogOf(scene, menu) == std::vector<std::string>{ "Button:OnClick" });
		Click(scene, { 100.0f + 1025.0f * 0.5f, 50.0f + 550.0f * 0.5f });
		CHECK(LogOf(scene, menu).size() == 1);

		// Pointer input off (the mouse is over another editor panel): no hover, and a held click is dropped.
		Press(scene, { 100.0f + 1075.0f * 0.5f, 50.0f + 550.0f * 0.5f });
		GameUI::SetPointerEnabled(false);
		CHECK_FALSE(GameUI::GetPointerPosition());
		Frame(scene);
		CHECK_FALSE(scene.GetUIInteraction()->IsPointerOverWidget());
		GameUI::SetPointerEnabled(true);
		Release(scene, { 100.0f + 1075.0f * 0.5f, 50.0f + 550.0f * 0.5f });
		CHECK(LogOf(scene, menu).size() == 1);
		CHECK(button);

		// Before the first mouse position there is no pointer: nothing at the canvas origin is hovered.
		Input::ResetMousePosition();
		CHECK_FALSE(GameUI::GetPointerPosition());
		GameUI::SetHostState({});
		Frame(scene);
		CHECK_FALSE(scene.GetUIInteraction()->IsPointerOverWidget());

		// Non-finite origins are ignored.
		GameUI::SetViewportOrigin({ 100.0f, 50.0f });
		GameUI::SetViewportOrigin({ std::numeric_limits<float>::quiet_NaN(), 0.0f });
		CHECK(GameUI::GetViewportOrigin() == glm::vec2(100.0f, 50.0f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Widget callbacks run before OnUpdate, and a duplicated menu calls its own copy")
	{
		WidgetInputScope input;
		TempProject project("UIDuplicate");
		Scene scene;
		const std::string script = project.WriteFile("Assets/Scripts/Order.lua", R"(
			local Order = {}
			function Order:OnCreate() self.Log = {} end
			function Order:OnClick(widget) table.insert(self.Log, "click " .. widget.Name) end
			function Order:OnUpdate() if #self.Log > 0 and self.Log[#self.Log] ~= "update" then table.insert(self.Log, "update") end end
			return Order
		)");
		Entity panel = scene.CreateEntity("Panel");
		panel.AddComponent<ScriptComponent>().Script = script;
		Entity button = AddWidget<UIButtonComponent>(scene, "Button", { 100.0f, 100.0f }, { 100.0f, 100.0f }, { .Target = panel.GetUUID() });
		scene.SetParent(button, panel);

		Entity copy = scene.DuplicateEntity(panel);
		REQUIRE(copy);
		const std::vector<UUID>& children = copy.GetComponent<RelationshipComponent>().Children;
		REQUIRE(children.size() == 1);
		Entity copiedButton = scene.GetEntityByUUID(children[0]);
		CHECK(copiedButton.GetComponent<UIButtonComponent>().Target == copy.GetUUID());
		CHECK(button.GetComponent<UIButtonComponent>().Target == panel.GetUUID());
		copiedButton.GetComponent<UIRectComponent>().Position = { 300.0f, 100.0f };

		scene.OnRuntimeStart();
		Click(scene, { 350.0f, 150.0f });
		CHECK(Field(scene, copy, "Log") == nlohmann::json({ "click Button", "update" }));
		CHECK(Field(scene, panel, "Log").empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Immediate-mode widgets return what the user did and take part in navigation")
	{
		WidgetInputScope input;
		TempProject project("UIImmediate");
		Scene scene;
		const std::string script = project.WriteFile("Assets/Scripts/Menu.lua", R"(
			local Menu = {}
			function Menu:OnCreate() self.Clicks = 0; self.Music = false; self.Volume = 0.5 end
			function Menu:OnUpdate()
				if UI.Button("Play", 100, 100, 200, 80, { Interactable = not LockPlay }) then self.Clicks = self.Clicks + 1 end
				self.Music = UI.Checkbox("Music", self.Music, 100, 300, 300, 40, { CheckColor = Vec4(1, 0, 0, 1) })
				self.Volume = UI.Slider("", self.Volume, 0, 1, -20, -100, 420, 40, { Id = "Volume", Anchor = "BottomRight", Step = 0.25, HandleWidth = 20 })
				UI.Button("Locked", 100, 500, 200, 80, { Interactable = false, Color = Vec4(0, 0, 1, 1), TextColor = Vec4(0, 0, 0, 1), Size = 20 })
			end
			return Menu
		)");
		Entity menu = scene.CreateEntity("Menu");
		menu.AddComponent<ScriptComponent>().Script = script;
		Entity component = AddWidget<UIButtonComponent>(scene, "Component", { 1000.0f, 100.0f }, { 200.0f, 80.0f });
		scene.OnRuntimeStart();
		Frame(scene);

		// Drawn this frame, hit-tested the next.
		Click(scene, { 150.0f, 120.0f });
		CHECK(Field(scene, menu, "Clicks") == 1);
		Frame(scene);
		CHECK(Field(scene, menu, "Clicks") == 1);
		CHECK(Eval(scene, "return UI.GetFocus()") == "nil");
		Click(scene, { 120.0f, 310.0f });
		CHECK(Field(scene, menu, "Music") == true);
		// The slider sits anchored to the bottom-right corner: its rect is (1480, 940) - (1900, 980).
		Press(scene, { 1480.0f + 10.0f + 300.0f, 960.0f });
		Release(scene, { 1480.0f + 10.0f + 300.0f, 960.0f });
		CHECK(Field(scene, menu, "Volume") == 0.75);
		// The disabled button takes no click (and covers nothing else).
		Click(scene, { 150.0f, 520.0f });
		CHECK(Eval(scene, "return UI.IsPointerOverWidget()") == "false");

		// Navigation reaches immediate widgets after the components (draw order), skipping the disabled one.
		CHECK(Eval(scene, "UI.SetFocus(nil)") == "");
		Tap(scene, KeyCode::Tab);
		CHECK(FocusOf(scene) == IDOf(component));
		Tap(scene, KeyCode::Tab);
		CHECK(FocusOf(scene) == UIWidgetID::ForName("Play"));
		Tap(scene, KeyCode::Enter);
		CHECK(Field(scene, menu, "Clicks") == 2);
		CHECK(Eval(scene, "UI.SetFocus('Volume')") == "");
		Tap(scene, KeyCode::Left);
		CHECK(Field(scene, menu, "Volume") == 0.5);

		// The draw commands: the focused slider's track, fill and handle, anchored to the corner.
		std::vector<UIDrawCommand> commands;
		GameUI::AppendCommands(commands);
		const auto handle = std::ranges::find_if(commands, [](const UIDrawCommand& command) { return command.Anchor == glm::vec2(1.0f, 1.0f) && command.Size == glm::vec2(20.0f, 40.0f); });
		REQUIRE(handle != commands.end());
		CHECK(ResolveUIBox(*handle, GameUI::GetCanvasSize()) == UIBox{ { 1680.0f, 940.0f }, { 20.0f, 40.0f } });
		const auto locked = std::ranges::find_if(commands, [](const UIDrawCommand& command) { return command.Kind == UIDrawKind::Text && command.Text == "Locked"; });
		REQUIRE(locked != commands.end());
		CHECK(locked->FontSize == 20.0f);
		CHECK(locked->Color == glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
		const auto checkMark = std::ranges::find_if(commands, [](const UIDrawCommand& command) { return command.Color == glm::vec4(1.0f, 0.0f, 0.0f, 1.0f); });
		CHECK(checkMark != commands.end());

		// Focus by entity, and its checks.
		CHECK(Eval(scene, "UI.SetFocus(Scene.FindEntityByName('Component')); return UI.GetFocus().Name") == "Component");
		CHECK(Eval(scene, "return UI.IsPointerOverWidget()") == "false");
		Input::SetMousePosition({ 1100.0f, 120.0f });
		Frame(scene);
		CHECK(Eval(scene, "return UI.IsPointerOverWidget()") == "true");

		// A widget disabled on the frame its click arrives (the script draws it disabled) does not report it.
		Press(scene, { 150.0f, 120.0f });
		CHECK(Eval(scene, "LockPlay = true") == "");
		Release(scene, { 150.0f, 120.0f });
		CHECK(Eval(scene, "LockPlay = false") == "");
		CHECK(Field(scene, menu, "Clicks") == 2);
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Immediate-mode widgets reject bad arguments and duplicate ids")
	{
		WidgetInputScope input;
		TempProject project("UIImmediateErrors");
		Scene scene;
		scene.CreateEntity("Plain");
		scene.OnRuntimeStart();
		REQUIRE(scene.GetScriptEngine());
		CHECK(Eval(scene, "return UI.Button('A', 0, 0, 10, 10)") == "false");
		const std::pair<const char*, const char*> cases[] = {
			{ "UI.Button('A', 0, 0, 10, 10)", "UI.Button: another widget already uses the id 'A' this frame" },
			{ "UI.Checkbox('A', false, 0, 0, 10, 10)", "UI.Checkbox: another widget already uses the id 'A'" },
			{ "UI.Button('', 0, 0, 10, 10)", "UI.Button: a widget needs a label or an 'Id' option" },
			{ "UI.Button('B', 0, 0, 0, 10)", "UI.Button: width and height must be positive" },
			{ "UI.Button('B', 0, 0, 10, 0/0)", "UI.Button: height must be between" },
			{ "UI.Button('B', 0, 0, 10, 10, { Id = '' })", "UI.Button: option 'Id' must not be empty" },
			{ "UI.Button('B', 0, 0, 10, 10, { Step = 1 })", "UI.Button: unknown option 'Step'" },
			{ "UI.Button('B', 0, 0, 10, 10, Vec4(1, 1, 1, 1))", "UI.Button: the last argument must be an options table" },
			{ "UI.Button('B', 0, 0, 10, 10, { Interactable = 1 })", "option 'Interactable' must be true or false" },
			{ "UI.Checkbox('C', false, 0, 0, 10, 10, { CheckColor = Vec4(2, 0, 0, 1) })", "CheckColor.x must be between 0 and 1" },
			{ "UI.Slider('S', 0.5, 1, 1, 0, 0, 10, 10)", "UI.Slider: max must be greater than min" },
			{ "UI.Slider('S', 2, 0, 1, 0, 0, 10, 10)", "UI.Slider: value must be between 0 and 1" },
			{ "UI.Slider('S', 0.5, 0, 1, 0, 0, 10, 10, { Step = -1 })", "Step must be between" },
			{ "UI.Slider('S', 0.5, 0, 1, 0, 0, 10, 10, { HandleWidth = 0/0 })", "HandleWidth must be between" },
			{ "UI.SetFocus('')", "UI.SetFocus: the widget id must not be empty" },
			{ "UI.SetFocus(42)", "UI.SetFocus: the argument must be a widget entity, a widget id or nil" },
			{ "UI.SetFocus(Scene.FindEntityByName('Plain'))", "UI.SetFocus: 'Plain' has no UIButton, UISlider, UICheckbox, UITextInput or UIDropdown" },
		};
		CheckLuaErrors(*scene.GetScriptEngine(), cases);
		// A failed call registers nothing: a later call can use the id.
		CHECK(Eval(scene, "return UI.Button('B', 0, 0, 10, 10)") == "false");
		scene.OnRuntimeStop();
	}

	TEST_CASE("Too many immediate-mode widgets in one frame is an error, not a silent drop")
	{
		UIInteraction interaction;
		UIImmediateInput input;
		for (size_t i = 0; i < UIInteraction::MaxImmediateWidgets; i++)
			REQUIRE(interaction.SubmitImmediate({ .ID = std::to_string(i), .Size = { 1.0f, 1.0f } }, input) == UIInteraction::SubmitResult::Accepted);
		CHECK(interaction.SubmitImmediate({ .ID = "one more", .Size = { 1.0f, 1.0f } }, input) == UIInteraction::SubmitResult::TooMany);
		CHECK(interaction.SubmitImmediate({ .ID = "0", .Size = { 1.0f, 1.0f } }, input) == UIInteraction::SubmitResult::DuplicateID);
	}

	TEST_CASE("Lua reads gamepads and hears about connections")
	{
		WidgetInputScope input;
		TempProject project("UIGamepadLua");
		Scene scene;
		Entity pads = AddScripted(scene, project, "Pads", "Assets/Scripts/Pads.lua", PadsScript);
		scene.OnRuntimeStart();

		Input::SetGamepadConnected(2, true, "Arcade Stick");
		REQUIRE(Input::SetGamepadButtonState(2, GamepadButton::Start, true));
		REQUIRE(Input::SetGamepadAxis(2, GamepadAxis::RightTrigger, 0.75f));
		CHECK(Eval(scene, "return Input.IsGamepadConnected(2)") == "true");
		CHECK(Eval(scene, "return Input.IsGamepadConnected(0)") == "false");
		CHECK(Eval(scene, "return Input.GetGamepadName(2)") == "Arcade Stick");
		CHECK(Eval(scene, "return Input.GetGamepadName(0)") == "nil");
		CHECK(Eval(scene, "return #Input.GetGamepads() .. ':' .. Input.GetGamepads()[1]") == "1:2");
		CHECK(Eval(scene, "return Input.IsGamepadButtonDown(2, 'start')") == "true");
		CHECK(Eval(scene, "return Input.IsGamepadButtonPressed(2, 'Start')") == "true");
		CHECK(Eval(scene, "return Input.GetGamepadAxis(2, 'RightTrigger')") == "0.75");
		Frame(scene);
		CHECK(Eval(scene, "return Input.IsGamepadButtonPressed(2, 'Start')") == "false");
		REQUIRE(Input::SetGamepadButtonState(2, GamepadButton::Start, false));
		CHECK(Eval(scene, "return Input.IsGamepadButtonReleased(2, 'Start')") == "true");
		Input::SetGamepadConnected(2, false);
		Frame(scene);
		CHECK(Field(scene, pads, "Log") == nlohmann::json({ "+2", "-2" }));

		const std::pair<const char*, const char*> cases[] = {
			{ "Input.IsGamepadConnected(-1)", "the gamepad index must be an integer from 0 to 15" },
			{ "Input.IsGamepadConnected(16)", "the gamepad index must be an integer from 0 to 15" },
			{ "Input.IsGamepadConnected(0.5)", "the gamepad index must be an integer from 0 to 15" },
			{ "Input.IsGamepadConnected('0')", "the gamepad index must be an integer from 0 to 15" },
			{ "Input.IsGamepadButtonDown(0, 'Turbo')", "unknown gamepad button 'Turbo'" },
			{ "Input.GetGamepadAxis(0, 'Wheel')", "unknown gamepad axis 'Wheel'" },
		};
		CheckLuaErrors(*scene.GetScriptEngine(), cases);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Widget components need a running scene to take input; the layout includes them")
	{
		WidgetInputScope input;
		Scene scene;
		Entity button = AddWidget<UIButtonComponent>(scene, "Button", { 10.0f, 20.0f }, { 30.0f, 40.0f });
		Entity bare = scene.CreateEntity("Bare");
		bare.AddComponent<UISliderComponent>();
		CHECK(UILayout::HasUI(scene));
		const std::vector<UILayoutItem> items = UILayout::Layout(scene, GameUI::GetCanvasSize());
		REQUIRE(items.size() == 2);
		CHECK(items[0].Rect == UIBox{ { 10.0f, 20.0f }, { 30.0f, 40.0f } });
		// A widget without a UIRect fills its parent's rect (here the canvas).
		CHECK(items[1].Rect == UIBox{ { 0.0f, 0.0f }, GameUI::GetCanvasSize() });
		CHECK(UILayout::GetRect(scene, button, GameUI::GetCanvasSize()));
		CHECK_FALSE(scene.GetUIInteraction());
		scene.OnSimulationStart();
		CHECK_FALSE(scene.GetUIInteraction());
		scene.OnSimulationStop();
	}

	TEST_CASE("A Lua error in a widget callback is reported once and disables that script")
	{
		WidgetInputScope input;
		TempProject project("UICallbackError");
		Scene scene;
		const std::string script = project.WriteFile("Assets/Scripts/Broken.lua", R"(
			local Broken = {}
			function Broken:OnCreate() self.Updates = 0 end
			function Broken:OnUpdate() self.Updates = self.Updates + 1 end
			function Broken:OnClick() error("boom") end
			return Broken
		)");
		Entity menu = scene.CreateEntity("Menu");
		menu.AddComponent<ScriptComponent>().Script = script;
		AddWidget<UIButtonComponent>(scene, "Button", { 0.0f, 0.0f }, { 100.0f, 100.0f }, { .Target = menu.GetUUID() });
		scene.OnRuntimeStart();
		Click(scene, { 50.0f, 50.0f });
		const nlohmann::json updates = Field(scene, menu, "Updates");
		Click(scene, { 50.0f, 50.0f });
		const std::vector<std::string>& errors = scene.GetScriptEngine()->GetErrors();
		REQUIRE(errors.size() == 1);
		CHECK(Contains(errors[0], "boom"));
		CHECK(Contains(errors[0], "(in OnClick)"));
		// Disabled like after any callback error: no more OnUpdate either.
		CHECK(Field(scene, menu, "Updates") == updates);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A held slider follows the pointer only when it moves, so a callback can override the value")
	{
		WidgetInputScope input;
		TempProject project("UISliderOverride");
		Scene scene;
		const std::string script = project.WriteFile("Assets/Scripts/Limit.lua", R"(
			local Limit = {}
			function Limit:OnCreate() self.Calls = 0 end
			function Limit:OnValueChanged(widget, value)
				self.Calls = self.Calls + 1
				if value > 5 then widget:SetComponent("UISlider", { Value = 5 }) end
			end
			return Limit
		)");
		Entity slider = AddWidget<UISliderComponent>(scene, "Volume", { 100.0f, 100.0f }, { 420.0f, 40.0f }, { .Value = 0.0f, .Min = 0.0f, .Max = 10.0f, .Step = 1.0f, .HandleWidth = 20.0f });
		slider.AddComponent<ScriptComponent>().Script = script;
		scene.OnRuntimeStart();
		// The pointer at the fraction for 8: one callback, clamped to 5, and holding still keeps it there.
		Press(scene, { 110.0f + 0.8f * 400.0f, 120.0f });
		Frame(scene, 5);
		CHECK(slider.GetComponent<UISliderComponent>().Value == 5.0f);
		CHECK(Field(scene, slider, "Calls") == 1);
		// Moving again drags again.
		Input::SetMousePosition({ 110.0f + 0.2f * 400.0f, 120.0f });
		Frame(scene);
		CHECK(slider.GetComponent<UISliderComponent>().Value == 2.0f);
		CHECK(Field(scene, slider, "Calls") == 2);
		Release(scene, { 110.0f + 0.2f * 400.0f, 120.0f });
		scene.OnRuntimeStop();
	}

	TEST_CASE("Disabling, hiding or destroying a held widget cancels the hold")
	{
		WidgetInputScope input;
		TempProject project("UIHoldCancel");
		Scene scene;
		Entity menu = AddRecorder(scene, project, "Menu");
		Entity panel = scene.CreateEntity("Panel");
		panel.AddComponent<UIRectComponent>(PlacedRect({ 0.0f, 0.0f }, { 1000.0f, 1000.0f }));
		Entity play = AddWidget<UIButtonComponent>(scene, "Play", { 100.0f, 100.0f }, { 200.0f, 80.0f }, { .Target = menu.GetUUID() });
		scene.SetParent(play, panel);
		Entity volume = AddWidget<UISliderComponent>(scene, "Volume", { 100.0f, 300.0f }, { 420.0f, 40.0f }, { .Value = 0.5f, .HandleWidth = 20.0f, .Target = menu.GetUUID() });
		scene.OnRuntimeStart();

		Press(scene, { 150.0f, 120.0f });
		play.GetComponent<UIButtonComponent>().Interactable = false;
		Release(scene, { 150.0f, 120.0f });
		play.GetComponent<UIButtonComponent>().Interactable = true;
		Press(scene, { 150.0f, 120.0f });
		panel.GetComponent<UIRectComponent>().Visible = false;
		Release(scene, { 150.0f, 120.0f });
		panel.GetComponent<UIRectComponent>().Visible = true;
		CHECK(LogOf(scene, menu).empty());

		// A slider disabled during a drag stops following the pointer.
		Press(scene, { 310.0f, 320.0f });
		volume.GetComponent<UISliderComponent>().Interactable = false;
		Input::SetMousePosition({ 500.0f, 320.0f });
		Frame(scene);
		Release(scene, { 500.0f, 320.0f });
		CHECK(volume.GetComponent<UISliderComponent>().Value == 0.5f);
		CHECK(LogOf(scene, menu).empty());

		// Destroyed while held: no click, no error.
		Press(scene, { 150.0f, 120.0f });
		scene.DestroyEntity(play);
		Release(scene, { 150.0f, 120.0f });
		CHECK(LogOf(scene, menu).empty());
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Losing the window's focus while a button is held does not click it")
	{
		WidgetInputScope input;
		TempProject project("UIFocusLoss");
		Scene scene;
		Entity menu = AddRecorder(scene, project, "Menu");
		AddWidget<UIButtonComponent>(scene, "Play", { 100.0f, 100.0f }, { 200.0f, 80.0f }, { .Target = menu.GetUUID() });
		scene.OnRuntimeStart();
		Press(scene, { 150.0f, 120.0f });
		// What WindowLostFocusEvent does: every button up, without a release.
		Input::Reset();
		Frame(scene, 2);
		CHECK(LogOf(scene, menu).empty());
		// A real click still works afterwards.
		Click(scene, { 150.0f, 120.0f });
		CHECK(LogOf(scene, menu) == std::vector<std::string>{ "Play:OnClick" });
		scene.OnRuntimeStop();
	}

	TEST_CASE("A mouse press drops the keyboard focus, on a widget or not")
	{
		WidgetInputScope input;
		Scene scene;
		Entity a = AddWidget<UIButtonComponent>(scene, "A", { 100.0f, 100.0f }, { 200.0f, 80.0f });
		Entity b = AddWidget<UIButtonComponent>(scene, "B", { 100.0f, 300.0f }, { 200.0f, 80.0f });
		scene.OnRuntimeStart();
		scene.GetUIInteraction()->SetFocus(IDOf(a));
		Frame(scene);
		Click(scene, { 900.0f, 900.0f });
		CHECK_FALSE(FocusOf(scene).IsValid());
		scene.GetUIInteraction()->SetFocus(IDOf(a));
		Frame(scene);
		Click(scene, { 150.0f, 320.0f });
		CHECK_FALSE(FocusOf(scene).IsValid());
		// Space then belongs to the game: nothing focused, nothing activated.
		Tap(scene, KeyCode::Space);
		CHECK_FALSE(FocusOf(scene).IsValid());
		CHECK(StateOf(scene, b) != UIWidgetState::Pressed);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A game can turn widget navigation off")
	{
		WidgetInputScope input;
		TempProject project("UINavigationOff");
		Scene scene;
		Entity menu = AddRecorder(scene, project, "Menu");
		Entity play = AddWidget<UIButtonComponent>(scene, "Play", { 100.0f, 100.0f }, { 200.0f, 80.0f }, { .Target = menu.GetUUID() });
		scene.OnRuntimeStart();
		CHECK(Eval(scene, "return UI.IsNavigationEnabled()") == "true");
		scene.GetUIInteraction()->SetFocus(IDOf(play));
		Frame(scene);
		// Turning it off drops the focus; arrows, Tab and Space then do nothing for the widgets.
		CHECK(Eval(scene, "UI.SetNavigationEnabled(false)") == "");
		CHECK(Eval(scene, "return UI.IsNavigationEnabled()") == "false");
		CHECK_FALSE(FocusOf(scene).IsValid());
		Tap(scene, KeyCode::Down);
		Tap(scene, KeyCode::Tab);
		Tap(scene, KeyCode::Space);
		CHECK_FALSE(FocusOf(scene).IsValid());
		CHECK(LogOf(scene, menu).empty());
		// The mouse still works.
		Click(scene, { 150.0f, 120.0f });
		CHECK(LogOf(scene, menu) == std::vector<std::string>{ "Play:OnClick" });
		CHECK(Eval(scene, "UI.SetNavigationEnabled(true)") == "");
		Tap(scene, KeyCode::Down);
		CHECK(FocusOf(scene) == IDOf(play));
		scene.OnRuntimeStop();
	}

	TEST_CASE("A diagonal stick moves along its dominant axis; the stick's X moves a focused slider")
	{
		WidgetInputScope input;
		Scene scene;
		Entity volume = AddWidget<UISliderComponent>(scene, "Volume", { 100.0f, 100.0f }, { 400.0f, 40.0f }, { .Value = 0.5f, .Step = 0.1f });
		Entity below = AddWidget<UIButtonComponent>(scene, "Below", { 100.0f, 300.0f }, { 400.0f, 40.0f });
		scene.OnRuntimeStart();
		Input::SetGamepadConnected(0, true);
		scene.GetUIInteraction()->SetFocus(IDOf(volume));
		Frame(scene);
		const auto push = [&](float x, float y) {
			REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftX, x));
			REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftY, y));
			Frame(scene);
			REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftX, 0.0f));
			REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftY, 0.0f));
			Frame(scene);
		};
		push(-1.0f, 0.0f);
		CHECK(volume.GetComponent<UISliderComponent>().Value == doctest::Approx(0.4f));
		push(0.6f, 0.2f);
		CHECK(volume.GetComponent<UISliderComponent>().Value == doctest::Approx(0.5f));
		// Mostly down: navigates down instead of moving the slider.
		push(0.6f, 0.7f);
		CHECK(volume.GetComponent<UISliderComponent>().Value == doctest::Approx(0.5f));
		CHECK(FocusOf(scene) == IDOf(below));
		// Exactly diagonal: vertical wins.
		scene.GetUIInteraction()->SetFocus(IDOf(volume));
		Frame(scene);
		push(0.7f, 0.7f);
		CHECK(volume.GetComponent<UISliderComponent>().Value == doctest::Approx(0.5f));
		CHECK(FocusOf(scene) == IDOf(below));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Scripts hear about gamepads connected before play and while paused")
	{
		WidgetInputScope input;
		TempProject project("UIGamepadAnnounce");
		Scene scene;
		Entity pads = AddScripted(scene, project, "Pads", "Assets/Scripts/Pads.lua", PadsScript);
		Input::SetGamepadConnected(1, true);
		Input::EndFrame();
		scene.OnRuntimeStart();
		Frame(scene);
		CHECK(Field(scene, pads, "Log") == nlohmann::json({ "+1" }));
		// While paused nothing updates; the change is announced when play resumes.
		scene.SetPaused(true);
		Input::SetGamepadConnected(1, false);
		Input::SetGamepadConnected(3, true);
		Frame(scene, 3);
		scene.SetPaused(false);
		Frame(scene);
		CHECK(Field(scene, pads, "Log") == nlohmann::json({ "+1", "-1", "+3" }));
		// A gamepad replaced at its index between two updates: a disconnect and a connect.
		Input::SetGamepadConnected(3, false);
		Input::SetGamepadConnected(3, true);
		Frame(scene);
		CHECK(Field(scene, pads, "Log") == nlohmann::json({ "+1", "-1", "+3", "-3", "+3" }));
		// An instance created later hears about the gamepads already connected, right after OnCreate.
		Entity late = scene.CreateEntity("Late");
		late.AddComponent<ScriptComponent>().Script = "Assets/Scripts/Pads.lua";
		Frame(scene);
		CHECK(Field(scene, late, "Log") == nlohmann::json({ "+3" }));
		CHECK(Field(scene, pads, "Log").size() == 5);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Loading a scene drops what the UI held, hovered and focused")
	{
		WidgetInputScope input;
		TempProject project("UISceneLoad");
		project.WriteFile("Assets/Scenes/Level.bscene", R"({
			"Format": "BasaltScene", "Version": 1, "Name": "Level",
			"Entities": [ { "ID": 7, "Name": "LevelButton", "Components": { "UIRect": { "AnchorMin": [0, 0], "AnchorMax": [0, 0], "Pivot": [0, 0], "Position": [100, 100], "Size": [200, 80] }, "UIButton": { "OnClick": "" } } } ]
		})");
		const std::string script = project.WriteFile("Assets/Scripts/Loader.lua", R"(
			local Loader = {}
			function Loader:OnClick() Scene.Load("Assets/Scenes/Level.bscene") end
			return Loader
		)");
		Scene scene;
		// The new scene's button has the same ID (as when a scene reloads): only the load itself can drop the state.
		Entity start = scene.CreateEntityWithUUID(UUID(7), "Start");
		start.AddComponent<UIRectComponent>(PlacedRect({ 100.0f, 100.0f }, { 200.0f, 80.0f }));
		start.AddComponent<UIButtonComponent>();
		start.AddComponent<ScriptComponent>().Script = script;
		scene.OnRuntimeStart();
		// Hold the mouse on Start, then activate it from the keyboard: the load lands at the end of that frame.
		Press(scene, { 150.0f, 120.0f });
		scene.GetUIInteraction()->SetFocus(IDOf(start));
		Frame(scene);
		Tap(scene, KeyCode::Enter);
		Entity levelButton = scene.FindEntityByName("LevelButton");
		REQUIRE(levelButton);
		CHECK_FALSE(FocusOf(scene).IsValid());
		// The mouse held through the load holds nothing in the new scene (the button at the same spot is only
		// hovered), and releasing it clicks nothing.
		CHECK(StateOf(scene, levelButton) == UIWidgetState::Highlighted);
		Release(scene, { 150.0f, 120.0f });
		CHECK(StateOf(scene, levelButton) == UIWidgetState::Highlighted);
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Immediate-mode widgets the old scene drew are gone after a load")
	{
		WidgetInputScope input;
		TempProject project("UISceneLoadImmediate");
		project.WriteFile("Assets/Scenes/Empty.bscene", R"({ "Format": "BasaltScene", "Version": 1, "Name": "Empty", "Entities": [] })");
		Scene scene;
		AddScripted(scene, project, "Menu", "Assets/Scripts/Menu.lua", R"(
			local Menu = {}
			function Menu:OnCreate() Scene.DontDestroyOnLoad(self.Entity) end
			function Menu:OnUpdate()
				if not InLevel then UI.Button("Menu", 100, 100, 200, 80) end
				if Input.IsKeyPressed("L") then Scene.Load("Assets/Scenes/Empty.bscene"); InLevel = true end
			end
			return Menu
		)");
		// An entity of the old scene draws and focuses a widget as it is destroyed by the load.
		AddScripted(scene, project, "Leaving", "Assets/Scripts/Leaving.lua", R"(
			local Leaving = {}
			function Leaving:OnDestroy() UI.Button("Goodbye", 100, 100, 200, 80); UI.SetFocus("Goodbye") end
			return Leaving
		)");
		scene.OnRuntimeStart();
		Frame(scene);
		Input::SetMousePosition({ 150.0f, 120.0f });
		Frame(scene);
		CHECK(scene.GetUIInteraction()->IsPointerOverWidget());
		// The frame that loads still draws the menu; the next one, in the new scene, must not hit it.
		Input::SetKeyState(KeyCode::L, true);
		Frame(scene);
		Input::SetKeyState(KeyCode::L, false);
		Frame(scene);
		CHECK_FALSE(scene.GetUIInteraction()->IsPointerOverWidget());
		CHECK_FALSE(FocusOf(scene).IsValid());
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("A focus set on an immediate-mode widget before it is drawn waits a frame for it")
	{
		WidgetInputScope input;
		TempProject project("UIFocusFirst");
		Scene scene;
		const std::string script = project.WriteFile("Assets/Scripts/Menu.lua", R"(
			local Menu = {}
			function Menu:OnCreate() self.Clicks = 0; UI.SetFocus("Play") end
			function Menu:OnUpdate()
				if ShowMenu ~= false and UI.Button("Play", 100, 100, 200, 80) then self.Clicks = self.Clicks + 1 end
			end
			return Menu
		)");
		Entity menu = scene.CreateEntity("Menu");
		menu.AddComponent<ScriptComponent>().Script = script;
		scene.OnRuntimeStart();
		Frame(scene, 2);
		CHECK(FocusOf(scene) == UIWidgetID::ForName("Play"));
		Tap(scene, KeyCode::Enter);
		CHECK(Field(scene, menu, "Clicks") == 1);
		// A focused immediate widget that is no longer drawn loses the focus (without a warning: it was a widget).
		const uint64_t since = Log::GetHistory().GetTotalCount();
		CHECK(Eval(scene, "ShowMenu = false") == "");
		Frame(scene, 2);
		CHECK(Eval(scene, "return UI.GetFocus()") == "nil");
		Tap(scene, KeyCode::Enter);
		CHECK(Field(scene, menu, "Clicks") == 1);
		CHECK(CountLogMessages(since, "dropped it") == 0);
		// An id that never shows up is dropped with a warning.
		CHECK(Eval(scene, "UI.SetFocus('Nope')") == "");
		Frame(scene, 2);
		CHECK(Eval(scene, "return UI.GetFocus()") == "nil");
		CHECK(CountLogMessages(since, "the focus was set to 'Nope', which is not a visible, interactable widget") == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Immediate-mode checkboxes and sliders drawn disabled ignore input that arrived before")
	{
		WidgetInputScope input;
		TempProject project("UIImmediateLock");
		Scene scene;
		const std::string script = project.WriteFile("Assets/Scripts/Menu.lua", R"(
			local Menu = {}
			function Menu:OnCreate() self.Music = false; self.Volume = 0.5 end
			function Menu:OnUpdate()
				self.Music = UI.Checkbox("Music", self.Music, 100, 100, 300, 40, { Interactable = not Lock })
				self.Volume = UI.Slider("Volume", self.Volume, 0, 1, 100, 200, 420, 40, { Interactable = not Lock, HandleWidth = 20 })
			end
			return Menu
		)");
		Entity menu = scene.CreateEntity("Menu");
		menu.AddComponent<ScriptComponent>().Script = script;
		scene.OnRuntimeStart();
		Frame(scene);
		Press(scene, { 120.0f, 120.0f });
		CHECK(Eval(scene, "Lock = true") == "");
		Release(scene, { 120.0f, 120.0f });
		CHECK(Field(scene, menu, "Music") == false);
		CHECK(Eval(scene, "Lock = false") == "");
		Frame(scene);
		CHECK(Eval(scene, "Lock = true") == "");
		Press(scene, { 500.0f, 220.0f });
		Release(scene, { 500.0f, 220.0f });
		CHECK(Field(scene, menu, "Volume") == 0.5);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Callback problems are reported once per kind of problem")
	{
		WidgetInputScope input;
		TempProject project("UICallbackKinds");
		Scene scene;
		Entity target = scene.CreateEntity("Target");
		AddWidget<UIButtonComponent>(scene, "Button", { 0.0f, 0.0f }, { 100.0f, 100.0f }, { .Target = target.GetUUID(), .OnClick = "Go" });
		scene.OnRuntimeStart();
		Click(scene, { 50.0f, 50.0f });
		// The game adds a script, but without the method: a new problem, reported too.
		target.AddComponent<ScriptComponent>().Script = project.WriteFile("Assets/Scripts/Empty.lua", "return {}");
		Frame(scene);
		Click(scene, { 50.0f, 50.0f });
		Click(scene, { 50.0f, 50.0f });
		const std::vector<std::string>& errors = scene.GetScriptEngine()->GetErrors();
		REQUIRE(errors.size() == 2);
		CHECK(Contains(errors[0], "'Target' has no running script"));
		CHECK(Contains(errors[1], "the script of 'Target' has no method Go"));
		scene.OnRuntimeStop();
	}

	TEST_CASE("A widget whose drawing would not fit in the frame's commands is not registered either")
	{
		WidgetInputScope input;
		Scene scene;
		scene.CreateEntity("Plain");
		scene.OnRuntimeStart();
		REQUIRE(scene.GetScriptEngine());
		Input::SetMousePosition({ 5.0f, 5.0f });
		while (GameUI::GetCommandCount() + 3 < GameUI::MaxCommands)
			REQUIRE(GameUI::Submit(UIDrawCommand{}) == GameUI::SubmitResult::Queued);
		const size_t commands = GameUI::GetCommandCount();
		CHECK(Contains(Eval(scene, "UI.Button('B', 0, 0, 10, 10)"), "UI.Button: more than 65536 UI commands this frame"));
		CHECK(GameUI::GetCommandCount() == commands);
		// Not registered: the next frame has nothing under the pointer.
		Frame(scene);
		CHECK_FALSE(scene.GetUIInteraction()->IsPointerOverWidget());
		scene.OnRuntimeStop();
	}

	TEST_CASE("A callback that destroys its target skips the target's other callbacks that frame")
	{
		WidgetInputScope input;
		TempProject project("UIDestroyedTarget");
		Scene scene;
		Entity menu = AddScripted(scene, project, "Menu", "Assets/Scripts/Close.lua", R"(
			local Close = {}
			function Close:OnClick() Scene.Destroy(self.Entity) end
			return Close
		)");
		const UUID menuID = menu.GetUUID();
		AddWidget<UIButtonComponent>(scene, "A", { 100.0f, 100.0f }, { 200.0f, 80.0f }, { .Target = menuID });
		Entity b = AddWidget<UIButtonComponent>(scene, "B", { 100.0f, 300.0f }, { 200.0f, 80.0f }, { .Target = menuID });
		scene.OnRuntimeStart();
		Press(scene, { 150.0f, 120.0f });
		scene.GetUIInteraction()->SetFocus(IDOf(b));
		// One frame: the release clicks A, and Enter clicks the focused B.
		Input::SetMouseButtonState(MouseCode::Left, false);
		Input::SetKeyState(KeyCode::Enter, true);
		Frame(scene);
		Input::SetKeyState(KeyCode::Enter, false);
		CHECK_FALSE(scene.GetEntityByUUID(menuID));
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("A widget focused by navigation and then hidden loses the focus without a warning")
	{
		WidgetInputScope input;
		Scene scene;
		Entity menu = AddWidget<UIButtonComponent>(scene, "Menu", { 100.0f, 100.0f }, { 200.0f, 80.0f });
		scene.OnRuntimeStart();
		const uint64_t since = Log::GetHistory().GetTotalCount();
		// Tab focuses it, and the game hides the menu on the same key press.
		Input::SetKeyState(KeyCode::Tab, true);
		Frame(scene);
		CHECK(FocusOf(scene) == IDOf(menu));
		menu.GetComponent<UIRectComponent>().Visible = false;
		Input::SetKeyState(KeyCode::Tab, false);
		Frame(scene, 2);
		CHECK_FALSE(FocusOf(scene).IsValid());
		CHECK(CountLogMessages(since, "dropped it") == 0);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Validating a widget component accepts exactly what sanitizing leaves unchanged")
	{
		std::mt19937 random(11);
		const float values[] = { 0.0f, 0.25f, 1.0f, -0.5f, 1.5f, 3e6f, -3e6f, 600.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() };
		const char* names[] = { "", "OnClick", "2x", "end", "a b", "_ok" };
		const auto pick = [&] { return values[random() % std::size(values)]; };
		const auto color = [&] { return glm::vec4(pick(), pick(), pick(), pick()); };
		const auto name = [&] { return std::string(names[random() % std::size(names)]); };
		for (int i = 0; i < 500; i++)
		{
			INFO("iteration ", i);
			CheckRulesAgree(UISliderComponent{ .Value = pick(), .Min = pick(), .Max = pick(), .Step = pick(), .FillColor = color(), .HandleWidth = pick(), .NormalTint = color(), .OnValueChanged = name() }, &SanitizeUISlider, &ValidateUISlider);
			CheckRulesAgree(UIButtonComponent{ .HighlightTint = color(), .PressedTint = color(), .OnClick = name() }, &SanitizeUIButton, &ValidateUIButton);
			CheckRulesAgree(UICheckboxComponent{ .BoxColor = color(), .CheckColor = color(), .DisabledTint = color(), .OnValueChanged = name() }, &SanitizeUICheckbox, &ValidateUICheckbox);
		}
	}
}

TEST_CASE("UI follow-up widgets validate text and choice data")
{
	UITextInputComponent text;
	CHECK(ValidateUITextInput(text).empty());
	text.Text = "a\xc3\xa9";
	text.MaxLength = 2;
	CHECK(ValidateUITextInput(text).empty());
	text.MaxLength = 1;
	CHECK_FALSE(ValidateUITextInput(text).empty());
	CHECK(SanitizeUITextInput(text).Text == "a");
	text.Text = "\xc0\x80";
	CHECK_FALSE(ValidateUITextInput(text).empty());
	CHECK(SanitizeUITextInput(text).Text.empty());
	text.Text = "line\nbreak";
	CHECK_FALSE(ValidateUITextInput(text).empty());
	UIDropdownComponent dropdown;
	dropdown.Options = "First\nSecond\nThird";
	dropdown.Selected = 3;
	CHECK_FALSE(ValidateUIDropdown(dropdown).empty());
	CHECK(SanitizeUIDropdown(dropdown).Selected == 2);
	dropdown.Options.clear();
	CHECK(SanitizeUIDropdown(dropdown).Selected == 0);
	UIScrollViewComponent scroll;
	scroll.Offset.x = std::numeric_limits<float>::infinity();
	CHECK_FALSE(ValidateUIScrollView(scroll).empty());
	CHECK(SanitizeUIScrollView(scroll).Offset.x == 0.0f);
}

TEST_CASE("Text input edits Unicode at its cursor and respects routing and limits")
{
	WidgetInputScope input;
	Input::SetGameFocus(true);
	Scene scene;
	Entity text = AddWidget<UITextInputComponent>(scene, "Text", { 20, 20 }, { 200, 40 });
	text.GetComponent<UITextInputComponent>().MaxLength = 3;
	scene.OnRuntimeStart();
	auto* interaction = scene.GetUIInteraction();
	interaction->SetFocus(UIWidgetID::ForEntity(text.GetUUID()));
	Input::AddTextCodepoint('a');
	Input::AddTextCodepoint(0xe9);
	Frame(scene);
	CHECK(text.GetComponent<UITextInputComponent>().Text == "a\xc3\xa9");
	Input::SetKeyState(KeyCode::Left, true);
	Frame(scene);
	Input::SetKeyState(KeyCode::Left, false);
	Input::AddTextCodepoint(0x1f600);
	Input::AddTextCodepoint('x');
	Frame(scene);
	CHECK(text.GetComponent<UITextInputComponent>().Text == "a\xf0\x9f\x98\x80\xc3\xa9");
	Input::SetKeyState(KeyCode::Backspace, true);
	Frame(scene);
	CHECK(text.GetComponent<UITextInputComponent>().Text == "a\xc3\xa9");
	Input::SetKeyState(KeyCode::Backspace, false);
	text.GetComponent<UITextInputComponent>().ReadOnly = true;
	Input::AddTextCodepoint('q');
	Frame(scene);
	CHECK(text.GetComponent<UITextInputComponent>().Text == "a\xc3\xa9");
	text.GetComponent<UITextInputComponent>().ReadOnly = false;
	Input::SetGameFocus(false);
	Input::AddTextCodepoint('q');
	Input::SetGameFocus(true);
	Frame(scene);
	CHECK(text.GetComponent<UITextInputComponent>().Text == "a\xc3\xa9");
	scene.OnRuntimeStop();
}

TEST_CASE("Dropdown popup accepts pointer and keyboard selection and dismisses on Escape")
{
	WidgetInputScope input;
	Scene scene;
	Entity dropdown = AddWidget<UIDropdownComponent>(scene, "Menu", { 20, 20 }, { 160, 40 });
	dropdown.GetComponent<UIDropdownComponent>().Options = "One\nTwo\nThree";
	scene.OnRuntimeStart();
	auto* interaction = scene.GetUIInteraction();
	Press(scene, { 40, 40 });
	Release(scene, { 40, 40 });
	CHECK(interaction->GetOpenDropdown() == dropdown.GetUUID());
	Press(scene, { 40, 120 });
	Release(scene, { 40, 120 });
	CHECK(dropdown.GetComponent<UIDropdownComponent>().Selected == 1);
	CHECK(interaction->GetOpenDropdown() == 0);
	Input::SetKeyState(KeyCode::Enter, true);
	Frame(scene);
	Input::SetKeyState(KeyCode::Enter, false);
	Frame(scene);
	Input::SetKeyState(KeyCode::Down, true);
	Frame(scene);
	Input::SetKeyState(KeyCode::Down, false);
	Input::SetKeyState(KeyCode::Enter, true);
	Frame(scene);
	CHECK(dropdown.GetComponent<UIDropdownComponent>().Selected == 2);
	Input::SetKeyState(KeyCode::Enter, false);
	Frame(scene);
	Input::SetKeyState(KeyCode::Enter, true);
	Frame(scene);
	Input::SetKeyState(KeyCode::Enter, false);
	Input::SetKeyState(KeyCode::Escape, true);
	Frame(scene);
	CHECK(interaction->GetOpenDropdown() == 0);
	scene.OnRuntimeStop();
}

TEST_CASE("Scroll view clips children and panel overlays block input and wheel")
{
	WidgetInputScope input;
	Scene scene;
	Entity view = AddWidget<UIScrollViewComponent>(scene, "View", { 20, 20 }, { 200, 100 });
	view.GetComponent<UIScrollViewComponent>().ContentSize = { 200, 400 };
	Entity child = AddWidget<UIButtonComponent>(scene, "Child", { 0, 160 }, { 100, 40 });
	scene.SetParent(child, view);
	Entity panel = AddWidget<UIPanelComponent>(scene, "Overlay", { 20, 20 }, { 200, 100 });
	panel.GetComponent<UIRectComponent>().Visible = false;
	scene.OnRuntimeStart();
	Input::SetMousePosition({ 40, 40 });
	Input::AddMouseScroll({ 0, -2 });
	Frame(scene);
	CHECK(view.GetComponent<UIScrollViewComponent>().Offset.y == 64.0f);
	const auto rect = UILayout::GetRect(scene, child, GameUI::GetCanvasSize());
	REQUIRE(rect.has_value());
	CHECK(rect->Min.y == doctest::Approx(116.0f));
	panel.GetComponent<UIRectComponent>().Visible = true;
	Input::AddMouseScroll({ 0, -2 });
	Frame(scene);
	CHECK(view.GetComponent<UIScrollViewComponent>().Offset.y == 64.0f);
	CHECK(scene.GetUIInteraction()->IsPointerOverWidget());
	Press(scene, { 40, 40 });
	CHECK_FALSE(scene.GetUIInteraction()->GetFocus().IsValid());
	scene.OnRuntimeStop();
}

TEST_CASE("Vertical sliders, explicit navigation and focus rings share widget geometry")
{
	WidgetInputScope input;
	Scene scene;
	UISliderComponent slider;
	slider.Vertical = true;
	slider.HandleWidth = 20.0f;
	Entity vertical = AddWidget<UISliderComponent>(scene, "Vertical", { 20, 20 }, { 40, 200 }, slider);
	Entity first = AddWidget<UIButtonComponent>(scene, "First", { 200, 20 }, { 100, 40 });
	Entity second = AddWidget<UIButtonComponent>(scene, "Second", { 400, 20 }, { 100, 40 });
	Entity third = AddWidget<UIButtonComponent>(scene, "Third", { 600, 20 }, { 100, 40 });
	first.AddComponent<UINavigationComponent>().Right = third.GetUUID();
	third.AddComponent<UIFocusRingComponent>();
	scene.OnRuntimeStart();
	Press(scene, { 40, 30 });
	CHECK(vertical.GetComponent<UISliderComponent>().Value == 1.0f);
	Release(scene, { 40, 30 });
	auto* interaction = scene.GetUIInteraction();
	interaction->SetFocus(UIWidgetID::ForEntity(vertical.GetUUID()));
	Input::SetKeyState(KeyCode::Down, true);
	Frame(scene);
	CHECK(vertical.GetComponent<UISliderComponent>().Value == doctest::Approx(0.95f));
	Input::SetKeyState(KeyCode::Down, false);
	Frame(scene);
	interaction->SetFocus(UIWidgetID::ForEntity(first.GetUUID()));
	Input::SetKeyState(KeyCode::Right, true);
	Frame(scene);
	CHECK(interaction->GetFocus() == UIWidgetID::ForEntity(third.GetUUID()));
	std::vector<UIDrawCommand> commands;
	UILayout::BuildCommands(scene, GameUI::GetCanvasSize(), commands);
	CHECK(commands.size() == 7);
	Input::SetKeyState(KeyCode::Right, false);
	Frame(scene);
	first.GetComponent<UINavigationComponent>().Right = second.GetUUID();
	second.GetComponent<UIButtonComponent>().Interactable = false;
	interaction->SetFocus(UIWidgetID::ForEntity(first.GetUUID()));
	Input::SetKeyState(KeyCode::Right, true);
	Frame(scene);
	CHECK(interaction->GetFocus() == UIWidgetID::ForEntity(first.GetUUID()));
	scene.OnRuntimeStop();
}

TEST_CASE("Typed events are Unicode-safe, frame bounded and routed when received")
{
	WidgetInputScope input;
	Input::SetGameFocus(true);
	KeyTypedEvent letter(0x1f600);
	Input::OnEvent(letter);
	CHECK_FALSE(letter.Handled);
	CHECK(Input::GetTextInput() == "\xf0\x9f\x98\x80");
	CHECK(GameInput::GetTextInput() == Input::GetTextInput());
	// Hosts reaffirm focus after polling platform events and before the scene updates.
	Input::SetGameFocus(true);
	CHECK(GameInput::GetTextInput() == Input::GetTextInput());
	Input::SetGameFocus(false);
	Input::SetGameFocus(true);
	CHECK(GameInput::GetTextInput().empty());
	Input::EndFrame();
	CHECK(Input::GetTextInput().empty());
	Input::SetGameFocus(false);
	Input::AddTextCodepoint('x');
	Input::SetGameFocus(true);
	CHECK(GameInput::GetTextInput().empty());
	Input::AddTextCodepoint(0xd800);
	Input::AddTextCodepoint(0x110000);
	Input::AddTextCodepoint('\n');
	CHECK(Input::GetTextInput() == "x");
	Input::Reset();
	for (size_t n = 0; n < 5000; n++)
		Input::AddTextCodepoint(0x1f600);
	CHECK(Input::GetTextInput().size() == 16384);
	Input::Reset();
	CHECK(Input::GetTextInput().empty());
}

TEST_CASE("Immediate widgets receive clicks above an overlapping dropdown popup")
{
	WidgetInputScope input;
	TempProject project("UIDropdownOverlap");
	Scene scene;
	const std::string script = project.WriteFile("Assets/Scripts/Menu.lua", R"(
		local Menu = {}
		function Menu:OnCreate() self.Clicks = 0 end
		function Menu:OnUpdate()
			if UI.Button("Overlay", 20, 100, 160, 40) then self.Clicks = self.Clicks + 1 end
		end
		return Menu
	)");
	Entity menu = scene.CreateEntity("Menu");
	menu.AddComponent<ScriptComponent>().Script = script;
	Entity dropdown = AddWidget<UIDropdownComponent>(scene, "Dropdown", { 20, 20 }, { 160, 40 });
	dropdown.GetComponent<UIDropdownComponent>().Options = "First\nSecond";
	dropdown.GetComponent<UIDropdownComponent>().OnValueChanged.clear();
	scene.OnRuntimeStart();
	Frame(scene);
	Click(scene, { 40, 40 });
	CHECK(scene.GetUIInteraction()->GetOpenDropdown() == dropdown.GetUUID());
	Click(scene, { 40, 120 });
	CHECK(Field(scene, menu, "Clicks") == 1);
	CHECK(dropdown.GetComponent<UIDropdownComponent>().Selected == 0);
	CHECK(scene.GetUIInteraction()->GetOpenDropdown() == 0);
	scene.OnRuntimeStop();
}
