#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>

#include "structmapper/strenum.hpp"
#include "structmapper/struct_reflect.hpp"
#include "structmapper/struct_to_json.hpp"
#include "structmapper/struct_to_schema.hpp"
#include "structmapper/xmlrpc_to_json.hpp"

using nlohmann::json;

// ---------------------------------------------------------------------
// Test fixtures: reflectable structs
// ---------------------------------------------------------------------

struct Socket {
    int id = 0;
    std::string name = "unset";

    BEGIN_STRUCT("inner widget")
        FIELD(id,   "socket id")
        FIELD(name, "socket name")
    END_STRUCT()
};

struct Camera {
    double fov = 60.0;
    double aspect = 1.777;
    using Kind = ::strenum::StringEnum<CTSTR("pinhole"), CTSTR("ortho")>;
    Kind kind = "pinhole";
    CTSTR("/cam/image_raw") topic{};
    bool enabled = true;
    std::vector<int> resolution = {1920, 1080};
    std::map<std::string, int> tags = {};
    Socket socket;

    BEGIN_STRUCT("camera parameters")
        FIELD(fov,        "field of view, degrees")
        FIELD(aspect,     "aspect ratio")
        FIELD(kind,       "camera projection kind")
        FIELD(topic,      "output topic")
        FIELD(enabled,    "whether the camera is active")
        FIELD(resolution, "pixel resolution [w,h]")
        FIELD(tags,       "arbitrary string->int tags")
        FIELD(socket,     "camera socket")
    END_STRUCT()
};

// // .../test_structmapper.cpp:53:9:   required from here
// // .../struct_reflect.hpp:123:47: error: static assertion failed: ...
// struct Sensors {
//     std::unordered_map<std::string, Camera> cameras = {};
//     BEGIN_STRUCT("sensor parameters")
//         FIELD(cameras, "set of cameras")
//     END_STRUCT()
// };

struct SelfRec {
    std::map<std::string, SelfRec> rec = {};

    BEGIN_STRUCT("test self-recursive type")
        FIELD(rec, "map to self")
    END_STRUCT()
};

using MyEnum = ::strenum::StringEnum<CTSTR("foo"), CTSTR("bar"), CTSTR("a")>;

// =======================================================================
// StringEnum
// =======================================================================

TEST(StringEnumTest, ValidValues)
{
    constexpr MyEnum e1 = "foo";

    EXPECT_STREQ(e1.c_str(), "foo");
    EXPECT_STREQ(static_cast<const char*>(e1), "foo");

    MyEnum e2("bar");
    EXPECT_STREQ(e2.c_str(), "bar");

    MyEnum e3("a");
    EXPECT_STREQ(e3.c_str(), "a");
}

TEST(StringEnumTest, Values)
{
    ASSERT_EQ(MyEnum::values.size(), 3u);

    EXPECT_STREQ(MyEnum::values[0], "foo");
    EXPECT_STREQ(MyEnum::values[1], "bar");
    EXPECT_STREQ(MyEnum::values[2], "a");
}

TEST(StringEnumTest, ValuesAreCanonicalPointers)
{
    MyEnum e1("foo");
    MyEnum e2("bar");
    MyEnum e3("a");

    EXPECT_EQ(e1.c_str(), MyEnum::values[0]);
    EXPECT_EQ(e2.c_str(), MyEnum::values[1]);
    EXPECT_EQ(e3.c_str(), MyEnum::values[2]);
}

TEST(StringEnumTest, RuntimeInvalidValueThrows)
{
    EXPECT_THROW(
        MyEnum("invalid"),
        std::invalid_argument
    );

    EXPECT_THROW(
        MyEnum("foobar"),
        std::invalid_argument
    );

    EXPECT_THROW(
        MyEnum(""),
        std::invalid_argument
    );
}

TEST(StringEnumTest, StringConversion)
{
    MyEnum e("foo");

    EXPECT_EQ(e.str(), "foo");
    EXPECT_STREQ(e.c_str(), "foo");

    const char* s = e;
    EXPECT_STREQ(s, "foo");
}

TEST(StringEnumTest, DifferentEnumsAreDifferentTypes)
{
    using EnumA = ::strenum::StringEnum<CTSTR("foo"), CTSTR("bar")>;
    using EnumB = ::strenum::StringEnum<CTSTR("foo"), CTSTR("baz")>;

    static_assert(!std::is_same<EnumA, EnumB>::value, "");
}

