# structmapper

Lightweight compile-time reflection for C++14, in the spirit of [`visit_struct`](https://github.com/cbeck88/visit_struct). Declare a struct's fields once, then get strongly-typed, in-order access to each field's name, description and real `T&`. The same declaration drives JSON (de)serialization, JSON Schema generation, deep equality, and more.

```cpp
#include "structmapper/struct_reflect.hpp"
#include "structmapper/struct_to_json.hpp"
#include "structmapper/struct_to_schema.hpp"
#include "structmapper/struct_equal.hpp"

struct Camera {
    double fov = 60.0;
    std::array<int, 2> resolution = {1920, 1080};

    using Kind = strenum::StringEnum<CTSTR("pinhole"), CTSTR("ortho")>;
    Kind kind = "pinhole";

    CTSTR("/cam/image_raw") topic{};      // fixed compile-time constant string

    BEGIN_STRUCT("camera parameters")
        FIELD(fov,        "field of view, degrees")
        FIELD(resolution, "resolution of image")
        FIELD(kind,       "camera projection kind")
        FIELD(topic,      "output topic")
    END_STRUCT()
};

int main() {
    Camera cam;

    structmapper::FromJsonStats stats;
    structmapper::from_json(cam, nlohmann::json::parse(text), stats.logger());
    if (!stats.ok()) { /* some field had the wrong JSON type */ }

    nlohmann::json j      = structmapper::convert_to_json(cam);
    nlohmann::json schema = structmapper::to_schema<Camera>();
    bool unchanged        = structmapper::struct_equal(cam, Camera{});
}
```

## Features

- **reflection**: intrinsive and external for third-party types.
- **Computed fields**: reinterpreted storage, getters/setters, enum/string mapping.
- **Real field types** are passed to visitors; nothing is type-erased.
- Works on `const` and non-const objects, and can walk **two objects in lock-step**.
- JSON load/save with per-field logging.
- JSON Schema generation
- NaN-aware deep equality
- XML-RPC, JSON conversion.

## Setup

Header-only; nothing to compile or link.

| Header | Provides |
|---|---|
| `struct_reflect.hpp` | Core: macros, `visit_struct`, external reflection, proxies |
| `strenum.hpp` | `CTSTR("...")` and `strenum::StringEnum<...>` |
| `struct_equal.hpp` | `struct_equal(a, b)` |
| `struct_to_json.hpp` | `to_json` / `from_json` with logging |
| `struct_to_schema.hpp` | `to_schema<T>()` |
| `xmlrpc_to_json.hpp` | `XmlRpcValue` ↔ `json` (standalone, no reflection) |

Dependencies: `struct_reflect` → `strenum`; `struct_equal`, `struct_to_json` → `struct_reflect`; `struct_to_schema` → `struct_reflect`, `struct_to_json`, `struct_equal`.

### Requirements

- C++14 with GCC or Clang (`CTSTR` uses the GNU string-literal operator template extension; not MSVC). On Clang add `-Wno-gnu-string-literal-operator-template`.
- [nlohmann/json](https://github.com/nlohmann/json) for everything except `strenum.hpp`.
- `xmlrpc_to_json.hpp` only: `<xmlrpcpp/XmlRpcValue.h>` (ROS `xmlrpcpp`).

## Supported field types

`is_json_convertible<T>` holds for these, and containers recurse:

| Type | Notes |
|---|---|
| `bool`, arithmetic types | |
| `std::string` | |
| `nlohmann::json` | passed through as-is |
| `strenum::StringEnum<...>` | serialized as its current string |
| `CTSTR("...")` | fixed value baked into the type, no per-object state |
| `std::vector<U>`, `std::array<U, N>`, `U[N]` | multi-dimensional raw arrays work |
| `std::map<std::string, U>` | string keys only |
| reflectable struct | intrusive or external, nested arbitrarily (including self-recursive maps) |

Not supported: `std::unordered_map`, non-string map keys, plain `enum`s (wrap in `enum_proxy`).

## Intrusive reflection

| Macro | Meaning |
|---|---|
| `BEGIN_STRUCT(DESC)` | Start declaration (switches access to `public:`) |
| `FIELD(VAR, DESC)` | Register member `VAR`, serialized under its identifier |
| `FIELD_NAMED(VAR, NAME, DESC)` | Same, with an explicit name |
| `END_STRUCT()` | End declaration |

No commas between fields. Nested reflected structs just work.

## Visiting fields

```cpp
structmapper::visit_struct(obj, visitor);         // visitor(name, value, desc, type_tag<T>)
structmapper::visit_struct(obj1, obj2, visitor);  // visitor(name, v1, v2, desc, type_tag<T>)
```

- Fields are visited in declaration order; `value` is a real `T&` (`const T&` for const objects).
- `type_tag<T>` carries the declared type (`typename decltype(tag)::type`).
- Both objects in the two-object form must be the same class.

```cpp
structmapper::visit_struct(cam, [](const char* name, const auto& value, const char* desc, auto tag) {
    using T = typename decltype(tag)::type;
    std::cout << name << " : " << desc << "\n";
});
```

## External reflection

Use this when you can't (or don't want to) add `BEGIN_STRUCT` to the class. Place the block at **global namespace scope**; members must be publicly accessible (or `friend structmapper::ExternalReflectTraits<CLASS>`).

```cpp
BEGIN_EXTERNAL_STRUCT(Eigen::Vector3d, "vector")
    FIELD_EXPR_NAMED(&self.x(), "x", "x coordinate")
    FIELD_EXPR_NAMED(&self.y(), "y", "y coordinate")
    FIELD_EXPR_NAMED(&self.z(), "z", "z coordinate")
END_EXTERNAL_STRUCT()
```

`FIELD_EXPR_NAMED(EXPR, NAME, DESC)` takes an expression in terms of `self` (`CLASS&` or `const CLASS&`). It can also be used inside an intrusive `BEGIN_STRUCT` block (name always required).

### Rules for `EXPR`

- Evaluated on every access; the result is returned by value.
- Must be pointer-like: raw pointer, `shared_ptr`, or a proxy with `operator*`.
- `operator*` must yield `T&` for non-const `self` and something convertible to `const T&` for const `self` (`static_assert`ed).
- Null is invalid (`assert`ed for anything convertible to `bool`).
- Temporaries owned by a proxy live for the duration of the visitor call.

### Common patterns

```cpp
FIELD_EXPR_NAMED(&self.fov(),       "fov",      "field of view")   // accessor returning a reference
FIELD_EXPR_NAMED(self.metadata_ptr, "metadata", "camera metadata") // shared_ptr member

// reinterpret flat double[9] as double[3][3] (constness preserved, size static_assert'ed)
FIELD_EXPR_NAMED(&::structmapper::view_as<double[3][3]>(self.mat.data), "mat", "matrix")
```

## Proxies

A proxy is a pointer-like object returned by `EXPR` for a field that isn't stored as a plain `T`. Visitors, `from_json` and `struct_equal` see an ordinary `T&`.

### `proxy(value)` / `proxy(value, setter)`

Computed value with write-back. Provide a const overload (read-only) and a non-const one (with setter):

```cpp
std::array<int, 2> get_resolution() const { return {width_, height_}; }
void set_resolution(const std::array<int, 2>& v) { width_ = v[0]; height_ = v[1]; }

// computed value with write-back
auto resolution_proxy() const { return structmapper::proxy(get_resolution()); }
auto resolution_proxy()       { return structmapper::proxy(get_resolution(),
                                    [this](const auto& v){ set_resolution(v); }); }

FIELD_EXPR_NAMED(self.resolution_proxy(), "resolution", "resolution")
```

`FieldProxy<T, Setter>` holds a **local copy** of the value and calls the setter when destroyed, i.e. after the visitor returns.

- Only the non-const `operator*` marks the proxy *dirty*. Write-back happens only if dirty, at most once. Proxies are move-only.
- A proxy without a setter (or from a const `self`) is read-only; the copy is discarded.
- On a **non-const** object, a proxy with a setter writes back after *every* visit, even if the visitor only read. Visit through a `const` reference to avoid this.
- unwinding will not trigger write-back, preventing crash due to escaping exception.

### `enum_proxy<Value>(ptr, {{Enum, value}, ...})`

Plain `enum`s aren't JSON-convertible. `enum_proxy` exposes one as `Value` (e.g. a `StringEnum`) through a mapping table:

```cpp
enum class Projection { Pinhole, Ortho };
using Kind = strenum::StringEnum<CTSTR("pinhole"), CTSTR("ortho")>;
Projection projection = Projection::Pinhole;

template <typename Self>
static auto kind_proxy(Self& self) {
    return structmapper::enum_proxy<Kind>(&self.projection, {
        {Projection::Pinhole, "pinhole"},
        {Projection::Ortho,   "ortho"},
    });
}

FIELD_EXPR_NAMED(Camera::kind_proxy(self), "kind", "projection kind")
```

- The static template serves both const and non-const `self`.
- A missing value on **read** and unknown value on **write** throws `std::invalid_argument` during write-back.

## JSON: `struct_to_json.hpp`

Does its own field-by-field walk (not nlohmann's `adl_serializer`) so loading can report what happened to every field.

```cpp
json j = structmapper::convert_to_json(cam);              // writing never fails or logs

bool ok = structmapper::from_json(cam, j);                // default logger, path "$"
bool ok = structmapper::from_json(cam, j, logger);
bool ok = structmapper::from_json(cam, j, "$.camera", logger);
Camera c = structmapper::convert_from_json<Camera>(j, logger);   // default-constructs first
```

Per-field outcomes when loading:

| Outcome | Level | Effect |
|---|---|---|
| loaded (`FromJsonLoadLog`) | INFO | assigned |
| missing key (`FromJsonMissingLog`) | WARNING | unchanged |
| type mismatch or invalid value (`FromJsonMismatchLog`) | ERROR | unchanged |
| unknown JSON key (`FromJsonUnknownLog`) | WARNING | ignored |

So **default-construct the object before `from_json`** to have defaults fill the gaps.

### Loading rules

- Integers must be integral JSON numbers *in range* (`1.5` for `int`, `-1` for unsigned, `300` for `uint8_t` are mismatches); floating point accepts any number.
- `StringEnum`: must be one of the alternatives. `CTSTR`: must equal the constant; nothing is assigned.
- `std::vector`: cleared and resized, elements loaded (failed ones stay value-initialized). `std::array` / `T[N]`: length must be exactly `N`, else mismatch. `std::map`: cleared, then filled.
- Proxy fields are written back after each field, including on failure (with the unchanged value).

A logger is `std::function<void(const FromJsonLog&)>` with `level()` and `msg()`. Paths look like `$.socket.id`, `$.resolution[1]`, `$.tags["left"]`. With no logger, everything is printed.

```cpp
structmapper::FromJsonStats stats;
structmapper::from_json(cam, j, stats.logger());        // print + count; logger(false) counts silently
std::cout << stats.summary() << "\n";   // "from_json summary: 8 loaded, 1 missing, 0 type mismatches, 0 unknown fields"
stats.ok();                              // true iff type_mismatches == 0
```

## Deep equality: `struct_equal.hpp`

```cpp
template <typename T> bool struct_equal(const T& a, const T& b);
```

Structural comparison of reflected fields only (never calls a user `operator==`); short-circuits on the first difference.

- Floating point: `==` but **NaN equals NaN**.
- `StringEnum`: equal iff same current value. `CTSTR`: always equal.
- Containers: same size and pairwise deep-equal (maps: same keys too). Structs: every reflected field equal.
- `nlohmann::json`: type-aware. Different kinds are never equal (`1`, `1.0`, `true` all differ); signed/unsigned integers compare by value; NaNs are equal, including nested.

## JSON Schema: `struct_to_schema.hpp`

```cpp
template <typename T>
nlohmann::json to_schema(const std::map<std::type_index, std::string>& refs = {});
```

| C++ type | Schema |
|---|---|
| `bool` | `{"type": "boolean"}` |
| integers | `{"type": "integer", "minimum": ..., "maximum": ...}` |
| floating point | `{"type": "number"}` |
| `std::string` | `{"type": "string"}` |
| `nlohmann::json` | `{}` (any type) |
| `StringEnum` | `{"enum": [...]}` |
| `CTSTR` | `{"const": "..."}` |
| `std::vector<T>` | `{"type": "array", "items": ...}` |
| `std::array<T,N>`, `T[N]` | as above plus `minItems` / `maxItems` = N |
| `std::map<std::string,V>` | `{"type": "object", "additionalProperties": ...}` |
| reflectable struct | `{"type": "object", "properties": {...}}` |

- No `required` list and no `additionalProperties: false`: all keys optional, unknown keys allowed.
- Each property gets its `FIELD` description. A `"default"` comes from default constructor and is emitted only if it differs from a value-initialized field type (via `struct_equal`), so `double fov = 60.0` gets one but `int id = 0` doesn't. All reflected types must be default-constructible.
- Struct-typed fields keep both descriptions by nesting the type schema under `anyOf`.
- **`$defs` deduplication:** any reflectable type occurring two or more times in the type graph is hoisted to `$defs`, and every occurrence becomes `{"$ref": "#/$defs/Name"}`. Self-recursive types are handled by the same mechanism.
- **External refs:** a type listed in `refs` is opaque and becomes `{"$ref": path}` wherever it appears.

```cpp
std::map<std::type_index, std::string> refs = { { typeid(Socket), "socket.schema.json" } };
auto schema = structmapper::to_schema<Camera>(refs);
```

## Strings and string-enums: `strenum.hpp`

No dependencies beyond the standard library, macro `CTSTR` requires GNU string-literal operator template extension.

**`CTSTR("foo")`** expands to a *type* (`CompileTimeString<'f','o','o'>`), so use it where a type is expected: `CTSTR("/cam/image_raw") topic{};`. It offers `c_str()`, implicit `const char*` conversion and a static `value[]`. Objects are stateless and always equal; on load the JSON must match the constant, and the schema is `{"const": ...}`.

### `StringEnum<Strings...>`

```cpp
using Kind = strenum::StringEnum<CTSTR("pinhole"), CTSTR("ortho")>;
constexpr Kind a = "pinhole";        // OK
// constexpr Kind b = "bogus";       // compile error
Kind d = runtime_str.c_str();        // throws std::invalid_argument if invalid
Kind e;                              // first alternative
const char* s = a;  std::string t = a.str();
for (const char* k : Kind::values)   // iterate all possible values
    std::cout << k << std::endl;
```

The stored value always points at the alternative's own static string.

## XML-RPC ↔ JSON: `xmlrpc_to_json.hpp`

Standalone helper for reading the ROS parameter server, pairing naturally with `from_json`:

```cpp
XmlRpc::XmlRpcValue v;
nh.getParam("camera", v);
nlohmann::json j = structmapper::convert_xmlrpc_to_json(v);   // takes non-const ref
Camera cam;
structmapper::from_json(cam, j);
```

Entry points: `xmlrpc_to_json(v, j, [path,] logger)`, `convert_xmlrpc_to_json(v, logger)`, `json_to_xmlrpc(j, v, [path,] logger)`, `convert_json_to_xmlrpc(j, logger)`.

Some conversions are lossy:
| Conversion | Notes |
|---|---|
| Unset → `null` | INFO logged |
| `TypeDateTime` → string (`%Y-%m-%dT%H:%M:%S`, no time zone) | ERROR |
| `TypeBase64` → `{"__type": "binary", "size": N}` (payload lost) | ERROR |
| JSON integer → `TypeInt` if it fits 32 bits, else `TypeDouble` | ERROR if it doesn't fit |

Logging mirrors `FromJsonStats` (`XmlRpcToJsonStats`, `JsonToXmlRpcStats`; `ok()` is false when a lossy replacement or oversized integer occurred). With no logger, a printing one is used.
