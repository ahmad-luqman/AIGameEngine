#pragma once

#include <doctest/doctest.h>

#include <Basalt/Animation/AnimationPose.h>
#include <Basalt/Animation/AnimationSystem.h>
#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/MeshImporter.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

#include <glm/gtc/epsilon.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>

// Helpers shared by the layer, IK and spring bone tests, around the generated Biped fixture (Tests/Data/Animation).
namespace AnimationTest {

	inline std::filesystem::path BipedPath()
	{
		return std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/Biped.gltf";
	}

	inline Basalt::Ref<Basalt::MeshSource> LoadModel(const std::filesystem::path& path)
	{
		std::string error;
		auto source = Basalt::MeshImporter::LoadGltf(path, path.string(), error);
		REQUIRE_MESSAGE(source, error);
		return source;
	}

	inline Basalt::Ref<Basalt::MeshSource> LoadBiped()
	{
		return LoadModel(BipedPath());
	}

	// Models loaded through Animators stay cached between tests otherwise.
	struct ClearAssetCache
	{
		~ClearAssetCache() { Basalt::AssetManager::Clear(); }
	};

	inline uint32_t Node(const Basalt::MeshSource& source, const std::string& name)
	{
		const int32_t node = Basalt::FindNode(source, name);
		REQUIRE_MESSAGE(node >= 0, name);
		return static_cast<uint32_t>(node);
	}

	inline bool NearPose(const std::vector<glm::mat4>& a, const std::vector<glm::mat4>& b, float epsilon = 1e-4f)
	{
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); i++)
			for (int c = 0; c < 4; c++)
				if (!glm::all(glm::epsilonEqual(a[i][c], b[i][c], epsilon)))
					return false;
		return true;
	}

	// The same rotation (q and -q are one rotation).
	inline bool NearRotation(const glm::quat& a, const glm::quat& b, float epsilon = 1e-4f)
	{
		return std::abs(std::abs(glm::dot(a, b)) - 1.0f) < epsilon;
	}

	inline glm::quat AxisAngle(const glm::vec3& axis, float degrees)
	{
		return glm::angleAxis(glm::radians(degrees), axis);
	}

	// The angle between two directions, in degrees.
	inline float Degrees(const glm::vec3& a, const glm::vec3& b)
	{
		return glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0f, 1.0f)));
	}

	inline bool Near(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-4f)
	{
		return glm::all(glm::epsilonEqual(a, b, epsilon));
	}

	// A joint's world transform in the running scene's current pose.
	inline glm::mat4 BoneWorldTransform(Basalt::Scene& scene, Basalt::Entity entity, const std::string& joint)
	{
		glm::mat4 bone(1.0f);
		std::string error;
		REQUIRE_MESSAGE(scene.GetAnimationSystem()->GetBoneTransform(entity, joint, bone, error), error);
		return scene.GetWorldTransform(entity) * bone;
	}

	inline glm::vec3 BoneWorld(Basalt::Scene& scene, Basalt::Entity entity, const std::string& joint)
	{
		return glm::vec3(BoneWorldTransform(scene, entity, joint)[3]);
	}

	// Runs the scene's animation (Update with LateUpdate) for `frames` frames of `seconds`.
	inline void RunAnimation(Basalt::Scene& scene, int frames, float seconds = 1.0f / 60.0f)
	{
		for (int i = 0; i < frames; i++)
			scene.GetAnimationSystem()->Update(seconds);
	}

}
