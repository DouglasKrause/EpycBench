#include "monitoring/host_q2_terminal_containment.hpp"
#include "monitoring/host_q2_diagnostic_transport.hpp"
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <unistd.h>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Buffer = MonitoringQ2DiagnosticBuffer;
auto stage_name(MonitoringQ2CollectionStage value) noexcept -> std::string_view {
    switch (value) {
    case MonitoringQ2CollectionStage::preparation:
    case MonitoringQ2CollectionStage::session:
        return "session";
    case MonitoringQ2CollectionStage::source_preparation:
        return "source_preparation";
    case MonitoringQ2CollectionStage::voluntary_event:
        return "voluntary_event";
    case MonitoringQ2CollectionStage::preemption_event:
        return "preemption_event";
    case MonitoringQ2CollectionStage::drain:
        return "drain";
    case MonitoringQ2CollectionStage::association:
        return "association";
    case MonitoringQ2CollectionStage::clock:
        return "clock";
    case MonitoringQ2CollectionStage::completeness:
        return "completeness";
    case MonitoringQ2CollectionStage::finalization:
        return "finalization";
    }
    std::terminate();
}
void projected(Buffer& out, std::string_view classification, std::string_view underlying,
               int native) noexcept {
    append_monitoring_q2_projected_failure(out, classification, underlying, native);
}
/*
    if (!safe(underlying))
        underlying = "monitoring.q2.contained_failure";
    out.add("{\"category\":\"monitoring\",\"classification\":\"");
    out.add(classification);
    out.add(
        "\",\"code\":\"monitoring.q2.backend.stage_failure\",\"context\":{\"underlying_code\":\"");
    out.add(underlying);
    out.add(
        "\"},\"continuation\":\"abort_command\",\"operation\":\"run_monitoring_host_q2_backend\"");
    if (native > 0) {
        out.add(",\"os_error\":");
        out.number(native);
    }
    out.add(",\"severity\":\"error\"}");
}
void factual(Buffer& out, const core::Error& value) noexcept {
    const auto category =
        core::detail::enum_name(value.category, core::detail::error_category_names);
    const auto severity =
        core::detail::enum_name(value.severity, core::detail::error_severity_names);
    const auto continuation =
        core::detail::enum_name(value.continuation, core::detail::continuation_policy_names);
    const auto code = value.code.value();
    std::string_view classification;
    if (value.context.is_object()) {
        const auto found = value.context.find("classification");
        if (found != value.context.end()) {
            if (!found->is_string()) {
                out.failed = true;
                return;
            }
            classification = found->get_ref<const std::string&>();
        }
    }
    if (!safe(code) || !safe(value.operation) ||
        (!classification.empty() && !safe(classification))) {
        out.failed = true;
        return;
    }
    out.add("{\"category\":\"");
    out.add(category);
    if (!classification.empty()) {
        out.add("\",\"classification\":\"");
        out.add(classification);
    }
    out.add("\",\"code\":\"");
    out.add(code);
    out.add("\",\"context\":{");
    bool context_comma = false;
    if (!value.context.is_object()) {
        out.failed = true;
        return;
    }
    for (const auto& [key, item] : value.context.items()) {
        if (key == "classification")
            continue;
        if (!item.is_string()) {
            out.failed = true;
            return;
        }
        const auto& text = item.get_ref<const std::string&>();
        if (!context_value_ok(key, text)) {
            out.failed = true;
            return;
        }
        if (context_comma)
            out.add(",");
        out.add("\"");
        out.add(key);
        out.add("\":\"");
        out.add(text);
        out.add("\"");
        context_comma = true;
    }
    out.add("},\"continuation\":\"");
    out.add(continuation);
    out.add("\",\"operation\":\"");
    out.add(value.operation);
    if (value.os_error) {
        out.add("\",\"os_error\":");
        out.number(value.os_error->value);
        out.add(",\"severity\":\"");
    } else
        out.add("\",\"severity\":\"");
    out.add(severity);
    out.add("\"}");
}
void factual_cleanup(Buffer& out, std::string_view code, std::string_view classification,
                     int native) noexcept {
    out.add("{\"category\":\"monitoring\",\"classification\":\"");
    out.add(classification);
    out.add("\",\"code\":\"");
    out.add(code);
    out.add("\",\"context\":{},\"continuation\":\"abort_command\",\"operation\":\"monitoring_q2_"
            "native_cleanup\"");
    if (native > 0) {
        out.add(",\"os_error\":");
        out.number(native);
    }
    out.add(",\"severity\":\"error\"}");
}
*/
void factual(Buffer& out, const core::Error& value) noexcept {
    append_monitoring_q2_factual_failure(out, value);
}
void factual_cleanup(Buffer& out, std::string_view code, std::string_view classification,
                     int native) noexcept {
    append_monitoring_q2_native_cleanup(out, code, classification, native);
}
} // namespace
struct MonitoringQ2TerminalContainmentStock::Impl final {
    std::optional<MonitoringQ2BridgeRetirementRecovery> recovery;
    std::optional<MonitoringQ2ContextSwitchSourceOwner> source;
    std::optional<MonitoringQ2CollectorPlan> plan;
    std::optional<MonitoringQ2HelperProvider> helper_provider;
    MonitoringQ2CollectionStage stage{};
    std::optional<core::Error> primary;
    std::optional<MonitoringQ2CompactDiagnostic> compact_primary;
    std::array<std::optional<core::Error>, 64> cleanup{};
    std::size_t cleanup_count{};
    bool cleanup_overflowed{};
    MonitoringQ2CompactLedger<64> diagnostics;
    std::unique_ptr<Buffer> transport;
};
MonitoringQ2TerminalContainmentStock::MonitoringQ2TerminalContainmentStock(
    std::unique_ptr<Impl> value) noexcept
    : impl_{std::move(value)} {}
