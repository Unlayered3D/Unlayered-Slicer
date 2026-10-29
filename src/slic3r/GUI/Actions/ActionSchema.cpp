#include "ActionSchema.hpp"

#include <cmath>
#include <set>

namespace Slic3r { namespace GUI { namespace Actions {

using json = nlohmann::json;

namespace {

const std::set<std::string>& allowed_keywords()
{
    static const std::set<std::string> k = {"type",      "enum",      "minimum",  "maximum",    "minLength",
                                            "maxLength", "minItems",  "maxItems", "items",      "properties",
                                            "required",  "additionalProperties", "description", "default",
                                            "x-exactlyOne"};
    return k;
}

const std::set<std::string>& allowed_types()
{
    static const std::set<std::string> t = {"object", "string", "number", "integer", "boolean", "array"};
    return t;
}

bool fail(SchemaError& err, const std::string& path, const std::string& message, const std::string& needs = {})
{
    err.path    = path.empty() ? "/" : path;
    err.message = message;
    err.needs   = needs;
    return false;
}

bool is_integer_value(const json& v)
{
    if (v.is_number_integer())
        return true;
    if (v.is_number_float()) {
        const double d = v.get<double>();
        return std::isfinite(d) && std::floor(d) == d && std::fabs(d) < 9.007199254740992e15;
    }
    return false;
}

bool type_matches(const std::string& type, const json& v)
{
    if (type == "object") return v.is_object();
    if (type == "string") return v.is_string();
    if (type == "boolean") return v.is_boolean();
    if (type == "array") return v.is_array();
    if (type == "integer") return is_integer_value(v);
    if (type == "number") return v.is_number() && std::isfinite(v.get<double>());
    return false;
}

bool is_nonneg_int(const json& v) { return v.is_number_integer() && v.get<long long>() >= 0; }

} // namespace

bool check_schema(const json& schema, SchemaError& err, const std::string& path)
{
    if (!schema.is_object())
        return fail(err, path, "a schema must be an object");
    for (auto it = schema.begin(); it != schema.end(); ++it)
        if (allowed_keywords().count(it.key()) == 0)
            return fail(err, path + "/" + it.key(), "keyword '" + it.key() + "' is outside the dialect");
    if (!schema.contains("type") || !schema["type"].is_string() || allowed_types().count(schema["type"].get<std::string>()) == 0)
        return fail(err, path + "/type", "type must be one of object|string|number|integer|boolean|array");
    const std::string type = schema["type"].get<std::string>();

    if (schema.contains("description") && !schema["description"].is_string())
        return fail(err, path + "/description", "description must be a string");
    if (schema.contains("enum")) {
        const json& e = schema["enum"];
        if (!e.is_array() || e.empty())
            return fail(err, path + "/enum", "enum must be a non-empty array");
        for (size_t i = 0; i < e.size(); ++i)
            if (!type_matches(type, e[i]))
                return fail(err, path + "/enum/" + std::to_string(i), "enum value does not match type");
    }
    for (const char* k : {"minimum", "maximum"})
        if (schema.contains(k)) {
            if (type != "number" && type != "integer")
                return fail(err, path + "/" + k, std::string(k) + " applies to numbers only");
            if (!schema[k].is_number())
                return fail(err, path + "/" + k, std::string(k) + " must be a number");
        }
    for (const char* k : {"minLength", "maxLength"})
        if (schema.contains(k)) {
            if (type != "string")
                return fail(err, path + "/" + k, std::string(k) + " applies to strings only");
            if (!is_nonneg_int(schema[k]))
                return fail(err, path + "/" + k, std::string(k) + " must be a non-negative integer");
        }
    for (const char* k : {"minItems", "maxItems"})
        if (schema.contains(k)) {
            if (type != "array")
                return fail(err, path + "/" + k, std::string(k) + " applies to arrays only");
            if (!is_nonneg_int(schema[k]))
                return fail(err, path + "/" + k, std::string(k) + " must be a non-negative integer");
        }
    if (type == "array") {
        if (!schema.contains("items"))
            return fail(err, path + "/items", "an array schema needs items");
        if (!check_schema(schema["items"], err, path + "/items"))
            return false;
    } else if (schema.contains("items")) {
        return fail(err, path + "/items", "items applies to arrays only");
    }

    if (type == "object") {
        if (!schema.contains("additionalProperties") || schema["additionalProperties"] != false)
            return fail(err, path + "/additionalProperties", "every object schema needs additionalProperties: false");
        const json props = schema.contains("properties") ? schema["properties"] : json::object();
        if (!props.is_object())
            return fail(err, path + "/properties", "properties must be an object");
        for (auto it = props.begin(); it != props.end(); ++it)
            if (!check_schema(it.value(), err, path + "/properties/" + it.key()))
                return false;
        if (schema.contains("required")) {
            const json& req = schema["required"];
            if (!req.is_array())
                return fail(err, path + "/required", "required must be an array");
            for (size_t i = 0; i < req.size(); ++i)
                if (!req[i].is_string() || !props.contains(req[i].get<std::string>()))
                    return fail(err, path + "/required/" + std::to_string(i), "required names an undeclared property");
        }
        if (schema.contains("x-exactlyOne")) {
            const json& groups = schema["x-exactlyOne"];
            if (!groups.is_array() || groups.size() < 2)
                return fail(err, path + "/x-exactlyOne", "x-exactlyOne must list at least two groups");
            for (size_t g = 0; g < groups.size(); ++g) {
                if (!groups[g].is_array() || groups[g].empty())
                    return fail(err, path + "/x-exactlyOne/" + std::to_string(g), "each group is a non-empty array of names");
                for (const json& n : groups[g])
                    if (!n.is_string() || !props.contains(n.get<std::string>()))
                        return fail(err, path + "/x-exactlyOne/" + std::to_string(g), "group names an undeclared property");
            }
        }
    } else {
        for (const char* k : {"properties", "required", "additionalProperties", "x-exactlyOne"})
            if (schema.contains(k))
                return fail(err, path + "/" + k, std::string(k) + " applies to objects only");
    }

    if (schema.contains("default")) {
        SchemaError inner;
        if (!validate(schema, schema["default"], inner, path + "/default"))
            return fail(err, path + "/default", "default does not validate: " + inner.message);
    }
    return true;
}

bool validate(const json& schema, const json& value, SchemaError& err, const std::string& path)
{
    const std::string type = schema.value("type", "");
    if (!type_matches(type, value))
        return fail(err, path, "expected " + type);

    if (schema.contains("enum")) {
        bool found = false;
        for (const json& e : schema["enum"])
            if (e == value || (e.is_number() && value.is_number() && e.get<double>() == value.get<double>())) {
                found = true;
                break;
            }
        if (!found)
            return fail(err, path, "must be one of " + schema["enum"].dump());
    }
    if (type == "number" || type == "integer") {
        const double d = value.get<double>();
        if (schema.contains("minimum") && d < schema["minimum"].get<double>())
            return fail(err, path, "must be >= " + schema["minimum"].dump());
        if (schema.contains("maximum") && d > schema["maximum"].get<double>())
            return fail(err, path, "must be <= " + schema["maximum"].dump());
    } else if (type == "string") {
        // Lengths in UTF-8 code points, as JSON Schema counts them.
        size_t n = 0;
        for (unsigned char c : value.get_ref<const std::string&>())
            if ((c & 0xC0) != 0x80)
                ++n;
        if (schema.contains("minLength") && n < schema["minLength"].get<size_t>())
            return fail(err, path, "shorter than " + schema["minLength"].dump() + " characters");
        if (schema.contains("maxLength") && n > schema["maxLength"].get<size_t>())
            return fail(err, path, "longer than " + schema["maxLength"].dump() + " characters");
    } else if (type == "array") {
        if (schema.contains("minItems") && value.size() < schema["minItems"].get<size_t>())
            return fail(err, path, "needs at least " + schema["minItems"].dump() + " items");
        if (schema.contains("maxItems") && value.size() > schema["maxItems"].get<size_t>())
            return fail(err, path, "allows at most " + schema["maxItems"].dump() + " items");
        for (size_t i = 0; i < value.size(); ++i)
            if (!validate(schema["items"], value[i], err, path + "/" + std::to_string(i)))
                return false;
    } else if (type == "object") {
        const json props = schema.contains("properties") ? schema["properties"] : json::object();
        for (auto it = value.begin(); it != value.end(); ++it)
            if (!props.contains(it.key()))
                return fail(err, path + "/" + it.key(), "unknown property '" + it.key() + "'");
        if (schema.contains("required"))
            for (const json& r : schema["required"]) {
                const std::string name = r.get<std::string>();
                if (!value.contains(name))
                    return fail(err, path + "/" + name, "missing required property '" + name + "'", name);
            }
        for (auto it = value.begin(); it != value.end(); ++it)
            if (!validate(props[it.key()], it.value(), err, path + "/" + it.key()))
                return false;
        if (schema.contains("x-exactlyOne")) {
            int         present = 0;
            std::string names;
            for (const json& group : schema["x-exactlyOne"]) {
                bool any = false;
                for (const json& n : group)
                    any = any || value.contains(n.get<std::string>());
                present += any ? 1 : 0;
                names += (names.empty() ? "" : " | ") + group.dump();
            }
            if (present != 1)
                return fail(err, path, "exactly one of " + names + " is required");
        }
    }
    return true;
}

json apply_defaults(const json& schema, const json& value)
{
    if (!schema.is_object() || schema.value("type", "") != "object" || !value.is_object())
        return value;
    json out = value;
    if (!schema.contains("properties"))
        return out;
    // A default never fills an x-exactlyOne alternative: it would make "neither" look like "this one".
    std::set<std::string> alternatives;
    if (schema.contains("x-exactlyOne"))
        for (const json& group : schema["x-exactlyOne"])
            for (const json& n : group)
                alternatives.insert(n.get<std::string>());
    for (auto it = schema["properties"].begin(); it != schema["properties"].end(); ++it) {
        const json& prop = it.value();
        if (out.contains(it.key()))
            out[it.key()] = apply_defaults(prop, out[it.key()]);
        else if (prop.contains("default") && alternatives.count(it.key()) == 0)
            out[it.key()] = prop["default"];
    }
    return out;
}

}}} // namespace Slic3r::GUI::Actions
