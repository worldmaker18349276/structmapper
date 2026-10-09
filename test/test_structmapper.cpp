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
#include "structmapper/struct_equal.hpp"
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
    std::array<int, 2> resolution = {1920, 1080};
    std::map<std::string, int> tags = {};
    Socket socket;
    std::uint32_t buffer_size = 10;

    BEGIN_STRUCT("camera parameters")
        FIELD(fov,        "field of view, degrees")
        FIELD(aspect,     "aspect ratio")
        FIELD(kind,       "camera projection kind")
        FIELD(topic,      "output topic")
        FIELD(enabled,    "whether the camera is active")
        FIELD(resolution, "pixel resolution [w,h]")
        FIELD(tags,       "arbitrary string->int tags")
        FIELD(socket,     "camera socket")
        FIELD(buffer_size,"buffer size")
    END_STRUCT()
};

struct Vector {
    double data[3];
    
    double& x() { return data[0]; }
    const double& x() const { return data[0]; }
    double& y() { return data[1]; }
    const double& y() const { return data[1]; }
    double& z() { return data[2]; }
    const double& z() const { return data[2]; }
};

BEGIN_EXTERNAL_STRUCT(Vector, "vector")
    FIELD_EXPR_NAMED(&self.x(), "x", "x coordinate")
    FIELD_EXPR_NAMED(&self.y(), "y", "y coordinate")
    FIELD_EXPR_NAMED(&self.z(), "z", "z coordinate")
END_EXTERNAL_STRUCT()

struct Quaternion {
    double data[4];
    
    double& x() { return data[0]; }
    const double& x() const { return data[0]; }
    double& y() { return data[1]; }
    const double& y() const { return data[1]; }
    double& z() { return data[2]; }
    const double& z() const { return data[2]; }
    double& w() { return data[3]; }
    const double& w() const { return data[3]; }
};

BEGIN_EXTERNAL_STRUCT(Quaternion, "quaternion")
    FIELD_EXPR_NAMED(&self.x(), "x", "x coordinate")
    FIELD_EXPR_NAMED(&self.y(), "y", "y coordinate")
    FIELD_EXPR_NAMED(&self.z(), "z", "z coordinate")
    FIELD_EXPR_NAMED(&self.w(), "w", "w coordinate")
END_EXTERNAL_STRUCT()

struct Pose {
    Vector position;
    Quaternion orientation;
    
    BEGIN_STRUCT("pose")
        FIELD(position, "position of pose")
        FIELD(orientation, "orientation of pose")
    END_STRUCT()
};

struct Matrix3x3 {
    double data[3][3];

    BEGIN_STRUCT("3x3 matrix")
        FIELD(data, "data")
    END_STRUCT()
};

struct EMatrix3x3 {
    double data[9];
};

#define FIELD_EMatrix3x3(VAR, DESC) FIELD_EXPR_NAMED(&::structmapper::view_as<double[3][3]>(self.VAR.data), #VAR, DESC)

struct WithExpr {
    EMatrix3x3 mat;
    int width_ = 1920;
    int height_ = 1080;

    std::array<int, 2> get_resolution() const { return {width_, height_}; }
    void set_resolution(const std::array<int, 2>& v) { width_ = v[0]; height_ = v[1]; }
    auto resolution_proxy() const { return structmapper::proxy(get_resolution()); }
    auto resolution_proxy() { return structmapper::proxy(get_resolution(), [this](const auto& v){ set_resolution(v); }); }

