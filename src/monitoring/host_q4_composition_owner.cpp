#include "monitoring/host_q4_composition_owner.hpp"

#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace {
thread_local unsigned char create_fault{};
auto native_error(const core::Error& value) noexcept -> int {
    return value.os_error ? value.os_error->value : 0;
}
auto emergency() noexcept -> MonitoringQ4CompositionFailure {
    return {.primary = {MonitoringQ4CompositionSource::contained,
                        MonitoringQ4CompositionFactKind::contained}};
}
} // namespace

class MonitoringQ4CompositionOwner::Impl final {
  public:
    struct TestSlot final {
        void* context{};
        MonitoringQ4CompositionRetirement (*retire)(void*) noexcept {};
        bool active{};
    };
    std::optional<MonitoringQ1PlanV2AuthoritySession> repository;
    std::optional<MonitoringQ4GovernedInputAuthority> governed;
    std::optional<MonitoringQ4GovernedInputCandidate> candidate;
    std::optional<MonitoringQ4CompleteQ3PredecessorAuthority> predecessor;
    std::optional<MonitoringRunningBuildAuthority> build;
    std::array<TestSlot, 4> tests{};
    MonitoringQ4CompositionRetirement cleanup{};
    bool retired{};

    void append(const MonitoringQ4CompositionFact fact) noexcept {
        if (cleanup.count < cleanup.facts.size())
            cleanup.facts[cleanup.count++] = fact;
        else
            cleanup.overflowed = true;
    }
    void append_retirement(MonitoringQ4CompositionRetirement value) noexcept {
        for (std::size_t index{}; index < value.count; ++index)
            append(value.facts[index]);
        cleanup.overflowed = cleanup.overflowed || value.overflowed;
    }
};

MonitoringQ4CompositionOwner::MonitoringQ4CompositionOwner(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}
MonitoringQ4CompositionOwner::MonitoringQ4CompositionOwner(
    MonitoringQ4CompositionOwner&& other) noexcept
    : impl_{std::move(other.impl_)} {}
MonitoringQ4CompositionOwner::~MonitoringQ4CompositionOwner() noexcept {
    if (impl_)
        static_cast<void>(retire());
}

auto MonitoringQ4CompositionOwner::create() noexcept
    -> std::expected<MonitoringQ4CompositionOwner, MonitoringQ4CompositionFailure> {
    try {
        if (create_fault == 1U)
            throw std::bad_alloc{};
        if (create_fault == 2U)
            throw 1;
        return MonitoringQ4CompositionOwner{std::make_unique<Impl>()};
    } catch (const std::bad_alloc&) {
        auto failure = emergency();
        failure.primary.kind = MonitoringQ4CompositionFactKind::allocation;
        return std::unexpected{failure};
    } catch (...) {
        return std::unexpected{emergency()};
    }
}
void MonitoringQ4CompositionOwner::set_create_fault(const unsigned char value) noexcept {
    create_fault = value;
}

void MonitoringQ4CompositionOwner::adopt_repository(
    MonitoringQ1PlanV2AuthoritySession value) noexcept {
    if (!impl_ || impl_->retired || impl_->repository || impl_->tests[0].active)
        std::terminate();
    impl_->repository.emplace(std::move(value));
}
void MonitoringQ4CompositionOwner::adopt_governed(
    MonitoringQ4GovernedInputAuthority authority,
    MonitoringQ4GovernedInputCandidate candidate) noexcept {
    if (!impl_ || impl_->retired || impl_->governed || impl_->candidate || impl_->tests[1].active)
        std::terminate();
    impl_->governed.emplace(std::move(authority));
    impl_->candidate.emplace(std::move(candidate));
}
void MonitoringQ4CompositionOwner::adopt_predecessor(
    MonitoringQ4CompleteQ3PredecessorAuthority value) noexcept {
    if (!impl_ || impl_->retired || impl_->predecessor || impl_->tests[2].active)
        std::terminate();
    impl_->predecessor.emplace(std::move(value));
}
void MonitoringQ4CompositionOwner::adopt_build(MonitoringRunningBuildAuthority value) noexcept {
    if (!impl_ || impl_->retired || impl_->build || impl_->tests[3].active)
        std::terminate();
    impl_->build.emplace(std::move(value));
}

