// conversion from reflectable structs to JSON Schema.
//
// structmapper::to_schema<T>(refs) builds a JSON Schema-ish description of T:
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
//
// `refs` maps a reflectable type to an external JSON Schema file path. Any
// type found in `refs` is treated as opaque: wherever it's encountered -
// root, a field, a vector element, a map value - it's emitted as
// {"$ref": path} and its fields are never visited. This is for types whose
// schema is defined (and presumably generated) elsewhere.
//
// Separately, any reflectable type *not* in `refs` that turns out to occur
// two or more times anywhere in T's type graph is automatically hoisted
// into a top-level "$defs" object (keyed by a best-effort type name) and
// every one of its occurrences - including the first - becomes
// {"$ref": "#/$defs/Name"}. A type used only once stays inlined, as before.
// This is decided by a full counting pass over the type graph before the
// schema is actually built (see count_types below); the interior of any
// type matched in `refs` is skipped during counting too, since it's opaque.
//
// This dedup is also what makes recursive/mutually-recursive types
// possible to schematize at all: re-entering a type that's still being
// counted (a cycle) counts as another occurrence, so it crosses the ">=2"
// threshold and gets deduped into $defs - and on the build side, a type's
// $defs name is recorded before its schema is actually built, so a cyclic
// reference back to itself just finds that name already assigned and emits
// {"$ref": ...} instead of recursing forever.
#pragma once
#include "struct_reflect.hpp"
#include "struct_to_json.hpp"
#include <nlohmann/json.hpp>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <typeindex>
#include <cctype>
#if defined(__GNUG__)
#include <cstdlib>
#include <cxxabi.h>
#endif

namespace structmapper {

    // Threaded through the build pass so nested calls can look up $ref
    // targets (both external and locally-deduped) and share one $defs
    // accumulator.
    struct SchemaRefs {
        const std::map<std::type_index, std::string>& ext_refs; // type -> external schema file path
        const std::map<std::type_index, int>& counts;           // occurrence counts, from the counting pass
        nlohmann::json& defs;                                   // top-level "$defs", filled in lazily
        std::map<std::type_index, std::string>& def_names;      // type -> its (unique) $defs key, once assigned
    };

    namespace detail {

        template <typename T, typename Enable = void>
        struct SchemaTraits {
            static_assert(sizeof(T) == 0, "to_schema: no schema known for this type");
            static constexpr bool is_scalar = true;
        };

        // bool
        template <>
        struct SchemaTraits<bool> {
            static nlohmann::json get(SchemaRefs&) { return {{"type", "boolean"}}; }
            static constexpr bool is_scalar = true;
        };

        // integers (excluding bool, which is handled above)
        template <typename T>
        struct SchemaTraits<T, typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value>::type> {
            static nlohmann::json get(SchemaRefs&) { return {{"type", "integer"}}; }
            static constexpr bool is_scalar = true;
        };

        // floating point
        template <typename T>
        struct SchemaTraits<T, typename std::enable_if<std::is_floating_point<T>::value>::type> {
            static nlohmann::json get(SchemaRefs&) { return {{"type", "number"}}; }
            static constexpr bool is_scalar = true;
        };

        // string
        template <>
        struct SchemaTraits<std::string> {
            static nlohmann::json get(SchemaRefs&) { return {{"type", "string"}}; }
            static constexpr bool is_scalar = true;
        };

        // any
        template <>
        struct SchemaTraits<nlohmann::json> {
            static nlohmann::json get(SchemaRefs&) { return nlohmann::json::object(); }
            static constexpr bool is_scalar = false;
        };

        // string enum
        template <typename... Strings>
        struct SchemaTraits<::strenum::StringEnum<Strings...>> {
            static nlohmann::json get(SchemaRefs&) { return {{"enum", {Strings::c_str()...}}}; }
            static constexpr bool is_scalar = true;
        };

        // compile-time string
        template <char... Cs>
        struct SchemaTraits<::CompileTimeString<Cs...>> {
            static nlohmann::json get(SchemaRefs&) { return {{"const", ::CompileTimeString<Cs...>::c_str()}}; }
            static constexpr bool is_scalar = true;
        };

        // Best-effort human-readable name for a reflectable type. This
        // demangles typeid(T).name().
        template <typename T>
        std::string demangled_type_name() {
            std::string name;
#if defined(__GNUG__)
            int status = 0;
            std::unique_ptr<char, void (*)(void*)> demangled(
                abi::__cxa_demangle(typeid(T).name(), nullptr, nullptr, &status), std::free);
            name = (status == 0 && demangled) ? demangled.get() : typeid(T).name();
#else
            name = typeid(T).name();
#endif
            return name;
        }

