#pragma once

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

        inline bool write_json_file(const std::string& path, const nlohmann::json& j)
        {
            std::ofstream out(path);
            if (!out)
            {
                std::cerr << "structmapper: cannot open '" << path << "' for writing\n";
                return false;
            }
            out << j.dump(2) << '\n';
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