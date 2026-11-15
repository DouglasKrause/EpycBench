#include "epycbench/build_manifest.hpp"

#include "epycbench/build_info_generated.hpp"

#include <nlohmann/json.hpp>

#include <string_view>

namespace epycbench {

auto build_manifest_json() -> nlohmann::json {
    auto enabled_features = nlohmann::json::array({"linux", "cxx23"});
    if (build_info::feature_testing) {
        enabled_features.push_back("testing");
    }
    if (build_info::feature_asan_ubsan) {
        enabled_features.push_back("asan_ubsan");
    }
    if (build_info::feature_tsan) {
        enabled_features.push_back("tsan");
    }
    if (build_info::feature_native_reference) {
        enabled_features.push_back("native_reference");
    }

    return nlohmann::json{
        {"project_version", build_info::project_version},
        {"git",
         {
             {"revision", build_info::git_revision},
             {"dirty", build_info::git_dirty},
         }},
        {"compiler",
         {
             {"id", build_info::compiler_id},
             {"version", build_info::compiler_version},
             {"target", build_info::compiler_target},
             {"cxx_standard", "C++23"},
             {"flags", build_info::compiler_flags},
         }},
        {"linker",
         {
             {"id", build_info::linker_id},
             {"version", build_info::linker_version},
         }},
        {"preset", build_info::preset},
        {"enabled_features", std::move(enabled_features)},
        {"dependencies",
         {
             {"hwloc", build_info::hwloc_version},
             {"libnuma", build_info::numa_version},
             {"libfyaml", build_info::libfyaml_version},
             {"CLI11", build_info::cli11_version},
             {"nlohmann-json", build_info::nlohmann_json_version},
             {"fmt", build_info::fmt_version},
             {"Catch2", build_info::catch2_version},
         }},
        {"build_identity", build_info::build_identity},
    };
}

} // namespace epycbench
