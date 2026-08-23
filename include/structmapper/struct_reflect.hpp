// visit_struct-style compile-time reflection for C++14.
//
// Usage:
//
//   class Camera {
//   public:
//       double fov = 60.0;
//       double aspect = 1.777;
//
//       using Kind = strenum::StringEnum<CTSTR("pinhole"), CTSTR("ortho")>;
//       Kind kind = "pinhole";
//
//       BEGIN_STRUCT("camera parameters")
//           FIELD(fov,    "field of view, degrees")
//           FIELD(aspect, "aspect ratio")
//           FIELD(kind,   "camera projection kind")
//       END_STRUCT()
//   };
//
// reflect_fields() returns a std::tuple of FieldInfo<Class, T> - one
// strongly-typed entry per field, holding a pointer-to-member, a name, and
// a description. This is intentionally *not* type-erased, so
// structmapper::visit_struct() can hand your visitor a real T&, exactly like the
// visit_struct library.
#pragma once
#include <tuple>
#include <utility>
#include <type_traits>
#include <string>
#include <vector>
#include <map>
#include "structmapper/strenum.hpp"

namespace structmapper {

    // -----------------------------------------------------------------
    // is_json_convertible<T>: true iff T is one of the types
    // to_json/from_json know how to handle:
    //   - bool
    //   - arithmetic (int, double, ...)
    //   - std::string
    //   - strenum::StringEnum<...>  (serialized as its current c_str() value)
    //   - std::vector<U>            where U is itself json-convertible
    //   - std::map<std::string, U>  where U is itself json-convertible
    //   - a reflectable struct (has reflect_fields())
    // Containers recurse, so vector<map<string, vector<int>>> etc. all work.
    // -----------------------------------------------------------------

    // Detects whether T was declared with BEGIN_STRUCT/END_STRUCT (i.e. has
    // a structmapper_reflectable_tag type). Uses declval, so T need not be
    // constructible.
    template <typename T>
    class is_reflectable {
        template <typename U> static std::true_type  test(typename U::structmapper_reflectable_tag*);
        template <typename U> static std::false_type test(...);
    public:
        static constexpr bool value = decltype(test<T>(nullptr))::value;
    };

    namespace detail {

        template <typename T>
        struct is_std_vector : std::false_type {};
        template <typename T, typename Alloc>
        struct is_std_vector<std::vector<T, Alloc>> : std::true_type {
            using value_type = T;
        };

        template <typename T>
        struct is_std_string_map : std::false_type {};
        template <typename T, typename Compare, typename Alloc>
        struct is_std_string_map<std::map<std::string, T, Compare, Alloc>> : std::true_type {
            using value_type = T;
        };

        // Recognizes ::strenum::StringEnum<Strings...> specializations, regardless of
        // how many alternatives it has.
        template <typename T>
        struct is_string_enum : std::false_type {};
        template <typename... Strings>
        struct is_string_enum<::strenum::StringEnum<Strings...>> : std::true_type {};

        // Non-container leaf case: bool / arithmetic / string / StringEnum / reflectable.
        template <typename T>
        struct is_json_leaf : std::integral_constant<bool,
            std::is_same<T, bool>::value ||
            std::is_arithmetic<T>::value ||
            std::is_same<T, std::string>::value ||
            is_string_enum<T>::value ||
            ::structmapper::is_reflectable<T>::value
        > {};

    } // namespace detail

    template <typename T>
    struct is_json_convertible; // fwd decl, so the recursive cases below can use it

    // Dispatches on "is it a vector" / "is it a string-keyed map" via extra
    // bool template params, so non-container T never instantiates a
    // recursive lookup at all - only vector<U>/map<string,U> recurse into U.
    template <typename T,
              bool IsVector = detail::is_std_vector<T>::value,
              bool IsStringMap = detail::is_std_string_map<T>::value>
    struct is_json_convertible_impl : detail::is_json_leaf<T> {};

    template <typename T>
    struct is_json_convertible_impl<T, /*IsVector=*/true, /*IsStringMap=*/false>
        : is_json_convertible<typename detail::is_std_vector<T>::value_type> {};

    template <typename T>
    struct is_json_convertible_impl<T, /*IsVector=*/false, /*IsStringMap=*/true>
        : is_json_convertible<typename detail::is_std_string_map<T>::value_type> {};

