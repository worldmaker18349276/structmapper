#pragma once

#include <algorithm>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <typeindex>

#include <nlohmann/json.hpp>

#include "structmapper/struct_to_schema.hpp"

namespace structmapper {

    using RefMap = std::map<std::type_index, std::string>; // type -> json file name
    using ManifestTypeMap = std::map<std::string, std::string>; // type name -> json file name

    namespace detail {

        // json text whose numbers with an exponent have a '.' and a signed exponent: 1e-09 -> 1.0e-09, 2e20 -> 2.0e+20.
        // they are still valid json, but yaml 1.1 loaders (PyYAML, which reads json files in monolaunch) only read a number
        // as float in that form, otherwise "1e-09" becomes a string. strings are copied as they are.
        inline std::string with_yaml_float_exponents(const std::string& text)
        {
            std::string out;
            out.reserve(text.size() + 16);
            const std::size_t n = text.size();
            auto is_digit = [&](std::size_t k) { return k < n && std::isdigit(static_cast<unsigned char>(text[k])); };
            std::size_t i = 0;
            while (i < n)
            {
                const char c = text[i];
                if (c == '"')
                {
                    std::size_t j = i + 1;
                    while (j < n && text[j] != '"')
                        j += (text[j] == '\\') ? 2 : 1;
                    j = std::min(j + 1, n);
                    out.append(text, i, j - i);
                    i = j;
                }
                else if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
                {
                    std::size_t j = i + (c == '-' ? 1 : 0);
                    while (is_digit(j)) ++j;
                    const bool has_dot = j < n && text[j] == '.';
                    if (has_dot)
                    {
                        ++j;
                        while (is_digit(j)) ++j;
                    }
                    const std::size_t mantissa_end = j;
                    if (j < n && (text[j] == 'e' || text[j] == 'E'))
                    {
                        std::size_t k = j + 1;
                        char sign = '+';
                        if (k < n && (text[k] == '+' || text[k] == '-')) sign = text[k++];
                        const std::size_t exponent_begin = k;
                        while (is_digit(k)) ++k;
                        out.append(text, i, mantissa_end - i);
                        if (!has_dot) out += ".0";
                        out += 'e';
                        out += sign;
                        out.append(text, exponent_begin, k - exponent_begin);
                        i = k;
                    }
                    else
                    {
                        out.append(text, i, mantissa_end - i);
                        i = mantissa_end;
                    }
                }
                else
                {
                    out += c;
                    ++i;
                }
            }
            return out;
        }

        inline bool write_json_file(const std::string& path, const nlohmann::json& j)
        {
            std::ofstream out(path);
            if (!out)
            {
                std::cerr << "structmapper: cannot open '" << path << "' for writing\n";
                return false;
            }
            out << with_yaml_float_exponents(j.dump(2)) << '\n';
            return static_cast<bool>(out);
        }

        template <typename T>
        inline void insert_own_type(RefMap& refs, ManifestTypeMap& manifest_types)
        {
            const std::string name = demangled_type_name<T>();
            refs[typeid(T)] = manifest_types[name] = sanitize_identifier(name) + ".schema.json";
        }

        template <typename T>
        inline bool generate_one(const std::string& out_dir, const RefMap& refs)
        {
            const std::string name = demangled_type_name<T>();

            RefMap refs_ = refs;
            refs_.erase(refs_.find(typeid(T)));
            try
            {
                const nlohmann::json schema = to_schema<T>(refs_);
                const std::string filename = refs.at(typeid(T));
                return write_json_file(out_dir + "/" + filename, schema);
            }
            catch (const std::exception& e)
            {
                std::cerr << "structmapper: schema generation failed for '" << name << "': " << e.what() << "\n";
                return false;
            }
        }

    }  // namespace detail

    // Generates one schema file per Types... into out_dir, then writes a
    // manifest listing every generated type (fully-qualified name -> filename).
    template <typename... Types>
    inline bool generate_schemas(const std::string& out_dir)
    {
        bool ok = true;

        RefMap refs;
        ManifestTypeMap manifest_types;
        (void)std::initializer_list<int>{
            (detail::insert_own_type<Types>(refs, manifest_types), 0)... };

        {
            bool gen_ok = true;
            (void)std::initializer_list<int>{
                (gen_ok = detail::generate_one<Types>(out_dir, refs) && gen_ok, 0)... };
            ok = ok && gen_ok;
        }

        nlohmann::json manifest;
        manifest["types"] = nlohmann::json::object();
        for (const auto& kv : manifest_types)
            manifest["types"][kv.first] = kv.second;
        std::string manifest_path = out_dir + "/manifest.json";
        if (!detail::write_json_file(manifest_path, manifest))
            ok = false;

        return ok;
    }
}  // namespace structmapper