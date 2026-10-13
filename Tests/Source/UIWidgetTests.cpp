#include <doctest/doctest.h>

#include <Basalt/Core/Input.h>
#include <Basalt/Core/Log.h>
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
			for (uint32_t index = 0; index < Input::MaxGamepads; index++)
				Input::SetGamepadConnected(index, false);
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
		CHECK(UIWidgets::SliderNavigationStep(0.0f, 10.0f, 0.0f) == 0.5f);
		CHECK(UIWidgets::SliderNavigationStep(0.0f, 10.0f, 2.0f) == 2.0f);
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
		Entity silent = AddWidget<UIButtonComponent>(scene, "Silent", { 100.0f, 500.0f }, { 200.0f, 80.0f }, { .Target = menu.GetUUID(), .OnClick = "" });
		scene.OnRuntimeStart();
		REQUIRE(scene.GetUIInteraction());

		Press(scene, { 150.0f, 120.0f });
		CHECK(LogOf(scene, menu).empty());
		CHECK(scene.GetUIInteraction()->GetState(IDOf(play), true) == UIWidgetState::Pressed);
		CHECK(scene.GetUIInteraction()->IsPointerOverWidget());
		Release(scene, { 160.0f, 130.0f });
		CHECK(LogOf(scene, menu) == std::vector<std::string>{ "Play:StartGame" });
		// The click focused it; the pointer still over it highlights it.
		CHECK(FocusOf(scene) == IDOf(play));
		CHECK(scene.GetUIInteraction()->GetState(IDOf(play), true) == UIWidgetState::Highlighted);

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
		CHECK(scene.GetUIInteraction()->GetState(IDOf(play), false) == UIWidgetState::Disabled);
		CHECK(silent);
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
		CHECK(scene.GetUIInteraction()->GetState(IDOf(volume), true) == UIWidgetState::Pressed);
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
		// Focused by the clicks: highlighted.
		CHECK(commands[0].Color == defaults.BoxColor * defaults.HighlightTint);
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
		// Held, but the pointer moved off: no longer pressed, still focused.
		Input::SetMousePosition({ 500.0f, 500.0f });
		Frame(scene);
		CHECK(imageColor() == glm::vec4(0.8f, 0.4f, 0.2f, 1.0f));
		Input::SetMouseButtonState(MouseCode::Left, false);
		Frame(scene);
		button.GetComponent<UIButtonComponent>().Interactable = false;
		Frame(scene);
		CHECK(imageColor() == glm::vec4(0.8f, 0.4f, 0.2f, 0.5f));
		CHECK_FALSE(FocusOf(scene).IsValid());

		// An entity with several widget components acts (and draws) as the first of button, slider, checkbox.
		slider.AddComponent<UIButtonComponent>();
		CHECK(UILayout::GetWidgetKind(slider) == UIWidgetKind::Button);
		CHECK(UILayout::CountWidgetComponents(slider) == 2);
		commands = BuildCommands(scene);
		CHECK(commands.size() == 1);
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
		CHECK(scene.GetUIInteraction()->GetState(IDOf(a), true) == UIWidgetState::Highlighted);
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
		scene.GetUIInteraction()->SetFocus(IDOf(c));
		Frame(scene);
		CHECK_FALSE(FocusOf(scene).IsValid());
		scene.GetUIInteraction()->SetFocus(IDOf(d));
		scene.DestroyEntity(d);
		Frame(scene);
		CHECK_FALSE(FocusOf(scene).IsValid());

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
		// t >= RepeatDelay (0.4 s: frame 26, t = 0.40625), then every RepeatInterval (t = 0.5: frame 32,
		// t = 0.6: frame 39).
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
		CHECK(scene.GetUIInteraction()->GetState(IDOf(play), true) == UIWidgetState::Pressed);
		// Activated on the press, once.
		Frame(scene, 3);
		Input::SetKeyState(KeyCode::Enter, false);
		Frame(scene);
		CHECK(scene.GetUIInteraction()->GetState(IDOf(play), true) == UIWidgetState::Highlighted);
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

		// Non-finite origins are ignored.
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
		CHECK(Eval(scene, "return UI.GetFocus()") == "Play");
		Click(scene, { 120.0f, 310.0f });
		CHECK(Field(scene, menu, "Music") == true);
		// The slider sits anchored to the bottom-right corner: its rect is (1480, 940) - (1900, 980).
		Press(scene, { 1480.0f + 10.0f + 300.0f, 960.0f });
		Release(scene, { 1480.0f + 10.0f + 300.0f, 960.0f });
		CHECK(Field(scene, menu, "Volume") == 0.75);
		Click(scene, { 150.0f, 520.0f });
		CHECK(Eval(scene, "return UI.GetFocus()") == "Volume");

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
			{ "UI.SetFocus(Scene.FindEntityByName('Plain'))", "UI.SetFocus: 'Plain' has no UIButton, UISlider or UICheckbox" },
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
		const std::string script = project.WriteFile("Assets/Scripts/Pads.lua", R"(
			local Pads = {}
			function Pads:OnCreate() self.Log = {} end
			function Pads:OnGamepadConnected(index) table.insert(self.Log, "+" .. index) end
			function Pads:OnGamepadDisconnected(index) table.insert(self.Log, "-" .. index) end
			return Pads
		)");
		Entity pads = scene.CreateEntity("Pads");
		pads.AddComponent<ScriptComponent>().Script = script;
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
			UISliderComponent slider{ .Value = pick(), .Min = pick(), .Max = pick(), .Step = pick(), .FillColor = color(), .HandleWidth = pick(), .NormalTint = color(), .OnValueChanged = name() };
			const UISliderComponent repairedSlider = SanitizeUISlider(slider);
			CHECK(ValidateUISlider(slider).empty() == (repairedSlider == slider));
			CHECK_MESSAGE(ValidateUISlider(repairedSlider).empty(), ValidateUISlider(repairedSlider), " min ", slider.Min, " max ", slider.Max, " value ", slider.Value, " step ", slider.Step);

			UIButtonComponent button{ .HighlightTint = color(), .PressedTint = color(), .OnClick = name() };
			const UIButtonComponent repairedButton = SanitizeUIButton(button);
			CHECK(ValidateUIButton(button).empty() == (repairedButton == button));
			CHECK(ValidateUIButton(repairedButton).empty());

			UICheckboxComponent checkbox{ .BoxColor = color(), .CheckColor = color(), .DisabledTint = color(), .OnValueChanged = name() };
			const UICheckboxComponent repairedCheckbox = SanitizeUICheckbox(checkbox);
			CHECK(ValidateUICheckbox(checkbox).empty() == (repairedCheckbox == checkbox));
			CHECK(ValidateUICheckbox(repairedCheckbox).empty());
		}
	}
}
