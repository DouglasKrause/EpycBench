#include "monitoring/host_q3_composition_owner.hpp"

#include "monitoring/host_q3_complete_q2_predecessor_authority.hpp"
#include "monitoring/host_q3_plan_public_input_authority.hpp"
#include "monitoring/host_running_build_authority.hpp"

#include <utility>

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ3CompositionOwner::Impl final {
  public:
    MonitoringQ1BoundedRetirement cleanup;
    std::optional<MonitoringQ1PlanV2AuthoritySession> repository;
    std::optional<MonitoringQ3PlanPublicInputAuthority> input;
    std::optional<MonitoringQ3CompleteQ2PredecessorAuthority> predecessor;
    std::optional<MonitoringRunningBuildAuthority> build;
    struct TestRetirement final {
        void* context{};
        std::optional<core::Error> (*retire)(void*) noexcept{};
    };
    std::array<TestRetirement, 4> test{};
    ~Impl() noexcept {
        for (auto iterator = test.rbegin(); iterator != test.rend(); ++iterator)
            if (iterator->retire) static_cast<void>(iterator->retire(iterator->context));
    }
};
MonitoringQ3CompositionOwner::MonitoringQ3CompositionOwner(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}
MonitoringQ3CompositionOwner::MonitoringQ3CompositionOwner(MonitoringQ3CompositionOwner&&) noexcept = default;
MonitoringQ3CompositionOwner::~MonitoringQ3CompositionOwner() noexcept = default;
auto MonitoringQ3CompositionOwner::create() noexcept
    -> std::expected<MonitoringQ3CompositionOwner, MonitoringQ1GitCommandFailure> {
    try { return MonitoringQ3CompositionOwner{std::make_unique<Impl>()}; }
    catch (...) {
        core::Error value{};
        value.category = core::ErrorCategory::monitoring;
        value.code = core::StableErrorCode{"monitoring.q3.composition.allocation_failed"};
        value.operation = "compose_monitoring_q3_authority";
        value.severity = core::ErrorSeverity::error;
        value.continuation = core::ContinuationPolicy::abort_command;
        return std::unexpected{MonitoringQ1GitCommandFailure{std::move(value), {}}};
    }
}
void MonitoringQ3CompositionOwner::adopt_repository(
    MonitoringQ1PlanV2AuthoritySession value) noexcept { impl_->repository.emplace(std::move(value)); }
void MonitoringQ3CompositionOwner::adopt_input(MonitoringQ3PlanPublicInputAuthority value) noexcept {
    impl_->input.emplace(std::move(value));
}
void MonitoringQ3CompositionOwner::adopt_predecessor(
    MonitoringQ3CompleteQ2PredecessorAuthority value) noexcept { impl_->predecessor.emplace(std::move(value)); }
void MonitoringQ3CompositionOwner::adopt_build(MonitoringRunningBuildAuthority value) noexcept {
    impl_->build.emplace(std::move(value));
}
void MonitoringQ3CompositionOwner::adopt_test(
    std::size_t slot, void* context, std::optional<core::Error> (*retirement)(void*) noexcept) noexcept {
    if (slot >= impl_->test.size() || impl_->test[slot].retire) std::terminate();
    impl_->test[slot] = {context, retirement};
}
auto MonitoringQ3CompositionOwner::repository() noexcept
    -> MonitoringQ1PlanV2AuthoritySession& { return *impl_->repository; }
auto MonitoringQ3CompositionOwner::take_repository() noexcept
    -> MonitoringQ1PlanV2AuthoritySession {
    auto value = std::move(*impl_->repository);
    impl_->repository.reset();
    return value;
}
auto MonitoringQ3CompositionOwner::input() noexcept -> MonitoringQ3PlanPublicInputAuthority& {
    return *impl_->input;
}
auto MonitoringQ3CompositionOwner::take_input() noexcept -> MonitoringQ3PlanPublicInputAuthority {
    auto value = std::move(*impl_->input); impl_->input.reset(); return value;
}
auto MonitoringQ3CompositionOwner::predecessor() noexcept
    -> MonitoringQ3CompleteQ2PredecessorAuthority& { return *impl_->predecessor; }
