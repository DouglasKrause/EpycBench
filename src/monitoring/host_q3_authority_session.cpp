#include "monitoring/host_q3_authority_session.hpp"

#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace {
auto failure(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
        .code = core::StableErrorCode{"monitoring.q3.authority.invalid"},
        .message = "Monitoring Q3 authority rejected",
        .operation = "compose_monitoring_q3_authority", .os_error = std::nullopt,
        .context = {{"classification", std::move(classification)}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command};
}
void retain(MonitoringQ1BoundedRetirement& ledger, core::Error value) noexcept {
    if (ledger.count < ledger.failures.size())
        ledger.failures[ledger.count++].emplace(std::move(value));
    else ledger.overflowed = true;
}
} // namespace

auto MonitoringQ3AuthoritySession::typed_failure(MonitoringQ1GitCommandFailure value,
    MonitoringQ3AuthorityStage stage) -> MonitoringQ3AuthorityFailure {
    MonitoringQ3AuthorityFailure result{stage, std::move(value.primary), {}};
    for (auto& item : value.cleanup) retain(result.cleanup, std::move(item));
    result.authored = std::move(value.governed_authored);
    return result;
}

MonitoringQ3SourceReady::MonitoringQ3SourceReady(MonitoringQ3PlanValue plan,
    MonitoringQ3CompleteQ2PredecessorValue predecessor, std::string commit,
    std::string qualification_id, std::string profile_sha256, std::string plan_bytes,
    core::Sha256Digest plan_sha256,
    core::Sha256Digest build_identity, std::string build_attestation) noexcept
    : plan_{std::move(plan)}, predecessor_{std::move(predecessor)}, commit_{std::move(commit)},
      qualification_id_{std::move(qualification_id)}, profile_sha256_{std::move(profile_sha256)},
      plan_bytes_{std::move(plan_bytes)}, plan_sha256_{plan_sha256},
      build_identity_{build_identity}, build_attestation_{std::move(build_attestation)} {}

MonitoringQ3AuthoritySession::MonitoringQ3AuthoritySession(
    MonitoringQ1PlanV2AuthoritySession repository, MonitoringQ3PlanPublicInputAuthority input,
    MonitoringQ3PlanValue plan, MonitoringQ3CompleteQ2PredecessorAuthority predecessor,
    MonitoringRunningBuildAuthority build, std::unique_ptr<HistoricalContext> historical,
    MonitoringQ1BoundedRetirement cleanup, std::string profile_sha256,
    std::string qualification_id) noexcept
    : repository_{std::move(repository)}, input_{std::move(input)}, plan_{std::move(plan)},
      predecessor_{std::move(predecessor)}, build_{std::move(build)},
      historical_{std::move(historical)}, cleanup_{std::move(cleanup)},
      profile_sha256_{std::move(profile_sha256)}, qualification_id_{std::move(qualification_id)} {}
MonitoringQ3AuthoritySession::MonitoringQ3AuthoritySession(
    LifecycleOperations operations, MonitoringQ3SourceReady ready) noexcept
    : test_lifecycle_{operations}, test_ready_{std::move(ready)} {}
MonitoringQ3AuthoritySession::MonitoringQ3AuthoritySession(MonitoringQ3AuthoritySession&& other) noexcept
    : repository_{std::move(other.repository_)}, input_{std::move(other.input_)},
      plan_{std::move(other.plan_)}, predecessor_{std::move(other.predecessor_)},
      build_{std::move(other.build_)}, historical_{std::move(other.historical_)},
      cleanup_{std::exchange(other.cleanup_, {})}, profile_sha256_{std::move(other.profile_sha256_)},
      qualification_id_{std::move(other.qualification_id_)},
      consumed_{std::exchange(other.consumed_, true)},
      test_lifecycle_{std::move(other.test_lifecycle_)},
      test_ready_{std::move(other.test_ready_)} {}
MonitoringQ3AuthoritySession::~MonitoringQ3AuthoritySession() noexcept {
    if (consumed_) return;
    consumed_ = true;
    if (test_lifecycle_) {
        static_cast<void>(test_lifecycle_->retire_build(test_lifecycle_->context));
        static_cast<void>(test_lifecycle_->retire_predecessor(test_lifecycle_->context));
        static_cast<void>(test_lifecycle_->retire_input(test_lifecycle_->context));
        static_cast<void>(test_lifecycle_->retire_repository(test_lifecycle_->context));
    } else {
        static_cast<void>(build_->retire());
        static_cast<void>(predecessor_->retire());
        static_cast<void>(input_->retire());
        static_cast<void>(repository_->retire_before_binding());
    }
    historical_.reset();
}
auto MonitoringQ3AuthoritySession::acquire_lifecycle(AcquisitionOperations operations) noexcept
    -> std::expected<void, MonitoringQ3AuthorityFailure> {
    try {
        const std::array stages{operations.repository, operations.fixed_records, operations.input,
            operations.decode, operations.q2_binding, operations.predecessor, operations.build};
        const std::array identities{MonitoringQ3AuthorityStage::repository,
            MonitoringQ3AuthorityStage::collection_plan, MonitoringQ3AuthorityStage::collection_plan,
            MonitoringQ3AuthorityStage::collection_plan, MonitoringQ3AuthorityStage::collection_plan,
            MonitoringQ3AuthorityStage::predecessor, MonitoringQ3AuthorityStage::build_attestation};
        for (std::size_t index{}; index < stages.size(); ++index) {
            try {
                auto result = stages[index](operations.context);
                if (!result)
                    return std::unexpected{operations.rollback(
                        operations.context, std::move(result.error()))};
            } catch (...) {
                return std::unexpected{operations.rollback(operations.context,
                    MonitoringQ3AuthorityFailure{identities[index], failure("contained"), {}})};
            }
        }
        return {};
    } catch (...) {
        return std::unexpected{operations.rollback(operations.context,
            MonitoringQ3AuthorityFailure{MonitoringQ3AuthorityStage::repository,
                                          failure("contained"), {}})};
    }
}

