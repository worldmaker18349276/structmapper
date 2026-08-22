// bidirectional conversion between XmlRpc::XmlRpcValue and nlohmann::json.
//
// structmapper::xmlrpc_to_json():   XmlRpcValue -> json
//   TypeInvalid  -> null                          (logged as a warning)
//   TypeBoolean  -> bool
//   TypeInt      -> integer
//   TypeDouble   -> number
//   TypeString   -> string
//   TypeDateTime -> string, ISO 8601              (logged as a warning)
//                     "%Y-%m-%dT%H:%M:%S"
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
    using LogLevel = int; // INFO = 0, WARNING = 1
    using Logger = std::function<void(LogLevel, const std::string&)>;

    struct XmlRpcToJsonStats {
        int invalid_values = 0;
        int datetime_values = 0;
        int binary_values = 0;
        bool ok() const { return datetime_values == 0 && binary_values == 0; }
        std::string summary() const {
            std::ostringstream summary;
            summary << "xmlrpc_to_json summary: " << invalid_values << " invalid, "
                    << datetime_values << " datetime value(s), "
                    << binary_values << " binary value(s) replaced with placeholders";
            return summary.str();
        }
    };

    struct JsonToXmlRpcStats {
        int null_values = 0;
        int oversized_integers = 0;
        bool ok() const { return oversized_integers == 0; }
        std::string summary() const {
            std::ostringstream summary;
            summary << "json_to_xmlrpc summary: " << null_values << " null, "
                    << oversized_integers << " oversized integer(s)";
            return summary.str();
        }
    };

    namespace detail {

        inline Logger xmlrpc_to_json_default_logger() {
            return [](LogLevel level, const std::string& msg) {
                if (level == 1) std::cerr << "[structmapper] WARNING: " << msg << "\n";
                else            std::cout << "[structmapper] " << msg << "\n";
            };
        }

        inline std::string format_datetime(struct tm& t) {
            char buf[32];
            // xmlrpc's dateTime.iso8601 carries no timezone info - it's whatever
            // the sender put there, taken as-is.
            if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t) == 0) return "";
            return std::string(buf);
        }

        void xmlrpc_to_json(XmlRpc::XmlRpcValue& v, json& j, const std::string& path, Logger& logger, XmlRpcToJsonStats& stats) {
            using Type = XmlRpc::XmlRpcValue::Type;
            switch (v.getType()) {
                case Type::TypeInvalid: {
                    ++stats.invalid_values;
                    logger(1, "unset/invalid value at " + path + " replaced with null");
                    j = json();
                    return;
                }
                case Type::TypeBoolean:
                    j = json(static_cast<bool>(v));
                    return;
                case Type::TypeInt:
                    j = json(static_cast<int>(v));
                    return;
                case Type::TypeDouble:
                    j = json(static_cast<double>(v));
                    return;
                case Type::TypeString:
                    j = json(static_cast<std::string>(v));
                    return;
                case Type::TypeDateTime: {
                    ++stats.datetime_values;
                    logger(1, "datetime value at " + path + " replaced with string");
                    j = json(format_datetime(static_cast<struct tm&>(v)));
                    return;
                }
                case Type::TypeBase64: {
                    auto& bin = static_cast<XmlRpc::XmlRpcValue::BinaryData&>(v);
                    ++stats.binary_values;
                    logger(1, "binary value at " + path + " replaced with placeholder");
                    j = json::object();
                    j["__type"] = "binary";
                    j["size"] = bin.size();
                    return;
                }
                case Type::TypeArray: {
                    j = json::array();
                    int n = v.size();
                    for (int i = 0; i < n; ++i) {
                        json e;
                        xmlrpc_to_json(v[i], e, path + "[" + std::to_string(i) + "]", logger, stats);
                        j.push_back(std::move(e));
                    }
                    return;
                }
                case Type::TypeStruct: {
                    j = json::object();
                    for (auto it = v.begin(); it != v.end(); ++it) {
                        json e;
                        xmlrpc_to_json(it->second, e, path + "." + it->first, logger, stats);
                        j[it->first] = std::move(e);
                    }
                    return;
                }
            }
            j = json();
            return;
        }

        inline XmlRpc::XmlRpcValue make_number(const json& j, const std::string& path, Logger& logger, JsonToXmlRpcStats& stats) {
            if (j.is_number_integer()) {
                long long v = j.get<long long>();
                if (v >= std::numeric_limits<int>::min() && v <= std::numeric_limits<int>::max()) {
                    return XmlRpc::XmlRpcValue(static_cast<int>(v));
                }
                ++stats.oversized_integers;
                logger(1, "integer at " + path + " (" + std::to_string(v) +
                                        ") doesn't fit XML-RPC's 32-bit int; encoded as a double instead");
                return XmlRpc::XmlRpcValue(static_cast<double>(v));
            }
            return XmlRpc::XmlRpcValue(j.get<double>());
        }

        void json_to_xmlrpc(const json& j, XmlRpc::XmlRpcValue& v, const std::string& path, Logger& logger, JsonToXmlRpcStats& stats) {
            if (j.is_null()) {
                ++stats.null_values;
                logger(1, "null at " + path + " has no XML-RPC equivalent; encoded as an unset value");
                v = XmlRpc::XmlRpcValue();
                return;
            }
            if (j.is_boolean()) {
                v = XmlRpc::XmlRpcValue(j.get<bool>());
                return;
            }
            if (j.is_number()) {
                v = make_number(j, path, logger, stats);
                return;
            }
            if (j.is_string()) {
                v = XmlRpc::XmlRpcValue(j.get<std::string>());
                return;
            }
            if (j.is_array()) {
                v = XmlRpc::XmlRpcValue();
                v.setSize(static_cast<int>(j.size()));
                for (size_t i = 0; i < j.size(); ++i) {
                    XmlRpc::XmlRpcValue e;
                    json_to_xmlrpc(j[i], e, path + "[" + std::to_string(i) + "]", logger, stats);
                    v[static_cast<int>(i)] = std::move(e);
                }
                return;
            }
            if (j.is_object()) {
                v = XmlRpc::XmlRpcValue();
                for (auto it = j.begin(); it != j.end(); ++it) {
                    XmlRpc::XmlRpcValue e;
                    json_to_xmlrpc(it.value(), e, path + "." + it.key(), logger, stats);
                    v[it.key()] = std::move(e);
                }
                return;
            }
            // Unreachable: is_null/is_boolean/is_number/is_string/is_array/is_object
            // cover every nlohmann::json value_t.
            v = XmlRpc::XmlRpcValue();
            return;
        }

    } // namespace detail

    // ---------------------------------------------------------------------
    // Public entry points.
    // ---------------------------------------------------------------------
    inline XmlRpcToJsonStats xmlrpc_to_json(XmlRpc::XmlRpcValue& v, json& j, const std::string& path = "$", Logger logger = detail::xmlrpc_to_json_default_logger()) {
        XmlRpcToJsonStats stats;
        detail::xmlrpc_to_json(v, j, path, logger, stats);
        logger(0, stats.summary());
        return stats;
    }

    inline JsonToXmlRpcStats json_to_xmlrpc(const json& j, XmlRpc::XmlRpcValue& v, const std::string& path = "$", Logger logger = detail::xmlrpc_to_json_default_logger()) {
        JsonToXmlRpcStats stats;
        detail::json_to_xmlrpc(j, v, path, logger, stats);
        logger(0, stats.summary());
        return stats;
    }

} // namespace structmapper