TEST(StringEnumTest, DuplicateValues)
{
    using EnumC = ::strenum::StringEnum<CTSTR("foo"), CTSTR("foo"), CTSTR("bar")>;

    EnumC e("foo");

    EXPECT_EQ(e.c_str(), EnumC::values[0]);
    EXPECT_EQ(e.c_str(), EnumC::values[1]);
}

TEST(StringEnumTest, EmptyEnum)
{
    using EmptyEnum = ::strenum::StringEnum<>;

    EXPECT_EQ(EmptyEnum::values.size(), 0u);
    EXPECT_STREQ(EmptyEnum::type_name(), "");

    EXPECT_THROW(
        EmptyEnum("foo"),
        std::invalid_argument
    );
}

// =======================================================================
// structmapper::to_json / from_json
// =======================================================================

TEST(IsJsonConvertible, AcceptsDocumentedTypes) {
    EXPECT_TRUE(structmapper::is_json_convertible<bool>::value);
    EXPECT_TRUE(structmapper::is_json_convertible<int>::value);
    EXPECT_TRUE(structmapper::is_json_convertible<double>::value);
    EXPECT_TRUE(structmapper::is_json_convertible<std::string>::value);
    EXPECT_TRUE((structmapper::is_json_convertible<std::vector<int>>::value));
    EXPECT_TRUE((structmapper::is_json_convertible<std::map<std::string, int>>::value));
    EXPECT_TRUE((structmapper::is_json_convertible<std::vector<std::map<std::string, int>>>::value));
    EXPECT_TRUE(structmapper::is_json_convertible<Camera>::value); // reflectable
}

TEST(IsJsonConvertible, RejectsUnsupportedTypes) {
    EXPECT_FALSE((structmapper::is_json_convertible<std::map<int, int>>::value)); // non-string key
    EXPECT_FALSE((structmapper::is_json_convertible<std::unordered_map<std::string, int>>::value));
    EXPECT_FALSE(structmapper::is_json_convertible<std::thread>::value);
    EXPECT_FALSE((structmapper::is_json_convertible<std::vector<std::thread>>::value)); // bad element type
    EXPECT_FALSE(structmapper::is_json_convertible<int*>::value);
}

TEST(StructToJson, RoundTripsAllFieldTypes) {
    Camera cam;
    cam.fov = 90.0;
    cam.aspect = 1.6;
    cam.enabled = false;
    cam.resolution = {640, 480};
    cam.tags = {{"a", 1}, {"b", 2}};
    cam.socket.id = 7;
    cam.socket.name = "cmos";

    json j = structmapper::convert_to_json(cam);

    EXPECT_EQ(j["fov"], 90.0);
    EXPECT_EQ(j["aspect"], 1.6);
    EXPECT_EQ(j["enabled"], false);
    EXPECT_EQ(j["resolution"], json::array({640, 480}));
    EXPECT_EQ(j["tags"]["a"], 1);
    EXPECT_EQ(j["tags"]["b"], 2);
    EXPECT_EQ(j["socket"]["id"], 7);
    EXPECT_EQ(j["socket"]["name"], "cmos");
}

TEST(StructFromJson, AllFieldsPresentAndMatchingTypeAreAllLoaded) {
    json j = {
        {"fov", 45.0},
        {"aspect", 2.0},
        {"kind", "pinhole"},
        {"topic", "/cam/image_raw"},
        {"enabled", true},
        {"resolution", {800, 600}},
        {"tags", {{"x", 1}}},
        {"socket", {{"id", 3}, {"name", "s1"}}},
    };

    Camera cam;
    structmapper::FromJsonStats stats;
    cam = structmapper::convert_from_json<Camera>(j, stats.logger());

    EXPECT_TRUE(stats.ok());
    EXPECT_EQ(stats.missing, 0);
    EXPECT_EQ(stats.type_mismatches, 0);
    EXPECT_EQ(stats.unknown_fields, 0);

    EXPECT_EQ(cam.fov, 45.0);
    EXPECT_EQ(cam.aspect, 2.0);
    EXPECT_TRUE(cam.enabled);
    EXPECT_EQ(cam.resolution, (std::vector<int>{800, 600}));
    EXPECT_EQ(cam.tags.at("x"), 1);
    EXPECT_EQ(cam.socket.id, 3);
    EXPECT_EQ(cam.socket.name, "s1");
}