        // Short form: just the last "::"-separated segment, e.g. "Foo" for
        // "ns::inner::Foo". This is what's tried first as a $defs key.
        template <typename T>
        std::string type_defs_name() {
            std::string name = demangled_type_name<T>();
            auto pos = name.find_last_of(':');
            return pos == std::string::npos ? name : name.substr(pos + 1);
        }

        // A $defs key (and $ref fragment) has to be usable as a plain JSON
        // object key / URI fragment, so anything that isn't alphanumeric or
        // '_' - "::", template "<...>", etc. - is folded to '_'. Leading
        // digits also get a "T_" prefix so the result stays a reasonable
        // identifier.
        inline std::string sanitize_defs_name(const std::string& raw) {
            std::string out;
            out.reserve(raw.size());
            for (char c : raw) {
                out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_');
            }
            if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) {
                out = "T_" + out;
            }
            return out;
        }

        // Picks a $defs key for T that isn't already taken by some *other*
        // type: the short name ("Foo") if free, else the fully-qualified
        // name ("ns_inner_Foo") if that's free, else the qualified name with
        // an incrementing numeric suffix. `def_names` holds every name
        // already handed out to a (different) type_index so far.
        template <typename T>
        std::string choose_unique_defs_name(const std::map<std::type_index, std::string>& def_names) {
            std::set<std::string> used;
            for (const auto& kv : def_names) used.insert(kv.second);

            std::string short_name = sanitize_defs_name(type_defs_name<T>());
            if (!used.count(short_name)) return short_name;

            std::string qualified = sanitize_defs_name(demangled_type_name<T>());
            if (!used.count(qualified)) return qualified;

            for (int i = 2;; ++i) {
                std::string candidate = qualified + "_" + std::to_string(i);
                if (!used.count(candidate)) return candidate;
            }
        }

        // Pass 1: count how many places in the schema each reflectable type
        // *not* matched in ext_refs would end up inlined, if nothing were
        // deduplicated. Types matched in ext_refs are opaque - not counted,
        // not descended into. The same dataclass will only be visited once.
        template <typename T>
        void count_types(const std::map<std::type_index, std::string>& ext_refs,
                          std::map<std::type_index, int>& counts);

        template <typename T, typename Enable = void>
        struct CountTraits {
            // scalar/leaf: nothing to count
            static void count(const std::map<std::type_index, std::string>&,
                               std::map<std::type_index, int>&) {}
        };

        template <typename T>
        struct CountTraits<std::vector<T>> {
            static void count(const std::map<std::type_index, std::string>& ext_refs,
                               std::map<std::type_index, int>& counts) {
                count_types<T>(ext_refs, counts);
            }
        };

        template <typename V>
        struct CountTraits<std::map<std::string, V>> {
            static void count(const std::map<std::type_index, std::string>& ext_refs,
                               std::map<std::type_index, int>& counts) {
                count_types<V>(ext_refs, counts);
            }
        };

        template <typename T>
        struct CountTraits<T, typename std::enable_if<is_reflectable<T>::value>::type> {
            static void count(const std::map<std::type_index, std::string>& ext_refs,
                               std::map<std::type_index, int>& counts) {
                std::type_index key(typeid(T));
                if (ext_refs.count(key)) return; // externally defined - opaque, not traversed

                counts[key]++;
                if (counts[key] >= 2) return; // already visited
                T default_obj{};
                visit_struct(default_obj, [&](const char*, const auto& value, const char*) {
                    using FieldType = typename std::decay<decltype(value)>::type;
                    count_types<FieldType>(ext_refs, counts);
                });
            }
        };

        template <typename T>
        void count_types(const std::map<std::type_index, std::string>& ext_refs,
                          std::map<std::type_index, int>& counts) {
            using Bare = typename std::decay<T>::type;
            CountTraits<Bare>::count(ext_refs, counts);
        }

        // forward declarations - schema for a type (using $ref for external
        // ext_refs matches and for locally-deduped types), and a field's
        // schema (its own description/default plus its type's schema).
        // Defined after SchemaTraits so they can use it; used by SchemaTraits
        // before that, which is fine since these are templates.
        template <typename T>
        nlohmann::json type_schema(SchemaRefs& ctx);

        template <typename T>
        nlohmann::json field_schema(const T& default_value, const char* desc, SchemaRefs& ctx);

        // vector<T> -> array
        template <typename T>
        struct SchemaTraits<std::vector<T>> {
            static nlohmann::json get(SchemaRefs& ctx) {
                nlohmann::json j;
                j["type"] = "array";
                j["items"] = type_schema<T>(ctx);
                return j;
            }
            static constexpr bool is_scalar = false;
        };

