// bidirectional conversion between XmlRpc::XmlRpcValue and nlohmann::json.
//
// structmapper::xmlrpc_to_json():   XmlRpcValue -> json
//   TypeInvalid  -> null                          (logged as a warning)
//   TypeBoolean  -> bool
//   TypeInt      -> integer
//   TypeDouble   -> number
//   TypeString   -> string
//   TypeDateTime -> string, ISO 8601 ("%Y-%m-%dT%H:%M:%S")
//   TypeBase64   -> placeholder object            (logged as a warning)
//                     {"__type": "binary", "size": <bytes>}
//   TypeArray    -> array (recursive)
//   TypeStruct   -> object (recursive)
//
// structmapper::json_to_xmlrpc(): json -> XmlRpcValue
//   null          -> TypeInvalid (unset)          (logged as a warning:
//                                                   XML-RPC has no null)
//   bool          -> TypeBoolean
//   integer       -> TypeInt if it fits in 32 bits, else TypeDouble
//                                                  (logged as a warning)
//   number        -> TypeDouble
//   string        -> TypeString                   (note: a string produced
//                                                   from a TypeDateTime by
//                                                   xmlrpc_to_json() comes
//                                                   back as TypeString, not
//                                                   TypeDateTime - the
//                                                   distinction isn't
//                                                   recoverable from JSON)
//   array         -> TypeArray (recursive)
//   object        -> TypeStruct (recursive)       (note: a object produced
//                                                   from TypeBase64 by
//                                                   xmlrpc_to_json() comes
//                                                   back as TypeStruct -
//                                                   distinction isn't
//                                                   recoverable from JSON)
//
// These two directions are not perfect inverses: dates collapse into
// plain strings and binary payloads collapse into a size-only placeholder
// on the way to JSON, and neither is recoverable on the way back. Both
// lossy spots are logged.
//
// XmlRpcValue's own accessors (operator bool&(), operator[](int), begin()/
// end(), ...) are all non-const in xmlrpcpp, so xmlrpc_to_json() takes its
// argument by non-const reference too, matching how the library is used
// elsewhere (e.g. reading params off the ROS parameter server).
#pragma once
#include <xmlrpcpp/XmlRpcValue.h>
#include <nlohmann/json.hpp>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <ctime>
#include <limits>

namespace structmapper {

using nlohmann::json;

// ---------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------
enum class LogLevel { Info, Warning };
using Logger = std::function<void(LogLevel, const std::string&)>;

inline Logger default_logger() {
    return [](LogLevel level, const std::string& msg) {
        if (level == LogLevel::Warning) std::cerr << "[xmlrpc<->json] WARNING: " << msg << "\n";
        else                            std::cout << "[xmlrpc<->json] " << msg << "\n";
    };
}

struct ToJsonStats {
    int invalid_values = 0;
    int binary_values = 0;
};

struct ToXmlRpcStats {
    int null_values = 0;
    int binary_placeholders = 0;
    int oversized_integers = 0;
};

namespace detail {

inline std::string join_path(const std::string& parent, const std::string& child) {
    return parent.empty() ? child : parent + "." + child;
}

inline std::string format_datetime(struct tm& t) {
    char buf[32];
    // xmlrpc's dateTime.iso8601 carries no timezone info - it's whatever
    // the sender put there, taken as-is.
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t) == 0) return "";
    return std::string(buf);
}

json convert_to_json(XmlRpc::XmlRpcValue& v, const std::string& path, Logger& logger, ToJsonStats& stats) {
    using Type = XmlRpc::XmlRpcValue::Type;
    switch (v.getType()) {
        case Type::TypeInvalid: {
            ++stats.invalid_values;
            logger(LogLevel::Warning, "unset/invalid value at '" + path + "' -> null");
            return nullptr;
        }
        case Type::TypeBoolean:
            return json(static_cast<bool>(v));
        case Type::TypeInt:
            return json(static_cast<int>(v));
        case Type::TypeDouble:
            return json(static_cast<double>(v));
        case Type::TypeString:
            return json(static_cast<std::string>(v));
        case Type::TypeDateTime:
            return json(format_datetime(static_cast<struct tm&>(v)));
        case Type::TypeBase64: {
            auto& bin = static_cast<XmlRpc::XmlRpcValue::BinaryData&>(v);
            ++stats.binary_values;
            logger(LogLevel::Warning, "binary value at '" + path + "' (" + std::to_string(bin.size()) +
                                       " bytes) replaced with placeholder");
            json j = json::object();
            j["__type"] = "binary";
            j["size"] = bin.size();
            return j;
        }
        case Type::TypeArray: {
            json arr = json::array();
            int n = v.size();
            for (int i = 0; i < n; ++i) {
                arr.push_back(convert_to_json(v[i], path + "[" + std::to_string(i) + "]", logger, stats));
            }
            return arr;
        }
        case Type::TypeStruct: {
            json obj = json::object();
            for (auto it = v.begin(); it != v.end(); ++it) {
                obj[it->first] = convert_to_json(it->second, join_path(path, it->first), logger, stats);
            }
            return obj;
        }
    }
    return nullptr;
}

