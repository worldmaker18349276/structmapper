// Deep equality for any JSON-convertible type (see is_json_convertible<T>),
// built on structmapper::visit_struct(obj1, obj2, visitor).
//
// Usage:
//
//   Camera a, b;
//   if (structmapper::struct_equal(a, b)) { ... }
//
// Semantics, per type category:
//   - bool / arithmetic / std::string / nlohmann::json : operator==
//       (floating point is NaN-aware: NaN == NaN; json recurses so nested NaNs match too)
//   - strenum::StringEnum<...>      : equal iff current c_str() values match
//   - CompileTimeString<...>        : always equal (value is part of the type)
//   - std::vector<U>                : same size, elements pairwise deep-equal
//   - std::array<U, N>              : elements pairwise deep-equal
//   - std::map<std::string, U>      : same size, same keys, values deep-equal
//   - reflectable struct            : every reflected field deep-equal
//                                     (BEGIN_STRUCT or BEGIN_EXTERNAL_STRUCT)
// Containers and structs nest arbitrarily.
#pragma once
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <type_traits>
#include <vector>
#include <array>
#include "structmapper/struct_reflect.hpp"

namespace structmapper {

    namespace detail {

        // Leaf comparison. Floating point is NaN-aware: two NaNs compare equal.
        template <typename T>
        typename std::enable_if<std::is_floating_point<T>::value, bool>::type
        leaf_eq(const T& a, const T& b) {
            return a == b || (std::isnan(a) && std::isnan(b));
        }

        template <typename T>
        typename std::enable_if<!std::is_floating_point<T>::value, bool>::type
        leaf_eq(const T& a, const T& b) {
            return a == b;
        }

        // nlohmann::json: type-aware, recursive comparison.
        //  - boolean, integer (signed or unsigned), float, string, null, array,
        //    object and binary are distinct kinds; values of different kinds
        //    are never equal (so json(1) != json(1.0) != json(true)).
        //  - NaN floats compare equal, including when nested in arrays/objects
        //    (json's own operator== says NaN != NaN).
        // Non-template overload, so it wins over the templates above.
        inline int json_kind(const nlohmann::json& j) {
            if (j.is_number_integer()) return 1; // true for signed and unsigned
            if (j.is_number_float())   return 2;
            return 3 + static_cast<int>(j.type()); // everything else: its own type
        }

        inline bool leaf_eq(const nlohmann::json& a, const nlohmann::json& b) {
            if (json_kind(a) != json_kind(b)) return false;

            if (a.is_number_float()) {
                return leaf_eq(a.get<double>(), b.get<double>());
            }
            if (a.is_array()) {
                if (a.size() != b.size()) return false;
                for (std::size_t i = 0; i < a.size(); ++i) {
                    if (!leaf_eq(a[i], b[i])) return false;
                }
                return true;
            }
            if (a.is_object()) {
                if (a.size() != b.size()) return false;
                for (auto it = a.begin(); it != a.end(); ++it) {
                    auto jt = b.find(it.key());
                    if (jt == b.end()) return false;
                    if (!leaf_eq(it.value(), *jt)) return false;
                }
                return true;
            }
            return a == b; // same kind: bool, integer (signed vs unsigned by value), string, null, binary
        }

        // Primary template: leaf types and reflectable structs.
        // Containers / StringEnum / CompileTimeString are partial
        // specializations below. (Class templates rather than overloaded
        // functions, so recursion needs no forward declarations.)
        template <typename T, typename Enable = void>
        struct DeepEqual {
            static_assert(is_json_convertible<T>::value,
                "structmapper::struct_equal: type is not JSON-convertible.");

            static bool eq(const T& a, const T& b) {
                return eq_impl(a, b, std::integral_constant<bool, is_reflectable<T>::value>{});
            }

        private:
            // Reflectable struct: compare field by field via visit_struct.
            static bool eq_impl(const T& a, const T& b, std::true_type) {
                bool result = true;
                // Class deduces to `const T`; FieldInfo/ExternalFieldInfo::get
                // have const overloads, so the visitor receives const refs of
                // the real field types.
                visit_struct(a, b,
                    [&result](const char* /*name*/, const auto& x, const auto& y, const char* /*desc*/, auto tag) {
                        if (!result) return; // already unequal, skip the rest
                        using F = typename decltype(tag)::type;
                        result = DeepEqual<F>::eq(x, y);
                    });
                return result;
            }

            // Leaf: bool, arithmetic, std::string, nlohmann::json.
            static bool eq_impl(const T& a, const T& b, std::false_type) {
                return leaf_eq(a, b);
            }
        };

        // StringEnum: compare the currently selected string.
        template <typename... Strings>
        struct DeepEqual<::strenum::StringEnum<Strings...>, void> {
            using T = ::strenum::StringEnum<Strings...>;
            static bool eq(const T& a, const T& b) {
                // just compare internal pointers
                return static_cast<const char*>(a) == static_cast<const char*>(b);
            }
        };

        // CompileTimeString: the value is encoded in the type, so two
        // objects of the same type are always equal.
        template <char... Cs>
        struct DeepEqual<::CompileTimeString<Cs...>, void> {
            using T = ::CompileTimeString<Cs...>;
            static bool eq(const T&, const T&) { return true; }
        };

        template <typename U, typename Alloc>
        struct DeepEqual<std::vector<U, Alloc>, void> {
            using T = std::vector<U, Alloc>;
            static bool eq(const T& a, const T& b) {
                if (a.size() != b.size()) return false;
                auto ia = a.begin();
                auto ib = b.begin();
                for (; ia != a.end(); ++ia, ++ib) {
                    // `const U&` conversion also handles vector<bool> proxies.
                    const U& x = *ia;
                    const U& y = *ib;
                    if (!DeepEqual<U>::eq(x, y)) return false;
                }
                return true;
            }
        };

        template <typename U, std::size_t N>
        struct DeepEqual<std::array<U, N>, void> {
            using T = std::array<U, N>;
            static bool eq(const T& a, const T& b) {
                for (std::size_t i = 0; i < N; ++i) {
                    if (!DeepEqual<U>::eq(a[i], b[i])) return false;
                }
                return true;
            }
        };

        template <typename U, std::size_t N>
        struct DeepEqual<U[N], void> {
            using T = U[N];
            static bool eq(const T& a, const T& b) {
                for (std::size_t i = 0; i < N; ++i) {
                    if (!DeepEqual<U>::eq(a[i], b[i])) return false;
                }
                return true;
            }
        };

        template <typename U, typename Compare, typename Alloc>
        struct DeepEqual<std::map<std::string, U, Compare, Alloc>, void> {
            using T = std::map<std::string, U, Compare, Alloc>;
            static bool eq(const T& a, const T& b) {
                if (a.size() != b.size()) return false;
                for (const auto& kv : a) {
                    auto it = b.find(kv.first);
                    if (it == b.end()) return false;
                    if (!DeepEqual<U>::eq(kv.second, it->second)) return false;
                }
                return true;
            }
        };

    } // namespace detail

    // Deep equality for any is_json_convertible<T> type.
    template <typename T>
    bool struct_equal(const T& a, const T& b) {
        static_assert(is_json_convertible<T>::value,
            "structmapper::struct_equal: type is not JSON-convertible. See is_json_convertible<T>.");
        return detail::DeepEqual<T>::eq(a, b);
    }

} // namespace structmapper