#include "results/current_build_manifest.hpp"

#include "epycbench/build_info_generated.hpp"

#include <array>

namespace epycbench::results::build_manifest {
auto retain_current(Limits limits) -> std::expected<Owner, core::Error> {
    namespace build=epycbench::results::build_manifest;
    std::array<build::Feature,6> features{build::Feature::linux_platform,build::Feature::cxx23};
    std::size_t count=2;
    if(build_info::feature_testing)features[count++]=build::Feature::testing;
    if(build_info::feature_asan_ubsan)features[count++]=build::Feature::asan_ubsan;
    if(build_info::feature_tsan)features[count++]=build::Feature::tsan;
    if(build_info::feature_native_reference)features[count++]=build::Feature::native_reference;
    return build::retain({build_info::project_version,{build_info::git_revision,build_info::git_dirty},
        {build_info::compiler_id,build_info::compiler_version,build_info::compiler_target,"C++23",build_info::compiler_flags},
        {build_info::linker_id,build_info::linker_version},build_info::preset,
        std::span{features}.first(count),
        {build_info::hwloc_version,build_info::numa_version,build_info::libfyaml_version,
         build_info::cli11_version,build_info::nlohmann_json_version,build_info::fmt_version,build_info::catch2_version},
        build_info::build_identity},limits);
}
} // namespace epycbench::results::build_manifest
