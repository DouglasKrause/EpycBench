#include "epycbench/monitoring/host_runner_context_session.hpp"

#include "epycbench/kernel/registry.hpp"
#include "core/json_ownership.hpp"
#include "monitoring/host_runner_context_session_access.hpp"
#include "monitoring/host_qualification_plan_access.hpp"
#include "runtime/runner_context_access.hpp"

#include <cerrno>
#include <exception>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/utsname.h>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace epycbench::monitoring::qualification {
namespace {
static_assert(std::is_nothrow_move_constructible_v<core::Error>);
auto retain_raw_failure(std::optional<core::Error>* sink, core::Error& actual) noexcept
    -> const core::Error* {
    if (sink != nullptr && !sink->has_value()) {
        sink->emplace(std::move(actual));
        return &**sink;
    }
    return &actual;
}
auto error(std::string code, std::string classification, const core::Error* underlying = nullptr)
    -> core::Error {
    auto context = core::detail::json_object<nlohmann::json>([&](auto& object) {
        object["classification"] = std::move(classification);
    });
    core::detail::JsonCleanup cleanup{context};
    if (underlying != nullptr)
        context["underlying_code"] = underlying->code.value();
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Monitoring host prerequisite acquisition failed",
            .operation = "acquire_runner_context_session",
            .os_error = underlying == nullptr ? std::nullopt : underlying->os_error,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto contained(bool unknown = false) -> core::Error {
    return error("monitoring.host_environment.contained_failure",
                 unknown ? "contained_unknown_failure" : "contained_exception");
}
auto contained_at(std::string operation, std::string message, bool unknown=false)->core::Error {
    auto result=contained(unknown);
    core::detail::JsonCleanup cleanup{result.context};
    result.operation=std::move(operation);
    result.message=std::move(message);
    return core::Error(std::move(result));
}
auto invalid_session() -> core::Error {
    return error("monitoring.host_environment.invalid_session", "moved_from_session");
}
auto revalidation_error(std::string classification, const core::Error* underlying=nullptr)
    -> core::Error {
    auto result=error("monitoring.host_environment.revalidation_failed",
                      std::move(classification),underlying);
    core::detail::JsonCleanup cleanup{result.context};
    result.message="Monitoring host revalidation failed";
    result.operation="revalidate_host_environment";
    return core::Error(std::move(result));
}
auto registration_error(std::string classification) -> core::Error {
    auto result=error("monitoring.host_environment.revalidation_registration_rejected",
                      std::move(classification));
    core::detail::JsonCleanup cleanup{result.context};
    result.message="Monitoring selected-resource registration failed";
    result.operation="register_selected_resources";
    return core::Error(std::move(result));
}
auto valid_digest_identity(std::string_view value) noexcept -> bool {
    return value.size()==71U && value.starts_with("sha256:") &&
           std::ranges::all_of(value.substr(7),[](char digit){
               return (digit>='0' && digit<='9') || (digit>='a' && digit<='f');
           });
}

struct ProductionState {
    runtime::RunnerContextProvider provider;
    std::optional<core::Error>* raw_failure{};
    bool source{};
    platform_linux::detail::PreparedTopologySourceIO* prepared{};
    const std::uint64_t* main_invocation_end{};
};
auto destroy_production(void* state) noexcept { delete static_cast<ProductionState*>(state); }
auto production_acquire(void* opaque) -> std::expected<runtime::RunnerContext, core::Error> {
    auto& state = *static_cast<ProductionState*>(opaque);
    if (state.source) return runtime::detail::acquire_runner_context_fallible(
        state.raw_failure, state.prepared, state.main_invocation_end ? *state.main_invocation_end : 0);
    return state.provider.acquire(state.provider.context);
}
auto production_platform(void*) -> std::expected<TypedSysnameDisposition, core::Error> {
    struct utsname identity{};
    if (::uname(&identity) != 0) {
        const int captured = errno;
        return std::unexpected{core::Error{
            .category = core::ErrorCategory::platform_linux,
            .code = core::StableErrorCode{"platform.uname_failed"},
            .message = "Unable to observe platform identity",
            .operation = "uname",
            .os_error = core::OsError::capture(std::error_code{captured, std::generic_category()}),
            .context = nlohmann::json::object(),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command}};
    }
    return std::string_view{identity.sysname} == "Linux" ? TypedSysnameDisposition::linux
                                                         : TypedSysnameDisposition::not_linux;
}
auto production_descriptor(void*, kernel::KernelId id) -> const kernel::KernelDescriptor* {
    return kernel::find_kernel_descriptor(id);
}
auto production_reobserve(void* opaque)
    -> std::expected<detail_HostRevalidationObservation, detail_HostObservationFailure> {
    auto& state = *static_cast<ProductionState*>(opaque);
    auto context = production_acquire(opaque);
    core::detail::ExpectedErrorCleanup cleanup{context};
    if (!context)
        return std::unexpected{detail_HostObservationFailure{
          detail_HostObservationFailureKind::expected_error,std::move(context.error())}};
    if (state.source) {
        return detail_HostRevalidationObservation::from_context_fallible(std::move(*context));
    }
    return detail_HostRevalidationObservation::from_context(std::move(*context));
}
auto make_production_operations(std::optional<core::Error>* raw_failure = nullptr, bool source = false,
    platform_linux::detail::PreparedTopologySourceIO* prepared = nullptr,
    const std::uint64_t* main_invocation_end = nullptr)
    -> std::expected<RunnerContextAcquisitionOperations, core::Error> {
    auto state = std::make_unique<ProductionState>(
        ProductionState{runtime::production_runner_context_provider(), raw_failure, source, prepared, main_invocation_end});
    auto operations = RunnerContextAcquisitionOperations::create(
        state.get(), destroy_production, production_acquire, production_platform,
        production_descriptor, production_reobserve);
    if (!operations)
        return std::unexpected{std::move(operations.error())};
    (void)state.release();
    return std::move(*operations);
}
} // namespace

auto RunnerContextAcquisitionOperations::create(
    void* state, void (*destroy)(void*) noexcept,
    std::expected<runtime::RunnerContext, core::Error> (*acquire_context)(void*),
    std::expected<TypedSysnameDisposition, core::Error> (*observe_platform)(void*),
    const kernel::KernelDescriptor* (*find_descriptor)(void*, kernel::KernelId),
    std::expected<detail_HostRevalidationObservation,
                  detail_HostObservationFailure> (*reobserve)(void*),
    std::expected<void, core::Error> (*before_registration)(void*)) noexcept
    -> std::expected<RunnerContextAcquisitionOperations, core::Error> {
    if (state == nullptr || destroy == nullptr) {
        return std::unexpected{
            error("monitoring.host_environment.acquisition_failed", "invalid_operations")};
    }
    return RunnerContextAcquisitionOperations{
        state, destroy, acquire_context, observe_platform, find_descriptor, reobserve,
        before_registration};
}

RunnerContextAcquisitionOperations::RunnerContextAcquisitionOperations(
    void* state, void (*destroy)(void*) noexcept,
    std::expected<runtime::RunnerContext, core::Error> (*acquire_context)(void*),
    std::expected<TypedSysnameDisposition, core::Error> (*observe_platform)(void*),
    const kernel::KernelDescriptor* (*find_descriptor)(void*, kernel::KernelId),
    std::expected<detail_HostRevalidationObservation,
                  detail_HostObservationFailure> (*reobserve)(void*),
    std::expected<void, core::Error> (*before_registration)(void*)) noexcept
    : state_{state}, destroy_{destroy}, acquire_context_{acquire_context},
      observe_platform_{observe_platform}, find_descriptor_{find_descriptor},
      reobserve_{reobserve}, before_registration_{before_registration} {}
RunnerContextAcquisitionOperations::RunnerContextAcquisitionOperations(
    RunnerContextAcquisitionOperations&& other) noexcept
    : state_{std::exchange(other.state_, nullptr)},
      destroy_{std::exchange(other.destroy_, nullptr)},
      acquire_context_{std::exchange(other.acquire_context_, nullptr)},
      observe_platform_{std::exchange(other.observe_platform_, nullptr)},
      find_descriptor_{std::exchange(other.find_descriptor_, nullptr)},
      reobserve_{std::exchange(other.reobserve_, nullptr)},
      before_registration_{std::exchange(other.before_registration_, nullptr)},
      raw_failure_sink_{std::exchange(other.raw_failure_sink_, nullptr)} {}
RunnerContextAcquisitionOperations::~RunnerContextAcquisitionOperations() noexcept {
    if (state_ != nullptr && destroy_ != nullptr)
        destroy_(state_);
}

auto detail_HostRevalidationObservation::create_for_testing(
    runtime::RunnerContext context,core::CpuSet cpus,core::NodeSet nodes,
    std::vector<detail_HostObservedServiceCore> service) noexcept
    ->std::expected<detail_HostRevalidationObservation,detail_HostObservationFailure> {
    try {
      return detail_HostRevalidationObservation{std::move(context),std::move(cpus),
                                                 std::move(nodes),std::move(service)};
    } catch(const std::exception&) { return std::unexpected{detail_HostObservationFailure{
        detail_HostObservationFailureKind::contained_exception,std::nullopt}}; }
      catch(...) { return std::unexpected{detail_HostObservationFailure{
        detail_HostObservationFailureKind::contained_unknown_failure,std::nullopt}}; }
}

auto detail_HostRevalidationObservation::from_context(runtime::RunnerContext context) noexcept
    ->std::expected<detail_HostRevalidationObservation,detail_HostObservationFailure> {
    return from_context_for_testing(std::move(context),
                                    detail_HostObservationConstructionInjection::none);
}
auto detail_HostRevalidationObservation::from_context_fallible(runtime::RunnerContext context)
    -> std::expected<detail_HostRevalidationObservation, detail_HostObservationFailure> {
    return from_context_impl(std::move(context), detail_HostObservationConstructionInjection::none);
}
auto detail_HostRevalidationObservation::from_context_for_testing(
    runtime::RunnerContext context,detail_HostObservationConstructionInjection injection) noexcept
    ->std::expected<detail_HostRevalidationObservation,detail_HostObservationFailure> {
    try {
      return from_context_impl(std::move(context), injection);
    } catch(const std::exception&) { return std::unexpected{detail_HostObservationFailure{
        detail_HostObservationFailureKind::contained_exception,std::nullopt}}; }
      catch(...) { return std::unexpected{detail_HostObservationFailure{
        detail_HostObservationFailureKind::contained_unknown_failure,std::nullopt}}; }
}
auto detail_HostRevalidationObservation::from_context_impl(
    runtime::RunnerContext context, detail_HostObservationConstructionInjection injection)
    -> std::expected<detail_HostRevalidationObservation,detail_HostObservationFailure> {
      if(injection==detail_HostObservationConstructionInjection::standard_exception)
        throw std::runtime_error{"private observation construction"};
      if(injection==detail_HostObservationConstructionInjection::unknown_exception) throw 7;
      const auto& machine=context.topology().machine();
      if(!machine.effective_cpus.value() || !machine.effective_nodes.value())
        return std::unexpected{detail_HostObservationFailure{
          detail_HostObservationFailureKind::expected_error,
          revalidation_error("selected_resource_observation_failed")}};
      std::vector<detail_HostObservedServiceCore> service;
      for(const auto& core:context.topology().cores()) {
        auto stable_core=context.stable_ids().core_id(core.id); if(!stable_core) continue;
        detail_HostObservedServiceCore observed{stable_core->get(),{}, {}};
        for(const auto& pu:context.topology().processing_units()) if(pu.core_id==core.id) {
          auto stable_pu=context.stable_ids().processing_unit_id(pu.id);
          if(stable_pu) observed.stable_pus.push_back(stable_pu->get());
          if(machine.effective_cpus.value()->contains(pu.linux_cpu_id))
            observed.sibling_cpus.push_back(pu.linux_cpu_id);
        }
        std::ranges::sort(observed.stable_pus);
        std::ranges::sort(observed.sibling_cpus);
        service.push_back(std::move(observed));
      }
      auto cpus=*machine.effective_cpus.value();
      auto nodes=*machine.effective_nodes.value();
      return detail_HostRevalidationObservation{std::move(context),std::move(cpus),
        std::move(nodes),std::move(service)};
}

class RunnerContextAcquisitionSession::Impl {
  public:
    Impl(RunnerContextAcquisitionOperations operations,
         MonitoringHostInputTemplateAuthority input_template, runtime::RunnerContext context,
         MonitoringHostPrerequisiteCapabilities prerequisites)
        : operations_{std::move(operations)}, template_{std::move(input_template)},
          context_{std::move(context)}, prerequisites_{std::move(prerequisites)},
          binding_{std::make_shared<detail_HostSessionBinding>(
              context_.fingerprints().physical, context_.fingerprints().effective)} {}
    RunnerContextAcquisitionOperations operations_;
    MonitoringHostInputTemplateAuthority template_;
    runtime::RunnerContext context_;
    MonitoringHostPrerequisiteCapabilities prerequisites_;
    std::shared_ptr<detail_HostSessionBinding> binding_;
    std::optional<detail_HostSelectedResourceRequirements> requirements_;
};

class detail_RunnerContextAcquisitionFactory {
  public:
    static auto acquire(MonitoringHostInputTemplateAuthority input_template,
                        RunnerContextAcquisitionOperations operations) noexcept
        -> std::expected<RunnerContextAcquisitionSession, core::Error> {
        try {
            return acquire_fallible(std::move(input_template), std::move(operations));
        } catch (const std::exception&) {
            return std::unexpected{contained()};
        } catch (...) {
            return std::unexpected{contained(true)};
        }
    }
    static auto acquire_fallible(MonitoringHostInputTemplateAuthority input_template,
                                RunnerContextAcquisitionOperations operations)
        -> std::expected<RunnerContextAcquisitionSession, core::Error> {
        if (operations.state_ == nullptr || operations.destroy_ == nullptr ||
            operations.acquire_context_ == nullptr || operations.observe_platform_ == nullptr ||
            operations.find_descriptor_ == nullptr || operations.reobserve_ == nullptr) {
            return std::unexpected{
                error("monitoring.host_environment.acquisition_failed", "invalid_operations")};
        }
        auto context = operations.acquire_context_(operations.state_);
        core::detail::ExpectedErrorCleanup context_cleanup{context};
        if (!context) {
            const auto* underlying = retain_raw_failure(operations.raw_failure_sink_, context.error());
            return std::unexpected{error("monitoring.host_environment.acquisition_failed",
                                         "runner_context_acquisition_failed", underlying)};
        }
        auto platform = operations.observe_platform_(operations.state_);
        core::detail::ExpectedErrorCleanup platform_cleanup{platform};
        if (!platform) {
            const auto* underlying = retain_raw_failure(operations.raw_failure_sink_, platform.error());
            return std::unexpected{error("monitoring.host_environment.acquisition_failed",
                                         "platform_identity_observation_failed", underlying)};
        }
        if (*platform != TypedSysnameDisposition::linux)
            return std::unexpected{error("monitoring.host_environment.prerequisite_rejected",
                                         "linux_platform_required")};
        const auto* bandwidth =
            operations.find_descriptor_(operations.state_, input_template.bandwidth_kernel());
        if (bandwidth == nullptr || bandwidth->kernel_id() != input_template.bandwidth_kernel())
            return std::unexpected{error("monitoring.host_environment.prerequisite_rejected",
                                         "bandwidth_kernel_unavailable")};
        const auto* latency =
            operations.find_descriptor_(operations.state_, input_template.latency_kernel());
        if (latency == nullptr || latency->kernel_id() != input_template.latency_kernel())
            return std::unexpected{error("monitoring.host_environment.prerequisite_rejected",
                                         "latency_kernel_unavailable")};
        auto calibration = measurement::CalibrationDurationPolicy::create(
            input_template.minimum_duration(), input_template.target_duration(),
            input_template.maximum_duration());
        core::detail::ExpectedErrorCleanup calibration_cleanup{calibration};
        if (!calibration)
            return std::unexpected{error("monitoring.host_environment.prerequisite_rejected",
                                         "calibration_policy_rejected", &calibration.error())};
        auto prerequisites = MonitoringHostPrerequisiteCapabilities{*bandwidth, *latency,
                                                                    std::move(*calibration)};
        auto implementation = std::make_unique<RunnerContextAcquisitionSession::Impl>(
            std::move(operations), std::move(input_template), std::move(*context),
            std::move(prerequisites));
        return RunnerContextAcquisitionSession{std::move(implementation)};
    }
};

RunnerContextAcquisitionSession::RunnerContextAcquisitionSession(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)} {}
RunnerContextAcquisitionSession::RunnerContextAcquisitionSession(
    RunnerContextAcquisitionSession&&) noexcept = default;
