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
//       CTSTR("/cam/image_raw") topic{};   // fixed, compile-time constant string
//
//       BEGIN_STRUCT("camera parameters")
//           FIELD(fov,    "field of view, degrees")
//           FIELD(aspect, "aspect ratio")
//           FIELD(kind,   "camera projection kind")
//           FIELD(topic,  "output topic")
//       END_STRUCT()
//   };
//
// for third-party structs you can't modify:
//
//   struct ThirdPartyCamera {
//       double fov_ = 60.0;
//       shared_ptr<double> aspect_ = 1.777;
//       std::array<int, 2> resolution_ = {1920, 1080};
// 
//       double& fov() { return fov_; }
//       const double& fov() const { return fov_; }
//       FieldProxy<int> resolution() { ... }
//       FieldProxy<const int> resolution() const { ... }
//   };
//
//   BEGIN_EXTERNAL_STRUCT(ThirdPartyCamera, "camera parameters")
//       FIELD_EXPR_NAMED(double, &self.fov(),       "fov",    "field of view, degrees")
//       FIELD_EXPR_NAMED(double, self.aspect_,      "aspect", "aspect ratio")             // shared_ptr<double> member
//       FIELD_EXPR_NAMED(double, self.resolution(), "resolution", "resolution of image")  // custom proxy
//   END_EXTERNAL_STRUCT()
//
// EXPR is evaluated per call and its result is returned BY VALUE. It must be a
// pointer-like object (raw pointer, shared_ptr, or a proxy with operator*) whose
// operator* yields TYPE& for a non-const self and something convertible to
// const TYPE& for a const self. visit_struct keeps the object alive for the
// duration of the visitor call, so a proxy can own temporaries the field refers
// to. Null is invalid (asserted for types convertible to bool).
//
// reflect_fields() returns a std::tuple of FieldInfo<Class, T> - one
// strongly-typed entry per field, holding a pointer-to-member, a name, and
// a description. This is intentionally *not* type-erased, so
// structmapper::visit_struct() can hand your visitor a real T&, exactly like the
// visit_struct library.
#pragma once
#include <cstddef>
#include <tuple>
#include <utility>
#include <functional>
#include <type_traits>
#include <string>
#include <vector>
#include <array>
#include <map>
#include <cassert>
#include <nlohmann/json.hpp>
#include "structmapper/strenum.hpp"

namespace structmapper {

    // -----------------------------------------------------------------
    // External reflection registry.
    //
    // Primary (unspecialized) template: by default a type has no externally
    // registered reflection. BEGIN_EXTERNAL_STRUCT/END_EXTERNAL_STRUCT add a
    // full specialization for a given CLASS with has_reflection = true, plus
    // fields()/reflect_struct_desc() static methods mirroring what BEGIN_STRUCT
    // would have generated as members of CLASS itself.
    // -----------------------------------------------------------------
    template <typename T>
    struct ExternalReflectTraits {
        static constexpr bool has_reflection = false;
    };

    // Detects whether T was declared with BEGIN_STRUCT/END_STRUCT (i.e. has
    // a structmapper_reflectable_tag type). Uses declval, so T need not be
    // constructible.
    template <typename T>
    class is_intrusively_reflectable {
        template <typename U> static std::true_type  test(typename U::structmapper_reflectable_tag*);
        template <typename U> static std::false_type test(...);
    public:
        static constexpr bool value = decltype(test<T>(nullptr))::value;
    };

    // Detects whether T was registered via BEGIN_EXTERNAL_STRUCT/END_EXTERNAL_STRUCT.
    template <typename T>
    struct is_externally_reflectable
        : std::integral_constant<bool, ExternalReflectTraits<T>::has_reflection> {};

    // True for either kind of reflection - this is the trait the rest of the
    // library (is_json_convertible, visit_struct, ...) actually cares about.
    // A type should never be reflected both ways at once, but if it somehow
    // were, this just needs either to be true.
    template <typename T>
    struct is_reflectable
        : std::integral_constant<bool,
            is_intrusively_reflectable<T>::value || is_externally_reflectable<T>::value> {};

