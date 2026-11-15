#include "monitoring/host_q1_plan_v2_authority_session.hpp"
#include "monitoring/host_frozen_source_launch.hpp"
#include "monitoring/host_q1_plan_v2_authority_session_test_access.hpp"

#include "core/governed_repository_root_authority_access.hpp"
#include "monitoring/host_q1_git_command_owner.hpp"
#include "monitoring/host_q1_git_fixed_results.hpp"
#include "monitoring/host_q1_git_repository_metadata.hpp"
#include "monitoring/host_q1_private_q0_value.hpp"
#include "monitoring/host_q1_public_input_authority.hpp"
#include "monitoring/host_q1_validated_public_values.hpp"
#include "monitoring/host_q2_authority_session.hpp"
#include "monitoring/host_q2_composition_owner.hpp"
#include <unistd.h>
#include "monitoring/host_q2_historical_q1_git_authority.hpp"
#include "monitoring/host_q2_plan_public_input_authority.hpp"
#include "monitoring/host_qualification_plan_access.hpp"
#include "monitoring/host_q3_authority_session.hpp"
#include "monitoring/host_q3_composition_owner.hpp"
#include "monitoring/host_q3_historical_git_authority.hpp"

#include <cstdlib>
#include <memory>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace {
thread_local void* boundary_context{};
thread_local MonitoringQ1PlanV2AuthoritySessionTestAccess::BoundaryHook boundary_hook{};
void observe(MonitoringQ1AuthorityBoundary boundary) {
    if (boundary_hook)
        boundary_hook(boundary_context, boundary);
}
auto failure(std::string_view code, std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Q1 plan authority rejected",
            .operation = "authenticate_monitoring_q1_plan",
            .os_error = std::nullopt,
            .context = {{"classification", std::string{classification}}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto changed() -> core::Error {
    return failure("monitoring.q1_plan_authority.authority_changed", "authority_changed");
}
auto changed_from(const core::Error& source) -> core::Error {
    auto result = changed();
    result.os_error = source.os_error;
    return result;
}
auto changed_from(MonitoringQ1GitCommandFailure source) -> MonitoringQ1GitCommandFailure {
    source.primary = changed_from(source.primary);
    return source;
}
auto cleanup() -> core::Error {
    return failure("monitoring.q1_plan_authority.cleanup_failed", "cleanup_failure");
}
auto contained(bool unknown) -> core::Error {
    return failure("monitoring.q1_plan_authority.contained_failure",
                   unknown ? "contained_unknown_exception" : "contained_standard_exception");
}
auto wrapped(core::Error value) -> MonitoringQ1GitCommandFailure { return {std::move(value), {}}; }
auto root_error(const core::Error& source, bool terminal) -> core::Error {
    auto result = terminal ? changed()
                           : failure("monitoring.q1_plan_authority.repository_rejected",
                                     "repository_authority");
    result.os_error = source.os_error;
    return result;
}
auto root_cleanup(const core::Error& source) -> core::Error {
    auto result = cleanup();
    result.os_error = source.os_error;
    return result;
}
auto root_failure(const core::Error& primary, std::span<const core::Error> cleanup_errors,
                  bool terminal) -> MonitoringQ1GitCommandFailure {
    MonitoringQ1GitCommandFailure result{root_error(primary, terminal), {}};
    for (const auto& error : cleanup_errors)
        result.cleanup.push_back(root_cleanup(error));
    return result;
}
auto held_root_checkpoint(const std::expected<bool, core::Error>& held, bool terminal)
    -> std::optional<MonitoringQ1GitCommandFailure> {
    if (!held)
        return root_failure(held.error(), {}, terminal);
    if (!*held)
        return wrapped(terminal ? changed()
                                : failure("monitoring.q1_plan_authority.repository_rejected",
                                          "repository_authority"));
    return std::nullopt;
}
auto pathname_root_checkpoint(const core::detail::GovernedRepositoryRootCheckResult& path,
                              bool terminal) -> std::optional<MonitoringQ1GitCommandFailure> {
    if (path.primary)
        return root_failure(*path.primary, path.cleanup, terminal);
    if (!path.matches || !*path.matches) {
        auto result = wrapped(terminal ? changed()
                                       : failure("monitoring.q1_plan_authority.repository_rejected",
                                                 "repository_authority"));
        for (const auto& error : path.cleanup)
            result.cleanup.push_back(root_cleanup(error));
        return result;
    }
    if (!path.cleanup.empty()) {
        MonitoringQ1GitCommandFailure result{cleanup(), {}};
        for (const auto& error : path.cleanup)
            result.cleanup.push_back(root_cleanup(error));
        return result;
    }
    return std::nullopt;
}
auto retire_replay(MonitoringQ1PublicInputAuthority& inputs,
                   MonitoringQ1GitCommandFailure primary) noexcept
    -> MonitoringQ1GitCommandFailure {
    return aggregate_monitoring_q1_replay_retirement(
        std::move(primary),
        {&inputs,
         [](void* raw) { return static_cast<MonitoringQ1PublicInputAuthority*>(raw)->retire(); }});
}
} // namespace

auto aggregate_monitoring_q1_replay_retirement(
    MonitoringQ1GitCommandFailure primary, MonitoringQ1ReplayRetirementOperation operation) noexcept
    -> MonitoringQ1GitCommandFailure {
    bool projection_failed = false;
    try {
        primary.cleanup.reserve(primary.cleanup.size() + 8U);
    } catch (...) {
        projection_failed = true;
    }
    try {
        if (operation.retire == nullptr)
            throw std::runtime_error{"missing replay retirement operation"};
        auto retirement = operation.retire(operation.context);
        if (!retirement) {
            try {
                primary.cleanup.push_back(std::move(retirement.error().primary));
                for (auto& error : retirement.error().cleanup)
                    primary.cleanup.push_back(std::move(error));
            } catch (...) {
                projection_failed = true;
            }
        }
    } catch (...) {
        projection_failed = true;
    }
    if (projection_failed)
        try {
            primary.cleanup.push_back(cleanup());
        } catch (...) {
        }
    return primary;
}

auto close_monitoring_q1_replay(MonitoringQ1ReplayRetirementOperation operation) noexcept
    -> MonitoringQ1ReplayCloseResult {
    MonitoringQ1GitCommandFailure failure{cleanup(), {}};
    try {
        failure.cleanup.reserve(8U);
        if (operation.retire == nullptr)
            throw std::runtime_error{"missing replay retirement operation"};
        auto retirement = operation.retire(operation.context);
        if (retirement)
            return {true, std::nullopt};
        failure.cleanup.push_back(std::move(retirement.error().primary));
        for (auto& error : retirement.error().cleanup)
            failure.cleanup.push_back(std::move(error));
    } catch (...) {
        // The fixed cleanup_failed primary is already fully formed and remains reportable.
    }
    return {false, std::move(failure)};
}

auto aggregate_monitoring_q1_authority_retirement(
    MonitoringQ1AuthorityRetirementOperations operations) noexcept
    -> MonitoringQ1AuthorityRetirementResult {
    MonitoringQ1AuthorityRetirementResult result;
    const auto retain = [&](core::Error error) noexcept {
        if (result.retirement.count < result.retirement.failures.size())
            result.retirement.failures[result.retirement.count++].emplace(std::move(error));
        else
            result.retirement.overflowed = true;
    };
    const auto append = [&](auto operation) noexcept {
        try {
            auto value = operation(operations.context);
            if (!value) {
                retain(std::move(value.error().primary));
                for (auto& error : value.error().cleanup)
                    retain(std::move(error));
            }
        } catch (...) {
            result.retirement.overflowed = true;
        }
    };
    if (!operations.q0 || !operations.inputs || !operations.metadata || !operations.executable ||
        !operations.root) {
        result.retirement.overflowed = true;
        return result;
    }
    append(operations.q0);
    append(operations.inputs);
    append(operations.metadata);
    append(operations.executable);
    append(operations.root);
    return result;
}

class MonitoringQ1PlanV2AuthoritySession::Impl final {
  public:
    Impl(core::detail::OwnedGovernedRepositoryRootDescriptor root,
         MonitoringQ1GitExecutable executable, MonitoringQ1GitRepositoryMetadata metadata,
         MonitoringQ1PublicInputAuthority inputs, MonitoringQ1ValidatedPublicValues values,
         std::optional<MonitoringQ1PrivateQ0Value> q0) noexcept
        : root_{std::move(root)}, executable_{std::move(executable)},
          metadata_{std::move(metadata)}, inputs_{std::move(inputs)}, values_{std::move(values)},
          q0_{std::move(q0)} {}
    core::detail::OwnedGovernedRepositoryRootDescriptor root_;
    MonitoringQ1GitExecutable executable_;
    MonitoringQ1GitRepositoryMetadata metadata_;
    MonitoringQ1PublicInputAuthority inputs_;
    MonitoringQ1ValidatedPublicValues values_;
    std::optional<MonitoringQ1PrivateQ0Value> q0_;

    auto retire() noexcept -> MonitoringQ1AuthorityRetirementResult {
        return aggregate_monitoring_q1_authority_retirement(
            {.context = this,
             .q0 = [](void* raw) { auto& q0=static_cast<Impl*>(raw)->q0_;return q0?q0->retire():std::expected<void,MonitoringQ1GitCommandFailure>{}; },
             .inputs = [](void* raw) { return static_cast<Impl*>(raw)->inputs_.retire(); },
             .metadata = [](void* raw) { return static_cast<Impl*>(raw)->metadata_.retire(); },
             .executable = [](void* raw) -> std::expected<void, MonitoringQ1GitCommandFailure> {
                 auto value = static_cast<Impl*>(raw)->executable_.retire();
                 if (!value)
                     return std::unexpected{wrapped(std::move(value.error()))};
                 return {};
             },
             .root = [](void* raw) -> std::expected<void, MonitoringQ1GitCommandFailure> {
                 auto value = static_cast<Impl*>(raw)->root_.retire();
                 if (!value)
                     return std::unexpected{wrapped(root_cleanup(value.error()))};
                 return {};
             }});
    }
};

auto MonitoringQ3AuthoritySession::authenticate(const std::filesystem::path& spelling,
                                                 std::string_view qualification_id) noexcept
    -> std::expected<MonitoringQ3AuthoritySession, MonitoringQ3AuthorityFailure> {
    struct State final {
        const std::filesystem::path* spelling;
        std::string_view qualification_id;
        MonitoringQ3CompositionOwner* owner;
        std::optional<MonitoringQ1GitFixedResults> git;
        std::optional<MonitoringQ3PlanValue> plan;
        std::string q2_plan;
        std::unique_ptr<HistoricalContext> historical;
        static void append(MonitoringQ1BoundedRetirement& out, core::Error value) noexcept {
            if(out.count<out.failures.size()) out.failures[out.count++].emplace(std::move(value));
            else out.overflowed=true;
        }
        static auto simple(core::Error error, MonitoringQ3AuthorityStage stage)
            -> std::expected<void,MonitoringQ3AuthorityFailure> {
            return std::unexpected{MonitoringQ3AuthorityFailure{stage,std::move(error),{}}};
        }
    };
    auto owner = MonitoringQ3CompositionOwner::create();
    if (!owner) return std::unexpected{typed_failure(std::move(owner.error()), MonitoringQ3AuthorityStage::repository)};
    State state{&spelling, qualification_id, &*owner, {}, {}, {}, {}};
    AcquisitionOperations operations{&state,
      [](void* raw) -> std::expected<void, MonitoringQ3AuthorityFailure> {
        auto& s=*static_cast<State*>(raw);
        auto value=MonitoringQ1PlanV2AuthoritySession::authenticate_shared(*s.spelling,s.qualification_id,true);
        if(!value) return std::unexpected{MonitoringQ3AuthoritySession::typed_failure(std::move(value.error()),MonitoringQ3AuthorityStage::repository)};
        s.owner->adopt_repository(std::move(*value)); return {};
      },
      [](void* raw) -> std::expected<void, MonitoringQ3AuthorityFailure> {
        auto& s=*static_cast<State*>(raw); auto& impl=*s.owner->repository().implementation_;
        auto value=MonitoringQ1GitFixedResultAuthority::run_with_q3_plan(impl.executable_,impl.root_,impl.metadata_);
        if(!value) return std::unexpected{MonitoringQ3AuthoritySession::typed_failure(std::move(value.error()),MonitoringQ3AuthorityStage::collection_plan)};
        if(value->records.size()!=5U) return State::simple(failure("monitoring.q3.authority.repository_invalid","fixed_results"),MonitoringQ3AuthorityStage::collection_plan);
        s.q2_plan=value->records[3].bytes_; s.git.emplace(std::move(*value)); return {};
      },
      [](void* raw) -> std::expected<void, MonitoringQ3AuthorityFailure> {
        auto& s=*static_cast<State*>(raw); auto& impl=*s.owner->repository().implementation_;
        auto value=MonitoringQ3PlanPublicInputAuthority::retain(impl.root_,*s.git);
        if(!value) return std::unexpected{MonitoringQ3AuthoritySession::typed_failure(std::move(value.error()),MonitoringQ3AuthorityStage::collection_plan)};
        s.owner->adopt_input(std::move(*value)); return {};
      },
      [](void* raw) -> std::expected<void, MonitoringQ3AuthorityFailure> {
        auto& s=*static_cast<State*>(raw); auto value=decode_monitoring_q3_plan(s.owner->input().bytes());
        if(!value) return State::simple(std::move(value.error()),MonitoringQ3AuthorityStage::collection_plan);
        s.plan.emplace(std::move(*value)); return {};
      },
      [](void* raw) -> std::expected<void, MonitoringQ3AuthorityFailure> {
        auto& s=*static_cast<State*>(raw); auto& impl=*s.owner->repository().implementation_;
        if(s.owner->input().captured_head()!=s.git->head() ||
           impl.inputs_.bytes(1)!=s.git->records[1].bytes_ ||
           impl.inputs_.bytes(2)!=s.git->records[2].bytes_ ||
           s.q2_plan!=s.git->records[3].bytes_ || s.owner->input().bytes()!=s.git->records[4].bytes_)
          return State::simple(failure("monitoring.q3.authority.repository_invalid","binding"),MonitoringQ3AuthorityStage::collection_plan);
        return {};
      },
      [](void* raw) -> std::expected<void, MonitoringQ3AuthorityFailure> {
        auto& s=*static_cast<State*>(raw); auto& impl=*s.owner->repository().implementation_;
        s.historical=std::make_unique<HistoricalContext>(); s.historical->repository=&impl;
        MonitoringQ3CompleteQ2PredecessorAuthenticator::Operations history{
            s.historical.get(), [](void* history_raw, std::string_view q0, std::string_view q1,
                std::string_view q2, std::string_view current, std::string_view profile,
                std::string_view q1_plan, std::string_view q2_plan_bytes,
                std::string_view q3_plan) {
                auto& context = *static_cast<HistoricalContext*>(history_raw);
                auto& held_repository = *static_cast<MonitoringQ1PlanV2AuthoritySession::Impl*>(
                    context.repository);
                return MonitoringQ3HistoricalGitAuthority::authenticate(held_repository.executable_,
                    held_repository.root_, held_repository.metadata_, q0, q1, q2, current, profile,
                    q1_plan, q2_plan_bytes, q3_plan);
            }};
        auto binding = MonitoringQ3CompleteQ2PredecessorBinding{s.qualification_id,
            s.owner->input().captured_head(), impl.inputs_.bytes(1), impl.inputs_.bytes(2), s.q2_plan,
            s.owner->input().bytes(), history,
            (s.qualification_id == "MON-007.host-v2.campaign-20261005-01" || s.qualification_id == "MON-007.host-v2.campaign-20261018-01" || s.qualification_id == "MON-007.host-v2.campaign-20261020-01" || s.qualification_id == "MON-007.host-v2.campaign-20261024-01" || s.qualification_id == "MON-007.host-v2.campaign-20261102-01") ? &impl.values_ : nullptr};
        auto predecessor = MonitoringQ3CompleteQ2PredecessorAuthority::retain(impl.root_, binding);
        if (!predecessor) {
            MonitoringQ3AuthorityFailure out{MonitoringQ3AuthorityStage::predecessor,std::move(predecessor.error().primary),{}};
            for(std::size_t i{};i<predecessor.error().cleanup_count;++i) State::append(out.cleanup,std::move(*predecessor.error().cleanup[i]));
            return std::unexpected{std::move(out)};
        }
        s.owner->adopt_predecessor(std::move(*predecessor)); return {};
      },
      [](void* raw) -> std::expected<void, MonitoringQ3AuthorityFailure> {
        auto& s=*static_cast<State*>(raw); auto value=MonitoringRunningBuildAuthority::retain(s.owner->input().captured_head());
        if(!value) return State::simple(std::move(value.error()),MonitoringQ3AuthorityStage::build_attestation);
        s.owner->adopt_build(std::move(*value)); return {};
      },
      [](void* raw, MonitoringQ3AuthorityFailure rejected) noexcept {
        auto& s=*static_cast<State*>(raw);
        for(std::size_t i{};i<rejected.cleanup.count;++i) s.owner->seed_cleanup(std::move(*rejected.cleanup.failures[i]));
        if(rejected.cleanup.overflowed) s.owner->note_cleanup_overflow();
        auto rolled=s.owner->rollback(std::move(rejected.primary));
        return MonitoringQ3AuthorityFailure{rejected.stage,std::move(rolled.primary),std::move(rolled.cleanup)};
      }};
    auto acquired=acquire_lifecycle(operations); if(!acquired) return std::unexpected{std::move(acquired.error())};
    auto& impl=*owner->repository().implementation_;
    auto profile_sha256=core::sha256(impl.inputs_.bytes(1)).to_hex();
    return MonitoringQ3AuthoritySession{owner->take_repository(),owner->take_input(),std::move(*state.plan),
      owner->take_predecessor(),owner->take_build(),std::move(state.historical),owner->take_cleanup(),
      std::move(profile_sha256),std::string{qualification_id}};
}

auto MonitoringQ2AuthoritySession::authenticate(const std::filesystem::path& spelling,
                                                 std::string_view qid) noexcept
    -> std::expected<MonitoringQ2AuthoritySession,MonitoringQ2AuthorityFailure> {
    return authenticate_with(spelling,qid,{});
}
auto MonitoringQ2AuthoritySession::authenticate_with(const std::filesystem::path& spelling,
                                                 std::string_view qid,
                                                 MonitoringRunningBuildAuthority::Operations build_operations) noexcept
    -> std::expected<MonitoringQ2AuthoritySession,MonitoringQ2AuthorityFailure> {
    const auto typed=[](MonitoringQ1GitCommandFailure value,MonitoringQ2AuthorityStage stage) noexcept {
      MonitoringQ2AuthorityFailure out{stage,std::move(value.primary)};
      for(auto& item:value.cleanup){if(out.cleanup_count<out.cleanup.size())out.cleanup[out.cleanup_count++].emplace(std::move(item));else out.cleanup_overflowed=true;}
      return out;
    };
    const auto typed_composition=[](MonitoringQ2CompositionFailure value,MonitoringQ2AuthorityStage stage) noexcept {
      MonitoringQ2AuthorityFailure out{stage,std::move(value.primary),std::move(value.cleanup.failures),value.cleanup.count,value.cleanup.overflowed};return out;
    };
    auto active_stage=MonitoringQ2AuthorityStage::repository;
    auto owner=MonitoringQ2CompositionOwner::create();if(!owner)return std::unexpected{typed(std::move(owner.error()),MonitoringQ2AuthorityStage::repository)};
    auto contained_error=contained(true);
    try {
      auto q1_adapter=prepare_monitoring_q2_q1_authority();
      auto q1=MonitoringQ1PlanV2AuthoritySession::authenticate_shared(spelling,qid,true);if(!q1)return std::unexpected{typed(std::move(q1.error()),MonitoringQ2AuthorityStage::repository)};
      owner->adopt_q1(std::move(*q1));auto& q1_held=owner->q1();auto& impl=*q1_held.implementation_;
      active_stage=MonitoringQ2AuthorityStage::collection_plan;
      auto git=MonitoringQ1GitFixedResultAuthority::run_with_q2_plan(impl.executable_,impl.root_,impl.metadata_);
      if(!git)return std::unexpected{typed_composition(owner->rollback(std::move(git.error())),MonitoringQ2AuthorityStage::collection_plan)};
      auto input=MonitoringQ2PlanPublicInputAuthority::retain(impl.root_,*git);
      if(!input)return std::unexpected{typed_composition(owner->rollback(std::move(input.error())),MonitoringQ2AuthorityStage::collection_plan)};
      owner->adopt_input(std::move(*input));
      auto value=decode_monitoring_q2_plan(owner->input().bytes());
      if(!value)return std::unexpected{typed_composition(owner->rollback(std::move(value.error())),MonitoringQ2AuthorityStage::collection_plan)};
      active_stage=MonitoringQ2AuthorityStage::predecessor;
      auto historical=std::make_unique<HistoricalContext>();historical->q1_impl=&impl;
      historical->authenticate=[](void* raw,std::string_view old_commit,std::string_view current,
        std::string_view q0,std::string_view profile,std::string_view plan){auto& held=*static_cast<MonitoringQ1PlanV2AuthoritySession::Impl*>(raw);
        return MonitoringQ2HistoricalQ1GitAuthority::authenticate(held.executable_,held.root_,held.metadata_,old_commit,current,q0,profile,plan);};
      MonitoringQ2CompleteQ1PredecessorOperations operations{historical.get(),
        [](void* raw,std::string_view old_commit,std::string_view current,std::string_view q0,std::string_view profile,std::string_view plan){
          auto& context=*static_cast<HistoricalContext*>(raw);return context.authenticate(context.q1_impl,old_commit,current,q0,profile,plan);},
        [](void*) noexcept->std::expected<void,core::Error>{return {};}};
      auto binding=MonitoringQ2CompleteQ1PredecessorBinding{impl.values_,owner->input().captured_head(),
        impl.inputs_.bytes(1),impl.inputs_.bytes(2),operations};
      auto predecessor=MonitoringQ2CompleteQ1PredecessorAuthority::retain(impl.root_,binding);
      if(!predecessor){MonitoringQ1GitCommandFailure failure{std::move(predecessor.error().primary),{}};
        for(std::size_t i{};i<predecessor.error().cleanup_count;++i)failure.cleanup.push_back(std::move(*predecessor.error().cleanup[i]));
        return std::unexpected{typed_composition(owner->rollback(std::move(failure)),MonitoringQ2AuthorityStage::predecessor)};}
      owner->adopt_predecessor(std::move(*predecessor));
      active_stage=MonitoringQ2AuthorityStage::build_attestation;
      auto running_build=build_operations.manifest
        ? MonitoringRunningBuildAuthority::retain_for_composition(owner->input().captured_head(),build_operations)
        : MonitoringRunningBuildAuthority::retain(owner->input().captured_head());
      if(!running_build){MonitoringQ1GitCommandFailure failure{std::move(running_build.error()),{}};
        return std::unexpected{typed_composition(owner->rollback(std::move(failure.primary)),MonitoringQ2AuthorityStage::build_attestation)};}
      owner->adopt_build(std::move(*running_build));
      q1_adapter->adopt(owner->take_q1());
      return MonitoringQ2AuthoritySession{std::move(q1_adapter),owner->take_input(),std::move(*value),
        owner->take_predecessor(),owner->take_build(),std::move(historical),
        {nullptr,[](void*) noexcept -> long { return ::sysconf(_SC_PAGESIZE); }},owner->take_cleanup_ledger()};
    } catch (...) { return std::unexpected{typed_composition(owner->rollback(std::move(contained_error)),active_stage)}; }
}

MonitoringQ1CollectorPlan::MonitoringQ1CollectorPlan(
    std::string qid, std::string commit, std::string profile_sha, std::string plan_sha,
    core::Sha256Digest digest, std::vector<core::WorkerId> workers, std::uint64_t n,
    std::uint64_t capacity, std::uint64_t total, std::uint64_t deadline, std::uint64_t tree,
    MonitoringQ1PlanDerivationMode mode, std::uint64_t allowance, std::uint64_t reserve,
    std::optional<runtime::detail::MonitoringQ1AdmittedWorkerSnapshot> worker_snapshot,
    MonitoringQ1EvidenceProvenance provenance) noexcept
    : qid_{std::move(qid)}, commit_{std::move(commit)}, profile_sha_{std::move(profile_sha)},
      plan_sha_{std::move(plan_sha)}, plan_digest_{digest}, workers_{std::move(workers)}, n_{n},
      capacity_{capacity}, total_{total}, deadline_{deadline}, tree_{tree}, mode_{mode},
      allowance_{allowance}, reserve_{reserve}, worker_snapshot_{std::move(worker_snapshot)},
      provenance_{std::move(provenance)} {}

MonitoringQ1PlanV2AuthoritySession::MonitoringQ1PlanV2AuthoritySession(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)} {}
MonitoringQ1PlanV2AuthoritySession::MonitoringQ1PlanV2AuthoritySession(
    MonitoringQ1PlanV2AuthoritySession&& other) noexcept
    : implementation_{std::move(other.implementation_)}, consumed_{other.consumed_},
      completed_values_taken_{other.completed_values_taken_} {
    other.consumed_ = true;
}
MonitoringQ1PlanV2AuthoritySession::~MonitoringQ1PlanV2AuthoritySession() noexcept = default;
void MonitoringQ1PlanV2AuthoritySession::ensure_active() const noexcept {
    if (consumed_ || !implementation_)
        std::terminate();
}
auto MonitoringQ1PlanV2AuthoritySessionTestAccess::inactive()
    -> MonitoringQ1PlanV2AuthoritySession {
    return MonitoringQ1PlanV2AuthoritySession{nullptr};
}
void MonitoringQ1PlanV2AuthoritySessionTestAccess::require_active(
    const MonitoringQ1PlanV2AuthoritySession& value) noexcept {
    value.ensure_active();
}
void MonitoringQ1PlanV2AuthoritySessionTestAccess::install_hook(void* context,
                                                                BoundaryHook hook) noexcept {
    boundary_context = context;
    boundary_hook = hook;
}
void MonitoringQ1PlanV2AuthoritySessionTestAccess::clear_hook() noexcept {
    boundary_hook = nullptr;
    boundary_context = nullptr;
}

