#pragma once

#include "monitoring/host_q4_composition_owner.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4CompositionOwnerTestAccess final {
  public:
    enum class CreateFault : unsigned char { none, allocation, contained };
    static void set_create_fault(const CreateFault value) noexcept {
        MonitoringQ4CompositionOwner::set_create_fault(static_cast<unsigned char>(value));
    }
    static void adopt(MonitoringQ4CompositionOwner& owner, std::size_t slot, void* context,
                      MonitoringQ4CompositionRetirement (*retire)(void*) noexcept) noexcept {
        owner.adopt_test(slot, context, retire);
    }
    static void adopt_build(MonitoringQ4CompositionOwner& owner,
                            MonitoringRunningBuildAuthority value) noexcept {
        owner.adopt_build(std::move(value));
    }
    static auto rollback(MonitoringQ4CompositionOwner& owner,
                         MonitoringQ4CompositionFact primary) noexcept {
        return owner.rollback(primary);
    }
    static auto retire(MonitoringQ4CompositionOwner& owner) noexcept { return owner.retire(); }
    static auto take_cleanup(MonitoringQ4CompositionOwner& owner) noexcept {
        return owner.take_cleanup();
    }
    static void seed(MonitoringQ4CompositionOwner& owner,
                     MonitoringQ4CompositionFact fact) noexcept {
        owner.seed_cleanup(fact);
    }
    static void overflow(MonitoringQ4CompositionOwner& owner) noexcept {
        owner.note_cleanup_overflow();
    }
    static void append_governed(MonitoringQ4CompositionOwner& owner,
                                const MonitoringQ4GovernedInputFailure& value) noexcept {
        owner.append_governed_failure(value);
    }
    static void append_governed_inactive(MonitoringQ4CompositionOwner& owner) noexcept {
        owner.append_governed_inactive();
    }
    static void append_predecessor(MonitoringQ4CompositionOwner& owner,
                                   const MonitoringQ4CompleteQ3Retirement& value) noexcept {
        owner.append_predecessor_retirement(value);
    }
    static void append_repository(MonitoringQ4CompositionOwner& owner,
                                  const MonitoringQ1BoundedRetirement& value) noexcept {
        owner.append_repository_retirement(value);
    }
    static auto predecessor(MonitoringQ4CompositionOwner& owner) noexcept
        -> MonitoringQ4CompleteQ3PredecessorAuthority& {
        return owner.predecessor();
    }
};
} // namespace epycbench::monitoring::qualification::detail