MonitoringQ2TerminalContainmentStock::MonitoringQ2TerminalContainmentStock(
    MonitoringQ2TerminalContainmentStock&&) noexcept = default;
MonitoringQ2TerminalContainmentStock::~MonitoringQ2TerminalContainmentStock() noexcept = default;
auto MonitoringQ2TerminalContainment::reserve(core::Error diagnostic) noexcept
    -> std::expected<MonitoringQ2TerminalContainmentStock, core::Error> {
    try {
        auto value = std::make_unique<MonitoringQ2TerminalContainmentStock::Impl>();
        value->transport = std::make_unique<Buffer>();
        return MonitoringQ2TerminalContainmentStock{std::move(value)};
    } catch (...) {
        return std::unexpected{std::move(diagnostic)};
    }
}
auto MonitoringQ2TerminalContainment::contain(
    MonitoringQ2TerminalContainmentStock&& stock, MonitoringQ2BridgeRetirementRecovery&& recovery,
    MonitoringQ2ContextSwitchSourceOwner&& source, MonitoringQ2CollectorPlan&& plan,
    MonitoringQ2CollectionStage stage, core::Error primary,
    std::array<std::optional<core::Error>, 64>&& prior_cleanup, std::size_t prior_cleanup_count,
    bool prior_cleanup_overflowed,
    std::optional<MonitoringQ2HelperProvider>&& helper_provider) noexcept
    -> MonitoringQ2TerminalContainment {
    if (!stock.impl_ || !stock.impl_->transport)
        std::terminate();
    auto failure = recovery.take_failure_for_containment();
    stock.impl_->recovery.emplace(std::move(recovery));
    stock.impl_->source.emplace(std::move(source));
    stock.impl_->plan.emplace(std::move(plan));
    if (helper_provider)
        stock.impl_->helper_provider.emplace(std::move(*helper_provider));
    stock.impl_->stage = stage;
    stock.impl_->primary.emplace(std::move(primary));
    stock.impl_->cleanup_overflowed = prior_cleanup_overflowed;
    const auto append = [&](core::Error value) {
        if (stock.impl_->cleanup_count < stock.impl_->cleanup.size())
            stock.impl_->cleanup[stock.impl_->cleanup_count++].emplace(std::move(value));
        else
            stock.impl_->cleanup_overflowed = true;
    };
    for (std::size_t i{}; i < prior_cleanup_count && i < prior_cleanup.size(); ++i)
        append(std::move(*prior_cleanup[i]));
    if (failure.has_primary)
        append(std::move(failure.primary));
    for (std::size_t i{}; i < failure.cleanup_count && i < failure.cleanup.size(); ++i)
        append(std::move(*failure.cleanup[i]));
    stock.impl_->cleanup_overflowed = stock.impl_->cleanup_overflowed || failure.cleanup_overflowed;
    for (std::size_t i{}; i < failure.compact_cleanup.size(); ++i)
        stock.impl_->diagnostics.append(failure.compact_cleanup[i]);
    if (failure.compact_cleanup.overflowed())
        stock.impl_->diagnostics.note_overflow();
    return MonitoringQ2TerminalContainment{std::move(stock.impl_)};
}
auto MonitoringQ2TerminalContainment::contain_session(
    MonitoringQ2TerminalContainmentStock&& stock, MonitoringQ2CollectorPlan&& plan,
    MonitoringQ2CollectionStage stage, core::Error primary,
    std::array<std::optional<core::Error>, 64>&& cleanup, std::size_t cleanup_count,
    bool cleanup_overflowed, bool caller_has_primary,
    std::optional<MonitoringQ2HelperProvider>&& helper_provider) noexcept
    -> MonitoringQ2TerminalContainment {
    if (!stock.impl_ || !stock.impl_->transport)
        std::terminate();
    if (plan.worker_session_) {
        auto native = plan.worker_session_->take_native_containment_diagnostics();
        if (!caller_has_primary && native.authored_primary)
            primary = std::move(*native.authored_primary);
        else if (!caller_has_primary)
            stock.impl_->compact_primary.emplace(native.primary);
        else if (native.authored_primary) {
            if (cleanup_count < cleanup.size())
                cleanup[cleanup_count++].emplace(std::move(*native.authored_primary));
            else
                cleanup_overflowed = true;
        } else
            stock.impl_->diagnostics.append(native.primary);
        for (std::size_t index{}; index < native.cleanup.size(); ++index)
            stock.impl_->diagnostics.append(native.cleanup[index]);
        if (native.cleanup.overflowed())
            stock.impl_->diagnostics.note_overflow();
    }
    stock.impl_->plan.emplace(std::move(plan));
    if (helper_provider)
        stock.impl_->helper_provider.emplace(std::move(*helper_provider));
    stock.impl_->stage = stage;
    if (caller_has_primary || !stock.impl_->compact_primary)
        stock.impl_->primary.emplace(std::move(primary));
    stock.impl_->cleanup_count = std::min(cleanup_count, stock.impl_->cleanup.size());
    for (std::size_t index{}; index < stock.impl_->cleanup_count; ++index)
        stock.impl_->cleanup[index].emplace(std::move(*cleanup[index]));
    stock.impl_->cleanup_overflowed =
        cleanup_overflowed || cleanup_count > stock.impl_->cleanup.size();
    return MonitoringQ2TerminalContainment{std::move(stock.impl_)};
}
MonitoringQ2TerminalContainment::MonitoringQ2TerminalContainment(
    std::unique_ptr<MonitoringQ2TerminalContainmentStock::Impl> value) noexcept
    : impl_{std::move(value)} {}