inline XmlRpc::XmlRpcValue make_number(const json& j, const std::string& path, Logger& logger, ToXmlRpcStats& stats) {
    if (j.is_number_integer()) {
        long long v = j.get<long long>();
        if (v >= std::numeric_limits<int>::min() && v <= std::numeric_limits<int>::max()) {
            return XmlRpc::XmlRpcValue(static_cast<int>(v));
        }
        ++stats.oversized_integers;
        logger(LogLevel::Warning, "integer at '" + path + "' (" + std::to_string(v) +
                                   ") doesn't fit XML-RPC's 32-bit int; encoded as a double instead");
        return XmlRpc::XmlRpcValue(static_cast<double>(v));
    }
    return XmlRpc::XmlRpcValue(j.get<double>());
}

XmlRpc::XmlRpcValue convert_to_xmlrpc(const json& j, const std::string& path, Logger& logger, ToXmlRpcStats& stats) {
    if (j.is_null()) {
        ++stats.null_values;
        logger(LogLevel::Warning, "null at '" + path + "' has no XML-RPC equivalent; encoded as an unset value");
        return XmlRpc::XmlRpcValue();
    }
    if (j.is_boolean()) {
        return XmlRpc::XmlRpcValue(j.get<bool>());
    }
    if (j.is_number()) {
        return make_number(j, path, logger, stats);
    }
    if (j.is_string()) {
        return XmlRpc::XmlRpcValue(j.get<std::string>());
    }
    if (j.is_array()) {
        XmlRpc::XmlRpcValue arr;
        arr.setSize(static_cast<int>(j.size()));
        for (size_t i = 0; i < j.size(); ++i) {
            arr[static_cast<int>(i)] = convert_to_xmlrpc(j[i], path + "[" + std::to_string(i) + "]", logger, stats);
        }
        return arr;
    }
    if (j.is_object()) {
        XmlRpc::XmlRpcValue obj;
        for (auto it = j.begin(); it != j.end(); ++it) {
            obj[it.key()] = convert_to_xmlrpc(it.value(), join_path(path, it.key()), logger, stats);
        }
        return obj;
    }
    // Unreachable: is_null/is_boolean/is_number/is_string/is_array/is_object
    // cover every nlohmann::json value_t.
    return XmlRpc::XmlRpcValue();
}

} // namespace detail

// ---------------------------------------------------------------------
// Public entry points.
// ---------------------------------------------------------------------
inline json xmlrpc_to_json(XmlRpc::XmlRpcValue& v, ToJsonStats* stats_out = nullptr, Logger logger = default_logger()) {
    ToJsonStats local_stats;
    ToJsonStats& stats = stats_out ? *stats_out : local_stats;
    json result = detail::convert_to_json(v, "$", logger, stats);
    if (stats.invalid_values > 0 || stats.binary_values > 0) {
        std::ostringstream summary;
        summary << "xmlrpc_to_json summary: " << stats.invalid_values << " invalid, "
                << stats.binary_values << " binary value(s) replaced with placeholders";
        logger(LogLevel::Warning, summary.str());
    }
    return result;
}

inline XmlRpc::XmlRpcValue json_to_xmlrpc(const json& j, ToXmlRpcStats* stats_out = nullptr, Logger logger = default_logger()) {
    ToXmlRpcStats local_stats;
    ToXmlRpcStats& stats = stats_out ? *stats_out : local_stats;
    XmlRpc::XmlRpcValue result = detail::convert_to_xmlrpc(j, "$", logger, stats);
    if (stats.null_values > 0 || stats.binary_placeholders > 0 || stats.oversized_integers > 0) {
        std::ostringstream summary;
        summary << "json_to_xmlrpc summary: " << stats.null_values << " null, " << stats.binary_placeholders
                << " binary placeholder(s), " << stats.oversized_integers << " oversized integer(s)";
        logger(LogLevel::Warning, summary.str());
    }
    return result;
}

} // namespace structmapper