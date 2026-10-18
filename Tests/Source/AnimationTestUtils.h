#pragma once

#include <doctest/doctest.h>

#include <Basalt/Animation/AnimationPose.h>
#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/MeshImporter.h>

#include <glm/gtc/epsilon.hpp>

#include <filesystem>

// Helpers shared by the layer and IK tests, around the generated Biped fixture (Tests/Data/Animation).
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

}