auto MonitoringQ3AuthoritySession::finalize_lifecycle(
    LifecycleOperations operations, MonitoringQ1BoundedRetirement cleanup) noexcept
    -> std::expected<void, MonitoringQ3AuthorityFailure> {
    std::optional<MonitoringQ3AuthorityFailure> rejected;
    const auto append_ledger = [&](MonitoringQ1BoundedRetirement source) noexcept {
        for (std::size_t index{}; index < source.count; ++index)
            retain(rejected ? rejected->cleanup : cleanup, std::move(*source.failures[index]));
        (rejected ? rejected->cleanup : cleanup).overflowed =
            (rejected ? rejected->cleanup : cleanup).overflowed || source.overflowed;
    };
    const auto reject = [&](MonitoringQ3AuthorityStage stage, MonitoringQ1BoundedFailure value) noexcept {
        if (!rejected) {
            rejected.emplace(MonitoringQ3AuthorityFailure{stage, std::move(value.primary),
                                                           std::move(cleanup)});
            append_ledger(std::move(value.cleanup));
        } else {
            retain(rejected->cleanup, std::move(value.primary));
            append_ledger(std::move(value.cleanup));
        }
    };
    const auto check = [&](auto operation, MonitoringQ3AuthorityStage stage,
                           std::string classification) noexcept {
        if (rejected) return;
        auto value = operation(operations.context);
        if (!value) reject(stage, std::move(value.error()));
        else if (!*value) reject(stage, MonitoringQ1BoundedFailure{failure(std::move(classification)), {}});
    };
    check(operations.recheck_repository, MonitoringQ3AuthorityStage::repository,
          "repository_changed");
    check(operations.recheck_input, MonitoringQ3AuthorityStage::collection_plan,
          "collection_plan_changed");
    check(operations.recheck_predecessor, MonitoringQ3AuthorityStage::predecessor,
          "predecessor_changed");
    check(operations.recheck_build, MonitoringQ3AuthorityStage::build_attestation,
          "running_build_changed");
    const auto retire_one = [&](auto operation) noexcept {
        auto value = operation(operations.context);
        if (!value)
            reject(MonitoringQ3AuthorityStage::retirement, std::move(value.error()));
    };
    retire_one(operations.retire_build);
    retire_one(operations.retire_predecessor);
    retire_one(operations.retire_input);
    retire_one(operations.retire_repository);
    if (rejected) return std::unexpected{std::move(*rejected)};
    if (cleanup.count != 0U || cleanup.overflowed) {
        auto primary = cleanup.count ? std::move(*cleanup.failures[0]) : failure("retirement_overflow");
        MonitoringQ1BoundedRetirement secondary;
        for (std::size_t index = 1U; index < cleanup.count; ++index)
            retain(secondary, std::move(*cleanup.failures[index]));
        secondary.overflowed = cleanup.overflowed;
        return std::unexpected{MonitoringQ3AuthorityFailure{MonitoringQ3AuthorityStage::retirement,
                                                             std::move(primary), std::move(secondary)}};
    }
    return {};
}