RunnerContextAcquisitionSession::~RunnerContextAcquisitionSession() noexcept = default;

auto RunnerContextAcquisitionSession::input_template() const noexcept
    -> std::expected<std::reference_wrapper<const MonitoringHostInputTemplateAuthority>,
                     core::Error> {
    return detail_RunnerContextAcquisitionAccess::input_template_fallible(*this);
}
auto detail_RunnerContextAcquisitionAccess::input_template_fallible(
    const RunnerContextAcquisitionSession& session)
    -> std::expected<std::reference_wrapper<const MonitoringHostInputTemplateAuthority>,
                     core::Error> {
    if (!session.implementation_)
        return std::unexpected{invalid_session()};
    return std::cref(session.implementation_->template_);
}
auto RunnerContextAcquisitionSession::runner_context() const noexcept
    -> std::expected<std::reference_wrapper<const runtime::RunnerContext>, core::Error> {
    return detail_RunnerContextAcquisitionAccess::runner_context_fallible(*this);
}
auto detail_RunnerContextAcquisitionAccess::runner_context_fallible(
    const RunnerContextAcquisitionSession& session)
    -> std::expected<std::reference_wrapper<const runtime::RunnerContext>, core::Error> {
    if (!session.implementation_)
        return std::unexpected{invalid_session()};
    return std::cref(session.implementation_->context_);
}
auto RunnerContextAcquisitionSession::prerequisites() const noexcept
    -> std::expected<std::reference_wrapper<const MonitoringHostPrerequisiteCapabilities>,
                     core::Error> {
    return detail_RunnerContextAcquisitionAccess::prerequisites_fallible(*this);
}
auto detail_RunnerContextAcquisitionAccess::prerequisites_fallible(
    const RunnerContextAcquisitionSession& session)
    -> std::expected<std::reference_wrapper<const MonitoringHostPrerequisiteCapabilities>,
                     core::Error> {
    if (!session.implementation_)
        return std::unexpected{invalid_session()};
    return std::cref(session.implementation_->prerequisites_);
}
auto RunnerContextAcquisitionSession::revalidate() const noexcept
    -> std::expected<HostRevalidationDisposition, core::Error> {
    if (!implementation_)
        return std::unexpected{invalid_session()};
    try {
        return detail_RunnerContextAcquisitionAccess::revalidate_fallible(*this);
    } catch (const std::exception&) {
        return std::unexpected{contained_at("revalidate_host_environment",
                                             "Monitoring host revalidation failed")};
    } catch (...) {
        return std::unexpected{contained_at("revalidate_host_environment",
                                             "Monitoring host revalidation failed",true)};
    }
}
auto detail_RunnerContextAcquisitionAccess::revalidate_fallible(
    const RunnerContextAcquisitionSession& session)
    -> std::expected<HostRevalidationDisposition, core::Error> {
    const auto& implementation_ = session.implementation_;
    if (!implementation_) return std::unexpected{invalid_session()};
    if(!implementation_->requirements_)
        return std::unexpected{revalidation_error("selected_resources_not_registered")};
    auto observed =
        implementation_->operations_.reobserve_(implementation_->operations_.state_);
    core::detail::JsonPointerCleanup observed_cleanup{
        !observed && observed.error().underlying
            ? &observed.error().underlying->context : nullptr};
    if (!observed) {
        if(observed.error().kind==detail_HostObservationFailureKind::contained_exception)
          return std::unexpected{contained_at("revalidate_host_environment",
                                               "Monitoring host revalidation failed")};
        if(observed.error().kind==detail_HostObservationFailureKind::contained_unknown_failure)
          return std::unexpected{contained_at("revalidate_host_environment",
                                               "Monitoring host revalidation failed",true)};
        if(!observed.error().underlying)
          return std::unexpected{revalidation_error("selected_resource_observation_failed")};
        const auto* underlying = retain_raw_failure(implementation_->operations_.raw_failure_sink_,
                                                    *observed.error().underlying);
        return std::unexpected{revalidation_error("selected_resource_observation_failed", underlying)};
    }
    const auto& requirements=*implementation_->requirements_;
    if(observed->context_.fingerprints().physical!=requirements.physical)
        return HostRevalidationDisposition::physical_fingerprint_mismatch;
    if(observed->context_.fingerprints().effective!=requirements.effective)
        return HostRevalidationDisposition::effective_fingerprint_mismatch;
    for(const auto& worker:requirements.workers) {
        if(!observed->effective_cpus_.contains(worker.cpu))
            return HostRevalidationDisposition::worker_cpu_unavailable;
        if(!observed->effective_nodes_.contains(core::NumaNodeId{worker.node.value()}))
            return HostRevalidationDisposition::worker_node_unavailable;
    }
    for(const auto& service:requirements.service) {
        auto core_it=std::ranges::find_if(observed->service_,[&](const auto& core){
            return core.stable_core==service.stable_core;
        });
        if(core_it==observed->service_.end())
            return HostRevalidationDisposition::service_core_incomplete;
        if(core_it->sibling_cpus!=service.sibling_cpus || core_it->stable_pus!=service.stable_pus)
            return HostRevalidationDisposition::service_core_incomplete;
    }
    return HostRevalidationDisposition::matched;
}