    template <typename T>
    struct is_json_convertible : is_json_convertible_impl<T> {};

    template <typename T>
    constexpr bool is_json_convertible_v = is_json_convertible<T>::value;

    // ---------------------------------------------------------------------
    // Field descriptor
    // ---------------------------------------------------------------------
    template <typename Class, typename T>
    struct FieldInfo {
        const char* name;
        const char* desc;
        T Class::*member;

        T&       get(Class& obj)       const { return obj.*member; }
        const T& get(const Class& obj) const { return obj.*member; }

        using value_type = T;
        using class_type = Class;
    };

    template <typename Class, typename T>
    constexpr FieldInfo<Class, T> make_field(const char* name, T Class::*member, const char* desc) {
        static_assert(is_json_convertible<T>::value,
            "structmapper: this field's type is not JSON-convertible. "
            "Allowed field types are: bool, an arithmetic type, std::string, "
            "a StringEnum<...>, std::vector<U>, std::map<std::string, U> "
            "(U checked recursively), or another reflectable struct declared "
            "with BEGIN_STRUCT/END_STRUCT.");
        return FieldInfo<Class, T>{name, desc, member};
    }

    // ---------------------------------------------------------------------
    // visit_struct: call visitor(name, value_ref, desc) for every field, in
    // declaration order, with the *real* field type (not type-erased). Works
    // for both mutable and const objects - 'obj.reflect_fields()' resolves to
    // the const-qualified overload automatically when obj is const.
    // ---------------------------------------------------------------------
    namespace detail {

        template <typename Class, typename Tuple, typename Visitor, std::size_t... I>
        void visit_impl(Class& obj, const Tuple& fields, Visitor&& visitor, std::index_sequence<I...>) {
            using expand = int[];
            (void)expand{0, (visitor(std::get<I>(fields).name, std::get<I>(fields).get(obj), std::get<I>(fields).desc), 0)...};
        }

        template <typename Class, typename Tuple, typename Visitor, std::size_t... I>
        void visit_impl(Class& obj1, Class& obj2, const Tuple& fields, Visitor&& visitor, std::index_sequence<I...>) {
            using expand = int[];
            (void)expand{0, (visitor(std::get<I>(fields).name, std::get<I>(fields).get(obj1), std::get<I>(fields).get(obj2), std::get<I>(fields).desc), 0)...};
        }

    } // namespace detail

    template <typename Class, typename Visitor>
    void visit_struct(Class& obj, Visitor&& visitor) {
        const auto fields = obj.reflect_fields();
        detail::visit_impl(obj, fields, std::forward<Visitor>(visitor),
                            std::make_index_sequence<std::tuple_size<decltype(fields)>::value>{});
    }

    template <typename Class, typename Visitor>
    void visit_struct(Class& obj1, Class& obj2, Visitor&& visitor) {
        const auto fields = obj1.reflect_fields();
        detail::visit_impl(obj1, obj2, fields, std::forward<Visitor>(visitor),
                            std::make_index_sequence<std::tuple_size<decltype(fields)>::value>{});
    }

} // namespace structmapper

// ---------------------------------------------------------------------
// The macros. Fields don't need commas between them - each FIELD()
// expansion starts with a leading comma and is fed into std::tuple_cat,
// so consecutive FIELD(...) lines just concatenate into one expression.
// ---------------------------------------------------------------------

#define BEGIN_STRUCT(DESC)                                                     \
public:                                                                        \
    using structmapper_reflectable_tag = void;                                 \
    static const char* reflect_struct_desc() { return DESC; }                  \
    auto reflect_fields() const {                                              \
        using ReflectSelf = std::remove_const<std::remove_pointer<decltype(this)>::type>::type; \
        static const auto fields_ = std::tuple_cat(                            \
            std::tuple<>{}

#define FIELD(VAR, DESC)                                                       \
            , std::make_tuple(::structmapper::make_field<ReflectSelf>(#VAR, &ReflectSelf::VAR, DESC))

#define FIELD_(VAR, NAME, DESC)                                                \
            , std::make_tuple(::structmapper::make_field<ReflectSelf>(NAME, &ReflectSelf::VAR, DESC))

#define END_STRUCT()                                                           \
        );                                                                     \
        return fields_;                                                        \
    }
