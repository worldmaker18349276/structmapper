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
    struct FromJsonLog {
        using LogLevel = int; // INFO = 0, WARNING = 1
        virtual LogLevel level() const = 0;
        virtual std::string msg() const = 0;
    };
    struct FromJsonLoadLog : public FromJsonLog {
        std::string path;
        json value;
        FromJsonLoadLog(std::string path, json value) : path(path), value(value) {}
        virtual LogLevel level() const override { return 0; }
        virtual std::string msg() const override {
            return "loaded " + path + " = " + value.dump();
        }
    };
    struct FromJsonMismatchLog : public FromJsonLog {
        std::string path;
        const char* expected;
        const char* got;
        FromJsonMismatchLog(std::string path, const char* expected, const char* got)
            : path(path), expected(expected), got(got) {}
        virtual LogLevel level() const override { return 1; }
        virtual std::string msg() const override {
            std::ostringstream msg;
            return "type mismatch at " + path + ": expected " + expected + ", got " + got;
        }
    };
    struct FromJsonMissingLog : public FromJsonLog {
        std::string path;
        FromJsonMissingLog(std::string path) : path(path) {}
        virtual LogLevel level() const override { return 1; }
        virtual std::string msg() const override {
            return "missing field " + path;
        }
    };
    struct FromJsonUnknownLog : public FromJsonLog {
        std::string path;
        FromJsonUnknownLog(std::string path) : path(path) {}
        virtual LogLevel level() const override { return 1; }
        virtual std::string msg() const override {
            return "unknown field " + path;
        }
    };

    using FromJsonLogger = std::function<void(const FromJsonLog&)>;

    struct FromJsonStats {
        int loaded = 0;
        int missing = 0;
        int type_mismatches = 0;
        int unknown_fields = 0;
        bool ok() const { return type_mismatches == 0; } // missing/unknown are warnings, not hard failures
        FromJsonLogger logger(bool print = true) {
            return [this, print](const FromJsonLog& log) {
                if (print) {
                    if (log.level() == 1) std::cerr << "[structmapper] WARNING: " << log.msg() << "\n";
                    else                  std::cout << "[structmapper] " << log.msg() << "\n";
                }

                if (dynamic_cast<const FromJsonLoadLog*>(&log))
                    ++this->loaded;
                if (dynamic_cast<const FromJsonMismatchLog*>(&log))
                    ++this->type_mismatches;
                if (dynamic_cast<const FromJsonMissingLog*>(&log))
                    ++this->missing;
                if (dynamic_cast<const FromJsonUnknownLog*>(&log))
                    ++this->unknown_fields;
            };
        }
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

        inline void to_json(bool v, json& j) { j = v; }
        inline void to_json(const std::string& v, json& j) { j = v; }

        template <typename... Strings>
        inline void to_json(typename ::strenum::StringEnum<Strings...>& v, json& j) { j = v.c_str(); }

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
            for (const auto& e : v) { json ej; ::structmapper::detail::to_json(e, ej); j.push_back(std::move(ej)); }
        }

        template <typename V>
        void to_json(const std::map<std::string, V>& m, json& j) {
            j = json::object();
            for (const auto& kv : m) { json vj; ::structmapper::detail::to_json(kv.second, vj); j[kv.first] = std::move(vj); }
        }

        template <typename T>
        typename std::enable_if<is_reflectable<T>::value>::type
        to_json(const T& obj, json& j) {
            j = json::object();
            visit_struct(obj, [&](const char* name, const auto& value, const char* /*desc*/) {
                json vj;
                ::structmapper::detail::to_json(value, vj);
                j[name] = std::move(vj);
            });
        }
    } // namespace detail

    template <typename T>
    void to_json(const T& v, json& j) { detail::to_json(v, j); }

    template <typename T>
    json convert_to_json(const T& v) { json j; detail::to_json(v, j); return j; }

    // ---------------------------------------------------------------------
    // Reading: json -> T, with logging.
    // Every overload gets a 'path' (for locating the field in nested structs)
    // and returns true on success (type matched, value assigned) or false
    // (type mismatch - caller logs and leaves the target untouched).
    // ---------------------------------------------------------------------
    namespace detail {

        // bool
        inline bool from_json(bool& out, const json& j, const std::string& path, FromJsonLogger& logger) {
            if (!j.is_boolean()) {
                logger(FromJsonMismatchLog(path, "boolean", j.type_name()));
                return false;
            }
            out = j.get<bool>();
            logger(FromJsonLoadLog(path, j));
            return true;
        }

        // arithmetic (excluding bool)
        template <typename T>
        typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value, bool>::type
        from_json(T& out, const json& j, const std::string& path, FromJsonLogger& logger) {
            if (!j.is_number_integer()) {
                logger(FromJsonMismatchLog(path, "integer", j.type_name()));
                return false;
            }
            out = j.get<T>();
            logger(FromJsonLoadLog(path, j));
            return true;
        }

        template <typename T>
        typename std::enable_if<std::is_floating_point<T>::value, bool>::type
        from_json(T& out, const json& j, const std::string& path, FromJsonLogger& logger) {
            if (!j.is_number()) { // floating number or integer
                logger(FromJsonMismatchLog(path, "number", j.type_name()));
                return false;
            }
            out = j.get<T>();
            logger(FromJsonLoadLog(path, j));
            return true;
        }

        // string
        inline bool from_json(std::string& out, const json& j, const std::string& path, FromJsonLogger& logger) {
            if (!j.is_string()) {
                logger(FromJsonMismatchLog(path, "string", j.type_name()));
                return false;
            }
            out = j.get<std::string>();
            logger(FromJsonLoadLog(path, j));
            return true;
        }

        template <typename... Strings>
        inline bool from_json(typename ::strenum::StringEnum<Strings...>& out, const json& j, const std::string& path, FromJsonLogger& logger) {
            using Enum = typename ::strenum::StringEnum<Strings...>;
            if (!j.is_string() || !Enum::is_valid(j.get<std::string>().c_str())) {
                logger(FromJsonMismatchLog(path, Enum::type_name(), j.type_name()));
                return false;
            }
            out = j.get<std::string>().c_str();
            logger(FromJsonLoadLog(path, j));
            return true;
        }

        template <typename T>
        bool from_json(std::vector<T>& out, const json& j, const std::string& path, FromJsonLogger& logger);
        template <typename V>
        bool from_json(std::map<std::string, V>& out, const json& j, const std::string& path, FromJsonLogger& logger);
        template <typename T>
        typename std::enable_if<is_reflectable<T>::value, bool>::type
        from_json(T& out, const json& j, const std::string& path, FromJsonLogger& logger);


        // vector<T> -> array; bad elements are logged and dropped, good ones kept
        template <typename T>
        bool from_json(std::vector<T>& out, const json& j, const std::string& path, FromJsonLogger& logger) {
            if (!j.is_array()) {
                logger(FromJsonMismatchLog(path, "array", j.type_name()));
                return false;
            }
            out.clear();
            out.resize(j.size());
            for (size_t i = 0; i < j.size(); ++i) {
                const std::string elem_path = path + "[" + std::to_string(i) + "]";
                ::structmapper::detail::from_json(out[i], j[i], elem_path, logger);
            }
            return true;
        }

        // map<string, V> -> object; bad entries are logged and dropped, good ones kept
        template <typename V>
        bool from_json(std::map<std::string, V>& out, const json& j, const std::string& path, FromJsonLogger& logger) {
            if (!j.is_object()) {
                logger(FromJsonMismatchLog(path, "dictionary", j.type_name()));
                return false;
            }
            out.clear();
            for (auto it = j.begin(); it != j.end(); ++it) {
                const std::string entry_path = path + "[\"" + it.key() + "\"]";
                ::structmapper::detail::from_json(out[it.key()], it.value(), entry_path, logger);
            }
            return true;
        }

        // reflectable struct -> object; recurses field by field, reporting
        // loaded/missing/type-mismatched/unknown fields along the way.
        template <typename T>
        typename std::enable_if<is_reflectable<T>::value, bool>::type
        from_json(T& out, const json& j, const std::string& path, FromJsonLogger& logger) {
            if (!j.is_object()) {
                logger(FromJsonMismatchLog(path, "struct", j.type_name()));
                return false;
            }

            std::vector<std::string> known_fields;
            visit_struct(out, [&](const char* name, auto& value, const char* /*desc*/) {
                const std::string field_path = path + "." + name;
                known_fields.push_back(name);

                if (!j.contains(name)) {
                    logger(FromJsonMissingLog(field_path));
                    return;
                }

                ::structmapper::detail::from_json(value, j.at(name), field_path, logger);
            });

            for (auto it = j.begin(); it != j.end(); ++it) {
                if (std::find(known_fields.begin(), known_fields.end(), it.key()) == known_fields.end()) {
                    const std::string field_path = path + "." + it.key();
                    logger(FromJsonUnknownLog(field_path));
                }
            }
            return true;
        }

    } // namespace detail

    // ---------------------------------------------------------------------
    // Public entry point.
    // ---------------------------------------------------------------------
    template <typename T>
    bool from_json(T& obj, const json& j, const std::string& path, FromJsonLogger logger = {}) {
        FromJsonStats local_stats;
        if (!logger) logger = local_stats.logger();
        return detail::from_json(obj, j, path, logger);
    }

    template <typename T>
    bool from_json(T& obj, const json& j, FromJsonLogger logger = {}) {
        FromJsonStats local_stats;
        if (!logger) logger = local_stats.logger();
        return detail::from_json(obj, j, "$", logger);
    }

    template <typename T>
    T convert_from_json(const json& j, FromJsonLogger logger = {}) {
        T obj;
        FromJsonStats local_stats;
        if (!logger) logger = local_stats.logger();
        detail::from_json(obj, j, "$", logger);
        return obj;
    }

} // namespace structmapper