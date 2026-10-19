#include "UIRectHandles.h"

#include <doctest/doctest.h>

using namespace Basalt;

TEST_CASE("UI rect resize keeps the opposite corner fixed under rotation and scale")
{
	UIRectComponent rect;
	rect.AnchorMin = rect.AnchorMax = { 0.0f, 0.0f };
	rect.Position = { 200.0f, 150.0f };
	rect.Size = { 100.0f, 80.0f };
	rect.Pivot = { 0.3f, 0.8f };
	rect.Rotation = 0.7f;
	rect.Scale = { 2.0f, 0.5f };
	const UIBox parent{ { 0.0f, 0.0f }, { 800.0f, 600.0f } };
	const UIBox initialBox = ComputeUIRect(parent, rect);
	for (const glm::vec2 handle : { glm::vec2(0, 0), glm::vec2(1, 1), glm::vec2(0, 1), glm::vec2(1, 0) })
	{
		const auto edited = ResizeUIRect(rect, initialBox, handle, { 30.0f, -10.0f });
		const UIBox editedBox = ComputeUIRect(parent, edited);
		const glm::vec2 opposite = glm::vec2(1.0f) - handle;
		const glm::vec2 before = glm::vec2(ComputeUITransform(initialBox, rect) * glm::vec3(initialBox.Min + initialBox.Size * opposite, 1.0f));
		const glm::vec2 after = glm::vec2(ComputeUITransform(editedBox, edited) * glm::vec3(editedBox.Min + editedBox.Size * opposite, 1.0f));
		CHECK(after.x == doctest::Approx(before.x));
		CHECK(after.y == doctest::Approx(before.y));
	}
}

TEST_CASE("UI rect edge resize preserves the other axis and prevents inversion")
{
	UIRectComponent rect;
	rect.AnchorMin = { 0.0f, 0.0f };
	rect.AnchorMax = { 1.0f, 0.0f };
	rect.Size = { -20.0f, 50.0f };
	const UIBox parent{ { 0.0f, 0.0f }, { 800.0f, 600.0f } };
	const UIBox initialBox = ComputeUIRect(parent, rect);
	const auto edited = ResizeUIRect(rect, initialBox, { 0.0f, 0.5f }, { 1000.0f, 900.0f });
	const UIBox box = ComputeUIRect(parent, edited);
	CHECK(box.Size.x == doctest::Approx(1.0f));
	CHECK(box.Size.y == doctest::Approx(50.0f));
	CHECK(edited.Position.y == rect.Position.y);
	CHECK(box.Max().x == doctest::Approx(initialBox.Max().x));
}