auto detail_HostQualificationPlanAccess::binding(
    const RunnerContextAcquisitionSession& session) noexcept
    -> std::expected<std::shared_ptr<const detail_HostSessionBinding>, core::Error> {
    return binding_fallible(session);
}
auto detail_HostQualificationPlanAccess::binding_fallible(
    const RunnerContextAcquisitionSession& session)
    -> std::expected<std::shared_ptr<const detail_HostSessionBinding>, core::Error> {
    if(!session.implementation_) return std::unexpected{invalid_session()};
    return session.implementation_->binding_;
}
auto detail_HostQualificationPlanAccess::binding(const HostEnvironmentAuthority& environment) noexcept
    -> const std::shared_ptr<const detail_HostSessionBinding>& { return environment.binding_; }
auto detail_HostQualificationPlanAccess::bind_environment(
    const RunnerContextAcquisitionSession& session,std::string_view digest) noexcept
    -> std::expected<void,core::Error> {
    return bind_environment_fallible(session, digest);
}
auto detail_HostQualificationPlanAccess::bind_environment_fallible(
    const RunnerContextAcquisitionSession& session, std::string_view digest)
    -> std::expected<void, core::Error> {
    if(!session.implementation_) return std::unexpected{invalid_session()};
    auto& binding=*session.implementation_->binding_;
    if(!binding.environment_digest_.empty() && binding.environment_digest_!=digest)
        return std::unexpected{registration_error("requirements_binding_mismatch")};
    if (binding.environment_digest_ == digest)
        return {};
    std::string retained{digest};
    binding.environment_digest_.swap(retained);
    return {};
}

