// visit_struct-style compile-time reflection for C++14.
//
// Usage:
//
//   class Camera {
//   public:
//       double fov = 60.0;
//       double aspect = 1.777;
//
//       BEGIN_STRUCT("camera parameters")
//           FIELD(fov,    "field of view, degrees")
//           FIELD(aspect, "aspect ratio")
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

namespace structmapper {

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
        return FieldInfo<Class, T>{name, desc, member};
    }

    // Detects whether T was declared with BEGIN_STRUCT/END_STRUCT (i.e. has
    // a reflect_fields() method). Uses declval, so T need not be constructible.
    template <typename T>
    class is_reflectable {
        template <typename U> static auto test(int) -> decltype(std::declval<U&>().reflect_fields(), std::true_type{});
        template <typename U> static std::false_type test(...);
    public:
        static constexpr bool value = decltype(test<T>(0))::value;
    };

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
