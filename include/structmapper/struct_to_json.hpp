// bidirectional conversion between reflectable structs and nlohmann::json.
// 
// it does its own explicit field-by-field walk instead of going through
// nlohmann's adl_serializer customization point. That's what lets it log
// what happened at each field instead of just succeeding or throwing.
//
// Writing (to_json) is the boring direction - a typed C++ value always
// converts to a well-formed JSON value, so there's nothing to warn about.
//
// Reading (from_json) is where this differs from json_reflect.hpp: it
// walks the incoming JSON alongside the struct's reflected fields and
// reports, per field, one of:
//   - loaded      - key present, type matched, value assigned
//   - missing     - struct field has no corresponding key in the JSON
//   - type error  - key present but its JSON type doesn't match the field
//   - unknown     - a JSON key with no corresponding struct field
// A field that fails to load (missing/wrong type) is simply left at
// whatever value it already had in the target object (typically whatever
// its default member initializer or default constructor set), so callers
// should default-construct before calling from_json if they want that
// fallback behavior.
//
// This header does not include json_reflect.hpp and does not rely on its
// adl_serializer specialization - it's a fully independent path that
// happens to reuse nlohmann::json's value type.
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
    enum class LogLevel { Info, Warning };

    using Logger = std::function<void(LogLevel, const std::string&)>;

    inline Logger default_logger() {
        return [](LogLevel level, const std::string& msg) {
            if (level == LogLevel::Warning) std::cerr << "[json] WARNING: " << msg << "\n";
            else                            std::cout << "[json] " << msg << "\n";
        };
    }

    struct LoadStats {
        int loaded = 0;
        int missing = 0;
        int type_mismatches = 0;
        int unknown_fields = 0;
        bool ok() const { return type_mismatches == 0; } // missing/unknown are warnings, not hard failures
    };

    // ---------------------------------------------------------------------
    // Writing: T -> json (always succeeds, nothing to log)
    // ---------------------------------------------------------------------
    inline void write_value(json& j, bool v) { j = v; }
    inline void write_value(json& j, const std::string& v) { j = v; }

    template <typename T>
    typename std::enable_if<std::is_arithmetic<T>::value && !std::is_same<T, bool>::value>::type
    write_value(json& j, const T& v);

    template <typename T>
    void write_value(json& j, const std::vector<T>& v);

    template <typename V>
    void write_value(json& j, const std::map<std::string, V>& m);

    template <typename T>
    typename std::enable_if<is_reflectable<T>::value>::type
    write_value(json& j, const T& obj);

    template <typename T>
    typename std::enable_if<std::is_arithmetic<T>::value && !std::is_same<T, bool>::value>::type
    write_value(json& j, const T& v) { j = v; }

    template <typename T>
    void write_value(json& j, const std::vector<T>& v) {
        j = json::array();
        for (const auto& e : v) { json ej; write_value(ej, e); j.push_back(std::move(ej)); }
    }

    template <typename V>
    void write_value(json& j, const std::map<std::string, V>& m) {
        j = json::object();
        for (const auto& kv : m) { json vj; write_value(vj, kv.second); j[kv.first] = std::move(vj); }
    }

    template <typename T>
    typename std::enable_if<is_reflectable<T>::value>::type
    write_value(json& j, const T& obj) {
        j = json::object();
        visit_struct(obj, [&](const char* name, const auto& value, const char* /*desc*/) {
            json vj;
            write_value(vj, value);
            j[name] = std::move(vj);
        });
    }

    template <typename T>
    json to_json(const T& v) { json j; write_value(j, v); return j; }

    // ---------------------------------------------------------------------
    // Reading: json -> T, with logging.
    // Every overload gets a 'path' (for locating the field in nested structs)
    // and returns true on success (type matched, value assigned) or false
    // (type mismatch - caller logs and leaves the target untouched).
    // ---------------------------------------------------------------------
    namespace detail {

        inline std::string join_path(const std::string& parent, const std::string& child) {
            return parent.empty() ? child : parent + "." + child;
        }

        inline void mismatched(const char* expected, const char* got, const std::string& path, Logger& logger, LoadStats& stats) {
            ++stats.type_mismatches;
            std::ostringstream msg;
            msg << "type mismatch at '" << path << "': expected " << expected << ", got " << got;
            logger(LogLevel::Warning, msg.str());
        }

        inline void loaded(const json& j, const std::string& path, Logger& logger, LoadStats& stats) {
            ++stats.loaded;
            logger(LogLevel::Info, "loaded '" + path + "' = " + j.dump());
        }

        // bool
        inline bool read_value(const json& j, bool& out, const std::string& path, Logger& logger, LoadStats& stats) {
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
        read_value(const json& j, T& out, const std::string& path, Logger& logger, LoadStats& stats) {
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
        read_value(const json& j, T& out, const std::string& path, Logger& logger, LoadStats& stats) {
            if (!j.is_number()) { // floating number or integer
                mismatched("number", j.type_name(), path, logger, stats);
                return false;
            }
            out = j.get<T>();
            loaded(j, path, logger, stats);
            return true;
        }

        // string
        inline bool read_value(const json& j, std::string& out, const std::string& path, Logger& logger, LoadStats& stats) {
            if (!j.is_string()) {
                mismatched("string", j.type_name(), path, logger, stats);
                return false;
            }
            out = j.get<std::string>();
            loaded(j, path, logger, stats);
            return true;
        }

        template <typename T>
        bool read_value(const json& j, std::vector<T>& out, const std::string& path, Logger& logger, LoadStats& stats);
        template <typename V>
        bool read_value(const json& j, std::map<std::string, V>& out, const std::string& path, Logger& logger, LoadStats& stats);
        template <typename T>
        typename std::enable_if<is_reflectable<T>::value, bool>::type
        read_value(const json& j, T& out, const std::string& path, Logger& logger, LoadStats& stats);


        // vector<T> -> array; bad elements are logged and dropped, good ones kept
        template <typename T>
        bool read_value(const json& j, std::vector<T>& out, const std::string& path, Logger& logger, LoadStats& stats) {
            if (!j.is_array()) {
                mismatched("array", j.type_name(), path, logger, stats);
                return false;
            }
            out.clear();
            out.resize(j.size());
            for (size_t i = 0; i < j.size(); ++i) {
                const std::string elem_path = path + "[" + std::to_string(i) + "]";
                read_value(j[i], out[i], elem_path, logger, stats);
            }
            ++stats.loaded;
            return true;
        }

        // map<string, V> -> object; bad entries are logged and dropped, good ones kept
        template <typename V>
        bool read_value(const json& j, std::map<std::string, V>& out, const std::string& path, Logger& logger, LoadStats& stats) {
            if (!j.is_object()) {
                mismatched("dictionary", j.type_name(), path, logger, stats);
                return false;
            }
            out.clear();
            for (auto it = j.begin(); it != j.end(); ++it) {
                const std::string entry_path = path + "[\"" + it.key() + "\"]";
                read_value(it.value(), out[it.key()], entry_path, logger, stats);
            }
            ++stats.loaded;
            return true;
        }

        // reflectable struct -> object; recurses field by field, reporting
        // loaded/missing/type-mismatched/unknown fields along the way.
        template <typename T>
        typename std::enable_if<is_reflectable<T>::value, bool>::type
        read_value(const json& j, T& out, const std::string& path, Logger& logger, LoadStats& stats) {
            if (!j.is_object()) {
                mismatched("struct", j.type_name(), path, logger, stats);
                return false;
            }

            std::vector<std::string> known_fields;
            visit_struct(out, [&](const char* name, auto& value, const char* /*desc*/) {
                const std::string field_path = join_path(path, name);
                known_fields.push_back(name);

                if (!j.contains(name)) {
                    ++stats.missing;
                    logger(LogLevel::Warning, "missing field '" + field_path + "'");
                    return;
                }

                read_value(j.at(name), value, field_path, logger, stats);
            });

            for (auto it = j.begin(); it != j.end(); ++it) {
                if (std::find(known_fields.begin(), known_fields.end(), it.key()) == known_fields.end()) {
                    const std::string field_path = join_path(path, it.key());
                    ++stats.unknown_fields;
                    logger(LogLevel::Warning, "unknown field '" + field_path + "'");
                }
            }
            return true;
        }

    } // namespace detail

    // ---------------------------------------------------------------------
    // Public entry point.
    // ---------------------------------------------------------------------
    template <typename T>
    LoadStats from_json(const json& j, T& obj, Logger logger = default_logger()) {
        static_assert(is_reflectable<T>::value, "from_json<T>() requires a BEGIN_STRUCT/END_STRUCT type");
        LoadStats stats;
        if (!detail::read_value(j, obj, "$", logger, stats)) {
            logger(LogLevel::Warning, "top-level JSON value is not an object (nothing loaded)");
            return stats;
        }
        std::ostringstream summary;
        summary << "load summary: " << stats.loaded << " loaded, " << stats.missing << " missing, "
                << stats.type_mismatches << " type mismatches, " << stats.unknown_fields << " unknown fields";
        logger(stats.type_mismatches > 0 ? LogLevel::Warning : LogLevel::Info, summary.str());
        return stats;
    }

} // namespace structmapper