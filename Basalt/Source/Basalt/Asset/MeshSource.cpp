#include "Basalt/Asset/MeshSource.h"

#include <glm/gtc/constants.hpp>

namespace Basalt {

	uint32_t MeshSource::AddSubmesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, uint32_t materialIndex)
	{
		Submesh submesh;
		submesh.BaseVertex = static_cast<uint32_t>(Vertices.size());
		submesh.BaseIndex = static_cast<uint32_t>(Indices.size());
		submesh.IndexCount = static_cast<uint32_t>(indices.size());
		submesh.VertexCount = static_cast<uint32_t>(vertices.size());
		submesh.MaterialIndex = materialIndex;
		for (const Vertex& vertex : vertices)
			submesh.Bounds.Expand(vertex.Position);

		Vertices.insert(Vertices.end(), vertices.begin(), vertices.end());
		Indices.insert(Indices.end(), indices.begin(), indices.end());
		Submeshes.push_back(submesh);
		return static_cast<uint32_t>(Submeshes.size() - 1);
	}

	void MeshSource::UpdateBounds()
	{
		Bounds = {};
		for (MeshData& mesh : Meshes)
		{
			mesh.Bounds = {};
			for (uint32_t submesh : mesh.Submeshes)
				mesh.Bounds.Expand(Submeshes[submesh].Bounds);
			Bounds.Expand(mesh.Bounds);
		}
	}

	void MeshSource::GenerateNormals(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
	{
		std::vector<glm::vec3> normals(vertices.size(), glm::vec3(0.0f));
		for (size_t i = 0; i + 2 < indices.size(); i += 3)
		{
			const uint32_t a = indices[i];
			const uint32_t b = indices[i + 1];
			const uint32_t c = indices[i + 2];
			if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size())
				continue;
			// Area-weighted face normal.
			const glm::vec3 normal = glm::cross(vertices[b].Position - vertices[a].Position, vertices[c].Position - vertices[a].Position);
			normals[a] += normal;
			normals[b] += normal;
			normals[c] += normal;
		}
		for (size_t i = 0; i < vertices.size(); i++)
		{
			const float length = glm::length(normals[i]);
			vertices[i].Normal = length > 1e-12f ? normals[i] / length : glm::vec3(0.0f, 1.0f, 0.0f);
		}
	}

	void MeshSource::GenerateTangents(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
	{
		std::vector<glm::vec3> tangents(vertices.size(), glm::vec3(0.0f));
		std::vector<glm::vec3> bitangents(vertices.size(), glm::vec3(0.0f));

		for (size_t i = 0; i + 2 < indices.size(); i += 3)
		{
			const uint32_t ia = indices[i];
			const uint32_t ib = indices[i + 1];
			const uint32_t ic = indices[i + 2];
			if (ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size())
				continue;
			const Vertex& a = vertices[ia];
			const Vertex& b = vertices[ib];
			const Vertex& c = vertices[ic];

			const glm::vec3 edge1 = b.Position - a.Position;
			const glm::vec3 edge2 = c.Position - a.Position;
			const glm::vec2 deltaUV1 = b.TexCoord - a.TexCoord;
			const glm::vec2 deltaUV2 = c.TexCoord - a.TexCoord;
			const float determinant = deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y;
			if (glm::abs(determinant) < 1e-12f)
				continue;
			const float inverse = 1.0f / determinant;
			const glm::vec3 tangent = (edge1 * deltaUV2.y - edge2 * deltaUV1.y) * inverse;
			const glm::vec3 bitangent = (edge2 * deltaUV1.x - edge1 * deltaUV2.x) * inverse;

			for (uint32_t index : { ia, ib, ic })
			{
				tangents[index] += tangent;
				bitangents[index] += bitangent;
			}
		}

		for (size_t i = 0; i < vertices.size(); i++)
		{
			const glm::vec3 normal = vertices[i].Normal;
			// Gram-Schmidt orthogonalize; fall back to any perpendicular vector for degenerate UVs.
			glm::vec3 tangent = tangents[i] - normal * glm::dot(normal, tangents[i]);
			if (glm::length(tangent) < 1e-6f)
			{
				const glm::vec3 axis = glm::abs(normal.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
				tangent = glm::cross(normal, axis);
			}
			tangent = glm::normalize(tangent);
			// glTF: bitangent = cross(N, T) * w must point towards +V in *image* space (up), i.e. against
			// increasing texture v, so w is negative when cross(N, T) agrees with dP/dv.
			const float sign = glm::dot(glm::cross(normal, tangent), bitangents[i]) < 0.0f ? 1.0f : -1.0f;
			vertices[i].Tangent = glm::vec4(tangent, sign);
		}
	}

	namespace Primitives {

		namespace {

			Ref<MeshSource> MakeSingleMesh(const std::string& key, const std::string& name, std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
			{
				MeshSource::GenerateTangents(vertices, indices);
				Ref<MeshSource> source = CreateRef<MeshSource>();
				source->Key = key;
				source->Materials.push_back(MaterialData{ .Name = "Default" });
				const uint32_t submesh = source->AddSubmesh(vertices, indices, 0);
				source->Meshes.push_back(MeshData{ name, { submesh }, {} });
				source->Nodes.push_back(MeshNode{ .Name = name, .MeshIndex = 0 });
				source->UpdateBounds();
				return source;
			}

			// Ring of vertices around Y at height y, used by cylinders and capsules.
			void AddRing(std::vector<Vertex>& vertices, uint32_t segments, float radius, float y, const glm::vec3& normalScale, float normalY, float v)
			{
				for (uint32_t i = 0; i <= segments; i++)
				{
					const float u = static_cast<float>(i) / static_cast<float>(segments);
					const float angle = u * glm::two_pi<float>();
					const glm::vec3 direction(glm::cos(angle), 0.0f, glm::sin(angle));
					Vertex vertex;
					vertex.Position = direction * radius + glm::vec3(0.0f, y, 0.0f);
					vertex.Normal = glm::normalize(direction * normalScale + glm::vec3(0.0f, normalY, 0.0f));
					vertex.TexCoord = { u, v };
					vertices.push_back(vertex);
				}
			}

			void ConnectRings(std::vector<uint32_t>& indices, uint32_t firstRing, uint32_t ringCount, uint32_t segments)
			{
				const uint32_t stride = segments + 1;
				for (uint32_t ring = 0; ring + 1 < ringCount; ring++)
				{
					for (uint32_t i = 0; i < segments; i++)
					{
						const uint32_t a = firstRing + ring * stride + i;
						const uint32_t b = a + stride;
						indices.insert(indices.end(), { a, a + 1, b, b, a + 1, b + 1 });
					}
				}
			}

		}

		Ref<MeshSource> CreateCube()
		{
			std::vector<Vertex> vertices;
			std::vector<uint32_t> indices;
			const glm::vec3 normals[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
			for (const glm::vec3& normal : normals)
			{
				// Two axes spanning the face, chosen so the winding is counter-clockwise seen from outside.
				const glm::vec3 up = glm::abs(normal.y) > 0.5f ? glm::vec3(0.0f, 0.0f, -normal.y) : glm::vec3(0.0f, 1.0f, 0.0f);
				const glm::vec3 right = glm::cross(up, normal);
				const uint32_t base = static_cast<uint32_t>(vertices.size());
				const glm::vec2 corners[4] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
				for (const glm::vec2& corner : corners)
				{
					Vertex vertex;
					vertex.Position = 0.5f * (normal + right * corner.x + up * corner.y);
					vertex.Normal = normal;
					vertex.TexCoord = { (corner.x + 1.0f) * 0.5f, 1.0f - (corner.y + 1.0f) * 0.5f };
					vertices.push_back(vertex);
				}
				indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
			}
			return MakeSingleMesh("builtin://Cube", "Cube", vertices, indices);
		}

		Ref<MeshSource> CreateSphere(uint32_t segments, uint32_t rings)
		{
			std::vector<Vertex> vertices;
			std::vector<uint32_t> indices;
			for (uint32_t ring = 0; ring <= rings; ring++)
			{
				const float v = static_cast<float>(ring) / static_cast<float>(rings);
				const float phi = v * glm::pi<float>();
				const float y = glm::cos(phi);
				const float radius = glm::sin(phi);
				AddRing(vertices, segments, 0.5f * radius, 0.5f * y, glm::vec3(radius), y, v);
				// Exact pole normals.
				if (ring == 0 || ring == rings)
				{
					for (size_t i = vertices.size() - (segments + 1); i < vertices.size(); i++)
						vertices[i].Normal = { 0.0f, y > 0.0f ? 1.0f : -1.0f, 0.0f };
				}
			}
			ConnectRings(indices, 0, rings + 1, segments);
			return MakeSingleMesh("builtin://Sphere", "Sphere", vertices, indices);
		}

		Ref<MeshSource> CreatePlane()
		{
			std::vector<Vertex> vertices(4);
			const glm::vec2 corners[4] = { { -0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.5f, -0.5f }, { -0.5f, -0.5f } };
			for (int i = 0; i < 4; i++)
			{
				vertices[i].Position = { corners[i].x, 0.0f, corners[i].y };
				vertices[i].Normal = { 0.0f, 1.0f, 0.0f };
				vertices[i].TexCoord = { corners[i].x + 0.5f, corners[i].y + 0.5f };
			}
			const std::vector<uint32_t> indices = { 0, 1, 2, 0, 2, 3 };
			return MakeSingleMesh("builtin://Plane", "Plane", vertices, indices);
		}

		Ref<MeshSource> CreateCylinder(uint32_t segments)
		{
			std::vector<Vertex> vertices;
			std::vector<uint32_t> indices;
			AddRing(vertices, segments, 0.5f, 0.5f, glm::vec3(1.0f), 0.0f, 0.0f);
			AddRing(vertices, segments, 0.5f, -0.5f, glm::vec3(1.0f), 0.0f, 1.0f);
			ConnectRings(indices, 0, 2, segments);

			// Caps: a center vertex and a ring with a flat normal each.
			for (float side : { 1.0f, -1.0f })
			{
				const uint32_t center = static_cast<uint32_t>(vertices.size());
				Vertex centerVertex;
				centerVertex.Position = { 0.0f, 0.5f * side, 0.0f };
				centerVertex.Normal = { 0.0f, side, 0.0f };
				centerVertex.TexCoord = { 0.5f, 0.5f };
				vertices.push_back(centerVertex);
				for (uint32_t i = 0; i <= segments; i++)
				{
					const float angle = static_cast<float>(i) / static_cast<float>(segments) * glm::two_pi<float>();
					Vertex vertex;
					vertex.Position = { 0.5f * glm::cos(angle), 0.5f * side, 0.5f * glm::sin(angle) };
					vertex.Normal = { 0.0f, side, 0.0f };
					vertex.TexCoord = { 0.5f + 0.5f * glm::cos(angle), 0.5f + 0.5f * glm::sin(angle) };
					vertices.push_back(vertex);
				}
				for (uint32_t i = 0; i < segments; i++)
				{
					if (side > 0.0f)
						indices.insert(indices.end(), { center, center + 2 + i, center + 1 + i });
					else
						indices.insert(indices.end(), { center, center + 1 + i, center + 2 + i });
				}
			}
			return MakeSingleMesh("builtin://Cylinder", "Cylinder", vertices, indices);
		}

		Ref<MeshSource> CreateCapsule(uint32_t segments, uint32_t rings)
		{
			constexpr float Radius = 0.5f;
			constexpr float HalfHeight = 0.5f;
			std::vector<Vertex> vertices;
			std::vector<uint32_t> indices;

			// Top hemisphere, cylinder band, bottom hemisphere: one continuous ring strip.
			const uint32_t totalRings = 2 * (rings + 1);
			for (uint32_t ring = 0; ring < totalRings; ring++)
			{
				const bool top = ring <= rings;
				const uint32_t local = top ? ring : ring - (rings + 1);
				const float phi = (top ? 0.0f : glm::half_pi<float>()) + static_cast<float>(local) / static_cast<float>(rings) * glm::half_pi<float>();
				const float y = glm::cos(phi);
				const float radius = glm::sin(phi);
				const float center = top ? HalfHeight : -HalfHeight;
				const float v = static_cast<float>(ring) / static_cast<float>(totalRings - 1);
				AddRing(vertices, segments, Radius * radius, center + Radius * y, glm::vec3(radius), y, v);
			}
			ConnectRings(indices, 0, totalRings, segments);
			return MakeSingleMesh("builtin://Capsule", "Capsule", vertices, indices);
		}

		Ref<MeshSource> CreateFromKey(const std::string& key)
		{
			if (key == "builtin://Cube")
				return CreateCube();
			if (key == "builtin://Sphere")
				return CreateSphere();
			if (key == "builtin://Plane")
				return CreatePlane();
			if (key == "builtin://Cylinder")
				return CreateCylinder();
			if (key == "builtin://Capsule")
				return CreateCapsule();
			return nullptr;
		}

		bool IsBuiltinKey(const std::string& key)
		{
			return key.rfind("builtin://", 0) == 0;
		}

	}

}