        // map<string, V> -> object with a uniform value schema
        template <typename V>
        struct SchemaTraits<std::map<std::string, V>> {
            static nlohmann::json get(SchemaRefs& ctx) {
                nlohmann::json j;
                j["type"] = "object";
                j["additionalProperties"] = type_schema<V>(ctx);
                return j;
            }
            static constexpr bool is_scalar = false;
        };

        // reflectable struct -> object with named properties
        template <typename T>
        struct SchemaTraits<T, typename std::enable_if<is_reflectable<T>::value>::type> {
            static nlohmann::json get(SchemaRefs& ctx) {
                nlohmann::json j;
                j["type"] = "object";
                j["description"] = reflect_struct_desc<T>();

                nlohmann::json props = nlohmann::json::object();
                T default_obj{}; // value-initialized: gives every field a concrete default
                visit_struct(default_obj, [&](const char* name, const auto& value, const char* desc) {
                    props[name] = field_schema(value, desc, ctx);
                });
                j["properties"] = std::move(props);
                return j;
            }
            static constexpr bool is_scalar = false;
        };

        // Schema for T on its own:
        //   - if T is matched in ctx.ext_refs: {"$ref": that path}, full stop -
        //     T's fields are never visited.
        //   - else if T occurs >=2 times anywhere in the type graph (per the
        //     counting pass): {"$ref": "#/$defs/Name"}, building the full
        //     schema into ctx.defs exactly once, under a name unique to T.
        //   - else: the plain inline schema, as in the original code.
        template <typename T>
        nlohmann::json type_schema(SchemaRefs& ctx) {
            using Bare = typename std::decay<T>::type;

            if (is_reflectable<Bare>::value) {
                std::type_index key(typeid(Bare));

                auto ext_it = ctx.ext_refs.find(key);
                if (ext_it != ctx.ext_refs.end()) {
                    return {{"$ref", ext_it->second}};
                }

                auto count_it = ctx.counts.find(key);
                int uses = (count_it != ctx.counts.end()) ? count_it->second : 1;
                if (uses >= 2) {
                    auto assigned = ctx.def_names.find(key);
                    if (assigned != ctx.def_names.end()) {
                        // Already seen T before - either its $defs entry is
                        // fully built already, or we're still in the middle
                        // of building it further up the call stack (T refers
                        // back to itself, directly or via another type).
                        // Either way, just point at its name; no rebuilding
                        // and, in the cyclic case, no infinite recursion.
                        return {{"$ref", "#/$defs/" + assigned->second}};
                    }
                    std::string name = choose_unique_defs_name<Bare>(ctx.def_names);
                    // Record the name *before* building T's schema, so that
                    // if building it recurses back into T (a cycle), the
                    // lookup above finds it and short-circuits instead of
                    // recursing forever.
                    ctx.def_names[key] = name;
                    ctx.defs[name] = SchemaTraits<Bare>::get(ctx);
                    return {{"$ref", "#/$defs/" + name}};
                }
            }
            return SchemaTraits<Bare>::get(ctx);
        }

        template <typename T>
        nlohmann::json field_schema(const T& default_value, const char* desc, SchemaRefs& ctx) {
            using Bare = typename std::decay<T>::type;
            nlohmann::json ts = type_schema<Bare>(ctx);

            nlohmann::json j;
            j["description"] = desc;
            if (SchemaTraits<Bare>::is_scalar) {
                j["default"] = ::structmapper::convert_to_json(default_value);
            }

            if (is_reflectable<Bare>::value) {
                // type_schema already carries its own "description" (the struct's);
                // keep both by nesting rather than letting one overwrite the other.
                j["anyOf"] = nlohmann::json::array({std::move(ts)});
            } else {
                for (auto it = ts.begin(); it != ts.end(); ++it) {
                    j[it.key()] = it.value();
                }
            }
            return j;
        }

    } // namespace detail

    // Public entry point: full JSON Schema for a reflectable type. `refs`
    // maps types with an externally-defined schema to that schema's file
    // path; wherever such a type is found, it's emitted as {"$ref": path}
    // and never expanded. Any other reflectable type that occurs two or
    // more times in T's type graph is automatically deduplicated into a
    // top-level "$defs" object instead.
    template <typename T>
    nlohmann::json to_schema(const std::map<std::type_index, std::string>& refs = {}) {
        std::map<std::type_index, int> counts;
        detail::count_types<T>(refs, counts);

        nlohmann::json defs = nlohmann::json::object();
        std::map<std::type_index, std::string> def_names;
        SchemaRefs ctx{refs, counts, defs, def_names};

        nlohmann::json root = detail::type_schema<T>(ctx);
        if (!defs.empty()) {
            root["$defs"] = std::move(defs);
        }
        return root;
    }

} // namespace structmapper
