#ifndef slic3r_GUI_Actions_ActionSchema_hpp_
#define slic3r_GUI_Actions_ActionSchema_hpp_

// The restricted JSON-Schema dialect action parameters are described in, and its validator. Both
// ends implement exactly this subset (the server validates before sending; the slicer validates
// every invoke, because the page is untrusted); resources/actions/schema_vectors.json holds the
// shared test vectors.
//
// Allowed keywords: type (object|string|number|integer|boolean|array), enum, minimum, maximum,
// minLength, maxLength, minItems, maxItems, items, properties, required, additionalProperties
// (must be present and false on every object), description, default, and x-exactlyOne: a list of
// property-name groups of which exactly one group must be present ([["face"],["rotation"]]).
// Nothing else -- no $ref, oneOf, pattern, format, nullable unions.
//
// Pure: no GUI. Paths are JSON-Pointer-like ("/params/axis", "/params/items/2/z").

#include <string>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI { namespace Actions {

struct SchemaError
{
    std::string path;    // where in the value (validate) or the schema (check_schema)
    std::string message; // short English sentence
    std::string needs;   // validate(): the missing required property, if that is the failure
};

// Is `schema` itself inside the dialect? The root of an action's params must be an object schema.
bool check_schema(const nlohmann::json& schema, SchemaError& err, const std::string& path = "");

// Does `value` satisfy `schema`? First failure only. `path` prefixes reported paths.
bool validate(const nlohmann::json& schema, const nlohmann::json& value, SchemaError& err,
              const std::string& path = "");

// `value` with every absent property that has a "default" filled in, recursively through
// object properties. Never overwrites a present value.
nlohmann::json apply_defaults(const nlohmann::json& schema, const nlohmann::json& value);

}}} // namespace Slic3r::GUI::Actions

#endif // slic3r_GUI_Actions_ActionSchema_hpp_
