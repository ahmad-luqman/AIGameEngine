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

TEST_CASE("UI rect gestures invert inherited nonuniform transforms and preserve their pivot")
{
	UIRectComponent rect;
	rect.Position = { 40.0f, -30.0f };
	rect.Pivot = { 0.2f, 0.8f };
	rect.Rotation = 0.2f;
	rect.Scale = { 2.0f, 0.75f };
	const UIBox parent{ { 100, 50 }, { 800, 600 } };
	const UIBox box = ComputeUIRect(parent, rect);
	glm::mat3 inherited(1.0f);
	const float c = std::cos(0.7f), s = std::sin(0.7f);
	inherited[0] = { c * 3.0f, s * 3.0f, 0.0f };
	inherited[1] = { -s * 0.4f, c * 0.4f, 0.0f };
	inherited[2] = { 80.0f, -45.0f, 1.0f };
	const glm::vec2 pivot = box.Min + box.Size * rect.Pivot;
	const auto canvas = [&](const glm::vec2& point) { return glm::vec2(inherited * glm::vec3(point, 1.0f)); };
	const glm::vec2 start = canvas(pivot + glm::vec2(80, 0));
	const auto moved = TransformUIRect(rect, box, inherited, start, canvas(pivot + glm::vec2(100, -15)), GizmoOperation::Translate);
	CHECK(moved.Position.x == doctest::Approx(60.0f));
	CHECK(moved.Position.y == doctest::Approx(-45.0f));
	const auto movedSnapped = TransformUIRect(rect, box, inherited, start, canvas(pivot + glm::vec2(103, -16)), GizmoOperation::Translate, 10.0f);
	CHECK(movedSnapped.Position.x == doctest::Approx(60.0f));
	CHECK(movedSnapped.Position.y == doctest::Approx(-50.0f));
	const auto rotated = TransformUIRect(rect, box, inherited, start, canvas(pivot + glm::vec2(0, 80)), GizmoOperation::Rotate);
	CHECK(rotated.Rotation == doctest::Approx(rect.Rotation + glm::radians(90.0f)));
	CHECK(rotated.Position == rect.Position);
	CHECK(rotated.Pivot == rect.Pivot);
	const float angle = glm::radians(37.0f);
	const auto rotatedSnapped = TransformUIRect(rect, box, inherited, start, canvas(pivot + glm::vec2(std::cos(angle), std::sin(angle)) * 80.0f), GizmoOperation::Rotate, 15.0f);
	CHECK(glm::degrees(rotatedSnapped.Rotation) == doctest::Approx(45.0f));
	const auto scaled = TransformUIRect(rect, box, inherited, start, canvas(pivot + glm::vec2(160, 0)), GizmoOperation::Scale);
	CHECK(scaled.Scale.x == doctest::Approx(4.0f));
	CHECK(scaled.Scale.y == doctest::Approx(1.5f));
	CHECK(scaled.Position == rect.Position);
	const auto scaledSnapped = TransformUIRect(rect, box, inherited, start, canvas(pivot + glm::vec2(192, 0)), GizmoOperation::Scale, 0.5f);
	CHECK(scaledSnapped.Scale.x == doctest::Approx(5.0f));
	CHECK(scaledSnapped.Scale.y == doctest::Approx(2.0f));
	CHECK(TransformUIRect(rect, box, inherited, canvas(pivot), start, GizmoOperation::Scale) == rect);
	CHECK(TransformUIRect(rect, box, inherited, canvas(pivot), start, GizmoOperation::Rotate) == rect);
}

TEST_CASE("UI rect snapped resize preserves the opposite corner through inherited transforms")
{
	UIRectComponent rect;
	rect.Size = { 200, 120 };
	rect.Pivot = { 0.3f, 0.8f };
	rect.Rotation = 0.6f;
	rect.Scale = { 1.5f, 0.4f };
	const UIBox parent{ { 0, 0 }, { 800, 600 } };
	const UIBox box = ComputeUIRect(parent, rect);
	glm::mat3 inherited(1.0f);
	inherited[0] = { 2.0f, 0.5f, 0.0f };
	inherited[1] = { -0.2f, 0.75f, 0.0f };
	const glm::mat3 transform = inherited * ComputeUITransform(box, rect);
	const glm::vec2 start = glm::vec2(transform * glm::vec3(box.Max(), 1.0f));
	const glm::vec2 end = glm::vec2(transform * glm::vec3(box.Max() + glm::vec2(23, 16), 1.0f));
	const auto edited = TransformUIRect(rect, box, inherited, start, end, GizmoOperation::Translate, 10.0f, glm::vec2(1.0f));
	CHECK(edited.Size.x == doctest::Approx(220.0f));
	CHECK(edited.Size.y == doctest::Approx(140.0f));
	const UIBox editedBox = ComputeUIRect(parent, edited);
	const glm::vec2 fixed = glm::vec2(transform * glm::vec3(box.Min, 1.0f));
	const glm::vec2 after = glm::vec2(inherited * ComputeUITransform(editedBox, edited) * glm::vec3(editedBox.Min, 1.0f));
	CHECK(after.x == doctest::Approx(fixed.x));
	CHECK(after.y == doctest::Approx(fixed.y));
}