auto MonitoringQ3CompositionOwner::take_predecessor() noexcept
    -> MonitoringQ3CompleteQ2PredecessorAuthority {
    auto value = std::move(*impl_->predecessor); impl_->predecessor.reset(); return value;
}
auto MonitoringQ3CompositionOwner::build() noexcept -> MonitoringRunningBuildAuthority& {
    return *impl_->build;
}
auto MonitoringQ3CompositionOwner::take_build() noexcept -> MonitoringRunningBuildAuthority {
    auto value = std::move(*impl_->build); impl_->build.reset(); return value;
}
auto MonitoringQ3CompositionOwner::take_cleanup() noexcept -> MonitoringQ1BoundedRetirement {
    return std::exchange(impl_->cleanup, {});
}
void MonitoringQ3CompositionOwner::seed_cleanup(core::Error value) noexcept {
    if (impl_->cleanup.count < impl_->cleanup.failures.size())
        impl_->cleanup.failures[impl_->cleanup.count++].emplace(std::move(value));
    else impl_->cleanup.overflowed = true;
}
void MonitoringQ3CompositionOwner::note_cleanup_overflow() noexcept {
    impl_->cleanup.overflowed = true;
}
auto MonitoringQ3CompositionOwner::rollback_git(
    MonitoringQ1GitCommandFailure failure, bool overflowed) noexcept
    -> MonitoringQ3CompositionFailure {
    for (auto& value : failure.cleanup) seed_cleanup(std::move(value));
    impl_->cleanup.overflowed = impl_->cleanup.overflowed || overflowed;
    return rollback(std::move(failure.primary));
}
auto MonitoringQ3CompositionOwner::rollback(core::Error primary) noexcept
    -> MonitoringQ3CompositionFailure {
    MonitoringQ3CompositionFailure result{std::move(primary), take_cleanup()};
    const auto retain = [&](core::Error value) noexcept {
        if (result.cleanup.count < result.cleanup.failures.size())
            result.cleanup.failures[result.cleanup.count++].emplace(std::move(value));
        else result.cleanup.overflowed = true;
    };
    if (impl_->build) { auto retired = impl_->build->retire(); if (!retired) retain(std::move(retired.error())); impl_->build.reset(); }
    if (impl_->test[3].retire) { auto value = impl_->test[3].retire(impl_->test[3].context); if (value) retain(std::move(*value)); impl_->test[3] = {}; }
    if (impl_->predecessor) {
        auto retired = impl_->predecessor->retire();
        if (!retired) {
            retain(std::move(retired.error().primary));
            for (std::size_t index{}; index < retired.error().cleanup_count; ++index)
                retain(std::move(*retired.error().cleanup[index]));
        }
        impl_->predecessor.reset();
    }
    if (impl_->test[2].retire) { auto value = impl_->test[2].retire(impl_->test[2].context); if (value) retain(std::move(*value)); impl_->test[2] = {}; }
    if (impl_->input) {
        auto retired = impl_->input->retire();
        if (!retired) {
            retain(std::move(retired.error().primary));
            for (auto& value : retired.error().cleanup) retain(std::move(value));
        }
        impl_->input.reset();
    }
    if (impl_->test[1].retire) { auto value = impl_->test[1].retire(impl_->test[1].context); if (value) retain(std::move(*value)); impl_->test[1] = {}; }
    if (impl_->repository) {
        auto retired = impl_->repository->retire_before_binding();
        for (std::size_t index{}; index < retired.count; ++index)
            retain(std::move(*retired.failures[index]));
        result.cleanup.overflowed = result.cleanup.overflowed || retired.overflowed;
        impl_->repository.reset();
    }
    if (impl_->test[0].retire) { auto value = impl_->test[0].retire(impl_->test[0].context); if (value) retain(std::move(*value)); impl_->test[0] = {}; }
    return result;
}
auto MonitoringQ3CompositionOwner::retire() noexcept -> MonitoringQ1BoundedRetirement {
    return rollback(core::Error{}).cleanup;
}
} // namespace epycbench::monitoring::qualification::detail
