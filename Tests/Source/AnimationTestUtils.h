#pragma once

#include <doctest/doctest.h>

#include <Breccia/Animation/AnimationPose.h>
#include <Breccia/Animation/AnimationSystem.h>
#include <Breccia/Asset/AssetManager.h>
#include <Breccia/Asset/MeshImporter.h>
#include <Breccia/Core/FileSystem.h>
#include <Breccia/Math/Math.h>
#include <Breccia/Scene/Entity.h>
#include <Breccia/Scene/Scene.h>

#include <glm/gtc/epsilon.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

// Helpers shared by the layer, IK and spring bone tests, around the generated Biped fixture (Tests/Data/Animation).
namespace AnimationTest {

	inline std::filesystem::path BipedPath()
	{
		return std::filesystem::path(BRECCIA_TEST_DATA_DIR) / "Animation/Biped.gltf";
	}

	inline Breccia::Ref<Breccia::MeshSource> LoadModel(const std::filesystem::path& path)
	{
		std::string error;
		auto source = Breccia::MeshImporter::LoadGltf(path, path.string(), error);
		REQUIRE_MESSAGE(source, error);
		return source;
	}

	inline Breccia::Ref<Breccia::MeshSource> LoadBiped()
	{
		return LoadModel(BipedPath());
	}

	// Models loaded through Animators stay cached between tests otherwise.
	struct ClearAssetCache
	{
		~ClearAssetCache() { Breccia::AssetManager::Clear(); }
	};

	inline uint32_t Node(const Breccia::MeshSource& source, const std::string& name)
	{
		const int32_t node = Breccia::FindNode(source, name);
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

	// A fixture in Tests/Data/Animation.
	inline std::filesystem::path FixturePath(const char* name)
	{
		return std::filesystem::path(BRECCIA_TEST_DATA_DIR) / "Animation" / name;
	}

	inline nlohmann::json ReadJson(const std::filesystem::path& path)
	{
		std::ifstream stream(path);
		return nlohmann::json::parse(stream);
	}

	// Writes `data` as `name` in `directory` and imports it.
	inline Breccia::Ref<Breccia::MeshSource> LoadModified(const nlohmann::json& data, const std::filesystem::path& directory, const char* name)
	{
		const auto path = directory / name;
		REQUIRE(Breccia::FileSystem::WriteTextFile(path, data.dump()));
		return LoadModel(path);
	}

	// A clip of an imported model by name (const char*, so no temporary outlives the returned reference).
	inline const Breccia::AnimationClip& Clip(const Breccia::MeshSource& source, const char* name)
	{
		const auto clip = std::ranges::find(source.Animations, name, &Breccia::AnimationClip::Name);
		REQUIRE_MESSAGE(clip != source.Animations.end(), name);
		return *clip;
	}

	inline Breccia::AnimationClip& MutableClip(Breccia::MeshSource& source, const char* name)
	{
		const auto clip = std::ranges::find(source.Animations, name, &Breccia::AnimationClip::Name);
		REQUIRE_MESSAGE(clip != source.Animations.end(), name);
		return *clip;
	}

	// A node's model-space rotation in composed globals.
	inline glm::quat GlobalRotation(const std::vector<glm::mat4>& globals, uint32_t node)
	{
		glm::vec3 translation, scale;
		glm::quat rotation;
		REQUIRE(Breccia::Math::DecomposeAffineTransform(globals[node], translation, rotation, scale));
		return glm::normalize(rotation);
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
	inline glm::mat4 BoneWorldTransform(Breccia::Scene& scene, Breccia::Entity entity, const std::string& joint)
	{
		glm::mat4 bone(1.0f);
		std::string error;
		REQUIRE_MESSAGE(scene.GetAnimationSystem()->GetBoneTransform(entity, joint, bone, error), error);
		return scene.GetWorldTransform(entity) * bone;
	}

	inline glm::vec3 BoneWorld(Breccia::Scene& scene, Breccia::Entity entity, const std::string& joint)
	{
		return glm::vec3(BoneWorldTransform(scene, entity, joint)[3]);
	}

	// Runs the scene's animation (Update with LateUpdate) for `frames` frames of `seconds`.
	inline void RunAnimation(Breccia::Scene& scene, int frames, float seconds = 1.0f / 60.0f)
	{
		for (int i = 0; i < frames; i++)
			scene.GetAnimationSystem()->Update(seconds);
	}

}
