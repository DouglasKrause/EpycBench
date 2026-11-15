#include "runtime/pairwise_v2_production_entry.hpp"
#include "runtime/controller_lifecycle_policy.hpp"
#include "runtime/kernel_module_session.hpp"

#include <array>
#include <cstdio>
#include <limits>

namespace epycbench::runtime::detail {
namespace {
constexpr int runner_control_descriptor = 3;
constexpr int runner_event_descriptor = 4;
auto native_adopt(void*, int descriptor)
    -> std::expected<platform_linux::LocalIpcEndpoint, core::Error> {
    return platform_linux::LocalIpcEndpoint::adopt(descriptor);
}
auto native_backend(void*) -> NativePreparedProductionDependencies {
    return {
        {PairwiseTypedLocalMonitoringCapacityProducer{}, PairwisePlatformLeafFacade::production(),
         platform_linux::detail::production_bounded_sysfs_operations(),
         platform_linux::detail::production_bounded_sysfs_operations(), {}, std::nullopt, {},
         stage_b_policy::Selection::stage_b_v1},
        production_service_platform_operations(), std::nullopt, std::nullopt,
        PairwiseV2AuthoritySource::native};
}
}
auto run_stage_b_runner() noexcept -> int {
    if (const auto modules = initialize_runner_kernel_modules(); !modules) {
        static_cast<void>(std::fprintf(stderr, "epycbench: %s\n", modules.error().c_str()));
        return 1;
    }
    constexpr std::array<std::string_view, 2> arguments{"epycbench", "__runner"};
    const auto result = run_stage_b_runner_with(arguments,
        {{nullptr, native_adopt}, production_runner_context_provider(),
         production_controller_clock(), {}, nullptr, native_backend});
    if (result.exit_code != 0) static_cast<void>(std::fputs("epycbench: internal runner failed\n", stderr));
    return result.exit_code;
}

auto run_stage_b_runner_with(std::span<const std::string_view> arguments,
    StageBRunnerEntryDependencies dependencies) noexcept -> StageBRunnerEntryOutcome {
    using Kind = NativePairwiseFailureKind;
    using Stage = NativePairwiseStage;
    StageBRunnerEntryOutcome outcome;
    auto fail = [&](Kind kind, Stage stage) noexcept {
        if (!outcome.primary) outcome.primary = NativePairwiseFailure{
            kind, stage, NativePairwisePhase::startup, std::nullopt};
    };
    if (classify_runner_invocation(arguments) != RunnerInvocationKind::valid_runner) {
        outcome.exit_code = 2;
        fail(Kind::invalid_input, Stage::preparation);
        return outcome;
    }
    if (!dependencies.endpoints.adopt || !dependencies.runner_context.acquire ||
        !dependencies.backend) {
        fail(Kind::invalid_input, Stage::preparation);
        return outcome;
    }
    const auto policy = controller_lifecycle_policy_v1();
    auto budget = NativeRunnerStartupBudget::start(dependencies.clock,
        policy.startup_timeout, policy.observation_quantum);
    if (!budget) { outcome.primary = budget.error(); return outcome; }
    std::optional<platform_linux::LocalIpcEndpoint> control, events;
    const auto close = [&]() noexcept {
        for (auto* endpoint : {&events, &control}) {
            if (!*endpoint) continue;
            try { if (!(**endpoint).close()) outcome.cleanup_failure = true; }
            catch (...) { outcome.cleanup_failure = true; }
            endpoint->reset();
        }
    };
    try {
        auto adopted = dependencies.endpoints.adopt(dependencies.endpoints.context,
            runner_control_descriptor);
        if (!adopted) { fail(Kind::native, Stage::channels); return outcome; }
        control.emplace(std::move(*adopted));
        adopted = dependencies.endpoints.adopt(dependencies.endpoints.context,
            runner_event_descriptor);
        if (!adopted) { fail(Kind::native, Stage::channels); close(); return outcome; }
        events.emplace(std::move(*adopted));
        if (!budget->point()) { outcome.primary = budget->failure(); close(); return outcome; }
        auto context = dependencies.runner_context.acquire(dependencies.runner_context.context);
        if (!budget->point()) { outcome.primary = budget->failure(); close(); return outcome; }
        if (!context) { fail(Kind::preparation, Stage::preparation); close(); return outcome; }
        auto backend = dependencies.backend(dependencies.backend_context);
        if (!budget->point()) { outcome.primary = budget->failure(); close(); return outcome; }
        auto runner = PairwiseV2NativeRunner::prepare_production_from_channel(
            {std::move(*control), std::move(*events)}, std::move(*context),
            dependencies.authority, std::move(backend),
            {PairwiseV2NativeRunner::production_auxiliary_bytes(true)}, std::move(*budget));
        // Moved endpoint holders no longer own descriptors; the runner preparation
        // consumes both endpoints even when admission fails.
        control.reset(); events.reset();
        if (!runner) { outcome.primary = runner.error(); return outcome; }
        auto completed = runner->run_production();
        outcome.primary = completed.lifecycle.primary;
        outcome.cleanup_failure = completed.lifecycle.cleanup_count != 0 ||
            completed.lifecycle.closed_with_error;
        outcome.exit_code = !outcome.primary && !outcome.cleanup_failure &&
            completed.lifecycle.completion_received ? 0 : 1;
    } catch (...) { fail(Kind::preparation, Stage::preparation); }
    close();
    return outcome;
}

auto stage_b_logical_child_token(std::uint64_t ordinal) noexcept
    -> std::expected<std::uint64_t, StageBEntryFailure> {
    if (ordinal == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(StageBEntryFailure::overflow);
    return ordinal + 1U;
}

auto stage_b_startup_session(std::string_view uuid, std::uint64_t ordinal) noexcept
    -> std::expected<wire_v1::detail::StartupSessionId, StageBEntryFailure> {
    if (uuid.size() != 36 || uuid[14] != '7' ||
        (uuid[19] != '8' && uuid[19] != '9' && uuid[19] != 'a' && uuid[19] != 'b'))
        return std::unexpected(StageBEntryFailure::identity);
    for (std::size_t i{}; i < uuid.size(); ++i) {
        const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
        const char ch = uuid[i];
        if (separator ? ch != '-' : !((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return std::unexpected(StageBEntryFailure::identity);
    }
    if (!stage_b_logical_child_token(ordinal))
        return std::unexpected(StageBEntryFailure::overflow);
    constexpr char domain[] = "epycbench.stage-b/startup-session/v1";
    std::array<std::byte, sizeof(domain) + 36U + 8U> preimage{};
    std::size_t offset{};
    for (char ch : domain) preimage[offset++] = std::byte{static_cast<unsigned char>(ch)};
    for (char ch : uuid) preimage[offset++] = std::byte{static_cast<unsigned char>(ch)};
    for (unsigned shift = 64; shift != 0; shift -= 8)
        preimage[offset++] = std::byte{static_cast<unsigned char>(ordinal >> (shift - 8))};
    try {
        auto result = wire_v1::detail::StartupSessionId::parse(core::sha256(preimage).to_hex());
        if (!result) return std::unexpected(StageBEntryFailure::allocation);
        return std::move(*result);
    } catch (...) { return std::unexpected(StageBEntryFailure::allocation); }
}
} // namespace epycbench::runtime::detail
