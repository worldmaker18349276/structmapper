// bidirectional conversion between reflectable structs and nlohmann::json.
// 
// It does its own explicit field-by-field walk instead of going through
// nlohmann's adl_serializer customization point. That's what lets it log
// what happened at each field instead of just succeeding or throwing.
//
// Reflectable struct can contain primitives and containers, where the
// convertible types are:
//   - bool
//   - arithmetic number as integer or number
//   - std::string
//   - std::vector<T> as array
//   - std::map<std::string, T> as object
//   - reflectable struct as object
// from_json/to_json will iterate through std::vector, std::map and
// reflectable structs recursively.
//
// from_json walks the incoming JSON alongside the struct's reflected
// fields and reports, per field, one of:
//   - loaded      - key present, type matched, value assigned
//   - missing     - struct field has no corresponding key in the JSON
//   - type error  - key present but its JSON type doesn't match the field
//   - unknown     - a JSON key with no corresponding struct field
// A field that fails to load (missing/wrong type) is simply left at
// whatever value it already had in the target object (typically whatever
// its default member initializer or default constructor set), so callers
// should default-construct before calling from_json if they want that
// fallback behavior.
#pragma once
#include "struct_reflect.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <map>
#include <string>

namespace structmapper {

    using json = nlohmann::json;

    // ---------------------------------------------------------------------
    // Logging
    // ---------------------------------------------------------------------
    using LogLevel = int; // INFO = 0, WARNING = 1
    using Logger = std::function<void(LogLevel, const std::string&)>;

    struct FromJsonStats {
        int loaded = 0;
        int missing = 0;
        int type_mismatches = 0;
        int unknown_fields = 0;
        bool ok() const { return type_mismatches == 0; } // missing/unknown are warnings, not hard failures
        std::string summary() const {
            std::ostringstream summary;
            summary << "from_json summary: " << loaded << " loaded, " << missing << " missing, "
                    << type_mismatches << " type mismatches, " << unknown_fields << " unknown fields";
            return summary.str();
        }
    };

    // ---------------------------------------------------------------------
    // Writing: T -> json (always succeeds, nothing to log)
    // ---------------------------------------------------------------------
    namespace detail {

        inline Logger struct_to_json_default_logger() {
            return [](LogLevel level, const std::string& msg) {
                if (level == 1) std::cerr << "[structmapper] WARNING: " << msg << "\n";
                else            std::cout << "[structmapper] " << msg << "\n";
            };
        }
    } // namespace detail

    inline void to_json(bool v, json& j) { j = v; }
    inline void to_json(const std::string& v, json& j) { j = v; }

    template <typename T>
    typename std::enable_if<std::is_arithmetic<T>::value && !std::is_same<T, bool>::value>::type
    to_json(const T& v, json& j);

    template <typename T>
    void to_json(const std::vector<T>& v, json& j);

    template <typename V>
    void to_json(const std::map<std::string, V>& m, json& j);

    template <typename T>
    typename std::enable_if<is_reflectable<T>::value>::type
    to_json(const T& obj, json& j);

    template <typename T>
    typename std::enable_if<std::is_arithmetic<T>::value && !std::is_same<T, bool>::value>::type
    to_json(const T& v, json& j) { j = v; }

    template <typename T>
    void to_json(const std::vector<T>& v, json& j) {
        j = json::array();
        for (const auto& e : v) { json ej; to_json(e, ej); j.push_back(std::move(ej)); }
    }

    template <typename V>
    void to_json(const std::map<std::string, V>& m, json& j) {
        j = json::object();
        for (const auto& kv : m) { json vj; to_json(kv.second, vj); j[kv.first] = std::move(vj); }
    }

    template <typename T>
    typename std::enable_if<is_reflectable<T>::value>::type
    to_json(const T& obj, json& j) {
        j = json::object();
        visit_struct(obj, [&](const char* name, const auto& value, const char* /*desc*/) {
            json vj;
            to_json(value, vj);
            j[name] = std::move(vj);
        });
    }

    template <typename T>
    json to_json(const T& v) { json j; to_json(v, j); return j; }

    // ---------------------------------------------------------------------
    // Reading: json -> T, with logging.
    // Every overload gets a 'path' (for locating the field in nested structs)
    // and returns true on success (type matched, value assigned) or false
    // (type mismatch - caller logs and leaves the target untouched).
    // ---------------------------------------------------------------------
    namespace detail {

        inline void mismatched(const char* expected, const char* got, const std::string& path, Logger& logger, FromJsonStats& stats) {
            ++stats.type_mismatches;
            std::ostringstream msg;
            msg << "type mismatch at " << path << ": expected " << expected << ", got " << got;
            logger(1, msg.str());
        }

        inline void loaded(const json& j, const std::string& path, Logger& logger, FromJsonStats& stats) {
            ++stats.loaded;
            logger(0, "loaded " + path + " = " + j.dump());
        }

