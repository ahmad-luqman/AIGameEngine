#pragma once

#include "Basalt/Scene/Entity.h"
#include "Basalt/UI/UIDrawCommand.h"
#include "Basalt/UI/UIGeometry.h"
#include "Basalt/UI/UIWidgets.h"

#include <glm/glm.hpp>

#include <optional>
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

	// Lays out the scene's UI components (UIRect, UIImage, UIText and the widgets) on a canvas. Headless: the editor overlay,
	// UI.GetRect, the ui.layout command and the renderer all share it, so they agree on every rect.
	class UILayout
	{
	public:
		// Whether any entity has a UI component (scenes without UI skip the hierarchy walk).
		static bool HasUI(Scene& scene);

		// Every visible entity with a UI component, in draw order (depth-first in hierarchy order: parents
		// before children, later siblings after earlier ones). An entity without a UIRect occupies its parent's
		// rect; entities without UI components pass their parent's rect on to their children.
		static std::vector<UILayoutItem> Layout(Scene& scene, const glm::vec2& canvasSize);

		// Appends the draw commands of the scene's UI components in draw order (each entity's image, then its
		// widget, then its text). Widgets look the way the scene's UIInteraction says (normal when not playing).
		static void BuildCommands(Scene& scene, const glm::vec2& canvasSize, std::vector<UIDrawCommand>& out);

		// The entity's rect; nullopt when it has no UI component or it, or an ancestor, is hidden.
		static std::optional<UIBox> GetRect(Scene& scene, Entity entity, const glm::vec2& canvasSize);

		// The rect the entity is laid out in: its nearest UI ancestor's rect, or the canvas. Computed as if
		// every ancestor were visible (the inspector's anchor presets work on hidden rects too).
		static UIBox GetParentRect(Scene& scene, Entity entity, const glm::vec2& canvasSize);

		// The entity's widget: the first of its UIButton, UISlider and UICheckbox (an entity with several acts as
		// that one alone). nullopt when it has none.
		static std::optional<UIWidgetKind> GetWidgetKind(Entity entity);
		// How many widget components the entity has (more than one is a mistake ui.layout reports).
		static int CountWidgetComponents(Entity entity);
	};

}