MonitoringQ2TerminalContainment::MonitoringQ2TerminalContainment(
    MonitoringQ2TerminalContainment&&) noexcept = default;
MonitoringQ2TerminalContainment::~MonitoringQ2TerminalContainment() noexcept {
    if (impl_)
        std::terminate();
}
[[noreturn]] void
consume_monitoring_q2_terminal_containment(MonitoringQ2TerminalContainment&& value) noexcept {
    const MonitoringQ2TerminalContainmentOperations operations{
        nullptr,
        [](void*, int fd, const void* bytes, std::size_t size) noexcept -> std::ptrdiff_t {
            return ::write(fd, bytes, size);
        },
        [](void*, int status) noexcept { std::_Exit(status); }};
    consume_monitoring_q2_terminal_containment_with(std::move(value), operations);
}
[[noreturn]] void consume_monitoring_q2_terminal_containment_with(
    MonitoringQ2TerminalContainment&& value,
    MonitoringQ2TerminalContainmentOperations operations) noexcept {
    auto* impl = value.impl_.get();
    if (!impl || !impl->transport)
        std::_Exit(1);
    auto& out = *impl->transport;
    const auto classification = stage_name(impl->stage);
    const auto underlying = impl->primary ? impl->primary->code.value()
                                          : std::string_view{"monitoring.q2.native_cleanup"};
    out.add("{\"failure\":");
    projected(out, classification, underlying,
              impl->primary && impl->primary->os_error ? impl->primary->os_error->value
              : impl->compact_primary                  ? impl->compact_primary->native_error
                                                       : 0);
    out.add(
        ",\"schema\":\"epycbench.monitoring-host-q2-backend-error/v1\",\"secondary_failures\":[");
    bool comma = false;
    constexpr std::size_t secondary_limit = 64U;
    const auto total = impl->cleanup_count + impl->diagnostics.size();
    const bool overflow =
        impl->cleanup_overflowed || impl->diagnostics.overflowed() || total > secondary_limit - 1U;
    const auto factual_limit = overflow ? std::min(total, secondary_limit - 1U) : total;
    const auto core_limit = std::min(impl->cleanup_count, factual_limit);
    for (std::size_t i{}; i < core_limit; ++i) {
        if (comma)
            out.add(",");
        const auto& item = *impl->cleanup[i];
        factual(out, item);
        comma = true;
    }
    const auto diagnostic_limit = factual_limit - core_limit;
    for (std::size_t i{}; i < diagnostic_limit; ++i) {
        if (comma)
            out.add(",");
        const auto item = impl->diagnostics[i];
        factual_cleanup(out, "monitoring.q2.native_cleanup", "native_cleanup", item.native_error);
        comma = true;
    }
    if (overflow) {
        if (comma)
            out.add(",");
        factual_cleanup(out, "monitoring.q2.backend.cleanup_overflow", "finalization", 0);
    }
    out.add("]}\n");
    finish_monitoring_q2_diagnostic_buffer(
        out, classification, underlying,
        impl->primary && impl->primary->os_error ? impl->primary->os_error->value
        : impl->compact_primary                  ? impl->compact_primary->native_error
                                                 : 0);
    static_cast<void>(
        write_monitoring_q2_diagnostic_buffer(out, {operations.context, operations.write}));
    if (operations.exit)
        operations.exit(operations.context, 1);
    std::_Exit(1);
}
} // namespace epycbench::monitoring::qualification::detail
