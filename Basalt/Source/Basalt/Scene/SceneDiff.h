#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	struct SceneDiffOptions
	{
		// Floats at most this far apart count as equal (physics rarely lands on the same float twice); integers compare
		// exactly.
		double Tolerance = 0.0;
	};

	// Compares two scene states in the scene file format (SceneSerializer::SerializeScene, .bscene files) the way a
	// test reads them: entities are matched by ID (play mode keeps the edit scene's IDs, so a snapshot taken before
	// Play matches the playing scene), then their names, parents and component fields are compared, and so are the
	// scene's own settings (Name, Renderer, Physics; Format and Version are ignored). Returns
	//   { equal, settings: [change], added: [{id, name}], removed: [{id, name}], changed: [{id, name, changes: [change]}],
	//     reordered }
	// where a change is { path, from, to }: a path such as "Transform.Translation", "Name" or "Renderer.Exposure", or
	// a component name whose from is null (component added) or whose to is null (component removed); reordered is true
	// when the entities both states share appear in a different order (sibling and UI draw order). Fails (nullopt,
	// outError) when an input is not a scene object, or an entity lacks a numeric ID or repeats one.
	std::optional<nlohmann::json> DiffScenes(const nlohmann::json& from, const nlohmann::json& to, const SceneDiffOptions& options, std::string& outError);

	// One line per difference in a DiffScenes result, at most maxLines (then a count of the rest), for messages.
	std::string DescribeSceneDiff(const nlohmann::json& diff, size_t maxLines = 10);

	// Whether two JSON values are equal: floats within the tolerance (>= 0), integers exactly (64-bit IDs), arrays and
	// objects element-wise.
	bool JsonNearlyEqual(const nlohmann::json& a, const nlohmann::json& b, double tolerance);

	// Checks that `actual` contains `expected`: an expected object's keys must all be present (actual may have more)
	// with matching values, arrays must have the same length and match element by element, values as JsonNearlyEqual
	// compares them. Appends "<path> is <actual>, expected <expected>" (or "<path> is missing")
	// per mismatch; `path` prefixes them.
	void MatchJsonSubset(const nlohmann::json& actual, const nlohmann::json& expected, double tolerance, const std::string& path, std::vector<std::string>& outMismatches);
	// Whether MatchJsonSubset would find no mismatch, without building any message: stops at the first difference and
	// allocates nothing, for callers that test many values (events.poll's patterns, under the event hub's lock).
	bool JsonContainsSubset(const nlohmann::json& actual, const nlohmann::json& expected, double tolerance);

}