    // -----------------------------------------------------------------
    // is_json_convertible<T>: true iff T is one of the types
    // to_json/from_json know how to handle:
    //   - bool
    //   - arithmetic (int, double, ...)
    //   - std::string
    //   - strenum::StringEnum<...>  (serialized as its current c_str() value)
    //   - CompileTimeString<...>    (i.e. CTSTR("..."); serialized as its fixed value)
    //   - std::vector<U>            where U is itself json-convertible
    //   - std::array<U, N>          where U is itself json-convertible
    //   - U[N]                      where U is itself json-convertible
    //   - std::map<std::string, U>  where U is itself json-convertible
    //   - a reflectable struct (has reflect_fields())
    // Containers recurse, so vector<map<string, vector<int>>> etc. all work.
    // -----------------------------------------------------------------

    namespace detail {

        template <typename T>
        struct is_std_vector : std::false_type {};
        template <typename T, typename Alloc>
        struct is_std_vector<std::vector<T, Alloc>> : std::true_type {
            using value_type = T;
        };

        template <typename T>
        struct is_std_array : std::false_type {};
        template <typename T, std::size_t N>
        struct is_std_array<std::array<T, N>> : std::true_type {
            using value_type = T;
        };

        template <typename T>
        struct is_raw_array : std::false_type {};
        template <typename T, std::size_t N>
        struct is_raw_array<T[N]> : std::true_type {
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

        // Recognizes ::CompileTimeString<Cs...> specializations - i.e. fields
        // declared as `CTSTR("...") name{};`. These carry a fixed, compile-time
        // value baked into the type itself (no per-object state), and are
        // serialized like any other string.
        template <typename T>
        struct is_compile_time_string : std::false_type {};
        template <char... Cs>
        struct is_compile_time_string<::CompileTimeString<Cs...>> : std::true_type {};

        // Non-container leaf case: bool / arithmetic / string / StringEnum /
        // CompileTimeString / reflectable.
        template <typename T>
        struct is_json_leaf : std::integral_constant<bool,
            std::is_same<T, bool>::value ||
            std::is_arithmetic<T>::value ||
            std::is_same<T, std::string>::value ||
            std::is_same<T, nlohmann::json>::value ||
            is_string_enum<T>::value ||
            is_compile_time_string<T>::value ||
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
              bool IsArray = detail::is_std_array<T>::value,
              bool IsRawArray = detail::is_raw_array<T>::value,
              bool IsStringMap = detail::is_std_string_map<T>::value>
    struct is_json_convertible_impl : detail::is_json_leaf<T> {};

    template <typename T>
    struct is_json_convertible_impl<T, /*IsVector=*/true, /*IsArray=*/false, /*IsRawArray=*/false, /*IsStringMap=*/false>
        : is_json_convertible<typename detail::is_std_vector<T>::value_type> {};

    template <typename T>
    struct is_json_convertible_impl<T, /*IsVector=*/false, /*IsArray=*/true, /*IsRawArray=*/false, /*IsStringMap=*/false>
        : is_json_convertible<typename detail::is_std_array<T>::value_type> {};

    template <typename T>
    struct is_json_convertible_impl<T, /*IsVector=*/false, /*IsArray=*/false, /*IsRawArray=*/true, /*IsStringMap=*/false>
        : is_json_convertible<typename detail::is_raw_array<T>::value_type> {};

    template <typename T>
    struct is_json_convertible_impl<T, /*IsVector=*/false, /*IsArray=*/false, /*IsRawArray=*/false, /*IsStringMap=*/true>
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

        T*       get(Class& obj)       const { return &(obj.*member); }
        const T* get(const Class& obj) const { return &(obj.*member); }

        using value_type = T;
        using class_type = Class;
    };

    template <typename Class, typename T>
    constexpr FieldInfo<Class, T> create_field_info(const char* name, const char* desc, T Class::*member) {
        return FieldInfo<Class, T>{name, desc, member};
    }

    template <typename Class, typename T,
                typename P  = T*,          // what accessor returns for Class&
                typename CP = const T*>    // what accessor returns for const Class&
    struct ExprFieldInfo {
        const char* name;
        const char* desc;
        std::function<P(Class&)> accessor;
        std::function<CP(const Class&)> const_accessor;

        P  get(Class& obj)       const { return accessor(obj); }
        CP get(const Class& obj) const { return const_accessor(obj); }

        using value_type = T;
        using class_type = Class;
    };

