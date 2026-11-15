#include "monitoring/host_running_build_attestation_value.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "monitoring/privacy_validation.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
namespace epycbench::monitoring::qualification::detail {
namespace {
using Json=nlohmann::ordered_json;
constexpr std::size_t manifest_limit=1024U*1024U;
auto backend_error(std::string code,std::string classification)->core::Error{
 return {.category=core::ErrorCategory::monitoring,.code=core::StableErrorCode{std::move(code)},
 .message="Monitoring Q1 backend failed",.operation="run_monitoring_host_q1_backend",
 .os_error=std::nullopt,.context={{"classification",std::move(classification)}},
 .severity=core::ErrorSeverity::error,.continuation=core::ContinuationPolicy::abort_command};
}
template<class Object> auto exact_keys(const Object& value,std::initializer_list<std::string_view> expected)->bool{
 if(!value.is_object()||value.size()!=expected.size())return false;
 return std::ranges::all_of(expected,[&](std::string_view key){return value.contains(key);});
}
}
auto attest_monitoring_running_build_value(const nlohmann::json& build, std::string_view commit)
    -> std::expected<Json, core::Error> {
    try {
        static constexpr std::array dependencies{"hwloc", "libnuma", "libfyaml", "CLI11",
                                                  "nlohmann-json", "fmt", "Catch2"};
        if (!exact_keys(build, {"project_version", "git", "compiler", "linker", "preset",
                                "enabled_features", "dependencies", "build_identity"}) ||
            !exact_keys(build.at("git"), {"revision", "dirty"}) ||
            !exact_keys(build.at("compiler"), {"id", "version", "target", "cxx_standard", "flags"}) ||
            !exact_keys(build.at("linker"), {"id", "version"}) ||
            !exact_keys(build.at("dependencies"), {"hwloc", "libnuma", "libfyaml", "CLI11",
                                                   "nlohmann-json", "fmt", "Catch2"}) ||
            !build.at("project_version").is_string() || !build.at("preset").is_string() ||
            !build.at("git").at("revision").is_string() ||
            !build.at("git").at("dirty").is_boolean() ||
            build.at("git").at("revision").get<std::string>() != commit ||
            build.at("git").at("dirty").get<bool>() ||
            build.at("compiler").at("cxx_standard") != "C++23" ||
            !build.at("compiler").at("flags").is_string() ||
            !build.at("enabled_features").is_array())
            return std::unexpected{backend_error("monitoring.q1.backend.build_invalid", "build_attestation")};
        std::vector<std::string> features;
        for (const auto& item : build.at("enabled_features")) {
            if (!item.is_string()) return std::unexpected{backend_error("monitoring.q1.backend.build_invalid", "build_attestation")};
            features.push_back(item.get<std::string>());
        }
        std::vector<std::string> expected{"linux", "cxx23"};
        for (auto optional : {"testing", "asan_ubsan", "tsan", "native_reference"})
            if (std::ranges::find(features, optional) != features.end()) expected.emplace_back(optional);
        if (features != expected) return std::unexpected{backend_error("monitoring.q1.backend.build_invalid", "build_attestation")};
        const auto text = [](const nlohmann::json& item) -> std::optional<std::string> {
            if (!item.is_string() || item.get_ref<const std::string&>().empty()) return std::nullopt;
            return item.get<std::string>();
        };
        const auto project = text(build.at("project_version"));
        const auto revision = text(build.at("git").at("revision"));
        const auto compiler_id = text(build.at("compiler").at("id"));
        const auto compiler_version = text(build.at("compiler").at("version"));
        const auto compiler_target = text(build.at("compiler").at("target"));
        const auto linker_id = text(build.at("linker").at("id"));
        const auto linker_version = text(build.at("linker").at("version"));
        const auto preset = text(build.at("preset"));
        if (!project || !revision || !compiler_id || !compiler_version || !compiler_target ||
            !linker_id || !linker_version || !preset)
            return std::unexpected{backend_error("monitoring.q1.backend.build_invalid", "build_attestation")};
        const auto flags = build.at("compiler").at("flags").get<std::string>();
        std::vector<std::string> tuple{*project, *revision, "false", *compiler_id,
            *compiler_version, *compiler_target, flags, *linker_id, *linker_version, *preset};
        for (auto optional : {"testing", "asan_ubsan", "tsan", "native_reference"})
            tuple.push_back(std::ranges::find(features, optional) != features.end() ? "true" : "false");
        for (auto name : dependencies) {
            auto dependency = text(build.at("dependencies").at(name));
            if (!dependency)
                return std::unexpected{backend_error("monitoring.q1.backend.build_invalid", "build_attestation")};
            tuple.push_back(std::move(*dependency));
        }
        std::string joined;
        for (std::size_t i = 0; i < tuple.size(); ++i) {
            if (i) joined.push_back('|');
            joined += tuple[i];
        }
        const auto identity = core::sha256(joined).to_hex();
        if (!build.at("build_identity").is_string() ||
            build.at("build_identity").get<std::string>() != identity || !privacy_safe(build))
            return std::unexpected{backend_error("monitoring.q1.backend.build_invalid", "build_attestation")};
        auto canonical = core::canonicalize_json(build);
        if (!canonical || canonical->size() + 1U > manifest_limit)
            return std::unexpected{backend_error("monitoring.q1.backend.build_invalid", "build_attestation")};
        return Json(build);
    } catch (...) {
        return std::unexpected{backend_error("monitoring.q1.backend.build_invalid", "build_attestation")};
    }
}
}
