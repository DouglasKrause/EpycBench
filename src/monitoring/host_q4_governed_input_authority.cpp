#include "monitoring/host_q4_governed_input_authority.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace epycbench::monitoring::qualification::detail {
namespace {
int open_at(void*, int parent, const char* name, int flags) noexcept {
    return ::openat(parent, name, flags);
}
int stat_fd(void*, int descriptor, struct stat* value) noexcept {
    return ::fstat(descriptor, value);
}
int stat_at(void*, int parent, const char* name, struct stat* value, int flags) noexcept {
    return ::fstatat(parent, name, value, flags);
}
ssize_t read_at(void*, int descriptor, void* bytes, std::size_t size, off_t offset) noexcept {
    return ::pread(descriptor, bytes, size, offset);
}
int close_fd(void*, int descriptor) noexcept { return ::close(descriptor); }
unsigned int effective_user(void*) noexcept { return static_cast<unsigned int>(::geteuid()); }
void no_fault(void*, unsigned int) {}

auto projected(MonitoringQ4GovernedInputFailureStage stage,
               const MonitoringGovernedTrackedBundleFailure& value)
    -> MonitoringQ4GovernedInputFailure {
    return {stage, value.kind, value.native_error, value.retirement};
}
} // namespace

MonitoringQ4GovernedInputValue::MonitoringQ4GovernedInputValue(
    MonitoringQ4GovernedArtifactSlots records, std::string head) noexcept
    : records_{std::move(records)}, head_{std::move(head)} {}

#define EPYCBENCH_Q4_ARTIFACT_ACCESSOR(name, index)                                                \
    auto MonitoringQ4GovernedInputValue::name() const noexcept                                     \
        -> const MonitoringQ4GovernedArtifact& {                                                   \
        return *records_[index];                                                                   \
    }
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(profile, 0)
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(q1_plan, 1)
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(q2_plan, 2)
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(q2_plan_schema, 3)
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(q2_result_schema, 4)
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(q2_error_schema, 5)
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(q2_authored_schema, 6)
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(q3_plan, 7)
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(q3_plan_schema, 8)
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(q3_evidence_schema, 9)
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(q3_result_schema, 10)
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(q4_plan, 11)
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(q4_plan_schema, 12)
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(q4_evidence_schema, 13)
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(q4_result_schema, 14)
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(q4_error_schema, 15)
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(q4_authored_schema, 16)
EPYCBENCH_Q4_ARTIFACT_ACCESSOR(current_q2_result_schema, 17)
#undef EPYCBENCH_Q4_ARTIFACT_ACCESSOR

#define EPYCBENCH_Q4_CANDIDATE_ACCESSOR(name, index)                                               \
    auto MonitoringQ4GovernedInputCandidate::name() const noexcept                                 \
        -> const MonitoringQ4GovernedArtifact& {                                                   \
        return *records_[index];                                                                   \
    }
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(profile, 0)
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(q1_plan, 1)
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(q2_plan, 2)
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(q2_plan_schema, 3)
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(q2_result_schema, 4)
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(q2_error_schema, 5)
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(q2_authored_schema, 6)
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(q3_plan, 7)
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(q3_plan_schema, 8)
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(q3_evidence_schema, 9)
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(q3_result_schema, 10)
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(q4_plan, 11)
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(q4_plan_schema, 12)
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(q4_evidence_schema, 13)
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(q4_result_schema, 14)
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(q4_error_schema, 15)
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(q4_authored_schema, 16)
EPYCBENCH_Q4_CANDIDATE_ACCESSOR(current_q2_result_schema, 17)
#undef EPYCBENCH_Q4_CANDIDATE_ACCESSOR

MonitoringQ4GovernedInputCandidate::MonitoringQ4GovernedInputCandidate(
    MonitoringQ4GovernedInputCandidate&& other) noexcept
    : records_{std::move(other.records_)}, head_{std::move(other.head_)},
      binding_{std::move(other.binding_)}, valid_{std::exchange(other.valid_, false)} {}

MonitoringQ4GovernedInputAuthority::MonitoringQ4GovernedInputAuthority(
    MonitoringQ4GovernedInputAuthority&& other) noexcept
    : core_{std::move(other.core_)}, consumed_{std::exchange(other.consumed_, true)},
      candidate_prepared_{std::exchange(other.candidate_prepared_, false)} {
    other.core_.reset();
}