auto MonitoringQ1PlanV2AuthoritySession::authenticate(const std::filesystem::path& spelling,
                                                      std::string_view qid) noexcept
    -> std::expected<MonitoringQ1PlanV2AuthoritySession, MonitoringQ1GitCommandFailure> {
    return authenticate_shared(spelling,qid,false);
}
auto MonitoringQ1PlanV2AuthoritySession::authenticate_shared(const std::filesystem::path& spelling,
                                                      std::string_view qid,bool q2_shared,
                                                      const HostFrozenSourceLaunchBinding* source) noexcept
    -> std::expected<MonitoringQ1PlanV2AuthoritySession, MonitoringQ1GitCommandFailure> {
    std::optional<core::detail::OwnedGovernedRepositoryRootDescriptor> retained;
    std::optional<MonitoringQ1GitExecutable> executable;
    std::optional<MonitoringQ1GitRepositoryMetadata> metadata;
    std::optional<MonitoringQ1PublicInputAuthority> inputs;
    std::optional<MonitoringQ1ValidatedPublicValues> values;
    std::optional<MonitoringQ1PrivateQ0Value> q0;
    const auto reject = [&](MonitoringQ1GitCommandFailure rejected)
        -> std::expected<MonitoringQ1PlanV2AuthoritySession, MonitoringQ1GitCommandFailure> {
        bool projection_failed = false;
        try {
            rejected.cleanup.reserve(rejected.cleanup.size() + 25U);
        } catch (...) {
            projection_failed = true;
        }
        const auto retain_error = [&](core::Error error) noexcept {
            try {
                rejected.cleanup.push_back(std::move(error));
            } catch (...) {
                projection_failed = true;
            }
        };
        const auto append = [&](auto operation) noexcept {
            try {
                auto result = operation();
                if (!result) {
                    if constexpr (std::same_as<decltype(result.error()), core::Error&>) {
                        retain_error(std::move(result.error()));
                    } else {
                        retain_error(std::move(result.error().primary));
                        for (auto& error : result.error().cleanup)
                            retain_error(std::move(error));
                    }
                }
            } catch (...) {
                projection_failed = true;
            }
        };
        if (q0)
            append([&] { return q0->retire(); });
        if (inputs)
            append([&] { return inputs->retire(); });
        if (metadata)
            append([&] { return metadata->retire(); });
        if (executable)
            append([&] { return executable->retire(); });
        if (retained)
            try {
                auto result = retained->retire();
                if (!result)
                    retain_error(root_cleanup(result.error()));
            } catch (...) {
                projection_failed = true;
            }
        q0.reset();
        values.reset();
        inputs.reset();
        metadata.reset();
        executable.reset();
        retained.reset();
        if (projection_failed)
            try {
                rejected.cleanup.push_back(cleanup());
            } catch (...) {
                // All acquired resources have already been retired.
            }
        return std::unexpected{std::move(rejected)};
    };
    try {
        if (source && (!source->ready() || !source->guard()))
            return reject(MonitoringQ1GitCommandFailure{cleanup(), {}});
        observe(MonitoringQ1AuthorityBoundary::root_retain);
        auto parsed = core::GovernedRepositoryRoot::create(spelling);
        if (!parsed)
            return reject(root_failure(parsed.error(), {}, false));
        auto acquired_root = core::detail::GovernedRepositoryRootAuthorityAccess::retain(*parsed);
        if (!acquired_root)
            return reject(root_failure(acquired_root.error(), acquired_root.cleanup, false));
        if (!acquired_root.cleanup.empty()) {
            MonitoringQ1GitCommandFailure result{cleanup(), {}};
            for (const auto& error : acquired_root.cleanup)
                result.cleanup.push_back(root_cleanup(error));
            return reject(std::move(result));
        }
        retained.emplace(std::move(*acquired_root));
        observe(MonitoringQ1AuthorityBoundary::executable_retain);
        auto acquired_executable = MonitoringQ1GitCommandOwner::retain_configured_executable();
        if (!acquired_executable)
            return reject(std::move(acquired_executable.error()));
        executable.emplace(std::move(*acquired_executable));
        executable->source_launch_ = source;
        observe(MonitoringQ1AuthorityBoundary::metadata_retain);
        auto acquired_metadata = MonitoringQ1GitRepositoryMetadata::retain(*retained);
        if (!acquired_metadata)
            return reject(std::move(acquired_metadata.error()));
        metadata.emplace(std::move(*acquired_metadata));
        observe(MonitoringQ1AuthorityBoundary::git_capture);
        auto git = MonitoringQ1GitFixedResultAuthority::run(*executable, *retained, *metadata);
        if (!git)
            return reject(std::move(git.error()));
        observe(MonitoringQ1AuthorityBoundary::public_inputs_retain);
        auto acquired_inputs = MonitoringQ1PublicInputAuthority::retain(*retained, *git);
        if (!acquired_inputs)
            return reject(std::move(acquired_inputs.error()));
        inputs.emplace(std::move(*acquired_inputs));
        observe(MonitoringQ1AuthorityBoundary::public_values_validate);
        auto validated = MonitoringQ1ValidatedPublicValues::validate(*inputs);
        if (!validated)
            return reject(std::move(validated.error()));
        values.emplace(std::move(*validated));
        if (values->qualification_id() != qid)
            return reject(
                wrapped(failure("monitoring.q1_plan_authority.plan_rejected", "binding")));
        if (!q2_shared) {
            observe(MonitoringQ1AuthorityBoundary::q0_retain);
            auto acquired_q0 = MonitoringQ1PrivateQ0Value::retain(*retained, *values);
            if (!acquired_q0)
                return reject(std::move(acquired_q0.error()));
            q0.emplace(std::move(*acquired_q0));
        }
        observe(MonitoringQ1AuthorityBoundary::authenticate_initial_checkpoint);
        auto root_held = retained->still_matches_held_descriptor();
        if (auto root_problem = held_root_checkpoint(root_held, true))
            return reject(std::move(*root_problem));
        const auto root_path = retained->still_matches_pathname();
        if (auto root_problem = pathname_root_checkpoint(root_path, true))
            return reject(std::move(*root_problem));
        auto git_ok = executable->recheck();
        if (!git_ok)
            return reject(wrapped(changed_from(git_ok.error())));
        if (!*git_ok)
            return reject(wrapped(changed()));
        auto metadata_ok = metadata->recheck();
        if (!metadata_ok)
            return reject(changed_from(std::move(metadata_ok.error())));
        if (!*metadata_ok)
            return reject(wrapped(changed()));
        auto inputs_ok = inputs->recheck();
        if (!inputs_ok)
            return reject(changed_from(std::move(inputs_ok.error())));
        if (!*inputs_ok)
            return reject(wrapped(changed()));
        if (q0) { auto q0_ok = q0->recheck(*values);
        if (!q0_ok) return reject(changed_from(std::move(q0_ok.error())));
        if (!*q0_ok) return reject(wrapped(changed())); }
        observe(MonitoringQ1AuthorityBoundary::authenticate_replay);
        auto repeated_git =
            MonitoringQ1GitFixedResultAuthority::run(*executable, *retained, *metadata);
        if (!repeated_git)
            return reject(changed_from(std::move(repeated_git.error())));
        auto repeated_inputs = MonitoringQ1PublicInputAuthority::retain(*retained, *repeated_git);
        if (!repeated_inputs)
            return reject(changed_from(std::move(repeated_inputs.error())));
        if (repeated_inputs->captured_head() != inputs->captured_head())
            return reject(retire_replay(*repeated_inputs, wrapped(changed())));
        auto repeated_values = MonitoringQ1ValidatedPublicValues::validate(*repeated_inputs);
        const auto original_workers = values->plan().worker_ids();
        if (!repeated_values)
            return reject(
                retire_replay(*repeated_inputs, changed_from(std::move(repeated_values.error()))));
        if (repeated_values->qualification_id() != values->qualification_id() ||
            repeated_values->plan().qualification_plan_digest() !=
                values->plan().qualification_plan_digest() ||
            repeated_values->plan().worker_ids().size() != original_workers.size()) {
            return reject(retire_replay(*repeated_inputs, wrapped(changed())));
        }
        for (std::size_t index = 0; index < original_workers.size(); ++index)
            if (repeated_values->plan().worker_ids()[index] != original_workers[index]) {
                return reject(retire_replay(*repeated_inputs, wrapped(changed())));
            }
        auto replay_retirement = close_monitoring_q1_replay(
            {&*repeated_inputs, [](void* raw) {
                 return static_cast<MonitoringQ1PublicInputAuthority*>(raw)->retire();
             }});
        if (!replay_retirement.succeeded)
            return reject(std::move(*replay_retirement.failure));
        observe(MonitoringQ1AuthorityBoundary::authenticate_final_checkpoint);
        root_held = retained->still_matches_held_descriptor();
        if (auto root_problem = held_root_checkpoint(root_held, true))
            return reject(std::move(*root_problem));
        const auto final_root_path = retained->still_matches_pathname();
        if (auto root_problem = pathname_root_checkpoint(final_root_path, true))
            return reject(std::move(*root_problem));
        git_ok = executable->recheck();
        if (!git_ok)
            return reject(wrapped(changed_from(git_ok.error())));
        if (!*git_ok)
            return reject(wrapped(changed()));
        metadata_ok = metadata->recheck();
        if (!metadata_ok)
            return reject(changed_from(std::move(metadata_ok.error())));
        if (!*metadata_ok)
            return reject(wrapped(changed()));
        inputs_ok = inputs->recheck();
        if (!inputs_ok)
            return reject(changed_from(std::move(inputs_ok.error())));
        if (!*inputs_ok)
            return reject(wrapped(changed()));
        if (q0) { auto q0_ok = q0->recheck(*values);
        if (!q0_ok) return reject(changed_from(std::move(q0_ok.error())));
        if (!*q0_ok) return reject(wrapped(changed())); }
        return MonitoringQ1PlanV2AuthoritySession{std::make_unique<Impl>(
            std::move(*retained), std::move(*executable), std::move(*metadata), std::move(*inputs),
            std::move(*values), std::move(q0))};
    } catch (const std::exception&) {
        try {
            return reject(wrapped(contained(false)));
        } catch (...) {
            return std::unexpected{wrapped(contained(false))};
        }
    } catch (...) {
        try {
            return reject(wrapped(contained(true)));
        } catch (...) {
            return std::unexpected{wrapped(contained(true))};
        }
    }
}

