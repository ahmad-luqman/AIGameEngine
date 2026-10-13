#pragma once

#include "Basalt/Scene/Entity.h"
#include "Basalt/UI/UIDrawCommand.h"
#include "Basalt/UI/UIGeometry.h"
#include "Basalt/UI/UIWidgets.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	class Scene;

	// An entity with UI components and the rect it occupies on the canvas: a snapshot, valid until the scene
	// changes.
	struct UILayoutItem
	{
		Entity Owner;
		UIBox Rect;
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

		// Every visible entity with a UI component, in draw order (depth-first in hierarchy order: parents
		// before children, later siblings after earlier ones). An entity without a UIRect occupies its parent's
		// rect; entities without UI components pass their parent's rect on to their children.
		static std::vector<UILayoutItem> Layout(Scene& scene, const glm::vec2& canvasSize);

		// Appends the draw commands of the scene's UI components in draw order (each entity's image, then its
		// widget, then its text). Widgets look the way the scene's UIInteraction says (when not playing: normal, or
		// disabled when not Interactable).
		static void BuildCommands(Scene& scene, const glm::vec2& canvasSize, std::vector<UIDrawCommand>& out);

		// The entity's rect; nullopt when it has no UI component or it, or an ancestor, is hidden.
		static std::optional<UIBox> GetRect(Scene& scene, Entity entity, const glm::vec2& canvasSize);

		// The rect the entity is laid out in: its nearest UI ancestor's rect, or the canvas. Computed as if
		// every ancestor were visible (the inspector's anchor presets work on hidden rects too).
		static UIBox GetParentRect(Scene& scene, Entity entity, const glm::vec2& canvasSize);

		// The entity's widget: the first of its UIButton, UISlider and UICheckbox (an entity with several acts as
		// that one alone, and the layout warns once). nullopt when it has none. Values come repaired (UIRules).
		static std::optional<UIWidgetInfo> GetWidget(Entity entity);
		// The kind GetWidget reports, without repairing or warning.
		static std::optional<UIWidgetKind> GetWidgetKind(Entity entity);
		// How many widget components the entity has (more than one is a mistake).
		static int CountWidgetComponents(Entity entity);
	};

}
