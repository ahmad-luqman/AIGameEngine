#pragma once

#include "Basalt/Scene/Entity.h"
#include "Basalt/UI/UIDrawCommand.h"
#include "Basalt/UI/UIGeometry.h"
#include "Basalt/UI/UIWidgets.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Basalt {

	class Scene;

	// One laid-out UI entity: its untransformed canvas box, composed canvas transform and inherited
	// canvas-aligned clip. A snapshot, valid until the scene changes. The entity's own child clip is inherited
	// only by its descendants, while scroll offsets are already included in their layout boxes.
	struct UILayoutItem
	{
		Entity Owner;
		UIBox Rect;
		glm::mat3 Transform = glm::mat3(1.0f);
		std::optional<UIBox> Clip;
		bool Contains(const glm::vec2& point) const { return (!Clip || Clip->Contains(point)) && ContainsUIBox(Rect, Transform, point); }
	};

	// A layout subtree omitted because its composed transform cannot be represented safely.
	struct UILayoutProblem
	{
		Entity Owner;
		std::string Message;
	};

	// What the interaction and the tools need to know about an entity's widget, from its repaired component.
	struct UIWidgetInfo
	{
		UIWidgetKind Kind = UIWidgetKind::Button;
		bool Interactable = true;
		// The component's Target as stored: 0 means the widget itself (see ResolveTarget).
		UUID Target = 0;
		// The callback's method name, empty for none (also when the stored name is not a method name: see
		// AuthoredCallback).
		std::string Callback;
		// The callback name as stored in the component.
		std::string AuthoredCallback;
		// Sliders only.
		float HandleWidth = 0.0f;
		// The entity has more than one widget component (it acts as this one alone).
		bool SeveralComponents = false;

		// The entity whose script the callback calls.
		UUID ResolveTarget(UUID widget) const { return Target != 0 ? Target : widget; }
	};

	// Lays out the scene's UI components (UIRect, UIImage, UIText and the widgets) on a canvas. Headless: the
	// editor overlay, UI.GetRect, the ui.layout command, the widgets' hit testing and the renderer all share it, so
	// they agree on every rect.
	class UILayout
	{
	public:
		// Whether any entity has a UI component (scenes without UI skip the hierarchy walk).
		static bool HasUI(Scene& scene);
		// Whether any entity has a widget component.
		static bool HasWidgets(Scene& scene);

		// Whether enabled scroll ancestors can reveal this item's currently clipped bounds. The map indexes
		// one Layout snapshot by entity UUID; fixed clipping ancestors still exclude unreachable controls.
		static bool CanReveal(const UILayoutItem& item, const std::unordered_map<uint64_t, const UILayoutItem*>& items);

		// Every non-hidden UI entity in depth-first draw order (parents first, later siblings on top), including
		// clipped entities. Rects inherit the nearest UI layout box; scroll views expand that box to ContentSize
		// and offset descendants. Affine transforms compose; child clips intersect transformed parent bounds.
		// Unusable composed transforms omit a subtree and optionally append a problem.
		static std::vector<UILayoutItem> Layout(Scene& scene, const glm::vec2& canvasSize, std::vector<UILayoutProblem>* problems = nullptr);

		// Appends images, widget geometry/text and focus rings in hierarchy order, then the open dropdown
		// popup on top. Commands carry composed transforms and intersect inherited clips with local text/row
		// clips. UIInteraction supplies runtime states; outside Play widgets are normal or disabled.
		static void BuildCommands(Scene& scene, const glm::vec2& canvasSize, std::vector<UIDrawCommand>& out);

		// The untransformed layout rect; nullopt without UI, when hidden by an ancestor, or when its
		// composed transform is unusable. Clipping alone does not remove a layout rect.
		static std::optional<UIBox> GetRect(Scene& scene, Entity entity, const glm::vec2& canvasSize);

		// The rect the entity is laid out in: its nearest UI ancestor's rect, or the canvas. Computed as if
		// every ancestor were visible (the inspector's anchor presets work on hidden rects too).
		static UIBox GetParentRect(Scene& scene, Entity entity, const glm::vec2& canvasSize);

		// First match: UIButton, UISlider, UICheckbox, UITextInput, UIDropdown, UIScrollView, then a
		// UIPanel with BlockInput. Several interactive controls warn once. Values come repaired (UIRules).
		static std::optional<UIWidgetInfo> GetWidget(Entity entity);
		// The kind GetWidget reports, without repairing or warning.
		static std::optional<UIWidgetKind> GetWidgetKind(Entity entity);
		// Number of interactive controls (button, slider, checkbox, text input, dropdown). Scroll views
		// and panels do not contribute; more than one control is a mistake.
		static int CountWidgetComponents(Entity entity);
	};

}