auto MonitoringQ4GovernedInputAuthority::authenticate(
    int root, MonitoringCurrentRevisionTrackedCapture&& capture)
    -> std::expected<MonitoringQ4GovernedInputAuthority, MonitoringQ4GovernedInputFailure> {
    return authenticate_with(
        root, std::move(capture),
        {nullptr, open_at, stat_fd, stat_at, read_at, close_fd, effective_user, no_fault});
}
auto MonitoringQ4GovernedInputAuthority::authenticate_with(
    int root, MonitoringCurrentRevisionTrackedCapture&& capture, Operations operations)
    -> std::expected<MonitoringQ4GovernedInputAuthority, MonitoringQ4GovernedInputFailure> {
    try {
        auto retained =
            MonitoringGovernedTrackedBundleCore::retain(root, std::move(capture), operations);
        if (!retained)
            return std::unexpected{
                projected(MonitoringQ4GovernedInputFailureStage::authentication, retained.error())};
        return MonitoringQ4GovernedInputAuthority{std::move(*retained)};
    } catch (...) {
        return std::unexpected{MonitoringQ4GovernedInputFailure{
            .stage = MonitoringQ4GovernedInputFailureStage::contained,
            .kind = MonitoringGovernedTrackedBundleFailureKind::contained_failure}};
    }
}
auto MonitoringQ4GovernedInputAuthority::freeze()
    -> std::expected<MonitoringQ4GovernedInputValue, MonitoringQ4GovernedInputFailure> {
    auto candidate = prepare_candidate();
    if (!candidate)
        return std::unexpected{candidate.error()};
    return finalize(std::move(*candidate));
}
auto MonitoringQ4GovernedInputAuthority::prepare_candidate()
    -> std::expected<MonitoringQ4GovernedInputCandidate, MonitoringQ4GovernedInputFailure> {
    if (consumed_ || candidate_prepared_ || !core_)
        return std::unexpected{MonitoringQ4GovernedInputFailure{
            .stage = MonitoringQ4GovernedInputFailureStage::contained,
            .kind = MonitoringGovernedTrackedBundleFailureKind::inactive}};
    try {
        MonitoringQ4GovernedArtifactSlots records;
        for (std::size_t slot{}; slot < MonitoringGovernedTrackedBundleCore::leaf_count; ++slot) {
            core_->operations_.fault(core_->operations_.context,
                                     static_cast<unsigned int>(80U + (slot * 2U)));
            std::string bytes = core_->bytes_[slot];
            core_->operations_.fault(core_->operations_.context,
                                     static_cast<unsigned int>(81U + (slot * 2U)));
            records[slot].emplace(
                MonitoringQ4GovernedArtifact{std::move(bytes), core_->digests_[slot]});
        }
        core_->operations_.fault(core_->operations_.context, 116U);
        std::string head = core_->head_;
        core_->operations_.fault(core_->operations_.context, 117U);
        MonitoringQ4GovernedInputCandidate candidate{std::move(records), std::move(head),
                                                     core_->capture_binding_};
        candidate_prepared_ = true;
        return candidate;
    } catch (...) {
        consumed_ = true;
        const auto retired = core_->retire();
        const auto outcome = retired ? core_->retirement_outcome() : retired.error().retirement;
        core_.reset();
        return std::unexpected{MonitoringQ4GovernedInputFailure{
            .stage = MonitoringQ4GovernedInputFailureStage::contained,
            .kind = MonitoringGovernedTrackedBundleFailureKind::contained_failure,
            .retirement = outcome}};
    }
}
auto MonitoringQ4GovernedInputAuthority::finalize(
    MonitoringQ4GovernedInputCandidate&& candidate) noexcept
    -> std::expected<MonitoringQ4GovernedInputValue, MonitoringQ4GovernedInputFailure> {
    if (consumed_ || !candidate_prepared_ || !candidate.valid_ || !core_ ||
        candidate.binding_ != core_->capture_binding_)
        return std::unexpected{MonitoringQ4GovernedInputFailure{
            .stage = MonitoringQ4GovernedInputFailureStage::contained,
            .kind = MonitoringGovernedTrackedBundleFailureKind::inactive}};
    consumed_ = true;
    candidate_prepared_ = false;
    candidate.valid_ = false;
    candidate.binding_.reset();
    try {
        const auto checked = core_->recheck();
        if (!checked || !*checked) {
            MonitoringQ4GovernedInputFailure result{
                .stage = MonitoringQ4GovernedInputFailureStage::recheck,
                .kind = checked ? MonitoringGovernedTrackedBundleFailureKind::mutation
                                : checked.error().kind,
                .native_error = checked ? 0 : checked.error().native_error};
            const auto retired = core_->retire();
            result.retirement = retired ? core_->retirement_outcome() : retired.error().retirement;
            core_.reset();
            return std::unexpected{std::move(result)};
        }
        const auto retired = core_->retire();
        const auto outcome = retired ? core_->retirement_outcome() : retired.error().retirement;
        core_.reset();
        if (!retired || !outcome.confirmed.all() || outcome.uncertain.any() || outcome.active.any())
            return std::unexpected{MonitoringQ4GovernedInputFailure{
                .stage = MonitoringQ4GovernedInputFailureStage::retirement,
                .kind = retired ? MonitoringGovernedTrackedBundleFailureKind::cleanup_failure
                                : retired.error().kind,
                .native_error = retired ? 0 : retired.error().native_error,
                .retirement = outcome}};
        return MonitoringQ4GovernedInputValue{std::move(candidate.records_),
                                              std::move(candidate.head_)};
    } catch (...) {
        const auto retired = core_->retire();
        const auto outcome = retired ? core_->retirement_outcome() : retired.error().retirement;
        core_.reset();
        return std::unexpected{MonitoringQ4GovernedInputFailure{
            .stage = MonitoringQ4GovernedInputFailureStage::contained,
            .kind = MonitoringGovernedTrackedBundleFailureKind::contained_failure,
            .retirement = outcome}};
    }
}

} // namespace epycbench::monitoring::qualification::detail
