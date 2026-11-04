#pragma once

#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Basalt {

	// Links prefab instances to their source file, merges later source edits into them and writes an instance's
	// overrides back (Apply). Merges are three-way: PrefabComponent::Baseline (the source as last merged), the
	// instance's current state and the new source, compared field by field through the component registry, so local
	// edits survive as overrides. Records are keyed by source entity ID and hold source-space entity references, which
	// keeps them comparable across instances. Functions that take an instance root require its PrefabComponent
	// (AssignSourceIds, EnclosingInstance, SaveTree, MergeOutdatedNested and AdoptNestedInstances excepted), and
	// sourceEntities is ReadSource output (Link takes any records of the tree).
	//
	// A prefab file may hold nested instances: a record whose Prefab component names another prefab stands for that
	// prefab's whole tree, and its Overrides (a JSON merge patch keyed by the entities' IDs in this file) hold only what
	// differs. A variant is a prefab whose root is such a record. Reading expands them into flat records, so instances
	// merge through the same three-way path and an inner prefab's edit reaches the tracked instances of the prefabs holding
	// it without rewriting those files. An instance's root owns all of its flat records; the roots of the nested instances
	// inside it keep their Prefab component as a marker with the path alone, which says what is nested and survives
	// unpacking. While they stay nested, only an open prefab tracks its outermost ones, like any scene's instances.
	class Prefab
	{
	public:
		// A chain of prefabs nested in or based on each other deeper than this is rejected.
		static constexpr size_t MaxNestingDepth = 32;
		// Expanding one file stops once its nested records, counted at every level, pass this: a few small files nesting
		// each other many times would otherwise multiply. The file's own records do not count.
		static constexpr size_t MaxExpandedEntities = 100000;
		// The first format version whose files can nest. A Prefab component in an older file, or one without a version, is
		// ignored, with a warning.
		static constexpr int NestingVersion = 4;

		struct NestedInstance
		{
			// The nested root's ID in the file.
			uint64_t Id = 0;
			std::string Prefab;
		};

		// What reading a prefab file found besides its entities. A read starts it over. After a failed read, the rest is what
		// was found before the failure.
		struct ReadReport
		{
			// The nested instances the file holds directly. The root's is the variant's base, also in Base.
			std::vector<NestedInstance> Nested;
			std::string Base;
			// Every prefab file reading reached, read or not, this one included, as FileKey -> name: this file's source
			// changes with any of them. A failed read names the file it stopped at, and none after it.
			std::map<std::string, std::string> Files;
			// What goes on working without being what the file says: overrides of entities their prefab no longer has,
			// entities moved to the root because their parent is gone, fields a nested record ignores, records replacing
			// a nested prefab's, and Prefab components ignored in a file too old to nest.
			std::vector<std::string> Warnings;
		};

		// A prefab read for several uses at once: an operation over many instances reads each file once.
		struct Source
		{
			// FileKey of the file.
			std::string Key;
			bool Readable = false;
			// ReadSource's result; Error says why when Readable is false.
			nlohmann::json Records;
			std::string Error;
			ReadReport Report;
		};

		// Lives for one operation, so it never outlasts a change to a file.
		class SourceCache
		{
		public:
			const Source& Read(const std::string& projectPath);

		private:
			std::unordered_map<std::string, Source> m_Sources;
		};

		// What ReadReport::Files is keyed by: one spelling per file, whichever spelled it. Ignores ASCII case on Windows and
		// macOS, whose disks usually do.
		static std::string FileKey(const std::filesystem::path& absolutePath);
		static std::string FileKeyOf(const std::string& projectPath);
		static bool SameFile(const std::string& projectPath, const std::string& otherPath);

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
		// missing prefab, a prefab that holds itself, a link that is malformed or leaves the project, a malformed
		// override, or past MaxNestingDepth or MaxExpandedEntities.
		static bool ReadFile(const std::filesystem::path& path, nlohmann::json& outEntities, std::string& outError, ReadReport* outReport = nullptr);
		// ReadFile on text that is about to be, or stands for, the file at path: a nested link back to that path is a cycle.
		static bool ReadText(std::string_view text, const std::filesystem::path& path, nlohmann::json& outEntities, std::string& outError, ReadReport* outReport = nullptr);
		// ReadFile, plus every component validated and rewritten as the serializer writes it (asset references as
		// paths, all fields present), so the records compare equal to unedited instance components.
		static bool ReadSource(const std::filesystem::path& path, nlohmann::json& outEntities, std::string& outError, ReadReport* outReport = nullptr);
		// That last step of ReadSource, on ReadFile output. name is for messages.
		static bool Normalize(nlohmann::json& entities, const std::string& name, std::string& outError);

		// Flat records (in the file's IDs) as a file holds them: each outermost nested instance becomes one record with
		// its overrides, and the rest stay whole. The root's Prefab component stays only for a variant, whose root's position
		// and rotation are never written: they follow the base. Fails when a nested prefab cannot be read or holds the file
		// at path. Differences from what the nested prefabs say now become overrides, so a tree must have merged them first
		// (MergeOutdatedNested).
		static bool ToFileEntities(nlohmann::json entities, const std::filesystem::path& path, PrefabFileRoot rootKind, nlohmann::json& outEntities, std::vector<std::string>& outWarnings, std::string& outError);
		// Catches up the tree about to be saved as prefabPath with what its prefabs say now: its tracked instances, the root
		// (unless it is a plain prefab written over itself), and the instance around it. Reads everything first, so a prefab
		// that cannot be read leaves the scene as it was. Sets outChanged when the scene changed, even on failure, and root
		// may be gone after that. A running scene cannot merge, so what it would have is in outWarnings. False when an
		// instance cannot read or merge its prefab.
		static bool MergeOutdatedNested(Entity root, const std::string& prefabPath, PrefabFileRoot rootKind, bool& outChanged, std::vector<std::string>& outWarnings, std::string& outError);
		// Writes a tree whose entity IDs are the file's own (an open prefab, say), keeping its nested instances, and for a
		// variant the root's link as its base. Instances added since get their entities' file IDs.
		static bool SaveTree(Entity root, const std::filesystem::path& path, PrefabFileRoot rootKind, std::vector<std::string>& outWarnings, std::string& outError);
		// Writes a new variant of a prefab: its root is an instance of base without any override.
		static bool CreateVariant(const std::string& basePath, const std::filesystem::path& path, std::vector<std::string>& outWarnings, std::string& outError);
		// Tracks the instances of a scene just made from ReadSource records against their own prefabs: the outermost nested
		// ones, and those added under their entities. An open prefab then lists their overrides and takes their prefabs'
		// edits. A nested prefab that cannot be read stays a marker and is reported in outWarnings.
		static void TrackNestedInstances(Scene& scene, std::vector<std::string>& outWarnings);
		// Makes an instance ordinary entities. A nested root's link goes, which is an override of the instance holding it.
		// An outer instance first catches up with its prefab, then its nested instances become instances of their own: its
		// link says exactly which entities each holds, and what differs from their prefabs becomes their overrides. Those
		// that cannot be read stay markers. outWarnings names them.
		static void Unpack(Entity root, std::vector<std::string>& outWarnings);

		// Gives a tree that is about to be saved as prefabPath (entities as SerializeEntityTree wrote them) its source
		// IDs, so the instances of an existing file keep merging. If the tree is an instance of that file, its members
		// keep their source IDs; the other entities reuse the file's IDs where they match it by name (as adoption
		// matches); the rest get fresh IDs that no scene entity has. Nested instances' entities take the IDs their nested
		// root's ID derives. Entity references inside the tree follow. requested Variant keeps the root an instance of its
		// prefab. Returns what the root is: an instance of an existing variant at prefabPath stays one. Nothing when that
		// instance's file cannot be read (it may be a variant). outWarnings says when another existing file could not be
		// read, so its IDs are lost, and what nested instances without tracking lost.
		static std::optional<PrefabFileRoot> AssignSourceIds(Entity root, const std::string& prefabPath, nlohmann::json& entities, PrefabFileRoot requested, std::vector<std::string>& outWarnings, std::string& outError);
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
		// Whether the instance has tracking (Members and Baseline); without it the next merge of its own prefab adopts it.
		static bool IsTracked(const PrefabComponent& link);
		// Whether a Prefab component tracks entities itself, rather than being a nested root or untracked. Instances created
		// during Play do, without being IsTracked.
		static bool ListsMembers(const PrefabComponent& link);
		// What a tree saved from root is: a root that is an instance makes a variant.
		static PrefabFileRoot FileRootOf(Entity root);
		// The normalized form PrefabComponent::Prefab holds (unresolved asset keys stay as they are).
		static std::string NormalizePath(const std::string& path);
		// Whether a tracked instance's baseline differs from the source: the source changed since its last merge.
		static bool IsOutdated(const PrefabComponent& link, const nlohmann::json& sourceEntities);

		// Edit mode only. Validates the merged data and hierarchy before changing the scene. Instances without source
		// tracking (saved before overrides existed, or copied out of an instance) are adopted first: matching entities
		// become members and their differences from this source become overrides, so this source's changes do not reach
		// them. A nested instance is rejected: it merges with the instance holding it. outWarnings says which of the
		// instance's own entities moved because the merge removed their parent.
		static RefreshResult Refresh(Entity root, const nlohmann::json& sourceEntities, RefreshMode mode, std::string& outError, std::vector<std::string>* outWarnings = nullptr);
		// Refresh that discards one override (a ComputeOverrides path) and keeps the others.
		static RefreshResult RevertOverride(Entity root, const nlohmann::json& sourceEntities, const std::string& path, std::string& outError, std::vector<std::string>* outWarnings = nullptr);
		// Writes the instance's overrides into the prefab file. The root's placement, the entities the instance added
		// and references to entities outside the prefab stay local (outWarnings names such references).
		static bool Apply(Entity root, const std::filesystem::path& path, std::vector<std::string>& outWarnings, std::string& outError);
	};

}