        // bool
        inline bool from_json(bool& out, const json& j, const std::string& path, Logger& logger, FromJsonStats& stats) {
            if (!j.is_boolean()) {
                mismatched("boolean", j.type_name(), path, logger, stats);
                return false;
            }
            out = j.get<bool>();
            loaded(j, path, logger, stats);
            return true;
        }

        // arithmetic (excluding bool)
        template <typename T>
        typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value, bool>::type
        from_json(T& out, const json& j, const std::string& path, Logger& logger, FromJsonStats& stats) {
            if (!j.is_number_integer()) {
                mismatched("integer", j.type_name(), path, logger, stats);
                return false;
            }
            out = j.get<T>();
            loaded(j, path, logger, stats);
            return true;
        }

        template <typename T>
        typename std::enable_if<std::is_floating_point<T>::value, bool>::type
        from_json(T& out, const json& j, const std::string& path, Logger& logger, FromJsonStats& stats) {
            if (!j.is_number()) { // floating number or integer
                mismatched("number", j.type_name(), path, logger, stats);
                return false;
            }
            out = j.get<T>();
            loaded(j, path, logger, stats);
            return true;
        }

        // string
        inline bool from_json(std::string& out, const json& j, const std::string& path, Logger& logger, FromJsonStats& stats) {
            if (!j.is_string()) {
                mismatched("string", j.type_name(), path, logger, stats);
                return false;
            }
            out = j.get<std::string>();
            loaded(j, path, logger, stats);
            return true;
        }

        template <typename T>
        bool from_json(std::vector<T>& out, const json& j, const std::string& path, Logger& logger, FromJsonStats& stats);
        template <typename V>
        bool from_json(std::map<std::string, V>& out, const json& j, const std::string& path, Logger& logger, FromJsonStats& stats);
        template <typename T>
        typename std::enable_if<is_reflectable<T>::value, bool>::type
        from_json(T& out, const json& j, const std::string& path, Logger& logger, FromJsonStats& stats);


        // vector<T> -> array; bad elements are logged and dropped, good ones kept
        template <typename T>
        bool from_json(std::vector<T>& out, const json& j, const std::string& path, Logger& logger, FromJsonStats& stats) {
            if (!j.is_array()) {
                mismatched("array", j.type_name(), path, logger, stats);
                return false;
            }
            out.clear();
            out.resize(j.size());
            for (size_t i = 0; i < j.size(); ++i) {
                const std::string elem_path = path + "[" + std::to_string(i) + "]";
                from_json(out[i], j[i], elem_path, logger, stats);
            }
            ++stats.loaded;
            return true;
        }

        // map<string, V> -> object; bad entries are logged and dropped, good ones kept
        template <typename V>
        bool from_json(std::map<std::string, V>& out, const json& j, const std::string& path, Logger& logger, FromJsonStats& stats) {
            if (!j.is_object()) {
                mismatched("dictionary", j.type_name(), path, logger, stats);
                return false;
            }
            out.clear();
            for (auto it = j.begin(); it != j.end(); ++it) {
                const std::string entry_path = path + "[\"" + it.key() + "\"]";
                from_json(out[it.key()], it.value(), entry_path, logger, stats);
            }
            ++stats.loaded;
            return true;
        }

        // reflectable struct -> object; recurses field by field, reporting
        // loaded/missing/type-mismatched/unknown fields along the way.
        template <typename T>
        typename std::enable_if<is_reflectable<T>::value, bool>::type
        from_json(T& out, const json& j, const std::string& path, Logger& logger, FromJsonStats& stats) {
            if (!j.is_object()) {
                mismatched("struct", j.type_name(), path, logger, stats);
                return false;
            }

            std::vector<std::string> known_fields;
            visit_struct(out, [&](const char* name, auto& value, const char* /*desc*/) {
                const std::string field_path = path + "." + name;
                known_fields.push_back(name);

                if (!j.contains(name)) {
                    ++stats.missing;
                    logger(1, "missing field " + field_path);
                    return;
                }

                from_json(value, j.at(name), field_path, logger, stats);
            });

            for (auto it = j.begin(); it != j.end(); ++it) {
                if (std::find(known_fields.begin(), known_fields.end(), it.key()) == known_fields.end()) {
                    const std::string field_path = path + "." + it.key();
                    ++stats.unknown_fields;
                    logger(1, "unknown field " + field_path);
                }
            }
            return true;
        }

    } // namespace detail

    // ---------------------------------------------------------------------
    // Public entry point.
    // ---------------------------------------------------------------------
    template <typename T>
    FromJsonStats from_json(T& obj, const json& j, const std::string& path = "$", Logger logger = detail::struct_to_json_default_logger()) {
        FromJsonStats stats;
        detail::from_json(obj, j, path, logger, stats);
        logger(0, stats.summary());
        return stats;
    }

} // namespace structmapper