auto MonitoringQ1PlanV2AuthoritySession::bind_fresh_plan_bounded(
    const RunnerContextAcquisitionSession& session, const MonitoringHostQualificationPlan& plan,
    const runtime::AdmittedPlacementRequest& admitted) noexcept
    -> std::expected<MonitoringQ1CollectorPlan, MonitoringQ1BoundedFailure> {
    ensure_active();
    consumed_ = true;
    const auto reject = [&](MonitoringQ1GitCommandFailure rejected)
        -> std::expected<MonitoringQ1CollectorPlan, MonitoringQ1BoundedFailure> {
        MonitoringQ1BoundedFailure failure{std::move(rejected.primary), {}};
        const auto retain = [&](core::Error error) noexcept {
            if (failure.cleanup.count < failure.cleanup.failures.size())
                failure.cleanup.failures[failure.cleanup.count++].emplace(std::move(error));
            else
                failure.cleanup.overflowed = true;
        };
        for (auto& error : rejected.cleanup)
            retain(std::move(error));
        observe(MonitoringQ1AuthorityBoundary::retire);
        auto retirement = implementation_->retire();
        implementation_.reset();
        for (std::size_t index{}; index < retirement.retirement.count; ++index)
            retain(std::move(*retirement.retirement.failures[index]));
        failure.cleanup.overflowed |= retirement.retirement.overflowed;
        return std::unexpected{std::move(failure)};
    };
    try {
        observe(MonitoringQ1AuthorityBoundary::bind_opaque);
        auto binding = detail_HostQualificationPlanAccess::binding(session);
        const auto mismatch =
            [&]() -> std::expected<MonitoringQ1CollectorPlan, MonitoringQ1BoundedFailure> {
            return reject(wrapped(
                failure("monitoring.q1_plan_authority.fresh_plan_mismatch", "qualification_plan")));
        };
        if (!binding || *binding != detail_HostQualificationPlanAccess::plan_binding(plan))
            return mismatch();
        observe(MonitoringQ1AuthorityBoundary::bind_context);
        auto session_context = session.runner_context();
        if (!session_context || &session_context->get() != &admitted.context())
            return mismatch();
        observe(MonitoringQ1AuthorityBoundary::bind_provenance);
        auto admitted_provenance = admitted.request().host_qualification_plan_provenance();
        if (!admitted_provenance ||
            admitted_provenance->get().host_qualification_plan_digest().text() !=
                plan.plan_digest())
            return mismatch();
        observe(MonitoringQ1AuthorityBoundary::bind_digest);
        if (plan.digest() != implementation_->values_.plan().qualification_plan_digest())
            return mismatch();
        const auto expected = implementation_->values_.plan().worker_ids();
        observe(MonitoringQ1AuthorityBoundary::bind_count);
        if (plan.bandwidth_workers().size() != expected.size() ||
            admitted.workers().size() != expected.size())
            return mismatch();
        observe(MonitoringQ1AuthorityBoundary::bind_ordered_workers);
        for (std::size_t index = 0; index < expected.size(); ++index) {
            const auto& planned = plan.bandwidth_workers()[index];
            const auto& admitted_cpu = admitted.workers()[index].request().cpu();
            if (planned.worker_id != expected[index].value() ||
                admitted_cpu.linux_cpu() != planned.linux_cpu ||
                admitted_cpu.stable_core_id() != planned.stable_core_id ||
                admitted_cpu.stable_pu_id() != planned.stable_pu_id)
                return mismatch();
        }
        observe(MonitoringQ1AuthorityBoundary::bind_revalidate);
        auto disposition = session.revalidate();
        if (!disposition || *disposition != HostRevalidationDisposition::matched)
            return mismatch();
        observe(MonitoringQ1AuthorityBoundary::bind_initial_checkpoint);
        auto root_held = implementation_->root_.still_matches_held_descriptor();
        if (auto root_problem = held_root_checkpoint(root_held, true))
            return reject(std::move(*root_problem));
        const auto root_path = implementation_->root_.still_matches_pathname();
        if (auto root_problem = pathname_root_checkpoint(root_path, true))
            return reject(std::move(*root_problem));
        auto executable = implementation_->executable_.recheck();
        if (!executable)
            return reject(wrapped(changed_from(executable.error())));
        if (!*executable)
            return reject(wrapped(changed()));
        auto metadata = implementation_->metadata_.recheck();
        if (!metadata)
            return reject(changed_from(std::move(metadata.error())));
        if (!*metadata)
            return reject(wrapped(changed()));
        auto inputs = implementation_->inputs_.recheck();
        if (!inputs)
            return reject(changed_from(std::move(inputs.error())));
        if (!*inputs)
            return reject(wrapped(changed()));
        if (implementation_->q0_) { auto q0 = implementation_->q0_->recheck(implementation_->values_);
        if (!q0) return reject(changed_from(std::move(q0.error())));
        if (!*q0) return reject(wrapped(changed())); }
        observe(MonitoringQ1AuthorityBoundary::bind_replay);
        auto repeated_git = MonitoringQ1GitFixedResultAuthority::run(
            implementation_->executable_, implementation_->root_, implementation_->metadata_);
        if (!repeated_git)
            return reject(changed_from(std::move(repeated_git.error())));
        auto repeated_inputs =
            MonitoringQ1PublicInputAuthority::retain(implementation_->root_, *repeated_git);
        if (!repeated_inputs)
            return reject(changed_from(std::move(repeated_inputs.error())));
        if (repeated_inputs->captured_head() != implementation_->inputs_.captured_head())
            return reject(retire_replay(*repeated_inputs, wrapped(changed())));
        auto repeated_values = MonitoringQ1ValidatedPublicValues::validate(*repeated_inputs);
        if (!repeated_values)
            return reject(
                retire_replay(*repeated_inputs, changed_from(std::move(repeated_values.error()))));
        if (repeated_values->qualification_id() != implementation_->values_.qualification_id() ||
            repeated_values->plan().qualification_plan_digest() !=
                implementation_->values_.plan().qualification_plan_digest() ||
            repeated_values->plan().worker_ids().size() != expected.size()) {
            return reject(retire_replay(*repeated_inputs, wrapped(changed())));
        }
        for (std::size_t index = 0; index < expected.size(); ++index)
            if (repeated_values->plan().worker_ids()[index] != expected[index]) {
                return reject(retire_replay(*repeated_inputs, wrapped(changed())));
            }
        auto replay_retirement = close_monitoring_q1_replay(
            {&*repeated_inputs, [](void* raw) {
                 return static_cast<MonitoringQ1PublicInputAuthority*>(raw)->retire();
             }});
        if (!replay_retirement.succeeded)
            return reject(std::move(*replay_retirement.failure));
        observe(MonitoringQ1AuthorityBoundary::bind_final_checkpoint);
        root_held = implementation_->root_.still_matches_held_descriptor();
        if (auto root_problem = held_root_checkpoint(root_held, true))
            return reject(std::move(*root_problem));
        const auto final_root_path = implementation_->root_.still_matches_pathname();
        if (auto root_problem = pathname_root_checkpoint(final_root_path, true))
            return reject(std::move(*root_problem));
        executable = implementation_->executable_.recheck();
        if (!executable)
            return reject(wrapped(changed_from(executable.error())));
        if (!*executable)
            return reject(wrapped(changed()));
        metadata = implementation_->metadata_.recheck();
        if (!metadata)
            return reject(changed_from(std::move(metadata.error())));
        if (!*metadata)
            return reject(wrapped(changed()));
        inputs = implementation_->inputs_.recheck();
        if (!inputs)
            return reject(changed_from(std::move(inputs.error())));
        if (!*inputs)
            return reject(wrapped(changed()));
        if (implementation_->q0_) { auto q0 = implementation_->q0_->recheck(implementation_->values_);
        if (!q0) return reject(changed_from(std::move(q0.error())));
        if (!*q0) return reject(wrapped(changed())); }
        auto worker_snapshot = runtime::detail::project_monitoring_q1_worker_snapshot(admitted);
        if (!worker_snapshot)
            return reject(wrapped(failure(
                "monitoring.q1_plan_authority.fresh_plan_mismatch", "worker_binding")));
        const auto& value = implementation_->values_.plan();
        MonitoringQ1CollectorPlan view{
            std::string{value.qualification_id()},
            std::string{implementation_->inputs_.captured_head()},
            std::string{implementation_->inputs_.digest(1)},
            std::string{implementation_->inputs_.digest(2)},
            value.qualification_plan_digest(),
            std::vector<core::WorkerId>{expected.begin(), expected.end()},
            value.observations_per_worker(),
            value.capacity(),
            value.total_observations(),
            value.execution_deadline_seconds(),
            value.maximum_successor_tree_bytes(),
            value.derivation_mode(),
            value.per_observation_allowance_ms(),
            value.fixed_reserve_seconds(),
            std::optional<runtime::detail::MonitoringQ1AdmittedWorkerSnapshot>{
                std::move(*worker_snapshot)},
            MonitoringQ1EvidenceProvenance{
                implementation_->values_.q0().manifest_sha256.to_hex(),
                implementation_->values_.q0().checksums_sha256.to_hex(),
                implementation_->values_.q0().environment_artifact_sha256.to_hex(),
                implementation_->values_.q0().characterization_artifact_sha256.to_hex(),
                implementation_->values_.q0().environment_digest}};
        observe(MonitoringQ1AuthorityBoundary::retire);
        auto retirement = implementation_->retire();
        implementation_.reset();
        if (retirement.retirement.count != 0U || retirement.retirement.overflowed) {
            return std::unexpected{MonitoringQ1BoundedFailure{
                cleanup(), std::move(retirement.retirement)}};
        }
        return view;
    } catch (const std::exception&) {
        if (!implementation_)
            return std::unexpected{MonitoringQ1BoundedFailure{
                wrapped(contained(false)).primary, {}}};
        try {
            return reject(wrapped(contained(false)));
        } catch (...) {
            implementation_.reset();
            return std::unexpected{MonitoringQ1BoundedFailure{
                wrapped(contained(false)).primary, {}}};
        }
    } catch (...) {
        if (!implementation_)
            return std::unexpected{MonitoringQ1BoundedFailure{
                wrapped(contained(true)).primary, {}}};
        try {
            return reject(wrapped(contained(true)));
        } catch (...) {
            implementation_.reset();
            return std::unexpected{MonitoringQ1BoundedFailure{
                wrapped(contained(true)).primary, {}}};
        }
    }
}