TEST(StructFromJson, MissingKeyLeavesFieldAtItsPriorValue) {
    // Per the doc comment: "A field that fails to load (missing/wrong
    // type) is simply left at whatever value it already had in the target
    // object" — so we pre-seed a non-default value and confirm it survives.
    json j = {
        {"aspect", 1.0},
        {"enabled", true},
        {"kind", "pinhole"},
        {"topic", "/cam/image_raw"},
        {"resolution", {1, 1}},
        {"tags", json::object()},
        {"socket", {{"id", 0}, {"name", "n"}}},
        // "fov" intentionally omitted
    };

    Camera cam;
    cam.fov = 123.0; // sentinel prior value

    structmapper::FromJsonStats stats;
    structmapper::from_json(cam, j, stats.logger());

    EXPECT_EQ(stats.missing, 1);
    EXPECT_EQ(stats.type_mismatches, 0);
    EXPECT_EQ(cam.fov, 123.0) << "missing field must retain its prior value";
}

TEST(StructFromJson, TypeMismatchLeavesFieldAtPriorValueAndIsCounted) {
    json j = {
        {"fov", "not-a-number"}, // wrong type: string instead of number
        {"aspect", 1.0},
        {"enabled", true},
        {"resolution", {1, 1}},
        {"tags", json::object()},
        {"socket", {{"id", 0}, {"name", "n"}}},
    };

    Camera cam;
    cam.fov = 77.0; // sentinel prior value

    structmapper::FromJsonStats stats;
    structmapper::from_json(cam, j, stats.logger());

    EXPECT_FALSE(stats.ok());
    EXPECT_EQ(stats.type_mismatches, 1);
    EXPECT_EQ(cam.fov, 77.0) << "type-mismatched field must retain prior value";
}

TEST(StructFromJson, EnumMismatchLeavesFieldAtPriorValueAndIsCounted) {
    json j = {
        {"fov", 45.0},
        {"aspect", 2.0},
        {"kind", "omni"},
        {"topic", "/left/image_raw"},
        {"enabled", true},
        {"resolution", {800, 600}},
        {"tags", {{"x", 1}}},
        {"socket", {{"id", 3}, {"name", "s1"}}},
    };

    Camera cam;

    structmapper::FromJsonStats stats;
    structmapper::from_json(cam, j, stats.logger());

    EXPECT_FALSE(stats.ok());
    EXPECT_EQ(stats.type_mismatches, 2);
    EXPECT_STREQ(cam.kind, "pinhole") << "type-mismatched field must retain prior value";
    EXPECT_STREQ(cam.topic.c_str(), "/cam/image_raw") << "type-mismatched field must retain prior value";
}

TEST(StructFromJson, UnknownJsonKeysAreCountedButDoNotError) {
    json j = {
        {"fov", 1.0},
        {"aspect", 1.0},
        {"enabled", true},
        {"resolution", {1, 1}},
        {"tags", json::object()},
        {"socket", {{"id", 0}, {"name", "n"}}},
        {"totally_unrecognized_key", 42},
    };

    Camera cam;

    structmapper::FromJsonStats stats;
    structmapper::from_json(cam, j, stats.logger());

    EXPECT_TRUE(stats.ok()) << "unknown fields alone must not flip ok() to false";
    EXPECT_EQ(stats.unknown_fields, 1);
}

TEST(StructFromJson, RecursesIntoNestedStructsAndReportsTheirStats) {
    json j = {
        {"fov", 1.0},
        {"aspect", 1.0},
        {"enabled", true},
        {"resolution", {1, 1}},
        {"tags", json::object()},
        {"socket", {{"id", "wrong-type"}, {"name", "n"}}}, // nested type error
    };

    Camera cam;

    structmapper::FromJsonStats stats;
    structmapper::from_json(cam, j, stats.logger());

    EXPECT_FALSE(stats.ok());
    EXPECT_GE(stats.type_mismatches, 1)
        << "a type error nested inside a sub-struct must propagate to the stats";
}

TEST(StructFromJson, DefaultConstructedTargetGetsDocumentedFallbackBehavior) {
    // Doc: "callers should default-construct before calling from_json if
    // they want that fallback behavior" — verify defaults survive when
    // the whole payload is empty.
    Camera cam; // default-constructed
    json j = json::object();

    structmapper::FromJsonStats stats;
    structmapper::from_json(cam, j, stats.logger());

    EXPECT_EQ(stats.loaded, 0);
    EXPECT_GT(stats.missing, 0);
    EXPECT_EQ(cam.fov, 60.0);
    EXPECT_EQ(cam.aspect, 1.777);
    EXPECT_TRUE(cam.enabled);
}

