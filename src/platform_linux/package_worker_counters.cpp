#include "platform_linux/package_worker_counters.hpp"

namespace epycbench::platform_linux::package_execution {
namespace {
using Unavailable = monitoring::HardwareCounterUnavailable;
using Evidence = monitoring::WorkerHardwareCounterEvidence;
}

auto WorkerPerfBracket::attach(core::WorkerId worker, bool supplied) -> WorkerPerfBracket {
    auto bracket = WorkerPerfBracket{worker};
    if (supplied) {
        bracket.unavailable_ = Unavailable::supplied_workers;
        return bracket;
    }
    auto thread = current_thread_id();
    if (!thread) {
        bracket.unavailable_ = Unavailable::attachment_failed;
        return bracket;
    }
    auto group = HardwareCounterGroup::attach(*thread);
    if (group) {
        bracket.group_.emplace(std::move(*group));
        return bracket;
    }
    const auto& code = group.error().code.value();
    bracket.unavailable_ = code.ends_with(".access_denied") ? Unavailable::access_denied
        : code.ends_with(".unsupported_event") ? Unavailable::unsupported_event
        : code.ends_with(".worker_retired") ? Unavailable::worker_retired
        : Unavailable::attachment_failed;
    return bracket;
}

void WorkerPerfBracket::begin() {
    if (!group_) return;
    if (group_->begin()) {
        started_ = true;
        return;
    }
    unavailable_ = Unavailable::begin_failed;
    group_.reset();
}

auto WorkerPerfBracket::finish(std::optional<std::uint64_t> callback_start, std::optional<std::uint64_t> callback_end)
    -> CounterEvidence {
    const auto unavailable = [&](Unavailable reason) {
        return CounterEvidence{Evidence::unavailable(id_, reason), {}, {}};
    };
    if (!group_ || !started_) return unavailable(unavailable_.value_or(Unavailable::begin_failed));
    const auto window = group_->finish();
    group_.reset();
    if (!window) return unavailable(Unavailable::finish_failed);
    if (!callback_start || !callback_end || *callback_start >= *callback_end)
        return unavailable(Unavailable::invalid_window);
    const auto start = RawMonotonicTimestamp::from_nanoseconds(*callback_start);
    const auto end = RawMonotonicTimestamp::from_nanoseconds(*callback_end);
    auto evidence = Evidence::create(id_, *window, start, end);
    if (!evidence) return unavailable(Unavailable::invalid_window);
    return CounterEvidence{std::move(*evidence), start, end};
}

} // namespace epycbench::platform_linux::package_execution
