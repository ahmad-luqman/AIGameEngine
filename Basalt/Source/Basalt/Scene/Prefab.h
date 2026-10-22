#pragma once

#include "Basalt/Scene/Entity.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Basalt {

	// Links prefab instances to their source file, merges later source edits into them and writes an instance's
	// overrides back (Apply). Merges are three-way: PrefabComponent::Baseline (the source as last merged), the
	// instance's current state and the new source, compared field by field through the component registry, so local
	// edits survive as overrides. Records are keyed by source entity ID and hold source-space entity references, which
	// keeps them comparable across instances. Functions that take an instance root require its PrefabComponent
	// (AssignSourceIds and EnclosingInstance excepted), and sourceEntities is always ReadSource output.
	class Prefab
	{
	public:
		enum class RefreshMode
		{
			// Takes source edits and keeps every override.
			Merge,
			// Replaces every member with the source; keeps the root's placement and parent and the entities the
			// instance added.
			RevertAll
		};

		enum class RefreshResult
		{
			// The instance already holds this source; nothing changed.
			Unchanged,
			Merged,
			// Refused before changing the scene (invalid merged data, a hierarchy cycle, a replaced root); outError
			// says why.
			Rejected,
			// Failed after changing the scene: an internal error, logged. The instance is partly merged and keeps its old
			// baseline, so merging again finishes the merge.
			Partial
		};

		static bool ChangedScene(RefreshResult result) { return result == RefreshResult::Merged || result == RefreshResult::Partial; }
		static bool Failed(RefreshResult result) { return result == RefreshResult::Rejected || result == RefreshResult::Partial; }

		// Checks a prefab file's structure only, not its component data: the format and version, unique nonzero IDs,
		// numeric parents and exactly one root, listed first. Nested prefab links are dropped: a prefab's entities are
		// never instances themselves.
		static bool ReadFile(const std::filesystem::path& path, nlohmann::json& outEntities, std::string& outError);
		// ReadFile, plus every component validated and rewritten as the serializer writes it (asset references as
		// paths, all fields present), so the records compare equal to unedited instance components.
		static bool ReadSource(const std::filesystem::path& path, nlohmann::json& outEntities, std::string& outError);

		// Gives a tree that is about to be saved as prefabPath (entities as SerializeEntityTree wrote them) its source
		// IDs, so the instances of an existing file keep merging. If the tree is an instance of that file, its members
		// keep their source IDs; the other entities reuse the file's IDs where they match it by name (as adoption
		// matches); the rest get fresh IDs that no scene entity has. Entity references inside the tree follow.
		// outWarnings says when an existing file could not be read, so its IDs are lost.
		static void AssignSourceIds(Entity root, const std::string& prefabPath, nlohmann::json& entities, std::vector<std::string>& outWarnings);
		// The instance root that tracks entity as one of its members, if any.
		static Entity EnclosingInstance(Entity entity);

		// Links an instance: a freshly instantiated tree, or the tree prefab.create just saved. instances[i] corresponds
		// to sourceEntities[i] and instances[0] is the root. Instances created while the scene runs record membership
		// only: play copies are discarded and never merge.
		static void Link(const std::vector<Entity>& instances, const nlohmann::json& sourceEntities, const std::string& prefabPath);
		// Brings tracking read from outside to its in-memory form: Lua hands IDs back as signed integers and saved
		// baselines hold UUID asset references. False when an asset reference is malformed or no project is active; a
		// UUID of a missing asset becomes the same opaque asset-uuid:// key component readers keep.
		static bool NormalizeTracking(PrefabComponent& link, std::string& outError);
		// Visits every asset value that a baseline's records declare (label: "<component>.<field>"), until visit
		// returns false; returns whether it ran to the end.
		static bool ForEachBaselineAssetValue(nlohmann::json& baseline, const std::function<bool(nlohmann::json& value, const nlohmann::json& record, const std::string& label)>& visit);

		// The instance's overrides: JSON-pointer paths into its source records ("/<source ID>" for a deleted entity,
		// "/<source ID>/Name", ".../Parent", ".../Components/<component>[/<field>...]") with the instance's value, null
		// where it removed something. The root's position and rotation are placement and never listed.
		static nlohmann::json ComputeOverrides(Entity root);
		// Splits an override path into its unescaped tokens.
		static std::vector<std::string> SplitOverridePath(const std::string& path);
		// Whether the instance has tracking (Members and Baseline); without it the next merge adopts it.
		static bool IsTracked(const PrefabComponent& link);
		// The normalized form PrefabComponent::Prefab holds (unresolved asset keys stay as they are).
		static std::string NormalizePath(const std::string& path);
		// Whether a tracked instance's baseline differs from the source: the source changed since its last merge.
		static bool IsOutdated(const PrefabComponent& link, const nlohmann::json& sourceEntities);

		// Edit mode only. Validates the merged data and hierarchy before changing the scene. Instances without source
		// tracking (saved before overrides existed) are adopted first: matching entities become members and their
		// differences from this source become overrides, so this source's changes do not reach them.
		static RefreshResult Refresh(Entity root, const nlohmann::json& sourceEntities, RefreshMode mode, std::string& outError);
		// Refresh that discards one override (a ComputeOverrides path) and keeps the others.
		static RefreshResult RevertOverride(Entity root, const nlohmann::json& sourceEntities, const std::string& path, std::string& outError);
		// Writes the instance's overrides into the prefab file. The root's placement, the entities the instance added
		// and references to entities outside the prefab stay local (outWarnings names such references).
		static bool Apply(Entity root, const std::filesystem::path& path, std::vector<std::string>& outWarnings, std::string& outError);
	};

}