TEST(FromJsonStats, SummaryIsNonEmptyAndOkReflectsOnlyTypeMismatches) {
    structmapper::FromJsonStats stats;
    stats.loaded = 5;
    stats.missing = 2;
    stats.unknown_fields = 3;
    stats.type_mismatches = 0;

    EXPECT_TRUE(stats.ok())
        << "ok() is documented as tracking type_mismatches only, "
           "not missing/unknown counts";
    EXPECT_FALSE(stats.summary().empty());

    stats.type_mismatches = 1;
    EXPECT_FALSE(stats.ok());
}

// =======================================================================
// structmapper::to_schema<T>()
// =======================================================================

TEST(ToSchema, Schema) {
    json schema = structmapper::to_schema<Camera>();
    json expect = {
        {"description", "camera parameters"},
        {"properties", {
            {"aspect", {
                {"default", 1.777},
                {"description", "aspect ratio"},
                {"type", "number"}
            }},
            {"enabled", {
                {"default", true},
                {"description", "whether the camera is active"},
                {"type", "boolean"}
            }},
            {"fov", {
                {"default", 60.0},
                {"description", "field of view, degrees"},
                {"type", "number"}
            }},
            {"kind", {
                {"default", "pinhole"},
                {"description", "camera projection kind"},
                {"enum", {
                    "pinhole",
                    "ortho"
                }}
            }},
            {"resolution", {
                {"description", "pixel resolution [w,h]"},
                {"items", {
                    {"type", "integer"}
                }},
                {"type", "array"}
            }},
            {"socket", {
                {"anyOf", {
                    {
                        {"description", "inner widget"},
                        {"properties", {
                            {"id", {
                                {"default", 0},
                                {"description", "socket id"},
                                {"type", "integer"}
                            }},
                            {"name", {
                                {"default", "unset"},
                                {"description", "socket name"},
                                {"type", "string"}
                            }}
                        }},
                        {"type", "object"}
                    }
                }},
                {"description", "camera socket"}
            }},
            {"tags", {
                {"additionalProperties", {
                    {"type", "integer"}
                }},
                {"description", "arbitrary string->int tags"},
                {"type", "object"}
            }},
            {"topic", {
                {"const", "/cam/image_raw"},
                {"default", "/cam/image_raw"},
                {"description", "output topic"}
            }}
        }},
        {"type", "object"},
    };

    EXPECT_EQ(schema, expect);
}

// =======================================================================
// structmapper::xmlrpc_to_json
// =======================================================================

class XmlRpcToJsonTest : public ::testing::Test {
};

TEST_F(XmlRpcToJsonTest, BooleanIntDoubleStringMapDirectly) {
    XmlRpc::XmlRpcValue v;
    v = true;
    json j;
    structmapper::XmlRpcToJsonStats stats;
    j = structmapper::convert_xmlrpc_to_json(v, stats.logger());
    EXPECT_EQ(j, true);
    EXPECT_TRUE(stats.ok());

    XmlRpc::XmlRpcValue vi;
    vi = 42;
    json ji = structmapper::convert_xmlrpc_to_json(vi);
    EXPECT_EQ(ji, 42);

    XmlRpc::XmlRpcValue vd;
    vd = 3.5;
    json jd = structmapper::convert_xmlrpc_to_json(vd);
    EXPECT_EQ(jd, 3.5);

    XmlRpc::XmlRpcValue vs;
    vs = std::string("hello");
    json js = structmapper::convert_xmlrpc_to_json(vs);
    EXPECT_EQ(js, "hello");
}

TEST_F(XmlRpcToJsonTest, InvalidValueBecomesNullAndIsLoggedAsWarning) {
    XmlRpc::XmlRpcValue v; // default-constructed == TypeInvalid
    json j;
    structmapper::XmlRpcToJsonStats stats;
    j = structmapper::convert_xmlrpc_to_json(v, stats.logger());

    EXPECT_TRUE(j.is_null());
    EXPECT_EQ(stats.invalid_values, 1);
    EXPECT_EQ(stats.datetime_values, 0);
    EXPECT_EQ(stats.binary_values, 0);
    EXPECT_TRUE(stats.ok()) << "invalid_values does not affect ok() per the struct's ok() def";
}