    BEGIN_STRUCT("with complex fields requiring expr accessor")
        FIELD_EMatrix3x3(mat, "matrix 3x3")
        FIELD_EXPR_NAMED(self.resolution_proxy(), "resolution", "resolution")
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

TEST(StructToJson, WithExternalType) {
    Pose pose{
        Vector{0.0, 0.0, 1.0},
        Quaternion{0.5, -0.5, 0.5, -0.5},
    };

    json j = structmapper::convert_to_json(pose);
    json expected = {
        {"position", {{"x", 0.0}, {"y", 0.0}, {"z", 1.0}}},
        {"orientation", {{"x", 0.5}, {"y", -0.5}, {"z", 0.5}, {"w", -0.5}}},
    };

    EXPECT_EQ(j, expected);
}

TEST(StructToJson, RawArray) {
    Matrix3x3 mat{
        {
            {0.0, 0.0, 1.0},
            {-1.0, 0.0, 0.0},
            {0.0, -1.0, 0.0},
        }
    };

    json j = structmapper::convert_to_json(mat);
    json expected = {
        {"data", 
            json::array({
                {0.0, 0.0, 1.0},
                {-1.0, 0.0, 0.0},
                {0.0, -1.0, 0.0},
            })
        },
    };

    EXPECT_EQ(j, expected);
}

TEST(StructToJson, RawArrayEq) {
    Matrix3x3 mat1{
        {
            {0.0, 0.0, 1.0},
            {-1.0, 0.0, 0.0},
            {0.0, -1.0, 0.0},
        }
    };

    Matrix3x3 mat2{
        {
            {0.0, 0.0, 1.0},
            {-1.0, 0.0, 0.0},
            {0.0, -0.9, 0.0},
        }
    };

    EXPECT_TRUE(structmapper::struct_equal<Matrix3x3>(mat1, mat1));
    EXPECT_FALSE(structmapper::struct_equal<Matrix3x3>(mat1, mat2));
}

TEST(StructToJson, RawArraySchema) {
    json schema = structmapper::to_schema<Matrix3x3>();
    json expect = {
        {"description", "3x3 matrix"},
        {"properties", {
            {"data", {
                {"description", "data"},
                {"items", {
                    {"items", {
                        {"type", "number"}
                    }},
                    {"maxItems", 3},
                    {"minItems", 3},
                    {"type", "array"}
                }},
                {"maxItems", 3},
                {"minItems", 3},
                {"type", "array"}
            }}
        }},
        {"type", "object"}
    };

    EXPECT_EQ(schema, expect);
}


TEST(StructToJson, WithFieldExpr) {
    WithExpr obj;
    for (int i = 0; i < 9; i++)
        obj.mat.data[i] = i;
    obj.width_ = 150;
    obj.height_ = 100;

    json j = structmapper::convert_to_json(obj);
    json expected = {
        {"mat", 
            json::array({
                {0.0, 1.0, 2.0},
                {3.0, 4.0, 5.0},
                {6.0, 7.0, 8.0},
            })
        },
        {"resolution", {150, 100}},
    };

    EXPECT_EQ(j, expected);

    WithExpr obj2 = structmapper::convert_from_json<WithExpr>(j);

    for (int i = 0; i < 9; i++)
        EXPECT_EQ(obj2.mat.data[i], i);
    EXPECT_EQ(obj2.width_, 150);
    EXPECT_EQ(obj2.height_, 100);
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
        {"buffer_size", 15},
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
    EXPECT_EQ(cam.resolution, (std::array<int, 2>{800, 600}));
    EXPECT_EQ(cam.tags.at("x"), 1);
    EXPECT_EQ(cam.socket.id, 3);
    EXPECT_EQ(cam.socket.name, "s1");
    EXPECT_EQ(cam.buffer_size, 15);
}

TEST(StructFromJson, WithExternalType) {
    json j = {
        {"position", {{"x", 0.0}, {"y", 0.0}, {"z", 1.0}}},
        {"orientation", {{"x", 0.5}, {"y", -0.5}, {"z", 0.5}, {"w", -0.5}}},
    };

    Pose pose;
    structmapper::FromJsonStats stats;
    pose = structmapper::convert_from_json<Pose>(j, stats.logger());

    EXPECT_TRUE(stats.ok());
    EXPECT_EQ(stats.missing, 0);
    EXPECT_EQ(stats.type_mismatches, 0);
    EXPECT_EQ(stats.unknown_fields, 0);

    EXPECT_EQ(pose.position.x(), 0.0);
    EXPECT_EQ(pose.position.y(), 0.0);
    EXPECT_EQ(pose.position.z(), 1.0);
    EXPECT_EQ(pose.orientation.x(), 0.5);
    EXPECT_EQ(pose.orientation.y(), -0.5);
    EXPECT_EQ(pose.orientation.z(), 0.5);
    EXPECT_EQ(pose.orientation.w(), -0.5);
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
        {"buffer_size", 10},
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

TEST(StructFromJson, RangeMismatchLeavesFieldAtPriorValueAndIsCounted) {
    json j = {
        {"fov", 45.0},
        {"aspect", 1.0},
        {"enabled", true},
        {"resolution", {1, 1}},
        {"tags", json::object()},
        {"socket", {{"id", 0}, {"name", "n"}}},
        {"buffer_size", -1},
    };

    Camera cam;

    structmapper::FromJsonStats stats;
    structmapper::from_json(cam, j, stats.logger());

    EXPECT_FALSE(stats.ok());
    EXPECT_EQ(stats.type_mismatches, 1);
    EXPECT_EQ(cam.buffer_size, 10) << "type-mismatched field must retain prior value";
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
            {"buffer_size", {
                {"default", 10},
                {"description", "buffer size"},
                {"type", "integer"},
                {"maximum", 4294967295},
                {"minimum", 0},
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
                {"description", "camera projection kind"},
                {"enum", {
                    "pinhole",
                    "ortho"
                }}
            }},
            {"resolution", {
                {"default", {1920, 1080}},
                {"description", "pixel resolution [w,h]"},
                {"items", {
                    {"type", "integer"},
                    {"maximum", 2147483647},
                    {"minimum", -2147483648}
                }},
                {"minItems", 2},
                {"maxItems", 2},
                {"type", "array"}
            }},
            {"socket", {
                {"anyOf", {
                    {
                        {"description", "inner widget"},
                        {"properties", {
                            {"id", {
                                {"description", "socket id"},
                                {"type", "integer"},
                                {"maximum", 2147483647},
                                {"minimum", -2147483648}
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
                    {"type", "integer"},
                    {"maximum", 2147483647},
                    {"minimum", -2147483648}
                }},
                {"description", "arbitrary string->int tags"},
                {"type", "object"}
            }},
            {"topic", {
                {"const", "/cam/image_raw"},
                {"description", "output topic"}
            }}
        }},
        {"type", "object"},
    };

    EXPECT_EQ(schema, expect);
}

TEST(ToSchema, SchemaWithExternalType) {
    json schema = structmapper::to_schema<Pose>();
    json expect = {
        {"description", "pose"},
        {"properties", {
            {"orientation", {
                {"anyOf", {
                    {
                        {"description", "quaternion"},
                        {"properties", {
                            {"w", {
                                {"description", "w coordinate"},
                                {"type", "number"},
                            }},
                            {"x", {
                                {"description", "x coordinate"},
                                {"type", "number"},
                            }},
                            {"y", {
                                {"description", "y coordinate"},
                                {"type", "number"},
                            }},
                            {"z", {
                                {"description", "z coordinate"},
                                {"type", "number"},
                            }},
                        }},
                        {"type", "object"}
                    }
                }},
                {"description", "orientation of pose"}
            }},
            {"position", {
                {"anyOf", {
                    {
                        {"description", "vector"},
                        {"properties", {
                            {"x", {
                                {"description", "x coordinate"},
                                {"type", "number"},
                            }},
                            {"y", {
                                {"description", "y coordinate"},
                                {"type", "number"},
                            }},
                            {"z", {
                                {"description", "z coordinate"},
                                {"type", "number"},
                            }},
                        }},
                        {"type", "object"}
                    }
                }},
                {"description", "position of pose"}
            }}
        }},
        {"type", "object"}
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

TEST_F(XmlRpcToJsonTest, NaNInfinityNumberTypeArePreserved) {
    {
        XmlRpc::XmlRpcValue v;
        v = std::numeric_limits<double>::quiet_NaN();
        json j;
        structmapper::XmlRpcToJsonStats stats;
        j = structmapper::convert_xmlrpc_to_json(v, stats.logger());
        EXPECT_TRUE(std::isnan(j.get<double>()));
        EXPECT_TRUE(stats.ok());
    }

    {
        XmlRpc::XmlRpcValue v;
        v = std::numeric_limits<double>::infinity();
        json j;
        structmapper::XmlRpcToJsonStats stats;
        j = structmapper::convert_xmlrpc_to_json(v, stats.logger());
        EXPECT_TRUE(j.get<double>() == std::numeric_limits<double>::infinity());
        EXPECT_TRUE(stats.ok());
    }

    {
        XmlRpc::XmlRpcValue v;
        v = 42;
        json j = structmapper::convert_xmlrpc_to_json(v);
        EXPECT_TRUE(j.is_number_integer());
    }

    {
        XmlRpc::XmlRpcValue v;
        v = 3.5;
        json j = structmapper::convert_xmlrpc_to_json(v);
        EXPECT_TRUE(j.is_number_float());
    }
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

// ---------------------------------------------------------------------
// Helpers (Vector/Quaternion/Pose have uninitialized storage by default)
// ---------------------------------------------------------------------
 
namespace {
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    constexpr double kInf = std::numeric_limits<double>::infinity();
}

static Vector MakeVec(double x, double y, double z) {
    Vector v;
    v.x() = x; v.y() = y; v.z() = z;
    return v;
}

static Quaternion MakeQuat(double x, double y, double z, double w) {
    Quaternion q;
    q.x() = x; q.y() = y; q.z() = z; q.w() = w;
    return q;
}

static Pose MakePose(double px = 1, double py = 2, double pz = 3) {
    Pose p;
    p.position = MakeVec(px, py, pz);
    p.orientation = MakeQuat(0, 0, 0, 1);
    return p;
}

// ---------------------------------------------------------------------
// Leaf types
// ---------------------------------------------------------------------

TEST(StructEqualLeaf, Bool) {
    EXPECT_TRUE(structmapper::struct_equal(true, true));
    EXPECT_TRUE(structmapper::struct_equal(false, false));
    EXPECT_FALSE(structmapper::struct_equal(true, false));
}

TEST(StructEqualLeaf, Integers) {
    EXPECT_TRUE(structmapper::struct_equal(42, 42));
    EXPECT_FALSE(structmapper::struct_equal(42, 43));
    EXPECT_TRUE(structmapper::struct_equal<std::uint32_t>(7u, 7u));
    EXPECT_FALSE(structmapper::struct_equal<std::uint32_t>(7u, 8u));
}

TEST(StructEqualLeaf, String) {
    EXPECT_TRUE(structmapper::struct_equal(std::string("abc"), std::string("abc")));
    EXPECT_FALSE(structmapper::struct_equal(std::string("abc"), std::string("abd")));
    EXPECT_TRUE(structmapper::struct_equal(std::string(), std::string()));
    EXPECT_FALSE(structmapper::struct_equal(std::string("a"), std::string()));
}

TEST(StructEqualLeaf, FloatingPoint) {
    EXPECT_TRUE(structmapper::struct_equal(1.5, 1.5));
    EXPECT_FALSE(structmapper::struct_equal(1.5, 1.6));
    EXPECT_TRUE(structmapper::struct_equal(0.0, -0.0));
    EXPECT_TRUE(structmapper::struct_equal(kInf, kInf));
    EXPECT_FALSE(structmapper::struct_equal(kInf, -kInf));
    EXPECT_TRUE(structmapper::struct_equal(1.5f, 1.5f));
}

TEST(StructEqualLeaf, NaNEqualsNaN) {
    EXPECT_TRUE(structmapper::struct_equal(kNaN, kNaN));
    EXPECT_TRUE(structmapper::struct_equal(kNaN, -kNaN));  // sign of NaN doesn't matter
    EXPECT_TRUE(structmapper::struct_equal(std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::quiet_NaN()));
    EXPECT_FALSE(structmapper::struct_equal(kNaN, 1.0));
    EXPECT_FALSE(structmapper::struct_equal(1.0, kNaN));
    EXPECT_FALSE(structmapper::struct_equal(kNaN, kInf));
}

// ---------------------------------------------------------------------
// StringEnum / CompileTimeString
// ---------------------------------------------------------------------

TEST(StructEqualStringEnum, SameAndDifferent) {
    MyEnum a = "foo";
    MyEnum b = "foo";
    MyEnum c = "bar";
    EXPECT_TRUE(structmapper::struct_equal(a, b));
    EXPECT_FALSE(structmapper::struct_equal(a, c));
    EXPECT_FALSE(structmapper::struct_equal(c, b));
}

TEST(StructEqualStringEnum, CompileTimeStringAlwaysEqual) {
    CTSTR("/some/topic") a{};
    CTSTR("/some/topic") b{};
    EXPECT_TRUE(structmapper::struct_equal(a, b));
}

// ---------------------------------------------------------------------
// nlohmann::json
// ---------------------------------------------------------------------

TEST(StructEqualJson, Scalars) {
    EXPECT_TRUE(structmapper::struct_equal(json(1), json(1)));
    EXPECT_FALSE(structmapper::struct_equal(json(1), json(2)));
    EXPECT_TRUE(structmapper::struct_equal(json("a"), json("a")));
    EXPECT_FALSE(structmapper::struct_equal(json("a"), json(1)));
    EXPECT_TRUE(structmapper::struct_equal(json(nullptr), json(nullptr)));
    EXPECT_FALSE(structmapper::struct_equal(json(nullptr), json(0)));
}

TEST(StructEqualJson, NaNScalar) {
    EXPECT_TRUE(structmapper::struct_equal(json(kNaN), json(kNaN)));
    EXPECT_FALSE(structmapper::struct_equal(json(kNaN), json(1.0)));
    EXPECT_FALSE(structmapper::struct_equal(json(kNaN), json(nullptr)));
    // sanity: json's own operator== disagrees, which is why we recurse
    EXPECT_FALSE(json(kNaN) == json(kNaN));
}

TEST(StructEqualJson, DifferentNumericKindsAreDifferent) {
    EXPECT_FALSE(structmapper::struct_equal(json(1), json(1.0)));
    EXPECT_FALSE(structmapper::struct_equal(json(true), json(1)));
    EXPECT_FALSE(structmapper::struct_equal(json(false), json(0)));
    EXPECT_FALSE(structmapper::struct_equal(json(1.0), json(true)));
    EXPECT_TRUE(structmapper::struct_equal(json(5), json(5u)));   // signed/unsigned: same kind
    EXPECT_FALSE(structmapper::struct_equal(json(-1), json(1u)));
    EXPECT_FALSE(structmapper::struct_equal(json::array({1}), json::array({1.0})));  // also nested
    // sanity: json's own operator== don't care the internal type of numbers
    EXPECT_TRUE(json(1) == json(1.0));
}

TEST(StructEqualJson, NaNNestedInArrayAndObject) {
    json a = json::object();
    a["k"] = kNaN;
    a["arr"] = json::array({1, kNaN, "x"});
    json b = a;
    EXPECT_TRUE(structmapper::struct_equal(a, b));

    b["arr"][1] = 2.0;
    EXPECT_FALSE(structmapper::struct_equal(a, b));
}

TEST(StructEqualJson, ArraysAndObjects) {
    json a = json::array({1, 2, 3});
    EXPECT_TRUE(structmapper::struct_equal(a, json::array({1, 2, 3})));
    EXPECT_FALSE(structmapper::struct_equal(a, json::array({1, 2})));
    EXPECT_FALSE(structmapper::struct_equal(a, json::array({1, 2, 4})));

    json o1 = json::object({{"a", 1}, {"b", 2}});
    json o2 = json::object({{"b", 2}, {"a", 1}});
    json o3 = json::object({{"a", 1}, {"c", 2}});  // same size, different key
    EXPECT_TRUE(structmapper::struct_equal(o1, o2));
    EXPECT_FALSE(structmapper::struct_equal(o1, o3));
    EXPECT_FALSE(structmapper::struct_equal(o1, json::object({{"a", 1}})));
}

TEST(StructEqualJson, ArrayVsObjectNotEqual) {
    EXPECT_FALSE(structmapper::struct_equal(json::array(), json::object()));
}

// ---------------------------------------------------------------------
// Containers
// ---------------------------------------------------------------------

TEST(StructEqualVector, Basics) {
    std::vector<int> a = {1, 2, 3};
    EXPECT_TRUE(structmapper::struct_equal(a, std::vector<int>{1, 2, 3}));
    EXPECT_FALSE(structmapper::struct_equal(a, std::vector<int>{1, 2}));
    EXPECT_FALSE(structmapper::struct_equal(a, std::vector<int>{1, 2, 4}));
    EXPECT_FALSE(structmapper::struct_equal(a, std::vector<int>{3, 2, 1}));
    EXPECT_TRUE(structmapper::struct_equal(std::vector<int>{}, std::vector<int>{}));
    EXPECT_FALSE(structmapper::struct_equal(std::vector<int>{}, a));
}

TEST(StructEqualVector, VectorBool) {
    std::vector<bool> a = {true, false, true};
    EXPECT_TRUE(structmapper::struct_equal(a, std::vector<bool>{true, false, true}));
    EXPECT_FALSE(structmapper::struct_equal(a, std::vector<bool>{true, true, true}));
}

TEST(StructEqualVector, NaNElements) {
    std::vector<double> a = {1.0, kNaN, 3.0};
    std::vector<double> b = {1.0, kNaN, 3.0};
    EXPECT_TRUE(structmapper::struct_equal(a, b));
    b[1] = 2.0;
    EXPECT_FALSE(structmapper::struct_equal(a, b));
}

TEST(StructEqualVector, OfStructs) {
    std::vector<Socket> a(2), b(2);
    a[0].id = 1; a[1].id = 2;
    b[0].id = 1; b[1].id = 2;
    EXPECT_TRUE(structmapper::struct_equal(a, b));
    b[1].name = "changed";
    EXPECT_FALSE(structmapper::struct_equal(a, b));
}

TEST(StructEqualArray, Basics) {
    std::array<int, 3> a = {1, 2, 3};
    std::array<int, 3> b = {1, 2, 3};
    EXPECT_TRUE(structmapper::struct_equal(a, b));
    b[2] = 4;
    EXPECT_FALSE(structmapper::struct_equal(a, b));

    std::array<int, 0> e1, e2;
    EXPECT_TRUE(structmapper::struct_equal(e1, e2));
}

TEST(StructEqualMap, Basics) {
    using M = std::map<std::string, int>;
    M a = {{"x", 1}, {"y", 2}};
    EXPECT_TRUE(structmapper::struct_equal(a, M{{"y", 2}, {"x", 1}}));
    EXPECT_FALSE(structmapper::struct_equal(a, M{{"x", 1}}));                  // size
    EXPECT_FALSE(structmapper::struct_equal(a, M{{"x", 1}, {"z", 2}}));        // key
    EXPECT_FALSE(structmapper::struct_equal(a, M{{"x", 1}, {"y", 3}}));        // value
    EXPECT_TRUE(structmapper::struct_equal(M{}, M{}));
}

TEST(StructEqualMap, OfStructs) {
    using M = std::map<std::string, Socket>;
    M a, b;
    a["s"].id = 5;
    b["s"].id = 5;
    EXPECT_TRUE(structmapper::struct_equal(a, b));
    b["s"].id = 6;
    EXPECT_FALSE(structmapper::struct_equal(a, b));
}

TEST(StructEqualNested, VectorOfMapOfVector) {
    using Inner = std::map<std::string, std::vector<int>>;
    std::vector<Inner> a = {{{"a", {1, 2}}, {"b", {}}}, {}};
    std::vector<Inner> b = a;
    EXPECT_TRUE(structmapper::struct_equal(a, b));

    b[0]["a"][1] = 99;
    EXPECT_FALSE(structmapper::struct_equal(a, b));

    b = a;
    b[1]["new"] = {1};
    EXPECT_FALSE(structmapper::struct_equal(a, b));
}

// ---------------------------------------------------------------------
// Intrusively reflected structs
// ---------------------------------------------------------------------

TEST(StructEqualStruct, SocketEquality) {
    Socket a, b;
    EXPECT_TRUE(structmapper::struct_equal(a, b));
    b.id = 1;
    EXPECT_FALSE(structmapper::struct_equal(a, b));
    b = a;
    b.name = "other";
    EXPECT_FALSE(structmapper::struct_equal(a, b));
}

TEST(StructEqualStruct, CameraDefaultsEqual) {
    Camera a, b;
    EXPECT_TRUE(structmapper::struct_equal(a, b));
}

TEST(StructEqualStruct, CameraEachFieldDetected) {
    const Camera base;

    { Camera c; c.fov = 90.0;                    EXPECT_FALSE(structmapper::struct_equal(base, c)) << "fov"; }
    { Camera c; c.aspect = 2.0;                  EXPECT_FALSE(structmapper::struct_equal(base, c)) << "aspect"; }
    { Camera c; c.kind = "ortho";                EXPECT_FALSE(structmapper::struct_equal(base, c)) << "kind"; }
    { Camera c; c.enabled = false;               EXPECT_FALSE(structmapper::struct_equal(base, c)) << "enabled"; }
    { Camera c; c.resolution[1] = 720;           EXPECT_FALSE(structmapper::struct_equal(base, c)) << "resolution"; }
    { Camera c; c.tags["k"] = 1;                 EXPECT_FALSE(structmapper::struct_equal(base, c)) << "tags"; }
    { Camera c; c.socket.id = 3;                 EXPECT_FALSE(structmapper::struct_equal(base, c)) << "socket.id"; }
    { Camera c; c.socket.name = "x";             EXPECT_FALSE(structmapper::struct_equal(base, c)) << "socket.name"; }
    { Camera c; c.buffer_size = 11;              EXPECT_FALSE(structmapper::struct_equal(base, c)) << "buffer_size"; }
}

TEST(StructEqualStruct, CameraEqualAfterIdenticalMutation) {
    Camera a, b;
    a.fov = b.fov = 75.0;
    a.kind = b.kind = "ortho";
    a.tags["x"] = b.tags["x"] = 4;
    a.socket.name = b.socket.name = "same";
    EXPECT_TRUE(structmapper::struct_equal(a, b));
}

TEST(StructEqualStruct, CameraNaNFieldsAreEqual) {
    Camera a, b;
    a.fov = kNaN;
    b.fov = kNaN;
    EXPECT_TRUE(structmapper::struct_equal(a, b));
    b.fov = 60.0;
    EXPECT_FALSE(structmapper::struct_equal(a, b));
}

TEST(StructEqualStruct, ConstAndNonConstObjects) {
    Camera a, b;
    const Camera& ca = a;
    EXPECT_TRUE(structmapper::struct_equal(ca, b));
    EXPECT_TRUE(structmapper::struct_equal(a, a));
}

TEST(StructEqualStruct, SymmetryAndReflexivity) {
    Camera a, b;
    b.buffer_size = 99;
    EXPECT_TRUE(structmapper::struct_equal(a, a));
    EXPECT_EQ(structmapper::struct_equal(a, b), structmapper::struct_equal(b, a));

    Camera n;
    n.fov = kNaN;
    EXPECT_TRUE(structmapper::struct_equal(n, n));  // reflexive even with NaN
}

// ---------------------------------------------------------------------
// Externally reflected structs
// ---------------------------------------------------------------------

TEST(StructEqualExternal, Vector) {
    EXPECT_TRUE(structmapper::struct_equal(MakeVec(1, 2, 3), MakeVec(1, 2, 3)));
    EXPECT_FALSE(structmapper::struct_equal(MakeVec(1, 2, 3), MakeVec(0, 2, 3)));
    EXPECT_FALSE(structmapper::struct_equal(MakeVec(1, 2, 3), MakeVec(1, 0, 3)));
    EXPECT_FALSE(structmapper::struct_equal(MakeVec(1, 2, 3), MakeVec(1, 2, 0)));
}

TEST(StructEqualExternal, VectorNaN) {
    EXPECT_TRUE(structmapper::struct_equal(MakeVec(kNaN, 2, 3), MakeVec(kNaN, 2, 3)));
    EXPECT_TRUE(structmapper::struct_equal(MakeVec(kNaN, kNaN, kNaN), MakeVec(kNaN, kNaN, kNaN)));
    EXPECT_FALSE(structmapper::struct_equal(MakeVec(kNaN, 2, 3), MakeVec(1, 2, 3)));
}

TEST(StructEqualExternal, Quaternion) {
    EXPECT_TRUE(structmapper::struct_equal(MakeQuat(0, 0, 0, 1), MakeQuat(0, 0, 0, 1)));
    EXPECT_FALSE(structmapper::struct_equal(MakeQuat(0, 0, 0, 1), MakeQuat(0, 0, 0, -1)));  // w
    EXPECT_FALSE(structmapper::struct_equal(MakeQuat(0, 0, 0, 1), MakeQuat(1, 0, 0, 1)));   // x
}

TEST(StructEqualExternal, NestedInIntrusiveStruct) {
    Pose a = MakePose();
    Pose b = MakePose();
    EXPECT_TRUE(structmapper::struct_equal(a, b));

    b.position.y() = 20;
    EXPECT_FALSE(structmapper::struct_equal(a, b));

    b = MakePose();
    b.orientation.w() = 0.5;
    EXPECT_FALSE(structmapper::struct_equal(a, b));
}

TEST(StructEqualExternal, NaNInNestedExternal) {
    Pose a = MakePose();
    Pose b = MakePose();
    a.orientation.x() = kNaN;
    b.orientation.x() = kNaN;
    EXPECT_TRUE(structmapper::struct_equal(a, b));
}

TEST(StructEqualExternal, InsideContainers) {
    std::vector<Pose> a = {MakePose(1, 2, 3), MakePose(4, 5, 6)};
    std::vector<Pose> b = {MakePose(1, 2, 3), MakePose(4, 5, 6)};
    EXPECT_TRUE(structmapper::struct_equal(a, b));
    b[1].position.z() = 0;
    EXPECT_FALSE(structmapper::struct_equal(a, b));

    std::map<std::string, Vector> m1, m2;
    m1["v"] = MakeVec(1, 2, 3);
    m2["v"] = MakeVec(1, 2, 3);
    EXPECT_TRUE(structmapper::struct_equal(m1, m2));
    m2["v"].x() = 9;
    EXPECT_FALSE(structmapper::struct_equal(m1, m2));
}

// ---------------------------------------------------------------------
// Self-recursive struct
// ---------------------------------------------------------------------

TEST(StructEqualRecursive, EmptyAndShallow) {
    SelfRec a, b;
    EXPECT_TRUE(structmapper::struct_equal(a, b));

    a.rec["child"];
    EXPECT_FALSE(structmapper::struct_equal(a, b));  // size differs
    b.rec["child"];
    EXPECT_TRUE(structmapper::struct_equal(a, b));
}

TEST(StructEqualRecursive, DeepTree) {
    SelfRec a, b;
    a.rec["x"].rec["y"].rec["z"];
    b.rec["x"].rec["y"].rec["z"];
    EXPECT_TRUE(structmapper::struct_equal(a, b));

    // differ only at the deepest level (same key counts all the way down)
    b.rec["x"].rec["y"].rec.erase("z");
    b.rec["x"].rec["y"].rec["w"];
    EXPECT_FALSE(structmapper::struct_equal(a, b));
}

TEST(StructEqualRecursive, SiblingsOrderIndependent) {
    SelfRec a, b;
    a.rec["p"]; a.rec["q"].rec["r"];
    b.rec["q"].rec["r"]; b.rec["p"];
    EXPECT_TRUE(structmapper::struct_equal(a, b));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
