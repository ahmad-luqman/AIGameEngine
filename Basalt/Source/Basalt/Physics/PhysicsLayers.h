#pragma once

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Basalt {

	// Named collision layers and the matrix of which layer pairs collide, shared by every scene of a
	// project. Layer 0 is always "Default"; every pair collides until SetCollides turns it off. The matrix
	// is kept symmetric, so ShouldCollide(a, b) == ShouldCollide(b, a). Headless: PhysicsWorld maps the
	// indices onto Jolt object layers.
	class PhysicsLayers
	{
	public:
		static constexpr uint32_t MaxLayers = 32;
		static constexpr std::string_view DefaultLayerName = "Default";

		PhysicsLayers();

		const std::vector<std::string>& GetNames() const { return m_Names; }
		uint32_t GetCount() const { return static_cast<uint32_t>(m_Names.size()); }
		std::optional<uint32_t> Find(std::string_view name) const;

		// Appends a layer that collides with every layer. Fails on an empty or duplicate name, or when all
		// MaxLayers are in use.
		bool Add(const std::string& name, std::string& outError);

		// Indices come from Find, so an index past GetCount() is a programmer error (asserted).
		bool ShouldCollide(uint32_t a, uint32_t b) const;
		void SetCollides(uint32_t a, uint32_t b, bool collides);
		// Bit i is set when the layer collides with layer i; only bits of defined layers are ever set, and an
		// undefined layer's mask is 0 (PhysicsWorld reads all MaxLayers rows).
		uint32_t GetCollisionMask(uint32_t layer) const;

		// Bit mask with one bit per named layer, for query filters. Fails on an unknown name.
		std::optional<uint32_t> MaskFromNames(const std::vector<std::string>& names, std::string& outError) const;

		// {"Names": [...], "IgnoredPairs": [["A", "B"], ...]}: only pairs that do not collide are stored,
		// so the file stays short and readable.
		nlohmann::json ToJson() const;
		// Strict: rejects unknown keys, a first name other than "Default", duplicate or empty names, more
		// than MaxLayers names and pairs that name an unknown layer. Both keys are optional (no Names means
		// only Default). A pair may name one layer twice (it then ignores itself); repeated pairs are harmless.
		static std::optional<PhysicsLayers> FromJson(const nlohmann::json& data, std::string& outError);

		// Same names and the same matrix between them.
		bool operator==(const PhysicsLayers& other) const;

	private:
		std::vector<std::string> m_Names;
		std::array<uint32_t, MaxLayers> m_Masks;
	};

}