TEST_F(XmlRpcToJsonTest, DateTimeBecomesIso8601StringAndIsLossy) {
    XmlRpc::XmlRpcValue v;
    struct tm t{};
    t.tm_year = 124; t.tm_mon = 0; t.tm_mday = 15; // 2024-01-15
    t.tm_hour = 10; t.tm_min = 30; t.tm_sec = 0;
    v = XmlRpc::XmlRpcValue(&t);

    json j;
    structmapper::XmlRpcToJsonStats stats;
    j = structmapper::convert_xmlrpc_to_json(v, stats.logger());

    ASSERT_TRUE(j.is_string());
    EXPECT_EQ(j.get<std::string>(), "2024-01-15T10:30:00");
    EXPECT_EQ(stats.invalid_values, 0);
    EXPECT_EQ(stats.datetime_values, 1);
    EXPECT_EQ(stats.binary_values, 0);
    EXPECT_FALSE(stats.ok()) << "ok() is false when datetime_values > 0";
}

TEST_F(XmlRpcToJsonTest, Base64BecomesPlaceholderObjectWithSize) {
    XmlRpc::XmlRpcValue v;
    std::vector<char> bytes = {'a', 'b', 'c', 'd', 'e'};
    // Assumes an xmlrpcpp-style vector<char> assignment for base64 payloads.
    v = XmlRpc::XmlRpcValue(bytes.data(), bytes.size());

    json j;
    structmapper::XmlRpcToJsonStats stats;
    j = structmapper::convert_xmlrpc_to_json(v, stats.logger());

    EXPECT_EQ(j["__type"], "binary");
    EXPECT_EQ(j["size"], 5);
    EXPECT_EQ(stats.binary_values, 1);
    EXPECT_FALSE(stats.ok());
}

TEST_F(XmlRpcToJsonTest, ArrayAndStructRecurseIntoChildren) {
    XmlRpc::XmlRpcValue arr;
    arr.setSize(2);
    arr[0] = 1;
    arr[1] = 2;

    json j;
    structmapper::XmlRpcToJsonStats stats;
    j = structmapper::convert_xmlrpc_to_json(arr, stats.logger());

    ASSERT_TRUE(j.is_array());
    EXPECT_EQ(j, json::array({1, 2}));
    EXPECT_TRUE(stats.ok());

    XmlRpc::XmlRpcValue st;
    st["a"] = 1;
    st["b"] = std::string("x");

    json js;
    structmapper::xmlrpc_to_json(st, js);
    ASSERT_TRUE(js.is_object());
    EXPECT_EQ(js["a"], 1);
    EXPECT_EQ(js["b"], "x");
}

// =======================================================================
// structmapper::json_to_xmlrpc
// =======================================================================

class JsonToXmlRpcTest : public ::testing::Test {
};

TEST_F(JsonToXmlRpcTest, NullBecomesInvalidAndIsLoggedAsWarning) {
    json j = nullptr;
    XmlRpc::XmlRpcValue v;
    structmapper::JsonToXmlRpcStats stats;
    v = structmapper::convert_json_to_xmlrpc(j, stats.logger());

    EXPECT_EQ(v.getType(), XmlRpc::XmlRpcValue::TypeInvalid);
    EXPECT_EQ(stats.null_values, 1);
    EXPECT_EQ(stats.oversized_integers, 0);
    EXPECT_TRUE(stats.ok()) << "null_values alone does not affect ok()";
}

TEST_F(JsonToXmlRpcTest, SmallIntegerBecomesTypeInt) {
    json j = 123;
    XmlRpc::XmlRpcValue v;
    structmapper::JsonToXmlRpcStats stats;
    v = structmapper::convert_json_to_xmlrpc(j, stats.logger());

    EXPECT_EQ(v.getType(), XmlRpc::XmlRpcValue::TypeInt);
    EXPECT_EQ(static_cast<int>(v), 123);
    EXPECT_EQ(stats.oversized_integers, 0);
    EXPECT_TRUE(stats.ok());
}