auto MonitoringQ1PlanV2AuthoritySession::bind_fresh_plan(
    const RunnerContextAcquisitionSession& session, const MonitoringHostQualificationPlan& plan,
    const runtime::AdmittedPlacementRequest& admitted) noexcept
    -> std::expected<MonitoringQ1CollectorPlan, MonitoringQ1GitCommandFailure> {
    auto result = bind_fresh_plan_bounded(session, plan, admitted);
    if (result)
        return std::move(*result);
    auto bounded = std::move(result.error());
    MonitoringQ1GitCommandFailure projected{std::move(bounded.primary), {}};
    bool projection_failed = bounded.cleanup.overflowed;
    try {
        projected.cleanup.reserve(bounded.cleanup.count +
                                  (bounded.cleanup.overflowed ? 1U : 0U));
        for (std::size_t index{}; index < bounded.cleanup.count; ++index)
            projected.cleanup.push_back(std::move(*bounded.cleanup.failures[index]));
    } catch (...) {
        projection_failed = true;
    }
    if (projection_failed)
        try {
            projected.cleanup.push_back(cleanup());
        } catch (...) {
            // The unchanged primary remains reportable at the public Q1 boundary.
        }
    return std::unexpected{std::move(projected)};
}

auto MonitoringQ1PlanV2AuthoritySession::authenticated_repository_commit() const noexcept
    -> std::string_view {
    ensure_active();
    return implementation_->inputs_.captured_head();
}
auto MonitoringQ1PlanV2AuthoritySession::retire_before_binding() noexcept
    -> MonitoringQ1BoundedRetirement {
    ensure_active();
    consumed_ = true;
    auto retirement = implementation_->retire();
    implementation_.reset();
    return std::move(retirement.retirement);
}
auto MonitoringQ1PlanV2AuthoritySession::q3_recheck() noexcept
    -> std::expected<bool, MonitoringQ1GitCommandFailure> {
    ensure_active();
    auto& impl = *implementation_;
    try {
        auto held = impl.root_.still_matches_held_descriptor();
        if (auto problem = held_root_checkpoint(held, true)) return std::unexpected{std::move(*problem)};
        auto path = impl.root_.still_matches_pathname();
        if (auto problem = pathname_root_checkpoint(path, true)) return std::unexpected{std::move(*problem)};
        auto executable = impl.executable_.recheck();
        if (!executable) return std::unexpected{wrapped(changed_from(executable.error()))};
        if (!*executable) return std::unexpected{wrapped(changed())};
        auto metadata = impl.metadata_.recheck();
        if (!metadata) return std::unexpected{changed_from(std::move(metadata.error()))};
        if (!*metadata) return std::unexpected{wrapped(changed())};
        auto inputs = impl.inputs_.recheck();
        if (!inputs) return std::unexpected{changed_from(std::move(inputs.error()))};
        if (!*inputs) return std::unexpected{wrapped(changed())};
        return true;
    } catch (...) { return std::unexpected{wrapped(contained(false))}; }
}

