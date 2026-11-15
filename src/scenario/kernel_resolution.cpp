#include "scenario/kernel_resolution.hpp"

#include <utility>

namespace epycbench::scenario::detail {

namespace {
auto location_for(const AuthoredScenario& scenario, const std::string& pointer, bool mapping_key)
    -> std::optional<SourceLocation> {
    const auto found = scenario.locations().find(pointer);
    if (found == scenario.locations().end()) return std::nullopt;
    if (mapping_key && found->second.mapping_key) return found->second.mapping_key;
    return found->second.value;
}
} // namespace

void KernelDiagnostics::add(std::string code, std::string pointer,
                            std::string classification,
                            std::map<std::string, std::string> context, bool mapping_key) {
    if (capped_) return;
    if (values_.size() == 63U) {
        values_.push_back({core::StableErrorCode{"scenario.kernel.diagnostic_limit"}, "",
                           std::nullopt, {{"classification", "diagnostic_limit"}}});
        capped_ = true;
        return;
    }
    context.insert_or_assign("classification", std::move(classification));
    values_.push_back({core::StableErrorCode{std::move(code)}, pointer,
                       location_for(scenario_, pointer, mapping_key), std::move(context)});
}

auto KernelDiagnostics::take() noexcept -> std::vector<ScenarioDiagnostic> {
    return std::move(values_);
}

auto resolve_workload_kernel_id(const nlohmann::ordered_json& workload,
                                const std::string& name, KernelDiagnostics& diagnostics)
    -> std::expected<std::optional<ResolvedKernelToken>, core::Error> {
    if (!workload.is_object() || !workload.contains("kernel") ||
        !workload.at("kernel").is_string())
        return std::unexpected{kernel_projection_internal_error(
            "normalized_workload_shape", "resolve_workload_kernel_id")};
    auto token = workload.at("kernel").get<std::string>();
    const auto id = kernel::parse_kernel_id(token);
    if (!id) {
        diagnostics.add("scenario.kernel.unknown_kernel", "/workloads/" + name + "/kernel",
                        "unknown_kernel", {{"kernel_category", "unrecognized"}});
        return std::nullopt;
    }
    return ResolvedKernelToken{std::move(token), *id};
}

auto kernel_projection_internal_error(std::string classification, std::string operation,
                                      std::string underlying) -> core::Error {
    auto context = nlohmann::json{{"classification", std::move(classification)}};
    if (!underlying.empty()) context["underlying_code"] = std::move(underlying);
    return {.category = core::ErrorCategory::scenario,
            .code = core::StableErrorCode{"scenario.kernel.internal_failure"},
            .message = "Scenario Kernel resolution failed internally",
            .operation = std::move(operation),
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

} // namespace epycbench::scenario::detail
