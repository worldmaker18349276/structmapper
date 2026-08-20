// conversion from reflectable structs to JSON Schema.
//
// structmapper::to_schema<T>() builds a JSON Schema-ish description of T:
//   - primitives/containers get a plain {"type": ...} shape
//   - reflectable structs get {"type":"object","description":<struct desc>,
//     "properties":{...}}
//   - every field gets its FIELD(...) description and a "default" pulled
//     from a value-initialized instance of the struct
//   - when a field's own type is itself a reflectable struct, the field
//     already has a description (from FIELD) *and* the struct type has its
//     own description (from BEGIN_STRUCT) - both are kept by nesting the
//     type schema under "anyOf" instead of flattening the two together:
//         {"description": "<field desc>", "default": {...},
//          "anyOf": [{"description": "<struct desc>", "type": "object", ...}]}
#pragma once
#include "struct_reflect.hpp"
#include "struct_to_json.hpp"
#include <nlohmann/json.hpp>

namespace structmapper {

    template <typename T, typename Enable = void>
    struct SchemaTraits {
        static_assert(sizeof(T) == 0, "to_schema: no schema known for this type");
        static constexpr bool is_scalar = true;
    };

    // bool
    template <>
    struct SchemaTraits<bool> {
        static nlohmann::json get() { return {{"type", "boolean"}}; }
        static constexpr bool is_scalar = true;
    };

    // integers (excluding bool, which is handled above)
    template <typename T>
    struct SchemaTraits<T, typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value>::type> {
        static nlohmann::json get() { return {{"type", "integer"}}; }
        static constexpr bool is_scalar = true;
    };

    // floating point
    template <typename T>
    struct SchemaTraits<T, typename std::enable_if<std::is_floating_point<T>::value>::type> {
        static nlohmann::json get() { return {{"type", "number"}}; }
        static constexpr bool is_scalar = true;
    };

    // string
    template <>
    struct SchemaTraits<std::string> {
        static nlohmann::json get() { return {{"type", "string"}}; }
        static constexpr bool is_scalar = true;
    };

    // forward declaration - combines a type's schema with a field's own
    // description/default. Defined after SchemaTraits so it can use it.
    template <typename T>
    nlohmann::json field_schema(const T& default_value, const char* desc);

    // vector<T> -> array
    template <typename T>
    struct SchemaTraits<std::vector<T>> {
        static nlohmann::json get() {
            nlohmann::json j;
            j["type"] = "array";
            j["items"] = SchemaTraits<T>::get();
            return j;
        }
        static constexpr bool is_scalar = false;
    };

    // map<string, V> -> object with a uniform value schema
    template <typename V>
    struct SchemaTraits<std::map<std::string, V>> {
        static nlohmann::json get() {
            nlohmann::json j;
            j["type"] = "object";
            j["additionalProperties"] = SchemaTraits<V>::get();
            return j;
        }
        static constexpr bool is_scalar = false;
    };

    // reflectable struct -> object with named properties
    template <typename T>
    struct SchemaTraits<T, typename std::enable_if<is_reflectable<T>::value>::type> {
        static nlohmann::json get() {
            nlohmann::json j;
            j["type"] = "object";
            j["description"] = T::reflect_struct_desc();

            nlohmann::json props = nlohmann::json::object();
            T default_obj{}; // value-initialized: gives every field a concrete default
            visit_struct(default_obj, [&](const char* name, const auto& value, const char* desc) {
                props[name] = field_schema(value, desc);
            });
            j["properties"] = std::move(props);
            return j;
        }
        static constexpr bool is_scalar = false;
    };

    template <typename T>
    nlohmann::json field_schema(const T& default_value, const char* desc) {
        using Bare = typename std::decay<T>::type;
        nlohmann::json type_schema = SchemaTraits<Bare>::get();

        nlohmann::json j;
        j["description"] = desc;
        if (SchemaTraits<Bare>::is_scalar) {
            j["default"] = to_json(default_value);
        }

        if (is_reflectable<Bare>::value) {
            // type_schema already carries its own "description" (the struct's);
            // keep both by nesting rather than letting one overwrite the other.
            j["anyOf"] = nlohmann::json::array({std::move(type_schema)});
        } else {
            for (auto it = type_schema.begin(); it != type_schema.end(); ++it) {
                j[it.key()] = it.value();
            }
        }
        return j;
    }

    // Public entry point: full JSON Schema for a reflectable type.
    template <typename T>
    nlohmann::json to_schema() {
        static_assert(is_reflectable<T>::value, "to_schema<T>() requires a BEGIN_STRUCT/END_STRUCT type");
        return SchemaTraits<T>::get();
    }

} // namespace structmapper