TEST_F(JsonToXmlRpcTest, OversizedIntegerFallsBackToTypeDoubleAndIsCounted) {
    json j = static_cast<int64_t>(1) << 40; // does not fit in 32 bits
    XmlRpc::XmlRpcValue v;
    structmapper::JsonToXmlRpcStats stats;
    v = structmapper::convert_json_to_xmlrpc(j, stats.logger());

    EXPECT_EQ(v.getType(), XmlRpc::XmlRpcValue::TypeDouble);
    EXPECT_EQ(stats.null_values, 0);
    EXPECT_EQ(stats.oversized_integers, 1);
    EXPECT_FALSE(stats.ok());
}

TEST_F(JsonToXmlRpcTest, FloatingPointNumberBecomesTypeDouble) {
    json j = 3.14;
    XmlRpc::XmlRpcValue v = structmapper::convert_json_to_xmlrpc(j);

    EXPECT_EQ(v.getType(), XmlRpc::XmlRpcValue::TypeDouble);
    EXPECT_DOUBLE_EQ(static_cast<double>(v), 3.14);
}

TEST_F(JsonToXmlRpcTest, StringBecomesTypeStringNeverTypeDateTime) {
    // Doc: a string round-tripped from a prior TypeDateTime comes back as
    // TypeString, not TypeDateTime — the distinction is not recoverable.
    json j = "2024-01-15T10:30:00";
    XmlRpc::XmlRpcValue v = structmapper::convert_json_to_xmlrpc(j);

    EXPECT_EQ(v.getType(), XmlRpc::XmlRpcValue::TypeString);
}

TEST_F(JsonToXmlRpcTest, ObjectBecomesTypeStructNeverTypeBase64) {
    // Doc: an object round-tripped from a prior TypeBase64 placeholder
    // comes back as TypeStruct, not TypeBase64.
    json j = {{"__type", "binary"}, {"size", 5}};
    XmlRpc::XmlRpcValue v = structmapper::convert_json_to_xmlrpc(j);

    EXPECT_EQ(v.getType(), XmlRpc::XmlRpcValue::TypeStruct);
    EXPECT_EQ(static_cast<std::string>(v["__type"]), "binary");
}

TEST_F(JsonToXmlRpcTest, ArrayAndObjectRecurseIntoChildren) {
    json j = json::array({1, 2, 3});
    XmlRpc::XmlRpcValue v;
    structmapper::JsonToXmlRpcStats stats;
    v = structmapper::convert_json_to_xmlrpc(j, stats.logger());

    EXPECT_EQ(v.getType(), XmlRpc::XmlRpcValue::TypeArray);
    EXPECT_EQ(v.size(), 3);
    EXPECT_TRUE(stats.ok());

    json jo = {{"a", 1}, {"b", "x"}};
    XmlRpc::XmlRpcValue vo;
    structmapper::json_to_xmlrpc(jo, vo);

    EXPECT_EQ(vo.getType(), XmlRpc::XmlRpcValue::TypeStruct);
    EXPECT_EQ(static_cast<int>(vo["a"]), 1);
    EXPECT_EQ(static_cast<std::string>(vo["b"]), "x");
}

// =======================================================================
// Round-trip lossiness (documented explicitly as NOT perfect inverses)
// =======================================================================

TEST(RoundTrip, DateTimeThroughJsonAndBackLosesTypeButKeepsText) {
    XmlRpc::XmlRpcValue original;
    struct tm t{};
    t.tm_year = 124; t.tm_mon = 5; t.tm_mday = 1; t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0;
    original = XmlRpc::XmlRpcValue(&t);

    json j;
    structmapper::xmlrpc_to_json(original, j);

    XmlRpc::XmlRpcValue back;
    structmapper::json_to_xmlrpc(j, back);

    EXPECT_EQ(back.getType(), XmlRpc::XmlRpcValue::TypeString)
        << "date/time-ness is not recoverable from JSON, per the header doc";
}

TEST(RoundTrip, Base64ThroughJsonAndBackLosesTypeButKeepsPlaceholder) {
    XmlRpc::XmlRpcValue original;
    std::vector<char> bytes = {'x', 'y', 'z'};
    original = XmlRpc::XmlRpcValue(bytes.data(), bytes.size());

    json j;
    structmapper::xmlrpc_to_json(original, j);

    XmlRpc::XmlRpcValue back;
    structmapper::json_to_xmlrpc(j, back);

    EXPECT_EQ(back.getType(), XmlRpc::XmlRpcValue::TypeStruct)
        << "binary-ness is not recoverable from JSON, per the header doc";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