    // ---------------------------------------------------------------------
    // reflect_fields(obj) / reflect_struct_desc<T>(): uniform access to a
    // reflected type's field tuple and description, regardless of whether
    // Class was reflected intrusively (BEGIN_STRUCT, member functions live on
    // Class itself) or externally (BEGIN_EXTERNAL_STRUCT, they live on
    // ExternalReflectTraits<Class> instead). Everything downstream (visit_struct,
    // future to_json/from_json, etc.) should go through these rather than
    // calling obj.reflect_fields() directly, so it works for both.
    // ---------------------------------------------------------------------
    namespace detail {

        template <typename T>
        decltype(auto) reflect_fields_dispatch(const T& obj, std::true_type /*intrusive*/) {
            return obj.reflect_fields();
        }

        template <typename T>
        decltype(auto) reflect_fields_dispatch(const T&, std::false_type /*intrusive*/) {
            return ExternalReflectTraits<T>::fields();
        }

        template <typename T>
        const char* reflect_struct_desc_dispatch(std::true_type /*intrusive*/) {
            return T::reflect_struct_desc();
        }

        template <typename T>
        const char* reflect_struct_desc_dispatch(std::false_type /*intrusive*/) {
            return ExternalReflectTraits<T>::reflect_struct_desc();
        }

    } // namespace detail

    template <typename T>
    decltype(auto) reflect_fields(const T& obj) {
        return detail::reflect_fields_dispatch(obj, std::integral_constant<bool, is_intrusively_reflectable<T>::value>{});
    }

    template <typename T>
    const char* reflect_struct_desc() {
        return detail::reflect_struct_desc_dispatch<T>(std::integral_constant<bool, is_intrusively_reflectable<T>::value>{});
    }

    // ---------------------------------------------------------------------
    // visit_struct: call visitor(name, value_ref, desc, type_tag) for every field, in
    // declaration order, with the *real* field type (not type-erased). Works
    // for both mutable and const objects, and for both intrusively- and
    // externally-reflected classes.
    // ---------------------------------------------------------------------

    // The declared field type of a visited value: strips reference and cv only,
    // never decays, so double[3][3] stays double[3][3].
    template <typename T>
    using field_type_t = typename std::remove_cv<typename std::remove_reference<T>::type>::type;

    // Empty tag carrying a type. Lets a generic lambda receive the field type
    // as a real parameter: `decltype(tag)::type`.
    template <typename T>
    struct type_tag { using type = T; };

    namespace detail {

        // Null is invalid. Checked only for types explicitly convertible to bool
        // (raw/smart pointers); proxy types without that conversion are skipped.
        // The second parameter is required by C++14.
        template <typename P>
        auto check_valid(const P& p, int) -> decltype(static_cast<bool>(p), void()) {
            assert(static_cast<bool>(p) && "structmapper: field accessor returned a null pointer");
        }
        template <typename P>
        void check_valid(const P&, long) {}

        template <typename Field, typename Class, typename Visitor>
        void visit_one(const Field& f, Class& obj, Visitor&& visitor) {
            auto p = f.get(obj);              // lives until the visitor returns
            check_valid(p, 0);
            visitor(f.name, *p, f.desc, type_tag<typename Field::value_type>{});
        }

        template <typename Field, typename Class, typename Visitor>
        void visit_one(const Field& f, Class& obj1, Class& obj2, Visitor&& visitor) {
            auto p1 = f.get(obj1);
            auto p2 = f.get(obj2);
            check_valid(p1, 0);
            check_valid(p2, 0);
            visitor(f.name, *p1, *p2, f.desc, type_tag<typename Field::value_type>{});
        }

        template <typename Class, typename Tuple, typename Visitor, std::size_t... I>
        void visit_impl(Class& obj, const Tuple& fields, Visitor&& visitor, std::index_sequence<I...>) {
            using expand = int[];
            (void)expand{0, (visit_one(std::get<I>(fields), obj, visitor), 0)...};
        }

        template <typename Class, typename Tuple, typename Visitor, std::size_t... I>
        void visit_impl(Class& obj1, Class& obj2, const Tuple& fields, Visitor&& visitor, std::index_sequence<I...>) {
            using expand = int[];
            (void)expand{0, (visit_one(std::get<I>(fields), obj1, obj2, visitor), 0)...};
        }

    } // namespace detail

    // visitor(name, value_ref, desc, type_tag<FieldType>)
    template <typename Class, typename Visitor>
    void visit_struct(Class& obj, Visitor&& visitor) {
        const auto& fields = reflect_fields(obj);
        using Tuple = typename std::remove_cv<typename std::remove_reference<decltype(fields)>::type>::type;
        detail::visit_impl(obj, fields, std::forward<Visitor>(visitor),
                        std::make_index_sequence<std::tuple_size<Tuple>::value>{});
    }