auto detail_HostQualificationPlanAccess::environment_digest(
    const std::shared_ptr<const detail_HostSessionBinding>& binding) noexcept
    -> const std::string& {
    return binding->environment_digest_;
}
auto detail_HostQualificationPlanAccess::register_requirements(
    const RunnerContextAcquisitionSession& session,
    detail_HostSelectedResourceRequirements requirements) noexcept -> std::expected<void,core::Error> {
    try {
        return register_requirements_fallible(session, std::move(requirements));
    } catch (const std::exception&) {
        return std::unexpected{contained_at("register_selected_resources",
                                            "Monitoring selected-resource registration failed")};
    } catch (...) {
        return std::unexpected{contained_at("register_selected_resources",
                                            "Monitoring selected-resource registration failed",
                                            true)};
    }
}
auto detail_HostQualificationPlanAccess::register_requirements_fallible(
    const RunnerContextAcquisitionSession& session,
    detail_HostSelectedResourceRequirements requirements, std::optional<core::Error>* raw_failure)
    -> std::expected<void, core::Error> {
    if(!session.implementation_) return std::unexpected{invalid_session()};
    auto& impl=*session.implementation_;
    if(impl.requirements_) return std::unexpected{registration_error("duplicate_registration")};
    if(requirements.binding.get()!=impl.binding_.get())
        return std::unexpected{registration_error("cross_session_requirements")};
    if(!valid_digest_identity(requirements.environment_digest) ||
       requirements.environment_digest!=impl.binding_->environment_digest_ ||
       requirements.physical!=impl.binding_->physical_ ||
       requirements.effective!=impl.binding_->effective_)
        return std::unexpected{registration_error("requirements_binding_mismatch")};
    if(requirements.workers.empty() || requirements.service.size()!=2U)
        return std::unexpected{registration_error("requirements_malformed")};
    const auto latency_count=std::ranges::count_if(requirements.workers,[](const auto& worker){
        return worker.family=="latency";
    });
    auto first_latency=std::ranges::find_if(requirements.workers,[](const auto& worker){
        return worker.family=="latency";
    });
    if(latency_count!=1 || first_latency==requirements.workers.end() ||
       first_latency==requirements.workers.begin() || std::next(first_latency)!=requirements.workers.end() ||
       requirements.workers.size()!=static_cast<std::size_t>(std::distance(requirements.workers.begin(),first_latency))+1U ||
       requirements.workers.front().family!="bandwidth" ||
       first_latency->worker_id!=requirements.workers.front().worker_id ||
       first_latency->stable_node!=requirements.workers.front().stable_node ||
       first_latency->stable_core!=requirements.workers.front().stable_core ||
       first_latency->stable_pu!=requirements.workers.front().stable_pu ||
       first_latency->cpu!=requirements.workers.front().cpu ||
       first_latency->node!=requirements.workers.front().node)
        return std::unexpected{registration_error("requirements_malformed")};
    std::set<core::LinuxCpuId> worker_cpus;
    std::set<core::LinuxNumaNodeId> worker_nodes;
    std::set<topology::StableNumaNodeId> worker_stable_nodes;
    std::set<topology::StableCoreId> worker_cores;
    std::set<topology::StableProcessingUnitId> worker_pus;
    std::uint32_t expected_worker=0;
    for(auto it=requirements.workers.begin();it!=first_latency;++it) {
        if(it->family!="bandwidth" || it->worker_id!=expected_worker++ ||
           !worker_cpus.insert(it->cpu).second || !worker_nodes.insert(it->node).second ||
           !worker_stable_nodes.insert(it->stable_node).second ||
           !worker_cores.insert(it->stable_core).second || !worker_pus.insert(it->stable_pu).second)
            return std::unexpected{registration_error("requirements_malformed")};
    }
    std::set<topology::StableCoreId> service_cores;
    std::set<topology::StableProcessingUnitId> service_pus;
    std::set<core::LinuxCpuId> service_cpus;
    for(std::size_t index=0;index<requirements.service.size();++index) {
        const auto& service=requirements.service[index];
        if(service.role!=(index==0U?"control":"measurement") || service.sibling_cpus.empty() ||
           service.stable_pus.size()!=service.sibling_cpus.size() ||
           !std::ranges::is_sorted(service.stable_pus) ||
           std::ranges::adjacent_find(service.stable_pus)!=service.stable_pus.end() ||
           !std::ranges::is_sorted(service.sibling_cpus) ||
           std::ranges::adjacent_find(service.sibling_cpus)!=service.sibling_cpus.end() ||
           !service_cores.insert(service.stable_core).second)
            return std::unexpected{registration_error("requirements_malformed")};
        for(auto cpu:service.sibling_cpus)
            if(worker_cores.contains(service.stable_core) || worker_cpus.contains(cpu) ||
               !service_cpus.insert(cpu).second)
                return std::unexpected{registration_error("requirements_malformed")};
        for(const auto& pu:service.stable_pus)
            if(worker_pus.contains(pu) || !service_pus.insert(pu).second)
                return std::unexpected{registration_error("requirements_malformed")};
        auto original_core=std::ranges::find_if(impl.context_.topology().cores(),[&](const auto& core){
            auto stable=impl.context_.stable_ids().core_id(core.id);
            return stable && stable->get()==service.stable_core;
        });
        if(original_core==impl.context_.topology().cores().end())
            return std::unexpected{registration_error("requirements_malformed")};
        std::vector<core::LinuxCpuId> original_siblings;
        std::vector<topology::StableProcessingUnitId> original_stable_pus;
        for(const auto& pu:impl.context_.topology().processing_units())
            if(pu.core_id==original_core->id) {
                original_siblings.push_back(pu.linux_cpu_id);
                auto stable=impl.context_.stable_ids().processing_unit_id(pu.id);
                if(!stable) return std::unexpected{registration_error("requirements_malformed")};
                original_stable_pus.push_back(stable->get());
            }
        std::ranges::sort(original_siblings);
        std::ranges::sort(original_stable_pus);
        if(original_siblings!=service.sibling_cpus || original_stable_pus!=service.stable_pus)
            return std::unexpected{registration_error("requirements_malformed")};
    }
      if(impl.operations_.before_registration_) {
        auto prepared=impl.operations_.before_registration_(impl.operations_.state_);
        core::detail::ExpectedErrorCleanup prepared_cleanup{prepared};
        if (!prepared) {
            const auto* underlying = retain_raw_failure(
                raw_failure ? raw_failure : impl.operations_.raw_failure_sink_, prepared.error());
            static_cast<void>(
                underlying); // Source retains the actual cause before legacy projection.
            return std::unexpected{registration_error("registration_commit_failed")};
        }
      }
      static_assert(std::is_nothrow_move_constructible_v<detail_HostSelectedResourceRequirements>);
      impl.requirements_.emplace(std::move(requirements));
      return {};
}

