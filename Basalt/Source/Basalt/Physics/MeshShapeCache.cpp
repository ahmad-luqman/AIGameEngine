#include "Basalt/Physics/MeshShapeCache.h"

#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Physics/JoltUtils.h"

#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>

#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

namespace Basalt::PhysicsInternal {

	namespace {

		struct CookedMeshShape
		{
			std::weak_ptr<MeshSource> Source;
			// Unscaled shape in the mesh's local space; null when cooking failed.
			JPH::Ref<JPH::Shape> Shape;
			std::string Error;
		};

		using MeshShapeKey = std::tuple<std::string, uint32_t, bool>;
		std::mutex s_MeshShapeMutex;
		std::map<MeshShapeKey, CookedMeshShape> s_MeshShapes;
		uint64_t s_MeshShapeCookCount = 0;

		CookedMeshShape CookMeshShape(const Ref<MeshSource>& source, uint32_t meshIndex, bool convex)
		{
			CookedMeshShape cooked;
			cooked.Source = source;
			const MeshData& mesh = source->Meshes[meshIndex];
			if (convex)
			{
				// Only vertices the index buffer draws: a glTF accessor may hold unused ones outside the visible
				// geometry, which would otherwise inflate the hull.
				JPH::Array<JPH::Vec3> points;
				for (uint32_t submeshIndex : mesh.Submeshes)
				{
					const Submesh& submesh = source->Submeshes[submeshIndex];
					std::vector<bool> used(submesh.VertexCount, false);
					for (uint32_t i = 0; i < submesh.IndexCount; i++)
					{
						const uint32_t index = source->Indices[submesh.BaseIndex + i];
						if (index < submesh.VertexCount && !used[index])
						{
							used[index] = true;
							const glm::vec3& position = source->Vertices[submesh.BaseVertex + index].Position;
							if (!IsFiniteVec(position))
							{
								cooked.Error = "the mesh has a non-finite vertex position";
								return cooked;
							}
							points.push_back(ToJolt(position));
						}
					}
				}
				JPH::ShapeSettings::ShapeResult result = JPH::ConvexHullShapeSettings(points).Create();
				if (result.HasError())
					cooked.Error = std::string("cannot build a convex hull: ") + result.GetError().c_str();
				else
					cooked.Shape = result.Get();
				return cooked;
			}

			JPH::VertexList vertices;
			JPH::IndexedTriangleList triangles;
			for (uint32_t submeshIndex : mesh.Submeshes)
			{
				const Submesh& submesh = source->Submeshes[submeshIndex];
				const uint32_t base = static_cast<uint32_t>(vertices.size());
				for (uint32_t i = 0; i < submesh.VertexCount; i++)
				{
					const glm::vec3& position = source->Vertices[submesh.BaseVertex + i].Position;
					if (!IsFiniteVec(position))
					{
						cooked.Error = "the mesh has a non-finite vertex position";
						return cooked;
					}
					vertices.push_back(JPH::Float3(position.x, position.y, position.z));
				}
				for (uint32_t i = 0; i + 2 < submesh.IndexCount; i += 3)
				{
					const uint32_t* index = &source->Indices[submesh.BaseIndex + i];
					if (index[0] >= submesh.VertexCount || index[1] >= submesh.VertexCount || index[2] >= submesh.VertexCount)
						continue;
					triangles.push_back(JPH::IndexedTriangle(base + index[0], base + index[1], base + index[2]));
				}
			}
			if (triangles.empty())
			{
				cooked.Error = "the mesh has no triangles";
				return cooked;
			}
			JPH::ShapeSettings::ShapeResult result = JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create();
			if (result.HasError())
				cooked.Error = std::string("cannot build a triangle mesh: ") + result.GetError().c_str();
			else
				cooked.Shape = result.Get();
			return cooked;
		}

	}

	JPH::Ref<JPH::Shape> GetMeshShape(const std::string& key, uint32_t meshIndex, bool convex, std::string& outError)
	{
		const Ref<MeshSource> source = AssetManager::GetMesh(key);
		if (!source)
		{
			const std::string error = AssetManager::GetError(key);
			outError = "cannot load mesh '" + key + "'" + (error.empty() ? "" : ": " + error);
			return nullptr;
		}
		if (meshIndex >= source->Meshes.size())
		{
			outError = "mesh '" + key + "' has no mesh " + std::to_string(meshIndex) + " (it has " + std::to_string(source->Meshes.size()) + ")";
			return nullptr;
		}

		std::scoped_lock lock(s_MeshShapeMutex);
		auto [it, inserted] = s_MeshShapes.try_emplace(MeshShapeKey(key, meshIndex, convex));
		if (inserted || it->second.Source.lock() != source)
		{
			it->second = CookMeshShape(source, meshIndex, convex);
			s_MeshShapeCookCount++;
			// Entries of unloaded or reloaded meshes would otherwise keep their shapes alive until the
			// cache is cleared; new cooks are rare, so pruning here costs little. Only after cooking: a
			// just-inserted entry has no Source yet and would count as expired.
			std::erase_if(s_MeshShapes, [](const auto& entry) { return entry.second.Source.expired(); });
		}
		outError = it->second.Error;
		return it->second.Shape;
	}

	uint64_t GetMeshShapeCookCount()
	{
		std::scoped_lock lock(s_MeshShapeMutex);
		return s_MeshShapeCookCount;
	}

	void ClearMeshShapeCache()
	{
		std::scoped_lock lock(s_MeshShapeMutex);
		s_MeshShapes.clear();
	}

}