auto MonitoringQ1PlanV2AuthoritySession::q4_root() noexcept
    -> core::detail::OwnedGovernedRepositoryRootDescriptor& {
    return implementation_->root_;
}
auto MonitoringQ1PlanV2AuthoritySession::q4_executable() noexcept -> MonitoringQ1GitExecutable& {
    return implementation_->executable_;
}
auto MonitoringQ1PlanV2AuthoritySession::q4_values() const noexcept
    -> const MonitoringQ1ValidatedPublicValues& {
    ensure_active();
    return implementation_->values_;
}
auto MonitoringQ1PlanV2AuthoritySession::take_completed_values(
    std::optional<MonitoringQ1ValidatedPublicValues>& hidden) noexcept -> bool {
    // Only the completed-Q4 coordinator calls this after the genuine rechecks.
    // It keeps the destination hidden until the existing native owners retire.
    if (!implementation_ || consumed_ || completed_values_taken_ || hidden)
        return false;
    hidden.emplace(std::move(implementation_->values_));
    completed_values_taken_ = true;
    return true;
}
auto MonitoringQ1PlanV2AuthoritySession::q4_metadata() noexcept
    -> MonitoringQ1GitRepositoryMetadata& {
    return implementation_->metadata_;
}
} // namespace epycbench::monitoring::qualification::detail