auto detail_HostQualificationPlanAccess::requirements(
    const RunnerContextAcquisitionSession& session) noexcept
    -> std::expected<std::reference_wrapper<const detail_HostSelectedResourceRequirements>,core::Error> {
    return requirements_fallible(session);
}
auto detail_HostQualificationPlanAccess::requirements_fallible(
    const RunnerContextAcquisitionSession& session)
    -> std::expected<std::reference_wrapper<const detail_HostSelectedResourceRequirements>,
                     core::Error> {
    if(!session.implementation_||!session.implementation_->requirements_)
        return std::unexpected{invalid_session()};
    return std::cref(*session.implementation_->requirements_);
}

auto detail_RunnerContextAcquisitionAccess::acquire_with(
    MonitoringHostInputTemplateAuthority input_template,
    RunnerContextAcquisitionOperations operations) noexcept
    -> std::expected<RunnerContextAcquisitionSession, core::Error> {
    return detail_RunnerContextAcquisitionFactory::acquire(std::move(input_template),
                                                           std::move(operations));
}
auto detail_RunnerContextAcquisitionAccess::production_operations(std::optional<core::Error>* raw_failure,
    platform_linux::detail::PreparedTopologySourceIO* prepared, const std::uint64_t* main_invocation_end)
    -> std::expected<RunnerContextAcquisitionOperations, core::Error> {
    if (!prepared || !main_invocation_end || !*main_invocation_end)
        return std::unexpected{error("monitoring.host_acquisition.invalid_operations", "missing_prepared_linux_source")};
    if (!prepared->invocation_ready())
        return std::unexpected{error("monitoring.host_acquisition.invalid_operations", "unready_prepared_linux_source")};
    return make_production_operations(raw_failure, true, prepared, main_invocation_end);
}
auto detail_RunnerContextAcquisitionAccess::acquire_with_fallible(
    MonitoringHostInputTemplateAuthority input_template, RunnerContextAcquisitionOperations operations)
    -> std::expected<RunnerContextAcquisitionSession, core::Error> {
    return detail_RunnerContextAcquisitionFactory::acquire_fallible(
        std::move(input_template), std::move(operations));
}
auto detail_RunnerContextAcquisitionAccess::acquire_context(
    RunnerContextAcquisitionOperations& operations)
    -> std::expected<runtime::RunnerContext, core::Error> {
    return operations.acquire_context_(operations.state_);
}
auto detail_RunnerContextAcquisitionAccess::observe_platform(
    RunnerContextAcquisitionOperations& operations)
    -> std::expected<TypedSysnameDisposition, core::Error> {
    return operations.observe_platform_(operations.state_);
}
auto detail_RunnerContextAcquisitionAccess::find_descriptor(
    RunnerContextAcquisitionOperations& operations, kernel::KernelId id)
    -> const kernel::KernelDescriptor* {
    return operations.find_descriptor_(operations.state_, id);
}
auto detail_RunnerContextAcquisitionAccess::reobserve(
    RunnerContextAcquisitionOperations& operations)
    -> std::expected<detail_HostRevalidationObservation, detail_HostObservationFailure> {
    return operations.reobserve_(operations.state_);
}
auto detail_RunnerContextAcquisitionAccess::before_registration(
    RunnerContextAcquisitionOperations& operations) -> std::expected<void, core::Error> {
    if (operations.before_registration_ == nullptr)
        return {};
    return operations.before_registration_(operations.state_);
}
void detail_RunnerContextAcquisitionAccess::attach_raw_failure_sink(
    RunnerContextAcquisitionOperations& operations, std::optional<core::Error>* sink) noexcept {
    operations.raw_failure_sink_ = sink;
}
auto acquire_runner_context_session(MonitoringHostInputTemplateAuthority input_template) noexcept
    -> std::expected<RunnerContextAcquisitionSession, core::Error> {
    try {
        auto operations = make_production_operations();
        if (!operations)
            return std::unexpected{std::move(operations.error())};
        return detail_RunnerContextAcquisitionFactory::acquire(std::move(input_template),
                                                               std::move(*operations));
    } catch (const std::exception&) {
        return std::unexpected{contained()};
    } catch (...) {
        return std::unexpected{contained(true)};
    }
}

} // namespace epycbench::monitoring::qualification
