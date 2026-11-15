#include "core/json_ownership.hpp"
#include "epycbench/kernel/configuration.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace epycbench::kernel {
namespace {

auto contract_error(std::string classification) -> core::Error {
    auto context = nlohmann::json::object();
    core::detail::JsonCleanup cleanup{context};
    context["classification"] = std::move(classification);
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{"kernel.contract.invalid"},
            .message = "Kernel contract invariant failed",
            .operation = "kernel_contract",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto decimal(std::uint64_t value) -> std::string { return std::to_string(value); }

} // namespace

auto detail::valid_kernel_identity_token(std::string_view token) noexcept -> bool {
    if (token.empty() || token.size() > maximum_kernel_identity_bytes ||
        token.front() == '/' || token.back() == '/' || token.contains("//")) return false;
    for (const auto ch : token) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_' || ch == '/'))
            return false;
    }
    return true;
}

auto kernel_id_token(const KernelId& id) noexcept -> std::string_view {
    return id.token();
}

auto parse_kernel_id(std::string_view token) noexcept -> std::optional<KernelId> {
    if (token == "memory.bandwidth.read/v1")
        return KernelId::sequential_read_bandwidth_v1;
    if (token == "memory.latency.dependent-load/v1")
        return KernelId::dependent_load_latency_v1;
    if (!detail::valid_kernel_identity_token(token) || !token.ends_with("/v1"))
        return std::nullopt;
    constexpr std::string_view bandwidth = "memory.bandwidth.read/";
    constexpr std::string_view latency = "memory.latency.dependent-load/";
    if (token.starts_with(bandwidth) && token.size() > bandwidth.size() + 3U)
        return KernelId{token, KernelFamily::bandwidth};
    if (token.starts_with(latency) && token.size() > latency.size() + 3U)
        return KernelId{token, KernelFamily::latency};
    return std::nullopt;
}

PointerCycleSeedIntent::PointerCycleSeedIntent(
    PointerCycleSeedMode mode, std::optional<std::uint64_t> value) noexcept
    : mode_{mode}, value_{value} {}

auto PointerCycleSeedIntent::explicit_value(std::uint64_t value) noexcept
    -> PointerCycleSeedIntent {
    return {PointerCycleSeedMode::explicit_value, value};
}

auto PointerCycleSeedIntent::derive_per_experiment() noexcept -> PointerCycleSeedIntent {
    return {PointerCycleSeedMode::derive_per_experiment, std::nullopt};
}

auto PointerCycleSeedIntent::mode() const noexcept -> PointerCycleSeedMode { return mode_; }
auto PointerCycleSeedIntent::value() const noexcept -> std::optional<std::uint64_t> {
    return value_;
}

SequentialReadBandwidthConfig::SequentialReadBandwidthConfig(
    core::ByteCount working_set_per_worker) noexcept
    : working_set_per_worker_{working_set_per_worker} {}

auto SequentialReadBandwidthConfig::create(core::ByteCount working_set_per_worker)
    -> std::expected<SequentialReadBandwidthConfig, core::Error> {
    if (working_set_per_worker.value() == 0U ||
        working_set_per_worker.value() % 64U != 0U) {
        return std::unexpected{contract_error("bandwidth_working_set")};
    }
    return SequentialReadBandwidthConfig{working_set_per_worker};
}

auto SequentialReadBandwidthConfig::working_set_per_worker() const noexcept -> core::ByteCount {
    return working_set_per_worker_;
}

DependentLoadLatencyConfig::DependentLoadLatencyConfig(
    core::ByteCount working_set, PointerCycleSeedIntent pointer_cycle_seed) noexcept
    : working_set_{working_set}, pointer_cycle_seed_{std::move(pointer_cycle_seed)} {}

auto DependentLoadLatencyConfig::create(core::ByteCount working_set,
                                        PointerCycleSeedIntent pointer_cycle_seed)
    -> std::expected<DependentLoadLatencyConfig, core::Error> {
    if (working_set.value() < 128U || working_set.value() % 64U != 0U) {
        return std::unexpected{contract_error("latency_working_set")};
    }
    return DependentLoadLatencyConfig{working_set, std::move(pointer_cycle_seed)};
}

auto DependentLoadLatencyConfig::working_set() const noexcept -> core::ByteCount {
    return working_set_;
}

auto DependentLoadLatencyConfig::pointer_cycle_seed() const noexcept
    -> const PointerCycleSeedIntent& {
    return pointer_cycle_seed_;
}

auto semantic_projection(KernelId id, const KernelConfiguration& configuration)
    -> std::expected<nlohmann::ordered_json, core::Error> {
    nlohmann::ordered_json parameters = nlohmann::ordered_json::object();
    core::detail::JsonCleanup parameters_cleanup{parameters};
    if (id.family() == KernelFamily::bandwidth) {
        const auto* value = std::get_if<SequentialReadBandwidthConfig>(&configuration);
        if (value == nullptr)
            return std::unexpected{contract_error("configuration_identity_mismatch")};
        parameters["working_set_bytes"] = decimal(value->working_set_per_worker().value());
        parameters["partition"] = "private-per-worker";
        parameters["access_pattern"] = "sequential";
        parameters["element_size_bytes"] = "8";
        parameters["alignment_bytes"] = "64";
    } else if (id.family() == KernelFamily::latency) {
        const auto* value = std::get_if<DependentLoadLatencyConfig>(&configuration);
        if (value == nullptr)
            return std::unexpected{contract_error("configuration_identity_mismatch")};
        parameters["working_set_bytes"] = decimal(value->working_set().value());
        parameters["partition"] = "private-per-worker";
        parameters["chains_per_worker"] = 1U;
        parameters["construction"] = "randomized-cycle";
        parameters["node_size_bytes"] = "64";
        parameters["alignment_bytes"] = "64";
        nlohmann::ordered_json seed = nlohmann::ordered_json::object();
        core::detail::JsonCleanup seed_cleanup{seed};
        if (value->pointer_cycle_seed().mode() == PointerCycleSeedMode::explicit_value) {
            seed["mode"] = "explicit";
            seed["value"] = decimal(*value->pointer_cycle_seed().value());
        } else {
            seed["mode"] = "derived";
            seed["profile"] = pointer_cycle_seed_profile;
        }
        core::detail::set_json(parameters, "chain_seed", std::move(seed));
    } else {
        return std::unexpected{contract_error("unknown_kernel_identity")};
    }
    nlohmann::ordered_json result = nlohmann::ordered_json::object();
    core::detail::JsonCleanup result_cleanup{result};
    result["kernel"] = kernel_id_token(id);
    core::detail::set_json(result, "parameters", std::move(parameters));
    return nlohmann::ordered_json(std::move(result));
}

} // namespace epycbench::kernel