void MonitoringQ4CompositionOwner::adopt_test(
    const std::size_t slot, void* context,
    MonitoringQ4CompositionRetirement (*retire_operation)(void*) noexcept) noexcept {
    if (!impl_ || impl_->retired || slot >= impl_->tests.size() || retire_operation == nullptr ||
        impl_->tests[slot].active)
        std::terminate();
    impl_->tests[slot] = {context, retire_operation, true};
}
void MonitoringQ4CompositionOwner::seed_cleanup(const MonitoringQ4CompositionFact fact) noexcept {
    if (!impl_ || impl_->retired)
        std::terminate();
    if (impl_->cleanup.count < MonitoringQ4CompositionRetirement::component_capacity)
        impl_->append(fact);
    else
        impl_->cleanup.overflowed = true;
}
void MonitoringQ4CompositionOwner::note_cleanup_overflow() noexcept {
    if (!impl_ || impl_->retired)
        std::terminate();
    impl_->cleanup.overflowed = true;
}
auto MonitoringQ4CompositionOwner::repository() noexcept -> MonitoringQ1PlanV2AuthoritySession& {
    return *impl_->repository;
}
auto MonitoringQ4CompositionOwner::governed() noexcept -> MonitoringQ4GovernedInputAuthority& {
    return *impl_->governed;
}
auto MonitoringQ4CompositionOwner::candidate() noexcept -> MonitoringQ4GovernedInputCandidate& {
    return *impl_->candidate;
}
auto MonitoringQ4CompositionOwner::predecessor() noexcept
    -> MonitoringQ4CompleteQ3PredecessorAuthority& {
    return *impl_->predecessor;
}
auto MonitoringQ4CompositionOwner::build() noexcept -> MonitoringRunningBuildAuthority& {
    return *impl_->build;
}
void MonitoringQ4CompositionOwner::append_governed_failure(
    const MonitoringQ4GovernedInputFailure& value) noexcept {
    impl_->append({MonitoringQ4CompositionSource::governed,
                   MonitoringQ4CompositionFactKind::cleanup, static_cast<unsigned int>(value.kind),
                   value.native_error});
    for (std::size_t index{}; index < value.retirement.cleanup_count; ++index)
        impl_->append(
            {MonitoringQ4CompositionSource::governed, MonitoringQ4CompositionFactKind::cleanup,
             value.retirement.cleanup_roles[index], value.retirement.cleanup_errors[index]});
    impl_->cleanup.overflowed = impl_->cleanup.overflowed || value.retirement.cleanup_overflowed;
}
void MonitoringQ4CompositionOwner::append_governed_inactive() noexcept {
    impl_->append(
        {MonitoringQ4CompositionSource::governed, MonitoringQ4CompositionFactKind::inactive});
}
void MonitoringQ4CompositionOwner::append_predecessor_retirement(
    const MonitoringQ4CompleteQ3Retirement& value) noexcept {
    for (std::size_t index{}; index < value.cleanup_count; ++index) {
        const auto& fact = value.cleanup_facts[index];
        impl_->append({MonitoringQ4CompositionSource::predecessor,
                       MonitoringQ4CompositionFactKind::cleanup,
                       static_cast<unsigned int>(fact.role), fact.native_error});
    }
    impl_->cleanup.overflowed = impl_->cleanup.overflowed || value.cleanup_overflowed;
}
void MonitoringQ4CompositionOwner::append_repository_retirement(
    const MonitoringQ1BoundedRetirement& value) noexcept {
    for (std::size_t index{}; index < value.count; ++index)
        impl_->append({MonitoringQ4CompositionSource::repository,
                       MonitoringQ4CompositionFactKind::cleanup, 0U,
                       native_error(*value.failures[index])});
    impl_->cleanup.overflowed = impl_->cleanup.overflowed || value.overflowed;
}

auto MonitoringQ4CompositionOwner::retire(
    std::optional<MonitoringQ4GovernedInputValue>* hidden_governed) noexcept
    -> MonitoringQ4CompositionRetirement {
    if (!impl_ || impl_->retired)
        return {};
    impl_->retired = true;
    for (std::size_t reverse{}; reverse < impl_->tests.size(); ++reverse) {
        const auto slot = impl_->tests.size() - 1U - reverse;
        auto& test = impl_->tests[slot];
        if (test.active) {
            test.active = false;
            impl_->append_retirement(test.retire(test.context));
        }
    }
    if (impl_->build) {
        auto value = impl_->build->retire();
        if (!value)
            impl_->append({MonitoringQ4CompositionSource::build,
                           MonitoringQ4CompositionFactKind::cleanup, 0U,
                           native_error(value.error())});
        impl_->build.reset();
    }
    if (impl_->predecessor) {
        const auto value = impl_->predecessor->retire();
        append_predecessor_retirement(value);
        impl_->predecessor.reset();
    }
    if (impl_->governed) {
        if (!impl_->candidate) {
            append_governed_inactive();
        } else {
            auto value = impl_->governed->finalize(std::move(*impl_->candidate));
            if (!value)
                append_governed_failure(value.error());
            else if (hidden_governed) {
                // Only the successful original finalizer supplies this value;
                // the caller keeps it hidden until the entire owner retires.
                static_assert(std::is_nothrow_move_constructible_v<MonitoringQ4GovernedInputValue>);
                static_assert(noexcept(hidden_governed->emplace(std::move(*value))));
                hidden_governed->emplace(std::move(*value));
            }
        }
        impl_->candidate.reset();
        impl_->governed.reset();
    }
    if (impl_->repository) {
        auto value = impl_->repository->retire_before_binding();
        append_repository_retirement(value);
        impl_->repository.reset();
    }
    return std::exchange(impl_->cleanup, {});
}

auto MonitoringQ4CompositionOwner::rollback(const MonitoringQ4CompositionFact primary) noexcept
    -> MonitoringQ4CompositionFailure {
    auto cleanup = retire();
    return {primary, std::move(cleanup)};
}
auto MonitoringQ4CompositionOwner::take_cleanup() noexcept -> MonitoringQ4CompositionRetirement {
    if (!impl_)
        return {};
    return std::exchange(impl_->cleanup, {});
}

} // namespace epycbench::monitoring::qualification::detail
