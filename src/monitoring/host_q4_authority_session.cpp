#include "monitoring/host_q4_authority_session.hpp"
#include "monitoring/host_q1_validated_public_values.hpp"
#include "monitoring/host_q4_cpufreq_evidence_json.hpp"

#include "monitoring/current_revision_tracked_capture.hpp"
#include "monitoring/host_q4_authority_failure_projection.hpp"

#include <type_traits>
#include <new>

namespace epycbench::monitoring::qualification::detail {
namespace {
thread_local bool bootstrap_fault{};
thread_local void (*source_provenance_checkpoint)(unsigned char) noexcept{};
auto fixed(MonitoringQ4AuthorityStage stage, MonitoringQ4CompositionSource source,
           MonitoringQ4CompositionFactKind kind, unsigned int detail = 0U, int native = 0) noexcept
    -> MonitoringQ4AuthorityFailure {
    return {.stage = stage, .primary = {source, kind, detail, native}};
}
} // namespace

void MonitoringQ4AuthoritySession::set_bootstrap_fault(bool value) noexcept {
    bootstrap_fault = value;
}
void MonitoringQ4AuthoritySession::set_source_provenance_checkpoint(
    void (*checkpoint)(unsigned char) noexcept) noexcept {
    source_provenance_checkpoint = checkpoint;
}

auto MonitoringQ4AuthoritySession::rollback_owner(MonitoringQ4CompositionOwner& owner,
                                                  MonitoringQ4AuthorityFailure value) noexcept
    -> MonitoringQ4AuthorityFailure {
    auto failed = owner.rollback(value.primary);
    value.cleanup = std::move(failed.cleanup);
    return project_retired(std::move(value));
}
auto MonitoringQ4AuthoritySession::project_retired(MonitoringQ4AuthorityFailure value) noexcept
    -> MonitoringQ4AuthorityFailure {
    value.projected.emplace(MonitoringQ4AuthorityFailureProjector::project(value));
    return value;
}

MonitoringQ4KernelMaximumReady::MonitoringQ4KernelMaximumReady(
    MonitoringQ4GovernedPlanSchemaBinding governed, MonitoringQ4HistoricalGitValue history,
    MonitoringQ4AuthenticatedHostProjection projection, std::string qid, std::string head,
    core::Sha256Digest build_identity, std::string build_attestation,
    MonitoringQ4EvidenceProvenance evidence_provenance,
    std::shared_ptr<const void> kernel_maximum_binding) noexcept
    : governed_{std::move(governed)}, history_{std::move(history)},
      projection_{std::move(projection)}, qid_{std::move(qid)}, head_{std::move(head)},
      build_identity_{build_identity}, build_attestation_{std::move(build_attestation)},
      evidence_provenance_{std::move(evidence_provenance)},
      kernel_maximum_binding_{std::move(kernel_maximum_binding)} {}

MonitoringQ4AuthoritySession::MonitoringQ4AuthoritySession(
    MonitoringQ4CompositionOwner owner, MonitoringQ4HistoricalGitValue history,
    MonitoringQ4GovernedPlanSchemaBinding binding,
    std::shared_ptr<const void> kernel_maximum_binding) noexcept
    : owner_{std::move(owner)}, history_{std::move(history)}, binding_{std::move(binding)},
      kernel_maximum_binding_{std::move(kernel_maximum_binding)} {}

auto MonitoringQ4AuthoritySession::acquire_body(AcquisitionOperations operations) noexcept
    -> std::expected<void, MonitoringQ4AuthorityFailure> {
    for (std::size_t stage{}; stage < 7U; ++stage) {
        try {
            auto result = operations.step(operations.context, stage);
            if (!result)
                return std::unexpected{
                    operations.rollback(operations.context, std::move(result.error()))};
        } catch (...) {
            return std::unexpected{operations.rollback(
                operations.context, fixed(MonitoringQ4AuthorityStage::contained,
                                          MonitoringQ4CompositionSource::contained,
                                          MonitoringQ4CompositionFactKind::contained))};
        }
    }
    return {};
}

auto MonitoringQ4AuthoritySession::finalize_body(FinalizeOperations operations) noexcept
    -> std::expected<void, MonitoringQ4AuthorityFailure> {
    std::optional<MonitoringQ4AuthorityFailure> rejected;
    for (std::size_t stage{}; stage < 4U && !rejected; ++stage) {
        try {
            auto checked = operations.check(operations.context, stage);
            if (!checked)
                rejected.emplace(std::move(checked.error()));
            else if (!*checked)
                rejected.emplace(fixed(MonitoringQ4AuthorityStage::final_recheck,
                                       static_cast<MonitoringQ4CompositionSource>(stage),
                                       MonitoringQ4CompositionFactKind::changed));
        } catch (...) {
            rejected.emplace(fixed(MonitoringQ4AuthorityStage::contained,
                                   MonitoringQ4CompositionSource::contained,
                                   MonitoringQ4CompositionFactKind::contained));
        }
    }
    auto retirement = operations.retire(operations.context);
    if (rejected) {
        rejected->cleanup = std::move(retirement);
        return std::unexpected{std::move(*rejected)};
    }
    if (retirement.count != 0U || retirement.overflowed)
        return std::unexpected{MonitoringQ4AuthorityFailure{
            MonitoringQ4AuthorityStage::retirement,
            {MonitoringQ4CompositionSource::contained, MonitoringQ4CompositionFactKind::primary},
            std::move(retirement)}};
    return {};
}

auto MonitoringQ4AuthoritySession::authenticate(const std::filesystem::path& root,
 std::string_view qid) noexcept -> std::expected<MonitoringQ4AuthoritySession, MonitoringQ4AuthorityFailure> {
 return authenticate_impl(root, qid, false);
}
auto MonitoringQ4AuthoritySession::authenticate_completed(const std::filesystem::path& root,
 std::string_view qid, const HostFrozenSourceLaunchBinding* source) noexcept
 -> std::expected<MonitoringQ4AuthoritySession, MonitoringQ4AuthorityFailure> {
 if ((qid != "MON-007.host-v2.campaign-20261005-01" && qid != "MON-007.host-v2.campaign-20261018-01" && qid != "MON-007.host-v2.campaign-20261020-01" && qid != "MON-007.host-v2.campaign-20261024-01" && qid != "MON-007.host-v2.campaign-20261102-01"))
  return std::unexpected{project_retired(fixed(MonitoringQ4AuthorityStage::repository,
   MonitoringQ4CompositionSource::repository, MonitoringQ4CompositionFactKind::inactive))};
 return authenticate_impl(root, qid, true, nullptr, source);
}
auto MonitoringQ4AuthoritySession::authenticate_impl(const std::filesystem::path& root,
 std::string_view qid, bool completed,
 const MonitoringRunningBuildAuthority::Operations* controlled_build,
 const HostFrozenSourceLaunchBinding* source) noexcept
 -> std::expected<MonitoringQ4AuthoritySession, MonitoringQ4AuthorityFailure> {
    if (bootstrap_fault)
        return std::unexpected{project_retired(fixed(MonitoringQ4AuthorityStage::contained,
                                                     MonitoringQ4CompositionSource::contained,
                                                     MonitoringQ4CompositionFactKind::allocation))};
    auto created = MonitoringQ4CompositionOwner::create();
    if (!created)
        return std::unexpected{project_retired(MonitoringQ4AuthorityFailure{
            MonitoringQ4AuthorityStage::contained, created.error().primary,
            std::move(created.error().cleanup)})};
    struct State final {
        const std::filesystem::path* root;
        std::string_view qid;
        MonitoringQ4CompositionOwner* owner;
        bool completed;
        const MonitoringRunningBuildAuthority::Operations* controlled_build;
        const HostFrozenSourceLaunchBinding* source;
        std::optional<MonitoringCurrentRevisionTrackedCapture> capture;
        std::optional<MonitoringQ4HistoricalGitValue> history;
        std::optional<MonitoringQ4GovernedPlanSchemaBinding> binding;
    } state{&root, qid, &*created, completed, controlled_build, source, {}, {}, {}};
    auto stepped = acquire_body(
        {&state,
         [](void* raw, std::size_t stage) -> std::expected<void, MonitoringQ4AuthorityFailure> {
             auto& s = *static_cast<State*>(raw);
             auto& owner = *s.owner;
             if (stage == 0U) {
                 auto value =
                     MonitoringQ1PlanV2AuthoritySession::authenticate_shared(*s.root, s.qid, true, s.source);
                 if (!value) {
                     auto rejected = fixed(MonitoringQ4AuthorityStage::repository,
                                           MonitoringQ4CompositionSource::repository,
                                           MonitoringQ4CompositionFactKind::primary);
                     rejected.detail = std::move(value.error());
                     return std::unexpected{std::move(rejected)};
                 }
                 owner.adopt_repository(std::move(*value));
                 return {};
             }
             auto& repository = owner.repository();
             if (stage == 1U) {
                 auto value = MonitoringQ4CurrentRevisionTrackedCapture::run(
                     repository.q4_executable(), repository.q4_root(), repository.q4_metadata());
                 if (!value) {
                     auto rejected = fixed(MonitoringQ4AuthorityStage::current_revision,
                                           MonitoringQ4CompositionSource::repository,
                                           MonitoringQ4CompositionFactKind::primary);
                     rejected.detail = std::move(value.error());
                     return std::unexpected{std::move(rejected)};
                 }
                 s.capture.emplace(std::move(*value));
                 return {};
             }
             if (stage == 2U) {
                 auto authority = MonitoringQ4GovernedInputAuthority::authenticate(
                     repository.q4_root().descriptor(), std::move(*s.capture));
                 if (!authority) {
                     auto rejected = fixed(MonitoringQ4AuthorityStage::governed_input,
                                           MonitoringQ4CompositionSource::governed,
                                           MonitoringQ4CompositionFactKind::primary,
                                           static_cast<unsigned int>(authority.error().kind),
                                           authority.error().native_error);
                     rejected.detail = authority.error();
                     return std::unexpected{std::move(rejected)};
                 }
                 auto candidate = authority->prepare_candidate();
                 if (!candidate) {
                     auto rejected = fixed(MonitoringQ4AuthorityStage::governed_input,
                                           MonitoringQ4CompositionSource::governed,
                                           MonitoringQ4CompositionFactKind::primary,
                                           static_cast<unsigned int>(candidate.error().kind),
                                           candidate.error().native_error);
                     rejected.detail = candidate.error();
                     return std::unexpected{std::move(rejected)};
                 }
                 owner.adopt_governed(std::move(*authority), std::move(*candidate));
                 return {};
             }
             const bool campaign = (repository.q4_values().qualification_id() == "MON-007.host-v2.campaign-20261005-01" ||
                 repository.q4_values().qualification_id() == "MON-007.host-v2.campaign-20261018-01" || repository.q4_values().qualification_id() == "MON-007.host-v2.campaign-20261020-01" || repository.q4_values().qualification_id() == "MON-007.host-v2.campaign-20261024-01" || repository.q4_values().qualification_id() == "MON-007.host-v2.campaign-20261102-01");
             if (stage == 3U) {
                 if (campaign) {
                     auto captured = s.completed
                         ? MonitoringQ4CompleteQ3PredecessorAuthority::capture_completed(
                             repository.q4_root(), owner.candidate(), repository.q4_values())
                         : MonitoringQ4CompleteQ3PredecessorAuthority::capture_campaign(
                             repository.q4_root(), owner.candidate(), repository.q4_values());
                     if (!captured) {
                         auto rejected = fixed(MonitoringQ4AuthorityStage::predecessor,
                             MonitoringQ4CompositionSource::predecessor,
                             MonitoringQ4CompositionFactKind::primary,
                             static_cast<unsigned int>(captured.error().primary), captured.error().native_error);
                         rejected.detail = captured.error();
                         return std::unexpected{std::move(rejected)};
                     }
                     // Adopt before any historical Git operation can fail.
                     owner.adopt_predecessor(std::move(*captured));
                 }
                 auto value = MonitoringQ4HistoricalGitAuthority::authenticate(
                     repository.q4_executable(), repository.q4_root(), repository.q4_metadata(),
                     owner.candidate(), campaign ? &owner.predecessor() : nullptr);
                 if (!value) {
                     auto rejected = fixed(MonitoringQ4AuthorityStage::history,
                                           MonitoringQ4CompositionSource::repository,
                                           MonitoringQ4CompositionFactKind::primary);
                     rejected.detail = std::move(value.error());
                     return std::unexpected{std::move(rejected)};
                 }
                 s.history.emplace(std::move(*value));
                 return {};
             }
             if (stage == 4U) {
                 if (campaign) {
                     auto bound = owner.predecessor().bind_campaign(*s.history);
                     if (!bound) {
                         auto rejected = fixed(MonitoringQ4AuthorityStage::predecessor,
                             MonitoringQ4CompositionSource::predecessor,
                             MonitoringQ4CompositionFactKind::primary,
                             static_cast<unsigned int>(bound.error().primary), bound.error().native_error);
                         rejected.detail = bound.error();
                         return std::unexpected{std::move(rejected)};
                     }
                     return {};
                 }
                 auto value = MonitoringQ4CompleteQ3PredecessorAuthority::retain(
                     repository.q4_root(), owner.candidate(), *s.history);
                 if (!value) {
                     auto rejected = fixed(MonitoringQ4AuthorityStage::predecessor,
                                           MonitoringQ4CompositionSource::predecessor,
                                           MonitoringQ4CompositionFactKind::primary,
                                           static_cast<unsigned int>(value.error().primary),
                                           value.error().native_error);
                     rejected.detail = value.error();
                     return std::unexpected{std::move(rejected)};
                 }
                 owner.adopt_predecessor(std::move(*value));
                 return {};
             }
             if (stage == 5U) {
                 const auto token =
                     MonitoringQ4GovernedPlanSchemaBinding::candidate_binding(owner.candidate());
                 auto value = MonitoringQ4GovernedPlanSchemaBinding::bind(owner.candidate(), token);
                 if (!value) {
                     auto rejected = fixed(MonitoringQ4AuthorityStage::plan_schema,
                                           MonitoringQ4CompositionSource::governed,
                                           MonitoringQ4CompositionFactKind::primary);
                     rejected.detail = std::move(value.error());
                     return std::unexpected{std::move(rejected)};
                 }
                 s.binding.emplace(std::move(*value));
                 return {};
             }
             auto value = s.controlled_build
                 ? MonitoringRunningBuildAuthority::retain_for_composition(
                     owner.candidate().captured_head(), *s.controlled_build)
                 : MonitoringRunningBuildAuthority::retain(owner.candidate().captured_head());
             if (!value) {
                 auto rejected = fixed(MonitoringQ4AuthorityStage::running_build,
                                       MonitoringQ4CompositionSource::build,
                                       MonitoringQ4CompositionFactKind::primary);
                 rejected.detail = std::move(value.error());
                 return std::unexpected{std::move(rejected)};
             }
             owner.adopt_build(std::move(*value));
             return {};
         },
         [](void* raw, MonitoringQ4AuthorityFailure value) noexcept {
             auto& state_value = *static_cast<State*>(raw);
             return MonitoringQ4AuthoritySession::rollback_owner(*state_value.owner,
                                                                 std::move(value));
         }});
    if (!stepped)
        return std::unexpected{std::move(stepped.error())};
    try {
        auto kernel_maximum_binding = std::make_shared<unsigned char>(0);
        MonitoringQ4AuthoritySession result{std::move(*state.owner), std::move(*state.history),
                                            std::move(*state.binding),
                                            std::move(kernel_maximum_binding)};
        result.completed_mode_ = completed;
        return result;
    } catch (...) {
        return std::unexpected{
            rollback_owner(*state.owner, fixed(MonitoringQ4AuthorityStage::contained,
                                               MonitoringQ4CompositionSource::contained,
                                               MonitoringQ4CompositionFactKind::allocation))};
    }
}

auto MonitoringQ4AuthoritySession::completed_predecessor_for_test() noexcept
    -> MonitoringQ4CompleteQ3PredecessorAuthority* {
    if (!completed_mode_ || consumed_ || !owner_) return nullptr;
    return &owner_->predecessor();
}

auto MonitoringQ4AuthoritySession::finalize_completed(
    std::optional<MonitoringQ1ValidatedPublicValues>& hidden_values,
    std::optional<MonitoringQ4RetainedEvidence>& hidden_evidence,
    std::optional<std::array<std::string, 7>>& hidden_predecessor,
    std::optional<MonitoringQ4GovernedInputValue>& hidden_governed,
    std::optional<MonitoringQ4CompletedSourceProvenance>* hidden_source_provenance,
    std::optional<MonitoringQ4HistoricalGitValue>* hidden_history) noexcept
    -> std::expected<void, MonitoringQ4AuthorityFailure> {
    if (consumed_ || !completed_mode_ || !owner_ || !history_ || !binding_ ||
        hidden_values || hidden_evidence || hidden_predecessor || hidden_governed ||
        (hidden_source_provenance && *hidden_source_provenance) ||
        (hidden_history && *hidden_history))
        return std::unexpected{project_retired(fixed(MonitoringQ4AuthorityStage::contained,
            MonitoringQ4CompositionSource::contained, MonitoringQ4CompositionFactKind::inactive))};
    consumed_ = true;
    auto& owner = *owner_;
    try {
        const auto token = MonitoringQ4GovernedPlanSchemaBinding::candidate_binding(owner.candidate());
        auto& snapshot = owner.predecessor();
        if (!snapshot.completed_snapshot_ || !snapshot.value_ || !snapshot.binding_completed_ ||
            !binding_->bound_to(token) || binding_->captured_head() != owner.candidate().captured_head() ||
            history_->current_commit() != owner.candidate().captured_head() ||
            history_->completed_q4_producer_commit() != snapshot.producer_commit_)
            return std::unexpected{rollback_owner(owner, fixed(MonitoringQ4AuthorityStage::final_recheck,
                MonitoringQ4CompositionSource::predecessor, MonitoringQ4CompositionFactKind::changed))};
        auto evidence = MonitoringQ4RetainedEvidence::verify(*snapshot.value_, *history_,
            *binding_, owner.candidate(), snapshot.q4_bytes_);
        if (!evidence) {
            auto error = fixed(MonitoringQ4AuthorityStage::predecessor,
                MonitoringQ4CompositionSource::predecessor, MonitoringQ4CompositionFactKind::primary);
            error.detail = std::move(evidence.error());
            return std::unexpected{rollback_owner(owner, std::move(error))};
        }
        // Rechecks precede the hidden move. The move is not a publication:
        // the caller keeps both facts inaccessible until retirement is confirmed.
        for (std::size_t stage{}; stage < 4U; ++stage) {
            std::expected<bool, MonitoringQ4AuthorityFailure> checked = [&]()
                -> std::expected<bool, MonitoringQ4AuthorityFailure> {
                if (stage == 0U) {
                    auto value = owner.repository().q3_recheck();
                    if (value) return *value;
                    for (const auto& cleanup : value.error().cleanup)
                        owner.seed_cleanup({MonitoringQ4CompositionSource::repository,
                            MonitoringQ4CompositionFactKind::cleanup, 0U,
                            cleanup.os_error ? cleanup.os_error->value : 0});
                    auto rejected = fixed(MonitoringQ4AuthorityStage::final_recheck,
                        MonitoringQ4CompositionSource::repository, MonitoringQ4CompositionFactKind::primary,
                        0U, value.error().primary.os_error ? value.error().primary.os_error->value : 0);
                    rejected.detail = std::move(value.error());
                    return std::unexpected{std::move(rejected)};
                }
                if (stage == 1U) {
                    auto value = owner.governed().recheck();
                    if (value) return *value;
                    for (std::size_t i{}; i < value.error().retirement.cleanup_count; ++i)
                        owner.seed_cleanup({MonitoringQ4CompositionSource::governed,
                            MonitoringQ4CompositionFactKind::cleanup,
                            value.error().retirement.cleanup_roles[i],
                            value.error().retirement.cleanup_errors[i]});
                    if (value.error().retirement.cleanup_overflowed) owner.note_cleanup_overflow();
                    auto rejected = fixed(MonitoringQ4AuthorityStage::final_recheck,
                        MonitoringQ4CompositionSource::governed, MonitoringQ4CompositionFactKind::primary,
                        static_cast<unsigned int>(value.error().kind), value.error().native_error);
                    rejected.detail = MonitoringQ4GovernedInputFailure{
                        MonitoringQ4GovernedInputFailureStage::recheck, value.error().kind,
                        value.error().native_error, std::move(value.error().retirement)};
                    return std::unexpected{std::move(rejected)};
                }
                if (stage == 2U) {
                    auto value = owner.predecessor().recheck();
                    if (value) return *value;
                    owner.append_predecessor_retirement(value.error().retirement);
                    auto rejected = fixed(MonitoringQ4AuthorityStage::final_recheck,
                        MonitoringQ4CompositionSource::predecessor, MonitoringQ4CompositionFactKind::primary,
                        static_cast<unsigned int>(value.error().primary), value.error().native_error);
                    rejected.detail = std::move(value.error());
                    return std::unexpected{std::move(rejected)};
                }
                auto value = owner.build().recheck();
                if (value) return *value;
                auto rejected = fixed(MonitoringQ4AuthorityStage::final_recheck,
                    MonitoringQ4CompositionSource::build, MonitoringQ4CompositionFactKind::primary,
                    0U, value.error().os_error ? value.error().os_error->value : 0);
                rejected.detail = std::move(value.error());
                return std::unexpected{std::move(rejected)};
            }();
            if (!checked) return std::unexpected{rollback_owner(owner, std::move(checked.error()))};
            if (!*checked) return std::unexpected{rollback_owner(owner,
                fixed(MonitoringQ4AuthorityStage::final_recheck,
                    static_cast<MonitoringQ4CompositionSource>(stage), MonitoringQ4CompositionFactKind::changed))};
        }
        // These copies are fallible while every genuine owner still lives.
        // Register the fully constructed output with a nonallocating move;
        // keep it hidden if any later consuming retirement fails.
        if (hidden_source_provenance) {
            const auto& root = owner.repository().q4_root();
            const auto& executable = owner.repository().q4_executable();
            auto& build = owner.build();
            try {
                if (source_provenance_checkpoint) source_provenance_checkpoint(0U);
                auto path = root.accepted_path();
                if (source_provenance_checkpoint) source_provenance_checkpoint(1U);
                auto bytes = std::string{build.canonical()};
                MonitoringQ4CompletedSourceProvenance provenance{
                    std::move(path), root.identity(), executable.held_identity(),
                    build.identity(), std::move(bytes)};
                static_assert(std::is_nothrow_move_constructible_v<MonitoringQ4CompletedSourceProvenance>);
                static_assert(noexcept(hidden_source_provenance->emplace(std::move(provenance))));
                hidden_source_provenance->emplace(std::move(provenance));
            } catch (const std::bad_alloc&) {
                return std::unexpected{rollback_owner(owner, fixed(MonitoringQ4AuthorityStage::contained,
                    MonitoringQ4CompositionSource::contained, MonitoringQ4CompositionFactKind::allocation))};
            }
        }
        if (!owner.repository().take_completed_values(hidden_values))
            return std::unexpected{rollback_owner(owner, fixed(MonitoringQ4AuthorityStage::final_recheck,
                MonitoringQ4CompositionSource::repository, MonitoringQ4CompositionFactKind::inactive))};
        hidden_evidence.emplace(std::move(*evidence));
        // The existing retirement path never rereads these strings. Transfer
        // the actual held bytes without copying or exposing them before cleanup.
        static_assert(std::is_nothrow_move_constructible_v<std::array<std::string, 7>>);
        static_assert(std::is_nothrow_move_constructible_v<MonitoringQ4RetainedEvidence>);
        static_assert(std::is_nothrow_move_constructible_v<MonitoringQ4GovernedInputValue>);
        static_assert(noexcept(hidden_evidence.emplace(std::move(*evidence))));
        static_assert(noexcept(hidden_predecessor.emplace(std::move(snapshot.bytes_))));
        hidden_predecessor.emplace(std::move(snapshot.bytes_));
        // Preserve the actual accepted semantic/history owner, including its
        // private candidate association. Copying commit strings or parsing the
        // Q4 artifact later cannot recreate this proof. Keep the moved value
        // hidden together with all other inputs until retirement succeeds.
        if (hidden_history) {
            static_assert(std::is_nothrow_move_constructible_v<MonitoringQ4HistoricalGitValue>);
            static_assert(noexcept(hidden_history->emplace(std::move(*history_))));
            hidden_history->emplace(std::move(*history_));
            history_.reset();
        }
        auto retirement = owner.retire(&hidden_governed);
        if (retirement.count != 0U || retirement.overflowed)
            return std::unexpected{project_retired(MonitoringQ4AuthorityFailure{
                MonitoringQ4AuthorityStage::retirement,
                {MonitoringQ4CompositionSource::contained, MonitoringQ4CompositionFactKind::primary},
                std::move(retirement)})};
        if (!hidden_values || !hidden_evidence || !hidden_predecessor || !hidden_governed ||
            (hidden_source_provenance && !*hidden_source_provenance) ||
            (hidden_history && !*hidden_history))
            return std::unexpected{project_retired(fixed(MonitoringQ4AuthorityStage::contained,
                MonitoringQ4CompositionSource::contained, MonitoringQ4CompositionFactKind::inactive))};
        owner_.reset();
        return {};
    } catch (...) {
        return std::unexpected{rollback_owner(owner, fixed(MonitoringQ4AuthorityStage::contained,
            MonitoringQ4CompositionSource::contained, MonitoringQ4CompositionFactKind::contained))};
    }
}

auto MonitoringQ4AuthoritySession::finalize_before_kernel_max() noexcept
    -> std::expected<MonitoringQ4KernelMaximumReady, MonitoringQ4AuthorityFailure> {
    if (consumed_ || completed_mode_ || !owner_ || !history_ || !binding_) {
        auto rejected =
            fixed(MonitoringQ4AuthorityStage::contained, MonitoringQ4CompositionSource::contained,
                  MonitoringQ4CompositionFactKind::inactive);
        return std::unexpected{project_retired(std::move(rejected))};
    }
    consumed_ = true;
    auto& owner = *owner_;
    try {
        const auto& predecessor = *owner.predecessor().value_;
        const auto token =
            MonitoringQ4GovernedPlanSchemaBinding::candidate_binding(owner.candidate());
        if (!binding_->bound_to(token) ||
            binding_->captured_head() != owner.candidate().captured_head() ||
            history_->current_commit() != owner.candidate().captured_head() ||
            predecessor.commits().back() != owner.candidate().captured_head())
            return std::unexpected{
                rollback_owner(owner, fixed(MonitoringQ4AuthorityStage::final_recheck,
                                            MonitoringQ4CompositionSource::contained,
                                            MonitoringQ4CompositionFactKind::changed))};
        const auto& projection = predecessor.projection();
        MonitoringQ4KernelMaximumReady ready{std::move(*binding_),
                                             std::move(*history_),
                                             projection,
                                             std::string{predecessor.qualification_id()},
                                             std::string{owner.candidate().captured_head()},
                                             owner.build().identity(),
                                             std::string{owner.build().canonical()},
                                             monitoring_q4_capture_evidence_provenance(
                                                 owner.candidate().profile().sha256, predecessor),
                                             std::move(kernel_maximum_binding_)};

        FinalizeOperations operations{
            &owner,
            [](void* raw,
               std::size_t stage) noexcept -> std::expected<bool, MonitoringQ4AuthorityFailure> {
                auto& value = *static_cast<MonitoringQ4CompositionOwner*>(raw);
                if (stage == 0U) {
                    auto checked = value.repository().q3_recheck();
                    if (checked)
                        return *checked;
                    for (const auto& cleanup : checked.error().cleanup)
                        value.seed_cleanup({MonitoringQ4CompositionSource::repository,
                            MonitoringQ4CompositionFactKind::cleanup, 0U,
                            cleanup.os_error ? cleanup.os_error->value : 0});
                    auto rejected = fixed(MonitoringQ4AuthorityStage::final_recheck,
                        MonitoringQ4CompositionSource::repository, MonitoringQ4CompositionFactKind::primary,
                        0U, checked.error().primary.os_error ? checked.error().primary.os_error->value : 0);
                    rejected.detail = std::move(checked.error());
                    return std::unexpected{std::move(rejected)};
                }
                if (stage == 1U) {
                    auto checked = value.governed().recheck();
                    if (checked)
                        return *checked;
                    for (std::size_t i{}; i < checked.error().retirement.cleanup_count; ++i)
                        value.seed_cleanup({MonitoringQ4CompositionSource::governed,
                            MonitoringQ4CompositionFactKind::cleanup,
                            checked.error().retirement.cleanup_roles[i],
                            checked.error().retirement.cleanup_errors[i]});
                    if (checked.error().retirement.cleanup_overflowed) value.note_cleanup_overflow();
                    auto rejected = fixed(MonitoringQ4AuthorityStage::final_recheck,
                        MonitoringQ4CompositionSource::governed, MonitoringQ4CompositionFactKind::primary,
                        static_cast<unsigned int>(checked.error().kind), checked.error().native_error);
                    rejected.detail = MonitoringQ4GovernedInputFailure{
                        MonitoringQ4GovernedInputFailureStage::recheck, checked.error().kind,
                        checked.error().native_error, std::move(checked.error().retirement)};
                    return std::unexpected{std::move(rejected)};
                }
                if (stage == 2U) {
                    auto checked = value.predecessor().recheck();
                    if (checked)
                        return *checked;
                    value.append_predecessor_retirement(checked.error().retirement);
                    auto rejected = fixed(MonitoringQ4AuthorityStage::final_recheck,
                        MonitoringQ4CompositionSource::predecessor, MonitoringQ4CompositionFactKind::primary,
                        static_cast<unsigned int>(checked.error().primary), checked.error().native_error);
                    rejected.detail = std::move(checked.error());
                    return std::unexpected{std::move(rejected)};
                }
                auto checked = value.build().recheck();
                if (checked)
                    return *checked;
                auto rejected = fixed(MonitoringQ4AuthorityStage::final_recheck,
                    MonitoringQ4CompositionSource::build, MonitoringQ4CompositionFactKind::primary,
                    0U, checked.error().os_error ? checked.error().os_error->value : 0);
                rejected.detail = std::move(checked.error());
                return std::unexpected{std::move(rejected)};
            },
            [](void* raw) noexcept {
                return static_cast<MonitoringQ4CompositionOwner*>(raw)->retire();
            }};
        auto finalized = finalize_body(operations);
        owner_.reset();
        if (!finalized)
            return std::unexpected{project_retired(std::move(finalized.error()))};
        return ready;
    } catch (...) {
        return std::unexpected{
            rollback_owner(owner, fixed(MonitoringQ4AuthorityStage::contained,
                                        MonitoringQ4CompositionSource::contained,
                                        MonitoringQ4CompositionFactKind::contained))};
    }
}

} // namespace epycbench::monitoring::qualification::detail