auto MonitoringQ3AuthoritySession::finalize_before_source() noexcept
    -> std::expected<MonitoringQ3SourceReady, MonitoringQ3AuthorityFailure> {
    if (consumed_) return std::unexpected{MonitoringQ3AuthorityFailure{
        MonitoringQ3AuthorityStage::final_recheck, failure("consumed"), {}}};
    if (test_lifecycle_) {
        consumed_ = true;
        auto finalized = finalize_lifecycle(*test_lifecycle_, std::exchange(cleanup_, {}));
        if (!finalized) return std::unexpected{std::move(finalized.error())};
        return std::move(*test_ready_);
    }
    std::optional<MonitoringQ3SourceReady> ready;
    try {
        ready.emplace(MonitoringQ3SourceReady{plan_, predecessor_->value(),
            std::string{input_->captured_head()}, qualification_id_, profile_sha256_,
            std::string{input_->bytes()}, input_->sha256(),
            build_->identity(), std::string{build_->canonical()}});
        consumed_ = true;
        LifecycleOperations operations{
            this,
            [](void* raw) noexcept -> std::expected<bool, MonitoringQ1BoundedFailure> {
                auto value = static_cast<MonitoringQ3AuthoritySession*>(raw)->repository_->q3_recheck();
                if (!value) {
                    MonitoringQ1BoundedFailure result{std::move(value.error().primary), {}};
                    for (auto& item : value.error().cleanup) retain(result.cleanup, std::move(item));
                    return std::unexpected{std::move(result)};
                }
                return *value;
            },
            [](void* raw) noexcept -> std::expected<bool, MonitoringQ1BoundedFailure> {
                auto value = static_cast<MonitoringQ3AuthoritySession*>(raw)->input_->recheck();
                if (!value) {
                    MonitoringQ1BoundedFailure result{std::move(value.error().primary), {}};
                    for (auto& item : value.error().cleanup) retain(result.cleanup, std::move(item));
                    return std::unexpected{std::move(result)};
                }
                return *value;
            },
            [](void* raw) noexcept -> std::expected<bool, MonitoringQ1BoundedFailure> {
                auto value = static_cast<MonitoringQ3AuthoritySession*>(raw)->predecessor_->recheck();
                if (!value) {
                    MonitoringQ1BoundedFailure result{std::move(value.error().primary), {}};
                    for (std::size_t index{}; index < value.error().cleanup_count; ++index)
                        retain(result.cleanup, std::move(*value.error().cleanup[index]));
                    return std::unexpected{std::move(result)};
                }
                return *value;
            },
            [](void* raw) noexcept -> std::expected<bool, MonitoringQ1BoundedFailure> {
                auto value = static_cast<MonitoringQ3AuthoritySession*>(raw)->build_->recheck();
                if (!value) return std::unexpected{MonitoringQ1BoundedFailure{std::move(value.error()), {}}};
                return *value;
            },
            [](void* raw) noexcept -> std::expected<void, MonitoringQ1BoundedFailure> {
                auto value = static_cast<MonitoringQ3AuthoritySession*>(raw)->build_->retire();
                if (!value) return std::unexpected{MonitoringQ1BoundedFailure{std::move(value.error()), {}}};
                return {};
            },
            [](void* raw) noexcept -> std::expected<void, MonitoringQ1BoundedFailure> {
                auto value = static_cast<MonitoringQ3AuthoritySession*>(raw)->predecessor_->retire();
                if (!value) {
                    MonitoringQ1BoundedFailure result{std::move(value.error().primary), {}};
                    for (std::size_t index{}; index < value.error().cleanup_count; ++index)
                        retain(result.cleanup, std::move(*value.error().cleanup[index]));
                    return std::unexpected{std::move(result)};
                }
                return {};
            },
            [](void* raw) noexcept -> std::expected<void, MonitoringQ1BoundedFailure> {
                auto value = static_cast<MonitoringQ3AuthoritySession*>(raw)->input_->retire();
                if (!value) {
                    MonitoringQ1BoundedFailure result{std::move(value.error().primary), {}};
                    for (auto& item : value.error().cleanup) retain(result.cleanup, std::move(item));
                    return std::unexpected{std::move(result)};
                }
                return {};
            },
            [](void* raw) noexcept -> std::expected<void, MonitoringQ1BoundedFailure> {
                auto value = static_cast<MonitoringQ3AuthoritySession*>(raw)->repository_->retire_before_binding();
                if (value.count == 0U && !value.overflowed) return {};
                auto primary = value.count ? std::move(*value.failures[0]) : failure("repository_retirement_overflow");
                MonitoringQ1BoundedFailure result{std::move(primary), {}};
                for (std::size_t index = 1U; index < value.count; ++index)
                    retain(result.cleanup, std::move(*value.failures[index]));
                result.cleanup.overflowed = value.overflowed;
                return std::unexpected{std::move(result)};
            }};
        auto finalized = finalize_lifecycle(operations, std::exchange(cleanup_, {}));
        historical_.reset();
        if (!finalized) return std::unexpected{std::move(finalized.error())};
        return std::move(*ready);
    } catch (...) {
        consumed_ = true;
        auto primary = failure("contained");
        auto build = build_->retire();
        if (!build) retain(cleanup_, std::move(build.error()));
        auto predecessor = predecessor_->retire();
        if (!predecessor) {
            retain(cleanup_, std::move(predecessor.error().primary));
            for (std::size_t index{}; index < predecessor.error().cleanup_count; ++index)
                retain(cleanup_, std::move(*predecessor.error().cleanup[index]));
        }
        auto input = input_->retire();
        if (!input) {
            retain(cleanup_, std::move(input.error().primary));
            for (auto& item : input.error().cleanup) retain(cleanup_, std::move(item));
        }
        auto repository = repository_->retire_before_binding();
        for (std::size_t index{}; index < repository.count; ++index)
            retain(cleanup_, std::move(*repository.failures[index]));
        cleanup_.overflowed = cleanup_.overflowed || repository.overflowed;
        historical_.reset();
        return std::unexpected{MonitoringQ3AuthorityFailure{
            MonitoringQ3AuthorityStage::final_recheck, std::move(primary),
            std::exchange(cleanup_, {})}};
    }
}
} // namespace epycbench::monitoring::qualification::detail
