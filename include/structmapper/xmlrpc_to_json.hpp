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
    struct XmlRpcToJsonLog {
        using LogLevel = int; // INFO = 0, WARNING = 1
        virtual LogLevel level() const = 0;
        virtual std::string msg() const = 0;
    };
    struct XmlRpcToJsonInvalidValueLog : public XmlRpcToJsonLog {
        std::string path;
        XmlRpcToJsonInvalidValueLog(std::string path) : path(path) {}
        virtual LogLevel level() const override { return 0; }
        virtual std::string msg() const override {
            return "unset/invalid value at " + path + " replaced with null";
        }
    };
    struct XmlRpcToJsonDatetimeValueLog : public XmlRpcToJsonLog {
        std::string path;
        XmlRpcToJsonDatetimeValueLog(std::string path) : path(path) {}
        virtual LogLevel level() const override { return 1; }
        virtual std::string msg() const override {
            return "datetime value at " + path + " replaced with string";
        }
    };
    struct XmlRpcToJsonBinaryValueLog : public XmlRpcToJsonLog {
        std::string path;
        XmlRpcToJsonBinaryValueLog(std::string path) : path(path) {}
        virtual LogLevel level() const override { return 1; }
        virtual std::string msg() const override {
            return "binary value at " + path + " replaced with placeholder";
        }
    };

    using XmlRpcToJsonLogger = std::function<void(const XmlRpcToJsonLog&)>;

    struct XmlRpcToJsonStats {
        int invalid_values = 0;
        int datetime_values = 0;
        int binary_values = 0;
        bool ok() const { return datetime_values == 0 && binary_values == 0; }
        XmlRpcToJsonLogger logger(bool print = true) {
            return [this, print](const XmlRpcToJsonLog& log) {
                if (print) {
                    if (log.level() == 1) std::cerr << "[structmapper] WARNING: " << log.msg() << "\n";
                    else                  std::cout << "[structmapper] " << log.msg() << "\n";
                }

                if (dynamic_cast<const XmlRpcToJsonInvalidValueLog*>(&log))
                    ++this->invalid_values;
                if (dynamic_cast<const XmlRpcToJsonDatetimeValueLog*>(&log))
                    ++this->datetime_values;
                if (dynamic_cast<const XmlRpcToJsonBinaryValueLog*>(&log))
                    ++this->binary_values;
            };
        }
        std::string summary() const {
            std::ostringstream summary;
            summary << "xmlrpc_to_json summary: " << invalid_values << " invalid, "
                    << datetime_values << " datetime value(s), "
                    << binary_values << " binary value(s) replaced with placeholders";
            return summary.str();
        }
    };


    struct JsonToXmlRpcLog {
        using LogLevel = int; // INFO = 0, WARNING = 1
        virtual LogLevel level() const = 0;
        virtual std::string msg() const = 0;
    };
    struct JsonToXmlRpcNullValueLog : public JsonToXmlRpcLog {
        std::string path;
        JsonToXmlRpcNullValueLog(std::string path) : path(path) {}
        virtual LogLevel level() const override { return 0; }
        virtual std::string msg() const override {
            return "null at " + path + " encoded as an unset value";
        }
    };
    struct JsonToXmlRpcOversizedIntegerLog : public JsonToXmlRpcLog {
        std::string path;
        long long value;
        JsonToXmlRpcOversizedIntegerLog(std::string path, long long value) : path(path), value(value) {}
        virtual LogLevel level() const override { return 1; }
        virtual std::string msg() const override {
            return "integer at " + path + " (" + std::to_string(value) + ") doesn't fit XML-RPC's 32-bit int; encoded as a double instead";
        }
    };

    using JsonToXmlRpcLogger = std::function<void(const JsonToXmlRpcLog&)>;

    struct JsonToXmlRpcStats {
        int null_values = 0;
        int oversized_integers = 0;
        bool ok() const { return oversized_integers == 0; }
        JsonToXmlRpcLogger logger(bool print = true) {
            return [this, print](const JsonToXmlRpcLog& log) {
                if (print) {
                    if (log.level() == 1) std::cerr << "[structmapper] WARNING: " << log.msg() << "\n";
                    else                  std::cout << "[structmapper] " << log.msg() << "\n";
                }

                if (dynamic_cast<const JsonToXmlRpcNullValueLog*>(&log))
                    ++this->null_values;
                if (dynamic_cast<const JsonToXmlRpcOversizedIntegerLog*>(&log))
                    ++this->oversized_integers;
            };
        }
        std::string summary() const {
            std::ostringstream summary;
            summary << "json_to_xmlrpc summary: " << null_values << " null, "
                    << oversized_integers << " oversized integer(s)";
            return summary.str();
        }
    };

    namespace detail {

        inline std::string format_datetime(struct tm& t) {
            char buf[32];
            // xmlrpc's dateTime.iso8601 carries no timezone info - it's whatever
            // the sender put there, taken as-is.
            if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t) == 0) return "";
            return std::string(buf);
        }

        void xmlrpc_to_json(XmlRpc::XmlRpcValue& v, json& j, const std::string& path, XmlRpcToJsonLogger& logger) {
            using Type = XmlRpc::XmlRpcValue::Type;
            switch (v.getType()) {
                case Type::TypeInvalid: {
                    logger(XmlRpcToJsonInvalidValueLog(path));
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
                    logger(XmlRpcToJsonDatetimeValueLog(path));
                    j = json(format_datetime(static_cast<struct tm&>(v)));
                    return;
                }
                case Type::TypeBase64: {
                    auto& bin = static_cast<XmlRpc::XmlRpcValue::BinaryData&>(v);
                    logger(XmlRpcToJsonBinaryValueLog(path));
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
                        ::structmapper::detail::xmlrpc_to_json(v[i], e, path + "[" + std::to_string(i) + "]", logger);
                        j.push_back(std::move(e));
                    }
                    return;
                }
                case Type::TypeStruct: {
                    j = json::object();
                    for (auto it = v.begin(); it != v.end(); ++it) {
                        json e;
                        ::structmapper::detail::xmlrpc_to_json(it->second, e, path + "." + it->first, logger);
                        j[it->first] = std::move(e);
                    }
                    return;
                }
            }
            j = json();
            return;
        }

        inline XmlRpc::XmlRpcValue make_number(const json& j, const std::string& path, JsonToXmlRpcLogger& logger) {
            if (j.is_number_integer()) {
                long long v = j.get<long long>();
                if (v >= std::numeric_limits<int>::min() && v <= std::numeric_limits<int>::max()) {
                    return XmlRpc::XmlRpcValue(static_cast<int>(v));
                }
                logger(JsonToXmlRpcOversizedIntegerLog(path, v));
                return XmlRpc::XmlRpcValue(static_cast<double>(v));
            }
            return XmlRpc::XmlRpcValue(j.get<double>());
        }

        void json_to_xmlrpc(const json& j, XmlRpc::XmlRpcValue& v, const std::string& path, JsonToXmlRpcLogger& logger) {
            if (j.is_null()) {
                logger(JsonToXmlRpcNullValueLog(path));
                v = XmlRpc::XmlRpcValue();
                return;
            }
            if (j.is_boolean()) {
                v = XmlRpc::XmlRpcValue(j.get<bool>());
                return;
            }
            if (j.is_number()) {
                v = make_number(j, path, logger);
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
                    ::structmapper::detail::json_to_xmlrpc(j[i], e, path + "[" + std::to_string(i) + "]", logger);
                    v[static_cast<int>(i)] = std::move(e);
                }
                return;
            }
            if (j.is_object()) {
                v = XmlRpc::XmlRpcValue();
                for (auto it = j.begin(); it != j.end(); ++it) {
                    XmlRpc::XmlRpcValue e;
                    ::structmapper::detail::json_to_xmlrpc(it.value(), e, path + "." + it.key(), logger);
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
    inline void xmlrpc_to_json(XmlRpc::XmlRpcValue& v, json& j, const std::string& path, XmlRpcToJsonLogger logger = {}) {
        XmlRpcToJsonStats local_stats;
        if (!logger) logger = local_stats.logger();
        detail::xmlrpc_to_json(v, j, path, logger);
    }

    inline void xmlrpc_to_json(XmlRpc::XmlRpcValue& v, json& j, XmlRpcToJsonLogger logger = {}) {
        XmlRpcToJsonStats local_stats;
        if (!logger) logger = local_stats.logger();
        detail::xmlrpc_to_json(v, j, "$", logger);
    }

    inline json convert_xmlrpc_to_json(XmlRpc::XmlRpcValue& v, XmlRpcToJsonLogger logger = {}) {
        json j;
        XmlRpcToJsonStats local_stats;
        if (!logger) logger = local_stats.logger();
        detail::xmlrpc_to_json(v, j, "$", logger);
        return j;
    }

    inline void json_to_xmlrpc(const json& j, XmlRpc::XmlRpcValue& v, const std::string& path, JsonToXmlRpcLogger logger = {}) {
        JsonToXmlRpcStats local_stats;
        if (!logger) logger = local_stats.logger();
        detail::json_to_xmlrpc(j, v, path, logger);
    }

    inline void json_to_xmlrpc(const json& j, XmlRpc::XmlRpcValue& v, JsonToXmlRpcLogger logger = {}) {
        JsonToXmlRpcStats local_stats;
        if (!logger) logger = local_stats.logger();
        detail::json_to_xmlrpc(j, v, "$", logger);
    }

    inline XmlRpc::XmlRpcValue convert_json_to_xmlrpc(const json& j, JsonToXmlRpcLogger logger = {}) {
        XmlRpc::XmlRpcValue v;
        JsonToXmlRpcStats local_stats;
        if (!logger) logger = local_stats.logger();
        detail::json_to_xmlrpc(j, v, "$", logger);
        return v;
    }

} // namespace structmapper