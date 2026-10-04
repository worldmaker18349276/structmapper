#pragma once
#include <string>
#include <array>
#include <stdexcept>
#include <type_traits>
#include <initializer_list>

// compile-time string type, built from a char pack
template <char... Cs>
struct CompileTimeString {
    static constexpr char value[sizeof...(Cs) + 1] = { Cs..., '\0' };
    constexpr operator const char*() const { return value; }
    static constexpr const char* c_str() { return value; }
};
template <char... Cs>
constexpr char CompileTimeString<Cs...>::value[sizeof...(Cs) + 1];

// GNU/Clang extension: decompose a string literal into a char... pack
template <typename T, T... Cs>
constexpr CompileTimeString<Cs...> operator""_ctstr() { return {}; }

// compile-time string: CTSTR("foo") -> CompileTimeString<'f','o','o'>
#define CTSTR(s) decltype(s ## _ctstr)

namespace strenum {
    namespace detail {
        // Concatenate two CompileTimeStrings.
        template <typename A, typename B>
        struct Concat;

        template <char... A, char... B>
        struct Concat<CompileTimeString<A...>, CompileTimeString<B...>> {
            using type = CompileTimeString<A..., B...>;
        };

        // Quote one CompileTimeString.
        template <typename T>
        struct Quote;

        template <char... Cs>
        struct Quote<CompileTimeString<Cs...>> {
            using type = CompileTimeString<'"', Cs..., '"'>;
        };

        // Join quoted strings with " | ".
        template <typename... CTStrings>
        struct JoinQuote;

        template <>
        struct JoinQuote<> {
            using type = CompileTimeString<>;
        };

        template <typename T>
        struct JoinQuote<T> {
            using type = typename Quote<T>::type;
        };

        template <typename T, typename U, typename... Rest>
        struct JoinQuote<T, U, Rest...> {
            using type = typename Concat<
                typename Quote<T>::type,
                typename Concat<
                    CompileTimeString<' ', '|', ' '>,
                    typename JoinQuote<U, Rest...>::type
                >::type
            >::type;
        };

        // constexpr-friendly string compare (std::strcmp isn't guaranteed constexpr)
        constexpr bool streq(const char* a, const char* b) {
            return *a == *b && (*a == '\0' || streq(a + 1, b + 1));
        }
    } // namespace detail

    // the enum-ish string type.
    // usage:
    // ```
    // using MyEnum = StringEnum<CTSTR("foo"), CTSTR("bar"), CTSTR("a")>;
    // constexpr MyEnum e1 = "foo"; // raise compile-time error for invalid value
    // MyEnum e2 = bar.c_str(); // raise runtime error for invalid value
    // const char* s1 = e1;  // convert to c-string
    // for (const char* value : MyEnum::values) { ... } // all valid values
    // ```
    template <typename... Strings>
    class StringEnum {
        const char* value_;

        // returns the matching Strings::c_str() pointer, or nullptr if none match
        static constexpr const char* find(const char* s) {
            const char* result = nullptr;
            (void)std::initializer_list<int>{
                (result = (!result && detail::streq(s, Strings::c_str())) ? Strings::c_str() : result, 0)...
            };
            return result;
        }

        // this function is intentionally not constexpr,
        // so that constexpr constructor fails at compile-time for invalid string,
        // and will show up this function name as hint.
        static void cannot_call_non_constexpr_function_means_input_string_is_invalid_for_this_enum(const char* s) {
            throw std::invalid_argument(std::string("StringEnum: invalid value \"") + s + "\", expected: " + type_name());
        }

    public:
        constexpr StringEnum() : value_(values[0]) {
            static_assert(values.size() > 0);
        }
        constexpr StringEnum(const char* s) : value_(find(s)) {
            if (!value_)
                cannot_call_non_constexpr_function_means_input_string_is_invalid_for_this_enum(s);
        }

        static constexpr bool is_valid(const char* s) { return find(s) != nullptr; }

        constexpr operator const char*() const { return value_; }
        constexpr const char* c_str() const { return value_; }
        std::string str() const { return value_; }

        static constexpr std::array<const char*, sizeof...(Strings)> values = { Strings::c_str()... };

        static constexpr const char* type_name() {
            return detail::JoinQuote<Strings...>::type::c_str();
        }
    };
    template <typename... Strings>
    constexpr std::array<const char*, sizeof...(Strings)> StringEnum<Strings...>::values;

} // namespace strenum
