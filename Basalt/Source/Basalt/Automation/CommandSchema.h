#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>

namespace Basalt::CommandSchema {

	// Builders for the JSON Schema subset used by automation commands. Optional properties are omitted
	// from Object's required list; Nullable preserves the legacy optional-parameter null fallback.
	nlohmann::json String(const std::string& description = "", bool nonEmpty = false);
	nlohmann::json Boolean(const std::string& description = "");
	nlohmann::json Integer(const std::string& description, int64_t minimum, uint64_t maximum);
	nlohmann::json Number(const std::string& description, double minimum, double maximum);
	nlohmann::json Vector(const std::string& description, size_t size);
	nlohmann::json Entity(const std::string& description);
	nlohmann::json Nullable(nlohmann::json schema);
	nlohmann::json Object(nlohmann::json properties = nlohmann::json::object(), std::initializer_list<const char*> required = {});

	// Reject unsupported schema keywords at registration rather than silently skipping constraints.
	void CheckDefinition(const nlohmann::json& schema);
	// Returns an actionable parameter path on failure. Does not coerce values or insert defaults.
	bool Validate(const nlohmann::json& schema, const nlohmann::json& value, std::string& outError, const std::string& path = "params");

}
