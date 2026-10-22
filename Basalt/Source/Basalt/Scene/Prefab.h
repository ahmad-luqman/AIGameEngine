#pragma once

#include "Basalt/Scene/Entity.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace Basalt {

	// Links prefab instances to their source file and merges later source edits into them. The merge is three-way:
	// PrefabComponent::Baseline (the source as last merged), the instance's current state and the new source, compared
	// field by field through the component registry, so local edits survive as overrides. Records are keyed by source
	// entity ID and hold source-space entity references, which keeps them comparable across instances.
	class Prefab
	{
	public:
		enum class RefreshMode
		{
			// Takes source edits and keeps every override.
			Merge,
			// Discards every override except the root's placement.
			RevertAll,
			// Discards one override path (as prefab.overrides reports it) and merges the rest.
			RevertField
		};

		// Reads a prefab file without validating component data: the format, one root first and nonzero unique IDs.
		// Nested prefab links are dropped, since a prefab's entities are never instances themselves.
		static bool ReadFile(const std::filesystem::path& path, nlohmann::json& outEntities, std::string& outError);
		// ReadFile, plus every component validated and rewritten as the serializer writes it (asset references as
		// paths, all fields present), so the records compare equal to unedited instance components.
		static bool ReadSource(const std::filesystem::path& path, nlohmann::json& outEntities, std::string& outError);

		// Links a newly created instance; instances[i] was created from sourceEntities[i]. Instances created while the
		// scene runs record membership only: play copies are discarded and never merge.
		static void Link(Entity root, const nlohmann::json& sourceEntities, const std::vector<Entity>& instances, const std::string& prefabPath);
		// Recomputes PrefabComponent::Overrides from the instance's current state.
		static void CaptureOverrides(Entity root);
		// Asset references saved as UUID objects become paths, the form instance components compare in.
		static void ResolveBaselineAssets(PrefabComponent& link);

		// Edit mode only. Validates the whole result before changing the scene, so a failure leaves it unchanged.
		// Instances without source tracking (saved before overrides existed) are adopted first: matching entities
		// become members and their differences from the source become overrides.
		static bool Refresh(Entity root, const nlohmann::json& sourceEntities, RefreshMode mode, std::string& outError, const std::string& field = {});
		// Writes the instance's overrides into the prefab file, except the root's placement and local added entities.
		static bool Apply(Entity root, const std::filesystem::path& path, std::string& outError);
	};

}
