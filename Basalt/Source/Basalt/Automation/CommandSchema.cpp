#include "Basalt/Automation/CommandSchema.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace Basalt::CommandSchema {

	using json = nlohmann::json;

	namespace {

		// Compare signed/unsigned integer bounds exactly, including on Apple arm64 and MSVC
		// where long double has double precision. Mixed floating-point values use host precision.
		bool Less(const json& a, const json& b)
		{
			if (a.is_number_integer() && b.is_number_integer())
			{
				if (a.is_number_unsigned() && !b.is_number_unsigned())
					return b.get<int64_t>() >= 0 && a.get<uint64_t>() < static_cast<uint64_t>(b.get<int64_t>());
				if (!a.is_number_unsigned() && b.is_number_unsigned())
					return a.get<int64_t>() < 0 || static_cast<uint64_t>(a.get<int64_t>()) < b.get<uint64_t>();
				return a < b;
			}
			return a.get<long double>() < b.get<long double>();
		}

	}

	json String(const std::string& description, bool nonEmpty)
	{
		json schema = { { "type", "string" }, { "description", description } };
		if (nonEmpty)
			schema["minLength"] = 1;
		return schema;
	}

	json Boolean(const std::string& description)
	{
		return { { "type", "boolean" }, { "description", description } };
	}

	json Integer(const std::string& description, int64_t minimum, uint64_t maximum)
	{
		return { { "type", "integer" }, { "description", description }, { "minimum", minimum }, { "maximum", maximum } };
	}

	json Number(const std::string& description, double minimum, double maximum)
	{
		return { { "type", "number" }, { "description", description }, { "minimum", minimum }, { "maximum", maximum } };
	}

	json Vector(const std::string& description, size_t size)
	{
		return { { "type", "array" }, { "description", description }, { "items", Number("", -std::numeric_limits<float>::max(), std::numeric_limits<float>::max()) }, { "minItems", size }, { "maxItems", size } };
	}

	json Array(const std::string& description, json items)
	{
		return { { "type", "array" }, { "description", description }, { "items", std::move(items) } };
	}

	json Entity(const std::string& description)
	{
		return { { "description", description }, { "anyOf", json::array({ String("", true), Integer("", 0, UINT64_MAX) }) } };
	}

	json Nullable(json schema)
	{
		const std::string description = schema.value("description", "");
		return { { "description", description }, { "anyOf", json::array({ std::move(schema), json{ { "type", "null" } } }) } };
	}

	json Object(json properties, std::initializer_list<const char*> required)
	{
		return { { "type", "object" }, { "properties", std::move(properties) }, { "required", required }, { "additionalProperties", false } };
	}

	bool CheckDefinition(const json& schema, std::string& outError)
	{
		const auto fail = [&](const std::string& message) { outError = message; return false; };
		if (!schema.is_object())
			return fail("command schema must be an object");
		static const std::set<std::string> s_Keywords = { "type", "description", "properties", "required", "additionalProperties", "items", "minItems", "maxItems", "minLength", "minimum", "maximum", "exclusiveMinimum", "enum", "anyOf" };
		for (const auto& [key, value] : schema.items())
		{
			if (!s_Keywords.contains(key))
				return fail("unsupported command schema keyword: " + key);
			if (key == "properties")
			{
				if (!value.is_object())
					return fail("schema properties must be an object");
				for (const auto& child : value)
					if (!CheckDefinition(child, outError))
						return false;
			}
			else if (key == "items")
			{
				if (!CheckDefinition(value, outError))
					return false;
			}
			else if (key == "anyOf")
			{
				if (!value.is_array() || value.empty())
					return fail("schema anyOf must be a non-empty array");
				for (const auto& child : value)
					if (!CheckDefinition(child, outError))
						return false;
			}
			else if (key == "type")
			{
				static const std::set<std::string> s_Types = { "object", "array", "string", "integer", "number", "boolean", "null" };
				if (!value.is_string() || !s_Types.contains(value.get<std::string>()))
					return fail("unsupported command schema type");
			}
			else if (key == "required")
			{
				if (!value.is_array())
					return fail("schema required must be an array");
				for (const auto& name : value)
				{
					if (!name.is_string() || !schema.contains("properties") || !schema["properties"].contains(name.get<std::string>()))
						return fail("required property missing from schema");
				}
			}
			else if (key == "additionalProperties" && !value.is_boolean())
				return fail("schema additionalProperties must be boolean");
			else if (key == "description" && !value.is_string())
				return fail("schema description must be a string");
			else if (key == "enum" && (!value.is_array() || value.empty()))
				return fail("schema enum must be a non-empty array");
			else if ((key == "minimum" || key == "maximum" || key == "exclusiveMinimum") && (!value.is_number() || !std::isfinite(value.get<double>())))
				return fail("schema numeric bound must be finite");
			else if ((key == "minItems" || key == "maxItems" || key == "minLength") && (!value.is_number_integer() || value.get<double>() < 0))
				return fail("schema size bound must be a non-negative integer");
		}
		if (schema.contains("minimum") && schema.contains("maximum") && Less(schema["maximum"], schema["minimum"]))
			return fail("schema minimum exceeds maximum");
		if (schema.contains("exclusiveMinimum") && schema.contains("maximum") && !Less(schema["exclusiveMinimum"], schema["maximum"]))
			return fail("schema exclusiveMinimum must be below maximum");
		if (schema.contains("minItems") && schema.contains("maxItems") && Less(schema["maxItems"], schema["minItems"]))
			return fail("schema minItems exceeds maxItems");
		if (schema.contains("enum"))
		{
			json constraints = schema;
			constraints.erase("enum");
			for (const auto& value : schema["enum"])
			{
				std::string error;
				if (!Validate(constraints, value, error))
					return fail("schema enum value violates its constraints: " + error);
			}
		}
		return true;
	}

	bool Validate(const json& schema, const json& value, std::string& outError, const std::string& path)
	{
		const auto fail = [&](const std::string& reason) { outError = "parameter '" + path + "' " + reason; return false; };
		if (schema.contains("anyOf"))
		{
			bool matched = false;
			std::string ignored;
			std::string errors;
			for (const auto& choice : schema["anyOf"])
			{
				matched = Validate(choice, value, ignored, path);
				if (matched)
					break;
				if (!errors.empty())
					errors += "; or ";
				errors += ignored;
			}
			if (!matched)
			{
				outError = errors;
				return false;
			}
		}
		const std::string type = schema.value("type", "");
		// Integer parameters intentionally require an integer JSON token: handlers consume int64/uint64.
		if ((type == "object" && !value.is_object()) || (type == "array" && !value.is_array()) || (type == "string" && !value.is_string()) || (type == "integer" && !value.is_number_integer()) || (type == "number" && !value.is_number()) || (type == "boolean" && !value.is_boolean()) || (type == "null" && !value.is_null()))
			return fail(type == "boolean" ? "must be true or false" : type == "string" && schema.value("minLength", 0) > 0 ? "must be a non-empty string"
																														   : "must be " + std::string(type == "integer" || type == "object" || type == "array" ? "an " : "a ") + type);
		if (schema.contains("enum"))
		{
			const auto& values = schema["enum"];
			if (std::find(values.begin(), values.end(), value) == values.end())
				return fail("must be one of " + values.dump());
		}
		if (value.is_number())
		{
			if (!std::isfinite(value.get<double>()))
				return fail("must be finite");
			if (schema.contains("minimum") && Less(value, schema["minimum"]))
				return fail("must be in [" + schema["minimum"].dump() + ", " + schema.value("maximum", json("unbounded")).dump() + "]");
			if (schema.contains("maximum") && Less(schema["maximum"], value))
				return fail("must be in [" + schema.value("minimum", json("unbounded")).dump() + ", " + schema["maximum"].dump() + "]");
			if (schema.contains("exclusiveMinimum") && !Less(schema["exclusiveMinimum"], value))
				return fail("must be > " + schema["exclusiveMinimum"].dump());
		}
		if (value.is_string() && schema.contains("minLength"))
		{
			const auto& text = value.get_ref<const std::string&>();
			const auto length = static_cast<size_t>(std::count_if(text.begin(), text.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; }));
			if (length < schema["minLength"].get<size_t>())
				return fail(schema["minLength"] == 1 ? "must be a non-empty string" : "is shorter than minLength");
		}
		if (value.is_object())
		{
			for (const auto& name : schema.value("required", json::array()))
			{
				if (!value.contains(name.get<std::string>()))
				{
					outError = "missing parameter '" + (path == "params" ? name.get<std::string>() : path + "." + name.get<std::string>()) + "'";
					return false;
				}
			}
			static const json s_EmptyProperties = json::object();
			const json& properties = schema.contains("properties") ? schema["properties"] : s_EmptyProperties;
			for (const auto& [key, child] : value.items())
			{
				if (properties.contains(key))
				{
					std::string childPath = path == "params" ? key : std::string(path).append(".").append(key);
					if (!Validate(properties[key], child, outError, childPath))
						return false;
				}
				else if (!schema.value("additionalProperties", true))
				{
					outError = "unknown parameter '";
					outError.append(key).append("' in ").append(path);
					return false;
				}
			}
		}
		if (value.is_array())
		{
			if ((schema.contains("minItems") && value.size() < schema["minItems"].get<size_t>()) || (schema.contains("maxItems") && value.size() > schema["maxItems"].get<size_t>()))
				return fail("has an invalid array length");
			if (schema.contains("items"))
			{
				for (size_t i = 0; i < value.size(); ++i)
					if (!Validate(schema["items"], value[i], outError, path + "[" + std::to_string(i) + "]"))
						return false;
			}
		}
		return true;
	}
}