    // visitor(name, value_ref1, value_ref2, desc, type_tag<FieldType>)
    template <typename Class, typename Visitor>
    void visit_struct(Class& obj1, Class& obj2, Visitor&& visitor) {
        const auto& fields = reflect_fields(obj1);
        using Tuple = typename std::remove_cv<typename std::remove_reference<decltype(fields)>::type>::type;
        detail::visit_impl(obj1, obj2, fields, std::forward<Visitor>(visitor),
                        std::make_index_sequence<std::tuple_size<Tuple>::value>{});
    }

    // Reinterpret an object's storage as To, keeping its constness.
    // Typical use: view a flat double[9] as double[3][3].
    template <typename To, typename From>
    typename std::conditional<std::is_const<From>::value, const To, To>::type&
    view_as(From& from) {
        static_assert(sizeof(To) == sizeof(From), "structmapper::view_as: size mismatch");
        using Ptr = typename std::conditional<std::is_const<From>::value, const To, To>::type*;
        return *reinterpret_cast<Ptr>(&from);
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

#define FIELD_NAMED(VAR, NAME, DESC)                                           \
            , std::make_tuple([]() { \
                using FieldType = ::structmapper::field_type_t<decltype((std::declval<ReflectSelf&>().VAR))>; \
                static_assert( \
                    ::structmapper::is_json_convertible<FieldType>::value, \
                    "structmapper: type of field " #VAR " is not JSON-convertible" \
                ); \
                return ::structmapper::create_field_info<ReflectSelf>(NAME, DESC, &ReflectSelf::VAR); \
            }())

#define FIELD(VAR, DESC) FIELD_NAMED(VAR, #VAR, DESC)

#define END_STRUCT()                                                           \
        );                                                                     \
        return fields_;                                                        \
    }

// ---------------------------------------------------------------------
// External reflection macros - same shape as BEGIN_STRUCT/FIELD/END_STRUCT,
// but used OUTSIDE the class, at namespace scope, for types you can't or
// don't want to add BEGIN_STRUCT/END_STRUCT to directly (third-party types,
// generated code, plain structs from another library, etc).
//
// This specializes structmapper::ExternalReflectTraits<CLASS> instead of
// adding members to CLASS itself, so CLASS needs no modification at all -
// but its reflected members do need to be accessible from outside CLASS
// (public, or friend structmapper::ExternalReflectTraits<CLASS>).
// ---------------------------------------------------------------------

#define BEGIN_EXTERNAL_STRUCT(CLASS, DESC)                                    \
namespace structmapper {                                                      \
    template <>                                                               \
    struct ExternalReflectTraits<CLASS> {                                     \
        static constexpr bool has_reflection = true;                          \
        using ReflectSelf = CLASS;                                            \
        static const char* reflect_struct_desc() { return DESC; }             \
        static const auto& fields() {                                         \
            static const auto fields_ = std::tuple_cat(                       \
                std::tuple<>{}

#define FIELD_EXPR_NAMED(TYPE, EXPR, NAME, DESC) \
    , std::make_tuple([]() { \
        using FieldType = TYPE; \
        auto accessor = [](auto&& self) { return (EXPR); }; \
        using P  = decltype(accessor(std::declval<ReflectSelf&>())); \
        using CP = decltype(accessor(std::declval<const ReflectSelf&>())); \
        static_assert(::structmapper::is_json_convertible<FieldType>::value, \
                      "structmapper: type of field expression " #EXPR " is not JSON-convertible"); \
        static_assert(std::is_convertible<decltype(*std::declval<P&>()), FieldType&>::value, \
                      "structmapper: " #EXPR " must yield a pointer-like object whose operator* gives " #TYPE "&"); \
        static_assert(std::is_convertible<decltype(*std::declval<CP&>()), const FieldType&>::value, \
                      "structmapper: " #EXPR " (const self) must yield a pointer-like object whose operator* gives const " #TYPE "&"); \
        return ::structmapper::ExprFieldInfo<ReflectSelf, FieldType, P, CP>{NAME, DESC, accessor, accessor}; \
    }())

#define END_EXTERNAL_STRUCT()                                                 \
            );                                                                \
            return fields_;                                                   \
        }                                                                     \
    };                                                                        \
}
