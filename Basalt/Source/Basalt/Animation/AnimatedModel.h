#pragma once

#include "Basalt/Asset/MeshSource.h"

#include <functional>
#include <span>

namespace Basalt {
	class Entity;
	// Visits active mesh nodes in world space using the running pose or edit-mode rest pose, with the node's morph
	// weights (empty without morph targets; the node's defaults in edit mode). Palettes are shared per skin for this
	// visit; callback references expire when the call returns.
	void ForEachAnimatedMesh(Entity entity, const std::function<void(const Ref<MeshSource>&, const MeshNode&, const glm::mat4&, const std::vector<glm::mat4>&, std::span<const float>)>& visit);
	// World bounds used by editor picking, focus and selection.
	AABB AnimatedModelBounds(Entity entity);
	// The entity a BoneAttachment follows: its Target, or its parent when Target is 0; null when that is missing.
	Entity GetBoneAttachmentTarget(Entity entity);
	// Non-fatal authoring diagnostics, shared by the inspector and scene.info automation.
	std::vector<std::string> GetAnimationWarnings(Entity entity);
}
