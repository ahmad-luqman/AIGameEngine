#pragma once

#include "Basalt/Scene/Entity.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Basalt {

	// Links prefab instances to their source file, merges later source edits into them and writes an instance's
	// overrides back (Apply). Merges are three-way: PrefabComponent::Baseline (the source as last merged), the
	// instance's current state and the new source, compared field by field through the component registry, so local
	// edits survive as overrides. Records are keyed by source entity ID and hold source-space entity references, which
	// keeps them comparable across instances. Functions that take an instance root require its PrefabComponent
	// (AssignSourceIds, EnclosingInstance and SaveTree excepted), and sourceEntities is always ReadSource output.
	//
	// A prefab file may hold nested instances: a record whose Prefab component names another prefab stands for that
	// prefab's whole tree, and its Overrides (a JSON merge patch keyed by the entities' IDs in this file) hold only what
	// differs. A variant is a prefab whose root is such a record. Reading expands them into flat records, so instances
	// merge through the same three-way path and an inner prefab's edit reaches every instance of the prefabs holding it
	// without rewriting those files. An instance's root owns all of its flat records; the roots of the nested instances
	// inside it keep their Prefab component as a marker with the path alone, which says what is nested and survives
	// unpacking. Only while a prefab is open, the outermost nested instances are tracked like any scene's.
	class Prefab
	{
	public:
		// A chain of prefabs nested in or based on each other deeper than this is rejected.
		static constexpr size_t MaxNestingDepth = 32;
		// Expanding one file stops here: a few small files nesting each other many times would otherwise multiply.
		static constexpr size_t MaxExpandedEntities = 100000;

		struct NestedInstance
		{
			// The nested root's ID in the file.
			uint64_t Entity = 0;
			std::string Prefab;
		};

		// What reading a prefab file found besides its entities.
		struct ReadReport
		{
			// The file's own nested instances. The root's is the variant's base, also in Base.
			std::vector<NestedInstance> Nested;
			std::string Base;
			// Every prefab file reading went through (FileKey), including one that failed: this file's source changes
			// with any of them.
			std::set<std::string> Files;
			// Overrides of entities their prefab no longer has, and entities moved to the root because their parent
			// is gone. Reading goes on without them.
			std::vector<std::string> Warnings;
		};

		// The form ReadReport::Files holds a file in.
		static std::string FileKey(const std::filesystem::path& absolutePath);
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
		// numeric parents and exactly one root, listed first. Nested instances are expanded into flat records (their
		// roots keep a Prefab marker with the path alone), so the result is what instantiating creates. Fails on a
		// missing prefab, a prefab that holds itself, or past MaxNestingDepth or MaxExpandedEntities.
		static bool ReadFile(const std::filesystem::path& path, nlohmann::json& outEntities, std::string& outError, ReadReport* outReport = nullptr);
		// ReadFile on text already in memory; name is for messages.
		static bool ReadText(std::string_view text, const std::string& name, nlohmann::json& outEntities, std::string& outError, ReadReport* outReport = nullptr);
		// ReadFile, plus every component validated and rewritten as the serializer writes it (asset references as
		// paths, all fields present), so the records compare equal to unedited instance components.
		static bool ReadSource(const std::filesystem::path& path, nlohmann::json& outEntities, std::string& outError, ReadReport* outReport = nullptr);
		// That last step of ReadSource, on ReadFile output.
		static bool Normalize(nlohmann::json& entities, const std::string& name, std::string& outError);

		// Flat records (in the file's IDs) as a file holds them: each outermost nested instance becomes one record with
		// its overrides, and the rest stay whole. The root's Prefab component stays only for a variant. Fails when a
		// nested prefab cannot be read or holds the file at path.
		static bool ToFileEntities(nlohmann::json entities, const std::filesystem::path& path, bool variant, nlohmann::json& outEntities, std::string& outError);
		// Writes a tree whose entity IDs are the file's own (an open prefab, say), keeping its nested instances, and with
		// variant the root's link as the variant's base. Instances added since get their entities' file IDs.
		static bool SaveTree(Entity root, const std::filesystem::path& path, bool variant, std::vector<std::string>& outWarnings, std::string& outError);
		// Writes a new variant of a prefab: its root is an instance of base without any override.
		static bool CreateVariant(const std::string& basePath, const std::filesystem::path& path, std::string& outError);
		// Tracks the outermost nested instances of a scene just made from ReadSource records, each against its own
		// prefab, so an open prefab lists their overrides and takes their prefabs' edits. A nested prefab that cannot be
		// read stays a marker and is reported in outWarnings.
		static void TrackNestedInstances(Scene& scene, std::vector<std::string>& outWarnings);
		// Starts tracking the nested instances that became top-level when root stopped being an instance (Unpack): what
		// differs from their prefabs becomes their overrides. Those that cannot be read stay markers, named in outWarnings.
		static void AdoptNestedInstances(Entity root, std::vector<std::string>& outWarnings);

		// Gives a tree that is about to be saved as prefabPath (entities as SerializeEntityTree wrote them) its source
		// IDs, so the instances of an existing file keep merging. If the tree is an instance of that file, its members
		// keep their source IDs; the other entities reuse the file's IDs where they match it by name (as adoption
		// matches); the rest get fresh IDs that no scene entity has. Nested instances' entities take the IDs their nested
		// root's ID derives. Entity references inside the tree follow. inOutVariant asks for the root to stay an instance
		// of its prefab, as a variant; it turns true when the tree is an instance of an existing variant at prefabPath,
		// which keeps its base. outWarnings says when an existing file could not be read, so its IDs are lost.
		static void AssignSourceIds(Entity root, const std::string& prefabPath, nlohmann::json& entities, bool& inOutVariant, std::vector<std::string>& outWarnings);
		// The instance root that tracks entity as one of its members, if any. A nested instance's root has one: it
		// merges through that instance and is never merged on its own.
		static Entity EnclosingInstance(Entity entity);

		// Links an instance: a freshly instantiated tree, or the tree prefab.create just saved. instances[i] corresponds
		// to sourceEntities[i] and instances[0] is the root, which now tracks everything: nested instances inside become
		// markers. Instances created while the scene runs record membership only: play copies are discarded and never merge.
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
