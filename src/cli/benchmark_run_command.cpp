#include "cli/benchmark_run_command.hpp"
#include "cli/results_command.hpp"
#include "cli/demo_scenario.hpp"
#include "results/incremental_event_pair.hpp"
#include "epycbench/build_info_generated.hpp"
#include "runtime/scenario_monitoring_policy_projection.hpp"
#include "runtime/production_admission_policy.hpp"
#include "core/json_ownership.hpp"

#include <limits>
#include <charconv>
#include <cstdlib>
#include <ostream>
#include "epycbench/topology/serialization.hpp"

namespace epycbench::cli::benchmark_run_command {
namespace {
struct Cancelled {};
auto cancellation_requested(platform_linux::detail::ControllerSignalScope* scope) noexcept -> bool {
    return scope&&scope->observe()!=platform_linux::detail::ControllerSignal::none;
}
}

namespace {
auto run_prepared(const Arguments& arguments,runtime::ControllerProcessProductionAuthority& authority,
    std::ostream& output,std::ostream& errors) noexcept -> int {
    namespace native=platform_linux::detail;
    namespace composition=benchmark_run_composition;
    const auto diagnostic=[&](int code) noexcept {
        constexpr std::string_view message="epycbench run: failed\n";
        try {errors.write(message.data(),static_cast<std::streamsize>(message.size()));}catch(...){}
        return code;
    };
    native::ControllerSignalScope signals;
    std::optional<native::CurrentProcessImage> image;
    bool delegated{};
    const auto retire=[&]() noexcept {
        if(image)static_cast<void>(image->close());
        static_cast<void>(signals.close());
    };
    try {
        auto bounds=run_limits();if(!bounds)return diagnostic(9);
        if(!signals.acquire())return diagnostic(7);
        const auto provider=runtime::production_runner_context_provider();
        auto context=provider.acquire(provider.context);
        core::detail::ExpectedErrorCleanup context_cleanup{context};
        if(!context){const int code=context.error().category==core::ErrorCategory::topology?5:4;retire();return diagnostic(code);}
        auto identity=native::RunStartIdentity::acquire();
        if(!identity){retire();return diagnostic(7);}
        auto held=native::CurrentProcessImage::acquire();
        if(!held){retire();return diagnostic(7);}image.emplace(std::move(*held));
        auto thread=platform_linux::current_thread_id();core::detail::ExpectedErrorCleanup thread_cleanup{thread};
        if(!thread){retire();return diagnostic(7);}
        const auto& machine=context->topology().machine();
        if(machine.present_cpus.cardinality()>1024||!machine.effective_cpus.value()||
           machine.effective_cpus.value()->cardinality()>1024||machine.page_sizes.empty()) {
            retire();return diagnostic(7);
        }
        std::vector<core::LinuxCpuId> present,selected;
        present.reserve(machine.present_cpus.cardinality());selected.reserve(machine.effective_cpus.value()->cardinality());
        for(const auto cpu:machine.present_cpus)present.push_back(cpu);
        for(const auto cpu:*machine.effective_cpus.value())selected.push_back(cpu);
        const auto page=std::min_element(machine.page_sizes.begin(),machine.page_sizes.end())->value();
        if(!page||page>65536||(page&(page-1))){retire();return diagnostic(7);}
        const auto observed=+[](void*,std::array<char,30>& bytes) noexcept {
            auto time=native::acquire_event_utc();if(!time)return false;bytes=*time;return true;
        };
        HostInputOperations host{platform_linux::benchmark_run_input::native_file_operations(),
            platform_linux::benchmark_run_input::native_host_operations(),nullptr,
            +[](void*)->std::expected<results::journal::Timestamp,platform_linux::benchmark_run_input::Failure> {
                auto time=native::acquire_event_utc();
                if(!time)return std::unexpected{platform_linux::benchmark_run_input::Failure{
                    platform_linux::benchmark_run_input::Stage::clock,time.error().error_number,{}}};
                auto parsed=results::journal::Timestamp::parse({time->data(),time->size()});
                if(!parsed)return std::unexpected{platform_linux::benchmark_run_input::Failure{
                    platform_linux::benchmark_run_input::Stage::clock,0,std::move(parsed.error())}};
                return std::move(*parsed);
            }};
        auto lower=runtime::detail::production_controller_dependencies();
        runtime::detail::NativePairwiseDependencies controller{};
        controller.channels=lower.channels;controller.clock=lower.clock;controller.fatal=lower.fatal;
        controller.runner_mode=runtime::detail::NativePairwiseRunnerMode::prepared_production;
        controller.policy=runtime::detail::stage_b_policy::Selection::stage_b_v1;
        controller.authority_source=runtime::detail::PairwiseV2AuthoritySource::native;
        ControllerCheckOperations checks{*thread,static_cast<std::size_t>(page),present,selected,
            native::production_bounded_task_stat_operations(),native::production_bounded_perf_context_switch_operations(),
            native::production_bounded_sysfs_operations(),native::production_bounded_sysfs_operations(),nullptr,host.clock};
        CommandDependencies dependencies{DependencyMode::native,nullptr,host,checks,{nullptr,observed},
            {{},controller,{nullptr,observed,runtime::detail::progress::Utc::acquired},{}}};
        std::string demo_directory;
        std::string_view journal_prefix;
        if(arguments.demo) {
            auto directory=platform_linux::benchmark_run_input::prepare_demo_directory();
            if(!directory.outcome.succeeded()) {
                errors << "Could not prepare the private epycbench-demo-results directory.\n";
                retire();return diagnostic(9);
            }
            demo_directory=std::move(directory.bytes);
            journal_prefix=demo_directory;
            output << "Demo results: " << demo_directory << '\n';
            if(!output){retire();return diagnostic(3);}
        } else {
            const char* environment=std::getenv("XDG_RUNTIME_DIR");
            std::size_t length{};if(environment)while(length<=4095&&environment[length])++length;
            if(!environment||!length||length>4095){retire();return diagnostic(9);}
            journal_prefix={environment,length};
        }
        delegated=true;
        std::string final_path;
        auto report=execute(arguments,*context,authority,*identity,signals,*image,journal_prefix,
            std::move(*bounds),std::move(dependencies),&final_path);
        const auto status=composition::write_summary(report,output,errors);
        if(report.final&&report.final->publication&&
           report.final->publication->receipt.residual==results::final_document::Residual::final&&
           !final_path.empty()) {
            output << "Result saved: " << final_path << '\n';
            const auto exported=save_results_export(report.final->publication->document,
                results::result_export::Profile::summary,final_path,output,errors);
            if(!status)return exported;
        }
        return status;
    }catch(...){if(!delegated)retire();return diagnostic(1);}
}

} // namespace

auto run(int argc,char* argv[],runtime::ControllerProcessProductionAuthority& authority,
    std::ostream& output,std::ostream& errors) noexcept -> int {
    const auto diagnostic=[&](int code) noexcept {
        constexpr std::string_view message="epycbench run: failed\n";
        try {errors.write(message.data(),static_cast<std::streamsize>(message.size()));}catch(...){}
        return code;
    };
    if(argc<2||argc>6||!argv)return diagnostic(2);
    std::array<std::string_view,4> words{};
    for(int i=2;i<argc;++i) {if(!argv[i])return diagnostic(2);words[static_cast<std::size_t>(i-2)]=argv[i];}
    auto arguments=parse_arguments(std::span{words}.first(static_cast<std::size_t>(argc-2)));
    if(!arguments)return diagnostic(2);
    if(arguments->help) {
        constexpr std::string_view help="Usage: epycbench run [--module-dir DIRECTORY] [--format yaml|json] SCENARIO\n";
        try {output.write(help.data(),static_cast<std::streamsize>(help.size()));return output?0:3;}
        catch(...){return 3;}
    }
    return run_prepared(*arguments,authority,output,errors);
}

auto run_demo(runtime::ControllerProcessProductionAuthority& authority,
    std::ostream& output,std::ostream& errors) noexcept -> int {
    try {
        output << "\nFixed demo: CPU 0 worker, CPU 15 service, NUMA nodes 0 and 1.\n"
                  "Bandwidth: 64 MiB; latency: 8 MiB; four blocks per comparison.\n"
                  "Both memory nodes and both CPUs must be available.\n";
        if(!output)return 3;
        const auto status=run_prepared(demo_arguments(),authority,output,errors);
        if(status==6)errors << "The fixed demo could not resolve on this machine. "
            "It requires CPU 0, a separate service core at CPU 15, and NUMA nodes 0 and 1.\n";
        return status;
    } catch(...) {return 1;}
}

auto read_scenario(const Arguments& arguments,
    platform_linux::benchmark_run_input::FileOperations files,std::string_view demo_directory)
    -> platform_linux::benchmark_run_input::TextRead {
    namespace input=platform_linux::benchmark_run_input;
    if(!arguments.demo)return input::read_authored(arguments.scenario_path,files);
    input::TextRead result;
    if(demo_directory.empty()) {
        result.outcome.primary=input::Failure{input::Stage::admission,0,{}};
        return result;
    }
    result.bytes=demo_scenario::source;
    result.bytes.append("\noutput:\n  directory: ");
    result.bytes.append(nlohmann::json(std::string(demo_directory)).dump());
    result.bytes.append("\n  format: json\n");
    return result;
}

auto execute(const Arguments& arguments,const runtime::RunnerContext& context,
    runtime::ControllerProcessProductionAuthority& authority,const platform_linux::detail::RunStartIdentity& identity,
    platform_linux::detail::ControllerSignalScope& signals,platform_linux::detail::CurrentProcessImage& image,
    std::string_view xdg,benchmark_run_composition::RunLimits limits,CommandDependencies dependencies,
    std::string* final_path)
    -> benchmark_run_composition::RunReport {
    namespace composition=benchmark_run_composition;
    namespace fs=results::filesystem_journal;
    namespace ep=results::event_profile;
    namespace ps=results::process_subject;
    namespace prep=results::preparation_observation;
    composition::RunReport report;
    std::copy(identity.run_id().begin(),identity.run_id().end(),report.run_id.begin());
    std::optional<SessionPreparation> prepared;
    std::optional<fs::Writer> writer;
    std::optional<prep::Adapter> observer;
    std::optional<composition::Output> output;
    std::optional<results::journal::RunId> run;
    bool handed_off{};
    // Raw attempts and per-attempt UTCs outlive failure closure/publication.
    std::array<KernelEvidence,2> evidence;
    std::optional<ControllerChecks> retained_checks;
    dependencies.host.cancellation=&signals;
    dependencies.checks.cancellation=&signals;
    const auto checkpoint=[&]() {if(cancellation_requested(&signals))throw Cancelled{};};
    const auto fail=[&](int code,composition::Error kind) {
        if(report.primary)report.secondary_failure=true;
        else {report.primary=composition::Failure{kind,{},{}};report.exit_code=code;}
    };
    const auto time=[&]() -> results::journal::Timestamp {
        auto value=dependencies.host.clock(dependencies.host.clock_context);
        if(!value)throw composition::Error::source;
        return std::move(*value);
    };
    const auto retire=[&]() {
        if(handed_off)return;
        if(writer) {
            const auto closed=writer->close();report.journal=writer->receipt();
            if(closed.primary)fail(9,composition::Error::journal);
        }
        auto closed=image.close();
        if(!closed){report.image_failure=std::move(closed.error());fail(7,composition::Error::execution);}
        auto restored=signals.close();
        if(!restored){report.signal_failure=restored.error();report.signal=restored.error().facts;fail(7,composition::Error::execution);}
        else report.signal=*restored;
    };
    try {
        const auto& files=dependencies.host.files.files;
        const auto& host_calls=dependencies.host.host;
        if(arguments.help||arguments.scenario_path.empty()||
           (dependencies.mode!=DependencyMode::native&&dependencies.mode!=DependencyMode::supplied)||
           !dependencies.host.clock||!dependencies.availability.read||
           !files.open_root||!files.open_at||!files.stat_fd||!files.stat_at||!files.next||!files.read||!files.close||
           !dependencies.host.files.effective_uid||!host_calls.affinity||!host_calls.set_affinity||!host_calls.cpuid_leaf1_eax||
           !host_calls.kernel_identity||!host_calls.process_thp||
           (dependencies.mode==DependencyMode::native&&dependencies.kernel)||
           (dependencies.mode==DependencyMode::supplied&&!dependencies.run.final_identity))
            throw composition::Error::source;
        auto valid=signals.validate();
        if(!valid||!image.active())throw composition::Error::identity;
        if(signals.observe()!=platform_linux::detail::ControllerSignal::none) {
            report.execution=composition::ExecutionDisposition::interrupted;fail(8,composition::Error::execution);
            retire();return report;
        }
        auto parsed_run=results::journal::RunId::parse(identity.run_id());
        core::detail::ExpectedErrorCleanup parsed_run_cleanup{parsed_run};
        auto start=results::journal::Timestamp::parse(identity.utc_start());
        core::detail::ExpectedErrorCleanup start_cleanup{start};
        if(!parsed_run||!start)throw composition::Error::identity;
        run=std::move(*parsed_run);
        checkpoint();
        auto source_read=read_scenario(arguments,dependencies.host.files,arguments.demo?xdg:std::string_view{});
        if(!source_read.outcome.succeeded()){fail(3,composition::Error::source);retire();return report;}
        checkpoint();
        const auto input_ceiling=producer_input_limit();if(!input_ceiling)throw composition::Error::capacity;
        // Session limits are immutable. Reserve the existing finite input ceiling
        // before Session construction, then admit actual inputs at each call.
        auto policy=producer_limits(*input_ceiling);if(!policy)throw composition::Error::capacity;
        const auto inputs=[&](std::initializer_list<std::size_t> stocks) {
            auto total=preparation_inputs(std::span{stocks.begin(),stocks.size()},*input_ceiling);
            if(!total)throw composition::Error::capacity;
            return *total;
        };
        auto source=results::authored_source::retain({results::authored_source::Availability::available,
            source_read.bytes,arguments.scenario_path,arguments.format,{},{}},policy->source);
        core::detail::ExpectedErrorCleanup source_cleanup{source};
        if(!source)throw composition::Error::source;
        checkpoint();
        auto resolved=results::authored_source::resolve(*source,
            {context.topology(),context.stable_ids(),context.fingerprints()},policy->source,policy->resolution);
        core::detail::ExpectedErrorCleanup resolved_cleanup{resolved};
        if(!resolved||resolved->state()!=results::authored_source::State::resolved||!resolved->plan()) {
            fail(6,composition::Error::source);retire();return report;
        }
        checkpoint();
        const auto normalized_at=time();
        const auto& document=resolved->plan()->matrix_expanded().kernel_dispatched().memory_placement_resolved()
            .service_cpu_resolved().resource_resolved().default_expanded().value();
        auto selected_output=composition::prepare_output(document,xdg,identity);
        if(!selected_output)throw selected_output.error();
        output=std::move(*selected_output);
        if(final_path)*final_path=output->final_root+"/"+output->final_leaf;
        for(const auto* root:{&output->journal_root,&output->final_root}) {
            auto accepted=platform_linux::journal_file::File::preflight_root(*root,limits.journal.file);
            if(!accepted)throw composition::Error::output_policy;
        }
        checkpoint();
        static_cast<void>(inputs({source_read.bytes.size(),source->logical_bytes(),resolved->usage().bytes}));
        prepared.emplace(prepare_session(*run,*start,*source,context,dependencies.host,*policy));
        if(prepared->failure||!prepared->session)throw composition::Error::source;
        auto& session=*prepared->session;
        auto created=fs::Writer::create(output->journal_root,*run,limits.final.events,limits.journal,fs::Durability::process);
        if(!created){report.primary=composition::Failure{composition::Error::journal,created.error(),{}};report.exit_code=9;retire();return report;}
        writer=std::move(*created);
        const bool native=dependencies.mode==DependencyMode::native;
        auto admitted=prep::Adapter::prepare(session,*writer,limits.final.events,native,native,
            native?dependencies.availability:prep::TimestampSource{});
        if(!admitted)throw composition::Error::journal;
        observer.emplace(std::move(*admitted));
        const auto& creation_facts=*session.entries().front().run_creation->facts();
        const bool topology_retained=creation_facts.host&&creation_facts.host->categories()&&
            std::holds_alternative<results::topology_evidence::Owner>(creation_facts.host->categories()->topology);
        if(!topology_retained) {
            // The resolved input is not a retained normalization without its
            // original Topology owner. C4 must publish the truthful absent ID.
            output->final_leaf=std::string{"unresolved__"}.append(identity.utc_start()).append("__")
                .append(identity.run_id()).append(".epycbench.json");
            if(final_path)*final_path=output->final_root+"/"+output->final_leaf;
            if(!observer->creation_failures(time()))throw composition::Error::journal;
            if(prepared->cancelled)throw Cancelled{};
            throw composition::Error::source;
        }
        // Resolution already completed before Host acquisition. Retain that
        // original result/time even when a later Host call failed; delivery of
        // both retained children occurs now, without re-resolution/backdating.
        ps::Input input{ps::Domain::supplied_observation,2,normalized_at,ps::Producer::controller,ps::NoCreation{}, {},{},
            ps::Normalization{std::move(*resolved),1}};
        static_cast<void>(inputs({source_read.bytes.size(),source->logical_bytes(),session.usage().retained_bytes,
            input.normalization->source.usage().bytes}));
        auto normalized=session.append(input);core::detail::ExpectedErrorCleanup normalized_cleanup{normalized};
        if(!normalized)throw composition::Error::source;
        const auto& retained_resolution=session.entries().back().normalization->source;
        const auto& scenario=*retained_resolution.plan();
        auto normalization=session.serialize_normalization(2,limits.final.events.child.normalization);
        if(!normalization||!normalization->topology())throw composition::Error::source;
        const auto retained_at=time();
        if(!writer->append(ep::Input{retained_at,normalization->topology(),ep::Reference{1,0,ep::RefKind::creation},{},{}})||
           !writer->append(ep::Input{retained_at,&*normalization,ep::Reference{1,0,ep::RefKind::creation},
                ep::Reference{2,0,ep::RefKind::topology},{}}))throw composition::Error::journal;
        if(!observer->creation_failures(time()))throw composition::Error::journal;
        if(!creation_facts.failures.empty())throw composition::Error::source;
        checkpoint();
        auto selection_limits=producer_limits(inputs({source_read.bytes.size(),source->logical_bytes(),
            session.usage().retained_bytes,normalization->usage().retained_bytes}));
        if(!selection_limits)throw composition::Error::capacity;
        auto selection=observer->selection(time(),std::move(*normalization),selection_limits->selection);
        if(!selection||observer->failure())throw composition::Error::source;
        std::array<bool,2> used{};
        for(const auto& binding:selection->bindings()) {
            const auto& workload=selection->workloads()[binding.workload_index];
            const auto family=results::build_object_association::detail::family_index(workload.semantic,workload.implementation);
            if(!family||*family>=used.size())throw composition::Error::source;
            used[*family]=true;
        }
        std::size_t kernel_bytes{};
        results::build_object_association::ApprovalInputs approvals;
        // Normal builds have already run the automatic kernel checks. Historical
        // evidence is accepted only through the supplied compatibility fixture path.
        for(std::size_t i{};i<used.size();++i)if(used[i]&&dependencies.kernel) {
            checkpoint();
            const auto family=i==0?results::kernel_report::Family::bandwidth:results::kernel_report::Family::latency;
            if(!acquire_kernel_evidence(*observer,time(),evidence[i],dependencies.mode,dependencies.kernel,family,
                dependencies.host.files,*policy,kernel_bytes,&signals))throw composition::Error::source;
            if(evidence[i].cancelled)throw Cancelled{};
            if(!evidence[i].approval)throw composition::Error::source;
            checkpoint();
            approvals[i]=std::move(evidence[i].approval);
        }
        checkpoint();
        auto base=session.serialize_base_build(1,limits.final.events.child.creation.base_build);
        if(!base)throw composition::Error::source;
        std::size_t approval_stock{};
        for(const auto& approval:approvals)if(approval)
            approval_stock=inputs({approval_stock,approval->usage().retained_bytes});
        auto association_limits=producer_limits(inputs({source_read.bytes.size(),source->logical_bytes(),
            session.usage().retained_bytes,selection->usage().retained_bytes,base->usage().retained_bytes,
            approval_stock,kernel_bytes}));
        if(!association_limits)throw composition::Error::capacity;
        auto association=observer->association(time(),std::move(*selection),std::move(*base),std::move(approvals),association_limits->association);
        if(!association||observer->failure())throw composition::Error::source;
        checkpoint();
        auto plan_limits=producer_limits(inputs({source_read.bytes.size(),source->logical_bytes(),
            session.usage().retained_bytes,association->usage().retained_bytes,kernel_bytes}));
        if(!plan_limits)throw composition::Error::capacity;
        auto plan=observer->execution_plan(time(),std::move(*association),plan_limits->plan);
        if(!plan||observer->failure())throw composition::Error::source;
        if(!observer->planning(time(),*plan))throw composition::Error::journal;
        checkpoint();
        const auto preflight_start=time();
        auto monitoring=runtime::detail::project_scenario_monitoring_inputs(document);
        core::detail::ExpectedErrorCleanup monitoring_cleanup{monitoring};
        if(!monitoring||!*monitoring)throw composition::Error::source;
        const auto& m=**monitoring;
        auto monitoring_policy=runtime::detail::project_production_monitoring_policy({m.required,m.optional,m.before,m.after,m.during,m.interval,m.unavailable});
        core::detail::ExpectedErrorCleanup monitoring_policy_cleanup{monitoring_policy};
        if(!monitoring_policy)throw composition::Error::source;
        struct CheckInvocation {
            std::optional<ControllerChecks>& retained;const results::journal::RunId& run;
            const monitoring::MonitoringExecutionPolicy& policy;const ControllerCheckOperations& operations;
            const ProducerLimits& limits;
        } call{retained_checks,*run,*monitoring_policy,dependencies.checks,*policy};
        const auto captured=observer->controller_checks(preflight_start,&call,
            +[](void* pointer,const ps::Session&)->prep::CheckCallResult {
                auto& invocation=*static_cast<CheckInvocation*>(pointer);
                invocation.retained.emplace(controller_checks(invocation.run,1,invocation.policy,
                    invocation.operations,invocation.limits.serialization.events.composition->preflight.monitoring));
                return capture_controller_checks(*invocation.retained,invocation.limits);
            });
        if(!retained_checks)throw composition::Error::source;
        auto& checks=*retained_checks;
        if(checks.cancelled)throw Cancelled{};
        if(checks.failed_channel||!checks.admission.succeeded())throw composition::Error::source;
        if(checks.assessment_error)throw composition::Error::output_policy;
        if(!captured||observer->failure())throw composition::Error::journal;
        checkpoint();
        const auto preflight_end=time();
        const auto& creation=*session.entries().front().run_creation->facts();
        const auto& host=*creation.host;
        std::array<results::preflight::TemperatureFact,1> temperatures;
        std::size_t temperature_count{};
        if(const auto* thermal=std::get_if<platform_linux::benchmark_run_input::TemperatureRead>(&checks.observations[2]);
           thermal&&thermal->outcome.succeeded()) {
            auto& fact=temperatures[0];
            fact.observed_at=checks.observed_at[2]?&*checks.observed_at[2]:nullptr;
            if(thermal->supported&&thermal->millicelsius&&fact.observed_at) {
                fact.state=results::preflight::ThermalState::available;
                fact.value=monitoring::TemperatureSnapshot::create(monitoring::TemperatureSensorKind::cpu_control,
                    monitoring::TemperatureMilliCelsius::from_millicelsius(*thermal->millicelsius));
            } else {
                fact.state=results::preflight::ThermalState::unavailable;
                fact.reason=results::preflight::ThermalReason::not_supported;
            }
            temperature_count=1;
        }
        results::preflight::Input preflight{{1,2,1,1},preflight_start,preflight_end,{},std::move(checks.assessment),
            std::span{temperatures}.first(temperature_count),{},{}};
        results::preflight::detail::Context preflight_context{session.run_id(),
            std::get<results::topology_evidence::Owner>(host.categories()->topology),host,
            retained_resolution,&*creation.build->manifest,true};
        static_cast<void>(inputs({source_read.bytes.size(),source->logical_bytes(),session.usage().retained_bytes,
            plan->usage().retained_bytes,kernel_bytes,checks.retained_bytes,
            preflight.monitoring?preflight.monitoring->usage().retained_bytes:0}));
        auto inventory=results::preflight::detail::declarations(preflight_context,preflight,policy->session.preflight,0,session.usage().retained_bytes);
        if(!inventory)throw composition::Error::capacity;
        std::vector<results::journal::Timestamp> times;times.reserve(inventory->rows.size());
        std::vector<results::preflight::CheckDeclaration> declarations;declarations.reserve(inventory->rows.size());
        for(const auto& row:inventory->rows) {
            const results::journal::Timestamp* checked{};
            if(row.checked_at) {
                auto stamp=results::journal::Timestamp::parse(*row.checked_at);core::detail::ExpectedErrorCleanup cleanup{stamp};
                if(!stamp)throw composition::Error::source;
                times.push_back(std::move(*stamp));checked=&times.back();
            } else if(!row.time_locked)checked=&preflight_end;
            declarations.push_back({row.check,row.locked_evaluation.value_or(results::preflight::Evaluation::evaluated),checked});
        }
        preflight.declarations=declarations;
        auto assessed=observer->preflight(3,preflight_end,preflight,&checks.snapshot);core::detail::ExpectedErrorCleanup assessed_cleanup{assessed};
        if(!assessed||observer->failure())throw composition::Error::capacity;
        const auto preflight_status=session.entries().back().preflight->facts()->status;
        if(preflight_status!=results::preflight::Status::accepted&&
           preflight_status!=results::preflight::Status::accepted_with_warnings)
            throw composition::Error::output_policy;
        checkpoint();
        handed_off=true;
        return composition::execute(session,*plan,scenario,context,authority,identity,signals,image,xdg,
            std::move(limits),std::move(dependencies.run),*writer,*observer);
    } catch(const Cancelled&) {
        if(!report.primary)report.execution=composition::ExecutionDisposition::interrupted;
        fail(8,composition::Error::execution);
    } catch(composition::Error error){fail(error==composition::Error::source?7:9,error);}
      catch(...){fail(9,composition::Error::capacity);}
    // Failed preparation retains its prefix. Close only when an actually
    // observed primary exists; absence never fabricates a failure or a plan.
    if(writer&&observer&&!observer->failure()&&output&&run)try {
        nlohmann::json primary=nullptr,secondary=nlohmann::json::array();
        core::detail::JsonCleanup primary_cleanup{primary},secondary_cleanup{secondary};
        bool planned{};
        for(const auto& record:writer->records()) {
            planned|=record.kind==ep::Kind::planning;
            if(record.kind!=ep::Kind::failure||record.scope!=ep::Scope{})continue;
            nlohmann::json ref{{"sequence",std::to_string(record.sequence)},{"ordinal","0"},{"kind","failure"}};
            core::detail::JsonCleanup ref_cleanup{ref};
            if(record.event_facts&&record.event_facts->at("role")=="primary")primary=ref;
            else secondary.push_back(ref);
        }
        if(!planned&&!primary.is_null()) {
            nlohmann::json closed{{"origin","actual_availability"},{"execution","failed"},{"plan_disposition","unavailable"},
                {"pairs",nlohmann::json::array()},{"primary_ref",primary},{"secondary_refs",secondary}};
            core::detail::JsonCleanup closed_cleanup{closed};
            if(!observer->close(time(),closed))throw composition::Error::journal;
        }
        auto final_identity=dependencies.run.final_identity?
            platform_linux::detail::RunStartIdentityAccess::acquire(dependencies.run.final_identity->clock,dependencies.run.final_identity->entropy):
            platform_linux::detail::RunStartIdentity::acquire();
        if(!final_identity||final_identity->run_id()==identity.run_id())throw composition::Error::finalization;
        auto final_id=results::journal::RunId::parse(final_identity->run_id());core::detail::ExpectedErrorCleanup id_cleanup{final_id};
        auto final_time=results::journal::Timestamp::parse(final_identity->utc_start());core::detail::ExpectedErrorCleanup time_cleanup{final_time};
        if(!final_id||!final_time||writer->receipt().written_bytes>=limits.total_written_bytes)throw composition::Error::finalization;
        auto publication=limits.journal.file;
        publication.file_bytes=std::min(publication.file_bytes,limits.total_written_bytes-writer->receipt().written_bytes);
        report.final=composition::finalize(*writer,*output,*run,{*final_id,*final_time},limits.journal,limits.final,publication);
        if(report.final->primary)fail(9,composition::Error::finalization);
    } catch(...){fail(9,composition::Error::finalization);}
    retire();return report;
}

auto retain_current_build(results::build_manifest::Limits limits)
    -> std::expected<results::build_manifest::Owner,core::Error> {
    namespace build=results::build_manifest;
    std::array<build::Feature,6> features{build::Feature::linux_platform,build::Feature::cxx23};
    std::size_t count=2;
    if(build_info::feature_testing)features[count++]=build::Feature::testing;
    if(build_info::feature_asan_ubsan)features[count++]=build::Feature::asan_ubsan;
    if(build_info::feature_tsan)features[count++]=build::Feature::tsan;
    if(build_info::feature_native_reference)features[count++]=build::Feature::native_reference;
    return build::retain({build_info::project_version,{build_info::git_revision,build_info::git_dirty},
        {build_info::compiler_id,build_info::compiler_version,build_info::compiler_target,"C++23",build_info::compiler_flags},
        {build_info::linker_id,build_info::linker_version},build_info::preset,
        std::span{features}.first(count),
        {build_info::hwloc_version,build_info::numa_version,build_info::libfyaml_version,
         build_info::cli11_version,build_info::nlohmann_json_version,build_info::fmt_version,build_info::catch2_version},
        build_info::build_identity},limits);
}
namespace {
namespace host_input=platform_linux::benchmark_run_input;
namespace subject=results::process_subject;
auto host_error(std::string_view code,const host_input::Failure* failure=nullptr) -> core::Error {
    if(failure&&failure->detail)return *failure->detail;
    return {core::ErrorCategory::results,core::StableErrorCode{std::string{code}},
        "Controller preparation source failed","acquire_host",std::nullopt,
        failure?nlohmann::json{{"stage",static_cast<unsigned>(failure->stage)},{"error_number",failure->error_number}}:
            nlohmann::json::object(),core::ErrorSeverity::error,core::ContinuationPolicy::abort_command};
}
auto source_token(std::string_view bytes) -> std::string_view {
    while(!bytes.empty()&&(bytes.back()=='\n'||bytes.back()=='\r'||bytes.back()==' '||bytes.back()=='\t'))bytes.remove_suffix(1);
    while(!bytes.empty()&&(bytes.front()==' '||bytes.front()=='\t'))bytes.remove_prefix(1);
    return bytes;
}
auto selected_token(std::string_view bytes) -> std::optional<std::string_view> {
    bytes=source_token(bytes);const auto first=bytes.find('['),last=bytes.find(']');
    if(first==bytes.npos||last==bytes.npos||last<=first+1||bytes.find('[',first+1)!=bytes.npos||
       bytes.find(']',last+1)!=bytes.npos)return std::nullopt;
    return bytes.substr(first+1,last-first-1);
}
auto source_uint(std::string_view bytes,int base=10) -> std::optional<std::uint32_t> {
    bytes=source_token(bytes);if(base==16&&bytes.starts_with("0x"))bytes.remove_prefix(2);
    std::uint32_t value{};const auto parsed=std::from_chars(bytes.data(),bytes.data()+bytes.size(),value,base);
    if(bytes.empty()||parsed.ec!=std::errc{}||parsed.ptr!=bytes.data()+bytes.size())return std::nullopt;
    return value;
}
struct HostTime {
    HostInputOperations& operations;
    std::span<std::optional<results::journal::Timestamp>> values;
    std::size_t count{};
    auto callback() noexcept -> host_input::ObservationTime {
        return {this,[](void* p)->std::expected<void,host_input::Failure> {
            auto& state=*static_cast<HostTime*>(p);
            if(state.count==state.values.size())return std::unexpected{host_input::Failure{host_input::Stage::clock,EOVERFLOW,{}}};
            auto& slot=state.values[state.count++];
            auto time=state.operations.clock(state.operations.clock_context);
            if(!time)return std::unexpected{std::move(time.error())};
            slot=std::move(*time);return {};
        }};
    }
};
}
auto acquire_host(const results::journal::RunId& run,const runtime::RunnerContext& context,
    HostInputOperations operations,const ProducerLimits& limits) -> HostEvidence {
    namespace controls=results::host_controls;
    namespace kernel=results::private_kernel_identity;
    namespace pu=results::pu_cpu_identity;
    namespace thp=results::anonymous_thp_policy_owner;
    namespace process=results::process_thp_restriction;
    HostEvidence result;
    constexpr std::size_t source_limit=16U*1024U*1024U;
    const auto& bounds=limits.serialization.events.child.creation.host;
    const auto& machine=context.topology().machine();
    if(!operations.clock||!operations.host.affinity||!operations.host.set_affinity||
       !operations.host.cpuid_leaf1_eax||!operations.host.kernel_identity||!operations.host.process_thp||
       !operations.files.files.open_root||!operations.files.files.open_at||!operations.files.files.stat_fd||
       !operations.files.files.read||!operations.files.files.close||!operations.files.files.next||!operations.files.files.stat_at||
       !operations.files.effective_uid||!machine.effective_cpus.value()||
       context.topology().processing_units().size()>1024||context.topology().numa_nodes().size()>64||
       machine.present_cpus.cardinality()>1024||machine.effective_cpus.value()->cardinality()>1024) {
        result.retention_failure=host_error("results.preparation.host_admission");return result;
    }
    // Fixed creation/error and observation stock is allocated before acquisition.
    result.failures.reserve(16);result.bindings.reserve(16);
    bool stopped{};
    const auto cancelled=[&]() {result.cancelled=cancellation_requested(operations.cancellation);return result.cancelled;};
    const auto record=[&](subject::Target target,const core::Error& error,bool secondary=false)->std::uint64_t {
        if(result.failures.size()==16)throw host_error("results.preparation.host_capacity");
        const auto& error_bound=limits.serialization.events.child.creation.error;
        auto retained_error=results::private_error::retain(error,{error_bound.context_nodes,error_bound.context_depth,
            error_bound.string_bytes,error_bound.binary_bytes,error_bound.retained_bytes});
        if(!retained_error)throw std::move(retained_error.error());
        const auto ordinal=static_cast<std::uint64_t>(result.failures.size());
        subject::FailureFact fact{std::move(*retained_error),subject::Producer::controller,{},{},{target},{}};
        if(target.category==subject::Category::process_thp&&target.selector==subject::Selector::process_observation)
            fact.process_sequence=1;
        if(!result.failures.empty()&&result.failures.front().process_sequence==fact.process_sequence) {
            fact.secondary_of=1;fact.secondary_creation_ordinal=0;
        }
        if(!secondary)result.bindings.push_back({target,ordinal});
        result.failures.push_back(std::move(fact));stopped=true;return ordinal;
    };
    const auto failed=[&](subject::Target target,const host_input::Outcome& outcome)->std::uint64_t {
        const auto primary=outcome.primary?host_error("results.preparation.host_source",&*outcome.primary):
            host_error("results.preparation.host_source");
        static_cast<void>(record(target,primary));
        for(std::size_t i=0;i<outcome.cleanup_count;++i)
            static_cast<void>(record(target,host_error("results.preparation.host_cleanup",&outcome.cleanup[i]),true));
        return 1;
    };
    const auto failure_time=[&](std::optional<results::journal::Timestamp>& value,
        HostTime& clock,subject::Target target) {
        if(value||clock.count)return;
        try {
            auto now=operations.clock(operations.clock_context);
            if(now)value=std::move(*now);
            else static_cast<void>(record(target,host_error("results.preparation.host_clock",&now.error()),true));
        } catch(...) {static_cast<void>(record(target,host_error("results.preparation.host_clock"),true));}
    };
    const auto debit=[&](std::size_t bytes) {
        if(bytes>source_limit-result.source_bytes)throw host_error("results.preparation.host_capacity");
        result.source_bytes+=bytes;
    };
    const auto retain_category=[&](auto retained,auto& slot,subject::Category category) {
        if(retained)slot=std::move(*retained);
        else {
            static_cast<void>(record({category,subject::Selector::owner_preparation},retained.error()));
            slot=results::host_assembly::Failed{1};
        }
    };
    try {
        if(cancelled())return result;
        auto serialized=topology::serialize_topology(context.topology(),context.stable_ids(),context.fingerprints());
        if(!serialized) {
            static_cast<void>(record({subject::Category::topology,subject::Selector::owner_preparation},serialized.error()));
            result.categories.topology=results::host_assembly::Failed{1};
            return result;
        }
        auto retained=results::topology_evidence::retain(serialized->canonical_json,
            results::topology_evidence::supported_limits(),context.fingerprints());
        retain_category(std::move(retained),result.categories.topology,subject::Category::topology);
        if(stopped||cancelled())return result;
        const auto& topology_owner=std::get<results::topology_evidence::Owner>(result.categories.topology);
        std::array<std::optional<results::journal::Timestamp>,5> control_times;
        controls::Input control_input{run,1,controls::Producer::controller,{},&topology_owner};
        constexpr std::array selectors{subject::Selector::smt_control,subject::Selector::smt_active,
            subject::Selector::numa_mask,subject::Selector::thp_enabled,subject::Selector::thp_defrag};
        for(std::size_t i=0;i<5&&!stopped;++i) {
            if(cancelled())break;
            HostTime time{operations,std::span{control_times}.subspan(i,1)};
            auto read=host_input::read_source(static_cast<host_input::SourceFile>(i),0,operations.files,time.callback());
            debit(read.bytes.size());auto& slot=control_input.observations[i];
            bool parsed=false;const auto token=source_token(read.bytes);
            if(read.outcome.succeeded()&&control_times[i]) {
                if(i==0) {
                    constexpr std::array names{"on","off","forceoff","notsupported","notimplemented"};
                    for(std::size_t j=0;j<names.size();++j)if(token==names[j]) {
                        slot.value=controls::SmtControl{static_cast<controls::SmtMode>(j),{}};parsed=true;
                    }
                    if(!parsed)if(auto threads=source_uint(token);threads&&*threads) {
                        slot.value=controls::SmtControl{controls::SmtMode::threads,*threads};parsed=true;
                    }
                } else if(i==1) {if(token=="0"||token=="1"){slot.value=token=="1";parsed=true;}}
                else if(i==2) {if(auto mask=source_uint(token);mask&&*mask<=3){slot.value=*mask;parsed=true;}}
                else if(auto selected=selected_token(token)) {
                    if(i==3) {
                        constexpr std::array names{"always","madvise","never"};
                        for(std::size_t j=0;j<names.size();++j)if(*selected==names[j]){slot.value=static_cast<controls::ThpEnabled>(j);parsed=true;}
                    } else {
                        constexpr std::array names{"always","defer","defer+madvise","madvise","never"};
                        for(std::size_t j=0;j<names.size();++j)if(*selected==names[j]){slot.value=static_cast<controls::ThpDefrag>(j);parsed=true;}
                    }
                }
            }
            if(parsed){slot.state=controls::State::available;slot.observed_at=&*control_times[i];}
            else {
                const auto reason=read.outcome.succeeded()?subject::FailureReason::malformed_source:subject::FailureReason::read_failed;
                slot.state=controls::State::failed;slot.value={};slot.reason=read.outcome.succeeded()?controls::Reason::malformed_source:controls::Reason::read_failed;
                const subject::Target target{subject::Category::system_controls,selectors[i],1,0,reason};
                slot.failure_event=failed(target,read.outcome);
                failure_time(control_times[i],time,target);if(control_times[i])slot.observed_at=&*control_times[i];
            }
        }
        retain_category(controls::retain(control_input,bounds.system_controls.controls),result.categories.system_controls,subject::Category::system_controls);
        if(stopped||cancelled())return result;
        std::array<std::optional<results::journal::Timestamp>,1> kernel_time;
        HostTime kt{operations,kernel_time};auto raw_kernel=host_input::read_kernel_identity(operations.host,kt.callback());
        kernel::Input kernel_input{run,1,kernel::Producer::controller,{},&topology_owner};
        if(raw_kernel.fields)for(const auto& field:*raw_kernel.fields)debit(field.size());
        if(raw_kernel.outcome.succeeded()&&raw_kernel.fields&&kernel_time[0]&&raw_kernel.fields->at(0)=="Linux") {
            kernel_input.observations[0]={kernel::State::available,kernel::SystemName::linux_kernel,&*kernel_time[0],{}, {}};
            for(std::size_t i=1;i<3;++i)kernel_input.observations[i]={kernel::State::available,
                std::string_view{raw_kernel.fields->at(i)},&*kernel_time[0],{}, {}};
        } else {
            const auto ordinal=failed({subject::Category::kernel_identity,subject::Selector::owner_preparation},raw_kernel.outcome);
            result.categories.kernel_identity=results::host_assembly::Failed{ordinal};return result;
        }
        retain_category(kernel::retain(kernel_input,bounds.kernel_identity.kernel_identity),result.categories.kernel_identity,subject::Category::kernel_identity);
        if(stopped||cancelled())return result;
        std::vector<pu::Row> pu_rows;pu_rows.reserve(context.topology().processing_units().size());
        std::vector<std::optional<results::journal::Timestamp>> pu_times(context.topology().processing_units().size()*2);
        for(const auto& processing_unit:context.topology().processing_units()) {
            if(!machine.effective_cpus.value()->contains(processing_unit.linux_cpu_id))continue;
            const auto stable=context.stable_ids().processing_unit_id(processing_unit.id);
            if(!stable)throw host_error("results.preparation.host_topology");
            const auto position=pu_rows.size();pu_rows.push_back({stable->get(),processing_unit.linux_cpu_id,{},{}});
            if(stopped||cancelled())continue;
            auto& row=pu_rows.back();HostTime time{operations,std::span{pu_times}.subspan(position*2,1)};
            auto signature=host_input::read_cpu_identity(row.linux_cpu,operations.host,time.callback());
            if(signature.outcome.succeeded()&&signature.eax&&pu_times[position*2])
                row.signature={pu::State::available,pu::Signature{*signature.eax},&*pu_times[position*2],{}, {}};
            else {
                row.signature={pu::State::failed,{},nullptr,pu::Reason::read_failed,
                    failed({subject::Category::pu_identity,subject::Selector::signature,1,row.linux_cpu.value(),subject::FailureReason::read_failed},signature.outcome)};
                failure_time(pu_times[position*2],time,{subject::Category::pu_identity,subject::Selector::signature,1,
                    row.linux_cpu.value(),subject::FailureReason::read_failed});
                if(pu_times[position*2])row.signature.observed_at=&*pu_times[position*2];
                continue;
            }
            if(cancelled())continue;
            HostTime micro_time{operations,std::span{pu_times}.subspan(position*2+1,1)};
            auto microcode=host_input::read_source(host_input::SourceFile::microcode,row.linux_cpu.value(),operations.files,micro_time.callback());
            debit(microcode.bytes.size());const auto revision=source_uint(microcode.bytes,16);
            if(microcode.outcome.succeeded()&&revision&&pu_times[position*2+1])
                row.microcode={pu::State::available,pu::Microcode{*revision},&*pu_times[position*2+1],{}, {}};
            else {
                const bool malformed=microcode.outcome.succeeded();
                row.microcode={pu::State::failed,{},nullptr,malformed?pu::Reason::malformed_source:pu::Reason::read_failed,
                    failed({subject::Category::pu_identity,subject::Selector::microcode,1,row.linux_cpu.value(),
                        malformed?subject::FailureReason::malformed_source:subject::FailureReason::read_failed},microcode.outcome)};
                failure_time(pu_times[position*2+1],micro_time,{subject::Category::pu_identity,subject::Selector::microcode,1,
                    row.linux_cpu.value(),malformed?subject::FailureReason::malformed_source:subject::FailureReason::read_failed});
                if(pu_times[position*2+1])row.microcode.observed_at=&*pu_times[position*2+1];
            }
        }
        retain_category(pu::retain({run,1,pu::Producer::controller,topology_owner,pu_rows},bounds.pu_identity.identity),
            result.categories.pu_identity,subject::Category::pu_identity);
        if(stopped||cancelled())return result;
        namespace frequency=results::frequency_policy_owner;
        std::vector<core::LinuxCpuId> present,selected;
        present.reserve(1024);selected.reserve(1024);
        for(const auto cpu:machine.present_cpus)present.push_back(cpu);
        for(const auto cpu:*machine.effective_cpus.value())selected.push_back(cpu);
        if(present.size()>1024||selected.size()>1024)throw host_error("results.preparation.host_capacity");
        std::array<std::optional<results::journal::Timestamp>,1024> frequency_times;
        HostTime ft{operations,frequency_times};
        auto raw_frequency=host_input::read_frequency(present,selected,operations.files.files,
            ft.callback(),{source_limit-result.source_bytes,{}});
        debit(raw_frequency.source_bytes);
        if(!raw_frequency.outcome.succeeded()) {
            auto error=host_error("results.preparation.host_frequency",
                raw_frequency.outcome.primary?&*raw_frequency.outcome.primary:nullptr);
            if(raw_frequency.source_failure)error.context["source_failure"]={
                {"kind",static_cast<unsigned>(raw_frequency.source_failure->primary.kind)},
                {"native",raw_frequency.source_failure->primary.native},
                {"disposition",static_cast<unsigned>(raw_frequency.source_failure->primary.disposition)}};
            error.context["source_cleanup"]=nlohmann::json::array();
            for(const auto& cleanup:raw_frequency.source_cleanup)error.context["source_cleanup"].push_back({
                {"kind",static_cast<unsigned>(cleanup.kind)},{"native",cleanup.native},
                {"disposition",static_cast<unsigned>(cleanup.disposition)}});
            error.context["retirement"]=nlohmann::json::array();
            for(const auto& close:raw_frequency.retirement)error.context["retirement"].push_back({
                {"attempted",close.attempted},{"kind",static_cast<unsigned>(close.kind)},{"native",close.native_error}});
            static_cast<void>(record({subject::Category::frequency,subject::Selector::owner_preparation},error));
            result.categories.frequency=results::host_assembly::Failed{1};return result;
        }
        std::vector<monitoring::FrequencyPolicyMetadata> metadata;metadata.reserve(raw_frequency.domains.size());
        std::vector<frequency::PolicyInput> policies;policies.reserve(raw_frequency.domains.size());
        std::vector<frequency::UncoveredInput> uncovered;uncovered.reserve(selected.size());
        for(std::size_t i=0;i<raw_frequency.domains.size();++i) {
            const auto& domain=raw_frequency.domains[i];
            if(!frequency_times[i]||domain.membership_offset>raw_frequency.memberships.size()||
               domain.membership_count>raw_frequency.memberships.size()-domain.membership_offset)
                throw host_error("results.preparation.host_frequency");
            const auto members=std::span{raw_frequency.memberships}.subspan(domain.membership_offset,domain.membership_count);
            auto id=monitoring::FrequencyDomainId::create({members.begin(),members.end()});
            if(!id)throw std::move(id.error());
            metadata.push_back(monitoring::FrequencyPolicyMetadata::create(std::move(*id),
                domain.kind==platform_linux::CpuFrequencyCurrentKind::hardware_current?
                    monitoring::FrequencySourceKind::hardware_current:monitoring::FrequencySourceKind::scaling_current,
                monitoring::FrequencyDriverId::create(domain.driver),monitoring::FrequencyGovernorId::create(domain.governor),
                monitoring::FrequencyHz::from_hz(domain.minimum_hz),monitoring::FrequencyHz::from_hz(domain.maximum_hz)));
            policies.push_back({metadata.back(),*frequency_times[i]});
        }
        std::optional<results::journal::Timestamp> coverage_time;
        if(raw_frequency.domains.empty()||std::ranges::any_of(selected,[&](auto cpu) {
            return std::ranges::find(raw_frequency.memberships,cpu)==raw_frequency.memberships.end();
        })) {
            auto now=operations.clock(operations.clock_context);
            if(!now) {
                host_input::Outcome outcome;outcome.primary=std::move(now.error());
                result.categories.frequency=results::host_assembly::Failed{
                    failed({subject::Category::frequency,subject::Selector::owner_preparation},outcome)};
                return result;
            }
            coverage_time=std::move(*now);
        }
        for(auto cpu:selected)if(std::ranges::find(raw_frequency.memberships,cpu)==raw_frequency.memberships.end())
            uncovered.push_back({cpu,frequency::State::unavailable,&*coverage_time,frequency::Reason::source_missing,{}});
        retain_category(frequency::retain({run,1,frequency::Producer::controller,topology_owner,policies,uncovered},bounds.frequency.source),
            result.categories.frequency,subject::Category::frequency);
        if(stopped||cancelled())return result;
        if(54U*4096U>source_limit-result.source_bytes)throw host_error("results.preparation.host_capacity");
        std::array<std::optional<results::journal::Timestamp>,55> thp_times;
        HostTime tt{operations,thp_times};auto inventory=host_input::read_thp_inventory(operations.files,tt.callback());
        std::vector<thp::RowInput> thp_rows;thp_rows.reserve(inventory.rows.size());
        thp::EnumerationInput enumeration;
        std::optional<std::uint64_t> row_failure;
        for(std::size_t i=0;i<inventory.rows.size();++i) {
            const auto& row=inventory.rows[i];debit(row.enabled.bytes.size());
            thp::RowInput mapped{core::ByteCount::from_base_units(row.bytes)};
            if(row.attempted) {
                const auto token=selected_token(row.enabled.bytes);std::optional<thp::Mode> mode;
                if(token) {
                    constexpr std::array names{"always","madvise","never","inherit"};
                    for(std::size_t j=0;j<names.size();++j)if(*token==names[j])mode=static_cast<thp::Mode>(j);
                }
                if(row.enabled.outcome.succeeded()&&mode&&thp_times[i]) {
                    mapped.state=thp::State::available;mapped.mode=mode;mapped.observed_at=&*thp_times[i];
                } else {
                    const bool malformed=row.enabled.outcome.succeeded();mapped.state=thp::State::failed;
                    mapped.reason=malformed?thp::Reason::malformed_source:thp::Reason::read_failed;
                    mapped.failure_event=failed({subject::Category::anonymous_thp,subject::Selector::page_size,1,row.bytes,
                        malformed?subject::FailureReason::malformed_source:subject::FailureReason::read_failed},row.enabled.outcome);
                    if(!row_failure)row_failure=result.bindings.back().failure_ordinal;
                    HostTime failed_time{operations,std::span{thp_times}.subspan(i,1)};
                    failure_time(thp_times[i],failed_time,{subject::Category::anonymous_thp,subject::Selector::page_size,1,row.bytes,
                        malformed?subject::FailureReason::malformed_source:subject::FailureReason::read_failed});
                    if(thp_times[i])mapped.observed_at=&*thp_times[i];
                }
            }
            thp_rows.push_back(mapped);
        }
        if(inventory.enumeration_complete&&inventory.outcome.succeeded()&&!row_failure&&tt.count&&thp_times[tt.count-1])
            enumeration={thp::EnumerationState::complete,&*thp_times[tt.count-1],{}, {}};
        else {
            enumeration.state=thp::EnumerationState::failed;enumeration.reason=thp::Reason::read_failed;
            const subject::Target target{subject::Category::anonymous_thp,subject::Selector::enumeration,1,0,
                subject::FailureReason::read_failed};
            if(row_failure) {
                enumeration.failure_event=1;
                result.failures[static_cast<std::size_t>(*row_failure)].targets.push_back(target);
                result.bindings.push_back({target,*row_failure});
                for(std::size_t i=0;i<inventory.outcome.cleanup_count;++i)
                    static_cast<void>(record(target,host_error("results.preparation.host_cleanup",&inventory.outcome.cleanup[i]),true));
            } else enumeration.failure_event=failed(target,inventory.outcome);
            HostTime failed_time{operations,std::span{thp_times}.last(1)};
            failure_time(thp_times.back(),failed_time,target);
            if(thp_times.back())enumeration.observed_at=&*thp_times.back();
        }
        retain_category(thp::retain({run,1,thp::Producer::controller,enumeration,thp_rows,&topology_owner},bounds.anonymous_thp.source),
            result.categories.anonymous_thp,subject::Category::anonymous_thp);
        if(stopped||cancelled())return result;
        std::array<std::optional<results::journal::Timestamp>,1> process_time;
        HostTime pt{operations,process_time};auto restriction=host_input::read_process_thp(operations.host,pt.callback());
        process::ObservationInput observation;
        if(restriction.outcome.succeeded()&&restriction.value&&*restriction.value<=1&&process_time[0])
            observation={process::State::available,*restriction.value?process::Restriction::disabled:process::Restriction::not_disabled,
                &*process_time[0],{}, {}};
        else {
            const bool malformed=restriction.outcome.succeeded();observation.state=process::State::failed;
            observation.reason=malformed?process::Reason::unsupported_value:process::Reason::read_failed;
            observation.failure_event=failed({subject::Category::process_thp,subject::Selector::process_observation,1,1,
                malformed?subject::FailureReason::unsupported_value:subject::FailureReason::read_failed},restriction.outcome);
        }
        retain_category(process::retain({run,1,process::Producer::controller,{process::Role::controller,1},observation,&topology_owner},
            bounds.process_thp.source),result.categories.process_thp,subject::Category::process_thp);
    } catch(const core::Error& error) {result.retention_failure=error;}
    catch(...) {result.retention_failure=host_error("results.preparation.host_capacity");}
    return result;
}
auto prepare_session(const results::journal::RunId& run,const results::journal::Timestamp& time,
    results::authored_source::SourceBundle& source,const runtime::RunnerContext& context,
    HostInputOperations operations,const ProducerLimits& limits) -> SessionPreparation {
    namespace ps=results::process_subject;
    SessionPreparation result;
    auto session=ps::Session::create(run,ps::Domain::supplied_observation,limits.session);
    if(!session){result.failure=std::move(session.error());return result;}
    auto manifest=retain_current_build(limits.build);
    HostEvidence host;
    ps::BaseBuild build{ps::BuildState::not_obtained,{},1};
    if(manifest) {
        build={ps::BuildState::available,std::move(*manifest),{}};
        host=acquire_host(run,context,operations,limits);
    } else {
        const auto& bound=limits.serialization.events.child.creation.error;
        auto error=results::private_error::retain(manifest.error(),{bound.context_nodes,bound.context_depth,
            bound.string_bytes,bound.binary_bytes,bound.retained_bytes});
        if(!error){result.failure=std::move(error.error());return result;}
        const ps::Target target{ps::Category::base_build,ps::Selector::base_build_acquisition};
        host.failures.push_back({std::move(*error),ps::Producer::controller,{},{},{target},{}});
        host.bindings.push_back({target,0});
    }
    result.source_bytes=host.source_bytes;
    result.cancelled=host.cancelled;
    if(host.retention_failure){result.failure=std::move(host.retention_failure);return result;}
    results::host_assembly::Input host_input_value{run,std::move(host.categories)};
    auto assembled=results::host_assembly::assemble(host_input_value,limits.host);
    if(!assembled){result.failure=std::move(assembled.error());return result;}
    result.uncommitted_creation.emplace(results::run_creation::Input{run,ps::Domain::supplied_observation,ps::Producer::controller,
        source,std::move(build),std::move(*assembled),std::move(host.failures),std::move(host.bindings)});
    auto created=session->append_creation(1,time,*result.uncommitted_creation);
    if(!created){result.failure=std::move(created.error());return result;}
    result.uncommitted_creation.reset();
    result.session=std::move(*session);return result;
}
namespace {
auto evidence_root_valid(std::string_view root) noexcept -> bool {
    if(root.empty()||root.front()!='/'||root.size()>4095||root.find('\0')!=root.npos)return false;
    std::size_t parts{},begin=1;
    while(begin<root.size()) {
        const auto end=root.find('/',begin);
        const auto part=root.substr(begin,end==root.npos?root.size()-begin:end-begin);
        if(part.empty()||part=="."||part==".."||part.size()>255||++parts>256)return false;
        if(end==root.npos)return true;
        begin=end+1;if(begin==root.size())return false;
    }
    return root=="/";
}
}
auto select_kernel_evidence(DependencyMode mode,const SuppliedKernelPolicy* supplied,
    results::kernel_report::Family family,const platform_linux::benchmark_run_input::FileOperations& operations)
    -> std::expected<KernelSelection,KernelInputError> {
    const auto& f=operations.files;
    if(!f.open_root||!f.open_at||!f.stat_fd||!f.read||!f.close||!operations.effective_uid)
        return std::unexpected{KernelInputError::admission};
    std::size_t index{};
    switch(family) {
    case results::kernel_report::Family::bandwidth:index=0;break;
    case results::kernel_report::Family::latency:index=1;break;
    default:return std::unexpected{KernelInputError::admission};
    }
    if(mode!=DependencyMode::supplied||!supplied)
        return std::unexpected{KernelInputError::admission};
    const std::string_view root=supplied->root;
    if(!evidence_root_valid(root))return std::unexpected{KernelInputError::admission};
    using Route=results::kernel_approval::ReviewRoute;
    const auto route=supplied->review_route;
    if(route!=Route::human_approval&&route!=Route::independent_agents)
        return std::unexpected{KernelInputError::admission};
    const auto expected=supplied->approval_digests[index];
    KernelSelection selected{root,{},expected,mode,route};
    constexpr std::string_view hex="0123456789abcdef";
    for(std::size_t i=0;i<expected.bytes().size();++i) {
        const auto value=std::to_integer<unsigned>(expected.bytes()[i]);
        selected.directory[2*i]=hex[value>>4U];selected.directory[2*i+1]=hex[value&15U];
    }
    return selected;
}
auto acquire_kernel_evidence(DependencyMode mode,const SuppliedKernelPolicy* supplied,
    results::kernel_report::Family family,const platform_linux::benchmark_run_input::FileOperations& operations,
    const ProducerLimits& limits,std::size_t& aggregate,platform_linux::detail::ControllerSignalScope* cancellation) -> KernelEvidence {
    namespace input=platform_linux::benchmark_run_input;
    KernelEvidence result;
    constexpr std::size_t mib=1024U*1024U;
    constexpr std::size_t total=66U*mib;
    if(aggregate>total){result.failure=KernelInputError::admission;return result;}
    const auto selected=select_kernel_evidence(mode,supplied,family,operations);
    if(!selected){result.failure=selected.error();return result;}
    const std::string_view directory{selected->directory.data(),selected->directory.size()};
    for(std::size_t i=0;i<result.files.size();++i) {
        if(cancellation_requested(cancellation)){result.cancelled=true;return result;}
        const auto ceiling=i==0?mib:8U*mib;
        auto read=input::read_evidence(selected->root,directory,static_cast<input::EvidenceFile>(i),
            std::min(ceiling,total-aggregate),operations);
        aggregate+=read.bytes.size();result.source_bytes+=read.bytes.size();
        result.files[i]=std::move(read);
        if(!result.files[i].outcome.succeeded()){result.failure=KernelInputError::acquisition;return result;}
        if(i==0&&core::sha256(result.files[i].bytes)!=selected->expected) {
            result.failure=KernelInputError::digest;return result;
        }
    }
    for(std::size_t i=0;i<result.reports.size();++i) {
        if(cancellation_requested(cancellation)){result.cancelled=true;return result;}
        auto retained=results::kernel_report::retain(result.files[1U+2U*i].bytes,limits.report);
        if(!retained){result.failure=KernelInputError::owner;result.producer_error=std::move(retained.error());return result;}
        if(!retained->report()||retained->report()->family!=family) {
            result.reports[i]=std::move(*retained);result.failure=KernelInputError::digest;return result;
        }
        result.reports[i]=std::move(*retained);
    }
    if(cancellation_requested(cancellation)){result.cancelled=true;return result;}
    // Select the reviewed route from closed expected-input policy, never by
    // sniffing returned bytes or falling back after a refused admission.
    auto retained=selected->review_route==results::kernel_approval::ReviewRoute::independent_agents
        ? results::kernel_approval::retain_agent(result.files[0].bytes,std::move(*result.reports[0]),
            std::move(*result.reports[1]),{result.files[2].bytes,result.files[4].bytes},limits.approval)
        : results::kernel_approval::retain(result.files[0].bytes,std::move(*result.reports[0]),
            std::move(*result.reports[1]),{result.files[2].bytes,result.files[4].bytes},limits.approval);
    if(!retained){result.failure=KernelInputError::owner;result.producer_error=std::move(retained.error());return result;}
    const auto* manifest=retained->manifest();
    if(!manifest||manifest->artifacts[0].machine_report.path!="gcc/object-code-audit.json"||
       manifest->artifacts[0].human_report.path!="gcc/object-code-audit.txt"||
       manifest->artifacts[1].machine_report.path!="clang/object-code-audit.json"||
       manifest->artifacts[1].human_report.path!="clang/object-code-audit.txt") {
        result.failure=KernelInputError::digest;return result;
    }
    result.approval=std::move(*retained);
    return result;
}

auto acquire_kernel_evidence(results::preparation_observation::Adapter& observer,
    const results::journal::Timestamp& stamp,KernelEvidence& retained,DependencyMode mode,
    const SuppliedKernelPolicy* policy,results::kernel_report::Family family,
    const platform_linux::benchmark_run_input::FileOperations& operations,
    const ProducerLimits& limits,std::size_t& aggregate,platform_linux::detail::ControllerSignalScope* cancellation)
    -> std::expected<void,results::preparation_observation::Error> {
    namespace observation=results::preparation_observation;
    struct Call {
        KernelEvidence& retained;DependencyMode mode;const SuppliedKernelPolicy* policy;
        results::kernel_report::Family family;
        const platform_linux::benchmark_run_input::FileOperations& operations;
        const ProducerLimits& limits;std::size_t& aggregate;
        platform_linux::detail::ControllerSignalScope* cancellation;
    } call{retained,mode,policy,family,operations,limits,aggregate,cancellation};
    return observer.kernel_evidence(stamp,&call,[](void* pointer,const results::process_subject::Session&)
        -> observation::CallFailures {
        auto& state=*static_cast<Call*>(pointer);
        // The original result is installed before any fallible diagnostic
        // mapping. A mapping failure cannot erase bytes, debit or disposition.
        state.retained=acquire_kernel_evidence(state.mode,state.policy,state.family,state.operations,state.limits,state.aggregate,state.cancellation);
        auto& output=state.retained;
        if(!output.failure)return {};
        try {
            using Stage=platform_linux::benchmark_run_input::Stage;
            const auto error=[](KernelInputError kind,const platform_linux::benchmark_run_input::Failure* native,bool cleanup) {
                std::string_view label;
                switch(kind) {
                case KernelInputError::admission:label="admission";break;
                case KernelInputError::unconfigured:label="unconfigured";break;
                case KernelInputError::acquisition:label="acquisition";break;
                case KernelInputError::digest:label="digest";break;
                default:throw observation::Error::source;
                }
                nlohmann::json stage=nullptr;
                if(native)switch(native->stage) {
                case Stage::admission:stage="admission";break;
                case Stage::open:stage="open";break;
                case Stage::identity:stage="identity";break;
                case Stage::read:stage="read";break;
                case Stage::close:stage="close";break;
                default:throw observation::Error::source;
                }
                return core::Error{.category=core::ErrorCategory::results,
                    .code=core::StableErrorCode{"results.preparation.kernel_acquisition"},
                    .message="Kernel evidence acquisition failed",.operation="acquire_kernel_evidence",
                    .os_error={},.context={{"kind",label},{"stage",std::move(stage)},
                        {"error_number",native?nlohmann::json(native->error_number):nlohmann::json(nullptr)},
                        {"cleanup",cleanup}},.severity=core::ErrorSeverity::error,
                    .continuation=core::ContinuationPolicy::abort_command};
            };
            const platform_linux::benchmark_run_input::Outcome* failed{};
            for(const auto& file:output.files)if(!file.outcome.succeeded()){failed=&file.outcome;break;}
            if(!output.producer_error)output.producer_error=error(*output.failure,
                failed&&failed->primary?&*failed->primary:nullptr,false);
            if(failed) {
                if(failed->cleanup_count>output.cleanup_errors.size())throw observation::Error::source;
                for(std::size_t i=0;i<failed->cleanup_count;++i)
                    output.cleanup_errors[output.cleanup_count++]=error(KernelInputError::acquisition,&failed->cleanup[i],true);
            }
            return {&*output.producer_error,{output.cleanup_errors.data(),output.cleanup_count}};
        } catch(const std::bad_alloc&) {throw observation::Error::capacity;}
      catch(observation::Error) {throw;}
      catch(results::event_profile::Error) {throw observation::Error::capacity;}
      catch(...) {throw observation::Error::source;}
    });
}

auto parse_arguments(std::span<const std::string_view> arguments) noexcept
    -> std::expected<Arguments,ArgumentError> {
    const auto invalid=std::unexpected{ArgumentError::usage};
    if(arguments.size()==1 && (arguments[0]=="--help" || arguments[0]=="-h"))
        return Arguments{{},scenario::ScenarioFormat::yaml,true};
    if(arguments.empty() || arguments.size()>4)return invalid;
    Arguments result;
    std::optional<scenario::ScenarioFormat> explicit_format;
    bool positional_only{};
    for(std::size_t i=0;i<arguments.size();++i) {
        const auto argument=arguments[i];
        if(argument.empty() || argument.size()>4095 || argument.find('\0')!=std::string_view::npos)
            return invalid;
        if(!positional_only && argument=="--") {positional_only=true;continue;}
        if(!positional_only && (argument=="--format" || argument.starts_with("--format="))) {
            if(explicit_format)return invalid;
            std::string_view value;
            if(argument=="--format") {
                if(++i==arguments.size())return invalid;
                value=arguments[i];
            } else value=argument.substr(9);
            if(value=="yaml")explicit_format=scenario::ScenarioFormat::yaml;
            else if(value=="json")explicit_format=scenario::ScenarioFormat::json;
            else return invalid;
            continue;
        }
        if(argument=="-" || (!positional_only && argument.front()=='-') || !result.scenario_path.empty())
            return invalid;
        result.scenario_path=argument;
    }
    if(result.scenario_path.empty())return invalid;
    const auto leaf=result.scenario_path.substr(result.scenario_path.find_last_of('/')+1);
    const auto dot=leaf.find_last_of('.');
    const auto extension=dot==std::string_view::npos || dot==0?std::string_view{}:leaf.substr(dot);
    std::optional<scenario::ScenarioFormat> inferred;
    if(extension==".yaml" || extension==".yml")inferred=scenario::ScenarioFormat::yaml;
    else if(extension==".json")inferred=scenario::ScenarioFormat::json;
    if((!inferred && !explicit_format) || (inferred && explicit_format && inferred!=explicit_format))
        return invalid;
    result.format=explicit_format?*explicit_format:*inferred;
    return result;
}

auto controller_checks(const results::journal::RunId& run,std::uint64_t ordinal,
    const monitoring::MonitoringExecutionPolicy& policy,const ControllerCheckOperations& operations,
    results::validation_event::Limits limits) -> ControllerChecks {
    namespace input=platform_linux::benchmark_run_input;
    namespace validation=results::validation_event;
    ControllerChecks result;
    std::array<validation::Slot,4> slots{};
    for(std::size_t i=0;i<slots.size();++i)
        slots[i].evaluation=policy.disposition(static_cast<monitoring::MonitoringChannel>(i))==
            monitoring::MonitoringChannelDisposition::disabled ? validation::Evaluation::not_requested : validation::Evaluation::not_evaluated;
    if(operations.source_text_bytes>16U*1024U*1024U) {
        result.admission.primary=input::Failure{input::Stage::admission,0,{}};return result;
    }
    for(std::size_t i=0;i<slots.size();++i) {
        if(slots[i].evaluation==validation::Evaluation::not_requested) continue;
        if(cancellation_requested(operations.cancellation)){result.cancelled=true;break;}
        const auto channel=static_cast<monitoring::MonitoringChannel>(i);
        if(!operations.clock) {
            result.admission.primary=input::Failure{input::Stage::admission,0,{}};
            switch(i) {
            case 0:result.observations[i]=input::PageFaultRead{};break;
            case 1:result.observations[i]=input::PerfCapability{};break;
            case 2:result.observations[i]=input::TemperatureRead{};break;
            case 3:result.observations[i]=input::FrequencyRead{};break;
            default:break;
            }
            std::visit([&](auto& value){if constexpr(requires{value.outcome;})value.outcome=result.admission;},result.observations[i]);
            result.failed_channel=channel;break;
        }
        struct Clock {
            const ControllerCheckOperations* operations;
            std::optional<results::journal::Timestamp>* destination;
            std::array<std::optional<results::journal::Timestamp>,1024>* domains;
            std::size_t attempts{};
        } clock{&operations,&result.observed_at[i],i==3?&result.frequency_observed_at:nullptr};
        input::ObservationTime time{&clock,[](void* p) -> std::expected<void,input::Failure> {
            auto& value=*static_cast<Clock*>(p);
            auto* destination=value.destination;
            if(value.domains) {
                if(value.attempts==value.domains->size())
                    return std::unexpected{input::Failure{input::Stage::admission,0,{}}};
                destination=&(*value.domains)[value.attempts++];
            }
            auto observed=value.operations->clock(value.operations->clock_context);
            if(!observed)return std::unexpected{std::move(observed.error())};
            *destination=std::move(*observed);return {};
        }};
        bool succeeded{},supported{};
        switch(i) {
        case 0: {
            auto value=input::read_page_faults(operations.thread,operations.page_faults,time);
            succeeded=value.outcome.succeeded();supported=value.counters.has_value();
            result.observations[i]=std::move(value);break;
        }
        case 1: {
            auto value=input::check_perf(operations.thread,operations.page_bytes,operations.context_switches,time);
            succeeded=value.outcome.succeeded();supported=value.observed;
            result.observations[i]=std::move(value);break;
        }
        case 2: {
            auto value=input::read_temperature(operations.temperature,time,
                {operations.source_text_bytes-result.source_bytes,{}});
            result.source_bytes+=value.source_bytes;
            succeeded=value.outcome.succeeded();supported=value.supported;
            result.observations[i]=std::move(value);break;
        }
        case 3: {
            auto value=input::read_frequency(operations.present,operations.selected,operations.frequency,time,
                {operations.source_text_bytes-result.source_bytes,{}});
            result.source_bytes+=value.source_bytes;
            succeeded=value.outcome.succeeded();supported=value.supported;
            result.frequency_attempts=value.domains.size();
            if(succeeded) {
                // No-domain unsupported observation has one aggregate clock
                // attempt but must not fabricate a frequency domain.
                if(value.domains.empty()) result.observed_at[i]=std::move(result.frequency_observed_at[0]);
                else result.observed_at[i]=result.frequency_observed_at[value.domains.size()-1];
            }
            if(value.domains.empty()) result.frequency_observed_at[0].reset();
            result.observations[i]=std::move(value);break;
        }
        default:break;
        }
        if(!succeeded) { result.failed_channel=channel;break; }
        slots[i]={validation::Evaluation::evaluated,
            supported?monitoring::MonitoringSourceSupport::supported:monitoring::MonitoringSourceSupport::unsupported,
            &*result.observed_at[i]};
    }
    auto retained=validation::retain({run,ordinal,validation::Producer::controller,policy,slots},limits);
    if(!retained)result.assessment_error=std::move(retained.error());
    else result.assessment=std::move(*retained);
    return result;
}
ControllerChecks::~ControllerChecks() {
    core::detail::retire_json(snapshot);
    for(auto& error:captured_errors)core::detail::retire_json(error.context);
    if(assessment_error)core::detail::retire_json(assessment_error->context);
    const auto retire_outcome=[](platform_linux::benchmark_run_input::Outcome& outcome) noexcept {
        if(outcome.primary&&outcome.primary->detail)core::detail::retire_json(outcome.primary->detail->context);
        for(auto& failure:outcome.cleanup)if(failure.detail)core::detail::retire_json(failure.detail->context);
    };
    retire_outcome(admission);
    for(auto& observation:observations)std::visit([&](auto& value) {
        if constexpr(requires{value.outcome;})retire_outcome(value.outcome);
    },observation);
}
auto ControllerChecks::operator=(ControllerChecks&& other) noexcept -> ControllerChecks& {
    if(this!=&other) {
        std::destroy_at(this);
        std::construct_at(this,std::move(other));
    }
    return *this;
}
auto capture_controller_checks(ControllerChecks& checks,const ProducerLimits& limits)
    -> results::preparation_observation::CheckCallResult {
    namespace input=platform_linux::benchmark_run_input;
    namespace observation=results::preparation_observation;
    using Json=nlohmann::json;
    constexpr std::array<std::string_view,4> channels{"page_faults","context_switches","temperature","frequency"};
    constexpr std::array stages{"admission","open","identity","read","close","affinity_save","affinity_set","cpuid","affinity_restore","kernel_identity","process_thp","perf_map","perf_metadata","perf_unmap","clock"};
    constexpr std::array dispositions{"success","native_failure","contained_standard","contained_unknown"};
    constexpr std::array task_kinds{"invalid_operations","open_failed","read_failed","close_failed","contained_exception","contained_unknown_failure","task_id","task_identity_mismatch","command_delimiter","truncated_record","minor_faults","minor_faults_out_of_range","major_faults","major_faults_out_of_range"};
    constexpr std::array temperature_kinds{"enumerate","read","value","ambiguous","enumeration_capacity","token_capacity","contained","preparation_rejected"};
    constexpr std::array frequency_kinds{"enumerate","enumeration_capacity","token_capacity","membership","metadata","value","ambiguous","read","contained","preparation_rejected"};
    try {
        // Conservative encoded stock precedes JSON growth, including escaping
        // and object keys. Dynamic source arrays remain independently bounded.
        std::size_t bound=65536,node_bound=1024;
        const auto charge_nodes=[&](std::size_t count,std::size_t cell) {
            if(count&&cell>(1048576U-node_bound)/count)throw observation::Error::capacity;
            node_bound+=count*cell;
        };
        const auto charge=[&](std::size_t count,std::size_t cell) {
            constexpr std::size_t maximum=64U*1024U*1024U;
            if(count&&cell>(maximum-bound)/count)throw observation::Error::capacity;
            bound+=count*cell;
        };
        for(const auto& attempt:checks.observations)if(const auto* frequency=std::get_if<input::FrequencyRead>(&attempt)) {
            if(frequency->domains.size()>1024||frequency->memberships.size()>16384||frequency->source_cleanup.size()>1048576||frequency->retirement.size()>1048576)
                throw observation::Error::capacity;
            charge_nodes(frequency->domains.size(),24);charge_nodes(frequency->memberships.size(),1);
            charge_nodes(frequency->source_cleanup.size(),8);charge_nodes(frequency->retirement.size(),8);
            charge(frequency->domains.size(),1024);charge(frequency->memberships.size(),24);
            charge(frequency->source_cleanup.size(),256);charge(frequency->retirement.size(),192);
            for(const auto& domain:frequency->domains) {
                if(domain.driver.size()>4096||domain.governor.size()>4096)throw observation::Error::capacity;
                charge(domain.driver.size()+domain.governor.size(),6);
            }
        }
        auto error_limits=limits.serialization.events.composition->error;
        error_limits.targets=0;error_limits.provenance_bytes=0;
        const auto detail=[&](const std::optional<core::Error>& error)->Json {
            if(!error)return nullptr;
            auto retained_error=results::private_error::retain(*error,{error_limits.context_nodes,error_limits.context_depth,
                error_limits.string_bytes,error_limits.binary_bytes,error_limits.retained_bytes});
            if(!retained_error)throw observation::Error::serialization;
            results::error_serialization::Usage usage{};
            auto encoded=results::error_serialization::detail::encode_value(*retained_error,error_limits,usage);
            if(!encoded)throw observation::Error::serialization;
            charge(encoded->size(),6);return std::move(*encoded);
        };
        using core::detail::json_object;
        using core::detail::set_json;
        using core::detail::append_json;
        using core::detail::append_object_json;
        const auto failure=[&](const input::Failure& value)->Json {
            return json_object<Json>([&](Json& object) {
                set_json(object,"stage",stages.at(static_cast<std::size_t>(value.stage)));
                set_json(object,"error_number",std::to_string(value.error_number));
                set_json(object,"detail",detail(value.detail));
            });
        };
        const auto outcome=[&](const input::Outcome& value)->Json {
            if(value.cleanup_count>value.cleanup.size())throw observation::Error::source;
            return json_object<Json>([&](Json& object) {
                set_json(object,"primary",value.primary?failure(*value.primary):Json(nullptr));
                set_json(object,"cleanup",Json::array());
                for(std::size_t i=0;i<value.cleanup_count;++i)append_json(object["cleanup"],failure(value.cleanup[i]));
            });
        };
        const auto lower=[&](const auto& value,const auto& kinds)->Json {
            const int native=[&] {if constexpr(requires{value.native;})return value.native;else return value.native_error;}();
            return json_object<Json>([&](Json& object) {
                set_json(object,"kind",kinds.at(static_cast<std::size_t>(value.kind)));
                set_json(object,"native",std::to_string(native));
                set_json(object,"disposition",dispositions.at(static_cast<std::size_t>(value.disposition)));
            });
        };
        const auto retired=[&](const auto& value)->Json {
            return json_object<Json>([&](Json& object) {
                set_json(object,"attempted",value.attempted);
                set_json(object,"disposition",dispositions.at(static_cast<std::size_t>(value.kind)));
                set_json(object,"native",std::to_string(value.native_error));
            });
        };
        auto snapshot=json_object<Json>([&](Json& object) {
            set_json(object,"profile","epycbench.controller-checks/v1");
            set_json(object,"attempts",Json::array());
        });
        core::detail::JsonCleanup snapshot_cleanup{snapshot};
        for(std::size_t index=0;index<checks.observations.size();++index) {
            if(std::holds_alternative<std::monostate>(checks.observations[index]))continue;
            Json value,observed_outcome;
            core::detail::JsonCleanup value_cleanup{value},outcome_cleanup{observed_outcome};
            std::visit([&](const auto& source) {
                using T=std::decay_t<decltype(source)>;
                if constexpr(!std::is_same_v<T,std::monostate>) {
                    observed_outcome=outcome(source.outcome);
                    value=json_object<Json>([&](Json& object) {
                        if constexpr(std::is_same_v<T,input::PageFaultRead>) {
                            const auto task=[&](const auto& f)->Json {
                                return json_object<Json>([&](Json& row) {
                                    set_json(row,"kind",task_kinds.at(static_cast<std::size_t>(f.kind)));
                                    set_json(row,"error_number",std::to_string(f.error_number));
                                });
                            };
                            set_json(object,"counters",source.counters?json_object<Json>([&](Json& row) {
                                set_json(row,"minor",std::to_string(source.counters->minor_faults()));
                                set_json(row,"major",std::to_string(source.counters->major_faults()));
                            }):Json(nullptr));
                            set_json(object,"source_failure",source.source_failure?json_object<Json>([&](Json& row) {
                                set_json(row,"primary",task(source.source_failure->primary));
                                set_json(row,"cleanup",source.source_failure->cleanup?task(*source.source_failure->cleanup):Json(nullptr));
                            }):Json(nullptr));
                        } else if constexpr(std::is_same_v<T,input::PerfCapability>)set_json(object,"observed",source.observed);
                        else {
                            set_json(object,"evaluated",source.evaluated);set_json(object,"supported",source.supported);
                            set_json(object,"source_bytes",std::to_string(source.source_bytes));
                            set_json(object,"source_failure",nullptr);set_json(object,"source_cleanup",Json::array());
                            set_json(object,"retirement",Json::array());
                            if constexpr(std::is_same_v<T,input::TemperatureRead>) {
                                set_json(object,"millicelsius",source.millicelsius?Json(std::to_string(*source.millicelsius)):Json(nullptr));
                                const auto count=source.source_failure?source.source_failure->cleanup_count:0;
                                if(count>source.source_cleanup.size()||source.retirement_count>source.retirement.size())throw observation::Error::source;
                                if(source.source_failure)set_json(object,"source_failure",json_object<Json>([&](Json& row) {
                                    set_json(row,"primary",lower(source.source_failure->primary,temperature_kinds));
                                    set_json(row,"cleanup_count",std::to_string(count));
                                }));
                                for(std::size_t i=0;i<count;++i)append_json(object["source_cleanup"],lower(source.source_cleanup[i],temperature_kinds));
                                for(std::size_t i=0;i<source.retirement_count;++i)append_json(object["retirement"],retired(source.retirement[i]));
                            } else {
                                set_json(object,"discovered_policies",std::to_string(source.discovered_policies));
                                set_json(object,"memberships",Json::array());set_json(object,"domains",Json::array());
                                for(auto cpu:source.memberships)append_json(object["memberships"],std::to_string(cpu.value()));
                                if(source.source_failure)set_json(object,"source_failure",json_object<Json>([&](Json& row) {
                                    set_json(row,"primary",lower(source.source_failure->primary,frequency_kinds));
                                    set_json(row,"cleanup_count",std::to_string(source.source_failure->cleanup_count));
                                }));
                                for(const auto& f:source.source_cleanup)append_json(object["source_cleanup"],lower(f,frequency_kinds));
                                for(const auto& f:source.retirement)append_json(object["retirement"],retired(f));
                                for(std::size_t i=0;i<source.domains.size();++i) {
                                    const auto& d=source.domains[i];
                                    append_object_json(object["domains"],[&](Json& row) {
                                        set_json(row,"membership_offset",std::to_string(d.membership_offset));
                                        set_json(row,"membership_count",std::to_string(d.membership_count));
                                        set_json(row,"kind",d.kind==platform_linux::CpuFrequencyCurrentKind::hardware_current?"hardware_current":"scaling_current");
                                        set_json(row,"driver",d.driver);set_json(row,"governor",d.governor);
                                        set_json(row,"minimum_hz",std::to_string(d.minimum_hz));set_json(row,"maximum_hz",std::to_string(d.maximum_hz));
                                        set_json(row,"current_hz",d.current_hz?Json(std::to_string(*d.current_hz)):Json(nullptr));
                                        set_json(row,"utc",checks.frequency_observed_at[i]?Json(checks.frequency_observed_at[i]->text()):Json(nullptr));
                                    });
                                }
                            }
                        }
                    });
                }
            },checks.observations[index]);
            append_object_json(snapshot["attempts"],[&](Json& row) {
                set_json(row,"channel",channels[index]);
                set_json(row,"utc",checks.observed_at[index]?Json(checks.observed_at[index]->text()):Json(nullptr));
                set_json(row,"outcome",std::move(observed_outcome));set_json(row,"value",std::move(value));
            });
        }
        core::detail::retire_json(checks.snapshot);
        checks.snapshot=std::move(snapshot);
        if(!results::event_profile::detail::controller_check_snapshot_valid(checks.snapshot,error_limits))throw observation::Error::source;
        const auto encoded=checks.snapshot.dump();
        std::size_t error_count{};
        for(const auto& attempt:checks.snapshot["attempts"]) {
            const auto& out=attempt.at("outcome");const auto& value=attempt.at("value");
            const bool owned_retirement=value.contains("retirement");
            error_count+=!out.at("primary").is_null()&&(!owned_retirement||out.at("primary").at("stage")!="close");
            for(const auto& part:out.at("cleanup"))error_count+=!owned_retirement||part.at("stage")!="close";
            if(value.contains("source_cleanup"))error_count+=value.at("source_cleanup").size();
            if(value.contains("retirement"))for(const auto& row:value.at("retirement"))
                error_count+=row.at("attempted")==true&&row.at("disposition")!="success";
        }
        if(error_count>=limits.serialization.events.records||
           (error_count&&encoded.size()>(64U*1024U*1024U)/error_count))throw observation::Error::capacity;
        // The snapshot is already bounded before growth. Charge its actual
        // retained shape, each mapped diagnostic copy, and actual lower vector
        // stocks before the next parent admission. Assessment stock is separate.
        namespace event_detail=results::event_profile::detail;
        event_detail::Work measuring{0,limits.serialization.events.work};
        const auto actual_shape=event_detail::shape(encoded,limits.serialization.events,measuring);
        const auto snapshot_stock=event_detail::json_stock(actual_shape);
        auto retained_stock=event_detail::add(sizeof(ControllerChecks),checks.source_bytes);
        retained_stock=event_detail::add(retained_stock,snapshot_stock);
        retained_stock=event_detail::add(retained_stock,event_detail::multiply(error_count,
            event_detail::add(snapshot_stock,65536U)));
        retained_stock=event_detail::add(retained_stock,event_detail::multiply(checks.frequency_attempts+4U,30U));
        for(const auto& attempt:checks.observations)if(const auto* frequency=std::get_if<input::FrequencyRead>(&attempt)) {
            retained_stock=event_detail::add(retained_stock,event_detail::multiply(frequency->domains.capacity(),sizeof(frequency->domains[0])));
            retained_stock=event_detail::add(retained_stock,event_detail::multiply(frequency->memberships.capacity(),sizeof(core::LinuxCpuId)));
            retained_stock=event_detail::add(retained_stock,event_detail::multiply(frequency->source_cleanup.capacity(),sizeof(frequency->source_cleanup[0])));
            retained_stock=event_detail::add(retained_stock,event_detail::multiply(frequency->retirement.capacity(),sizeof(frequency->retirement[0])));
        }
        const auto maximum=producer_input_limit();
        if(!maximum||retained_stock>*maximum)throw observation::Error::capacity;
        checks.retained_bytes=retained_stock;
        checks.captured_errors.reserve(error_count);
        const auto append_error=[&](const Json& attempt,std::string_view stage,const Json& native,const Json& source_failure) {
            core::Error error{.category=core::ErrorCategory::results,
                .code=core::StableErrorCode{"results.preparation.controller_checks"},.message="Controller source check failed",
                .operation="acquire_controller_checks",.os_error={},.context=nullptr,
                .severity=core::ErrorSeverity::error,.continuation=core::ContinuationPolicy::abort_command};
            core::detail::JsonCleanup error_cleanup{error.context};
            error.context=json_object<Json>([&](Json& object) {
                set_json(object,"channel",attempt.at("channel"));set_json(object,"stage",stage);
                set_json(object,"error_number",native);set_json(object,"cleanup",!checks.captured_errors.empty());
                set_json(object,"source_failure",source_failure);set_json(object,"observations",checks.snapshot);
            });
            checks.captured_errors.push_back(std::move(error));
        };
        const Json absent_source_failure=nullptr;
        for(const auto& attempt:checks.snapshot["attempts"]) {
            const auto& out=attempt.at("outcome");const auto& value=attempt.at("value");
            const auto& first=out.at("primary");
            const bool owned_retirement=value.contains("retirement");
            if(!first.is_null()&&(!owned_retirement||first.at("stage")!="close"))append_error(attempt,first.at("stage").get_ref<const std::string&>(),first.at("error_number"),value.contains("source_failure")?value.at("source_failure"):absent_source_failure);
            for(const auto& f:out.at("cleanup"))if(!owned_retirement||f.at("stage")!="close")
                append_error(attempt,f.at("stage").get_ref<const std::string&>(),f.at("error_number"),nullptr);
            if(value.contains("source_cleanup"))for(const auto& f:value.at("source_cleanup"))append_error(attempt,"close",f.at("native"),f);
            if(value.contains("retirement"))for(const auto& f:value.at("retirement"))if(f.at("attempted")==true&&f.at("disposition")!="success")
                append_error(attempt,"close",f.at("native"),f);
        }
        const auto count=checks.captured_errors.size();
        return {{count?&checks.captured_errors[0]:nullptr,count?std::span{checks.captured_errors}.subspan(1):std::span<const core::Error>{}},
            &checks.snapshot,checks.assessment?&*checks.assessment:nullptr};
    } catch(const std::bad_alloc&) {throw observation::Error::capacity;}
      catch(observation::Error) {throw;}
      catch(results::event_profile::Error) {throw observation::Error::capacity;}
      catch(...) {throw observation::Error::source;}
}
namespace {
constexpr std::size_t mib = 1024U * 1024U;
constexpr std::size_t bytes = 64U * mib;
constexpr std::size_t retained = 256U * mib;
constexpr std::size_t records = 4096;
constexpr std::size_t nodes = 1048576;
constexpr std::size_t work = std::size_t{1} << 40U;
struct Capacity {};
auto add(std::size_t a, std::size_t b) -> std::size_t {
    if (b > std::numeric_limits<std::size_t>::max() - a) throw Capacity{};
    return a + b;
}
auto multiply(std::size_t a, std::size_t b) -> std::size_t {
    if (b && a > std::numeric_limits<std::size_t>::max() / b) throw Capacity{};
    return a * b;
}
auto phase(std::size_t input, std::size_t output, std::size_t fixed,
           std::size_t rows, std::size_t cell,
           std::span<const std::size_t> children = {}) -> std::size_t {
    auto result = add(input, output);
    result = add(result, std::size_t{1} << 30U);
    result = add(result, fixed);
    result = add(result, multiply(rows, cell));
    result = add(result, bytes);
    for (const auto child : children) result = add(result, child);
    return result;
}
}
auto preparation_phase(std::size_t input, std::size_t output, std::size_t fixed,
    std::size_t rows, std::size_t cell, std::span<const std::size_t> children)
    -> std::expected<std::size_t, Error> {
    try { return phase(input, output, fixed, rows, cell, children); }
    catch (const Capacity&) { return std::unexpected{Error::capacity}; }
}
auto producer_input_limit() -> std::expected<std::size_t,Error> {
    try {
        const auto selection=add(multiply(2,retained),results::implementation_selection::detail::fixed_stock());
        const auto association=add(add(selection,multiply(4,retained)),results::build_object_association::detail::fixed_stock());
        const auto plan=add(add(association,retained),results::execution_plan::detail::fixed_stock());
        return add(add(multiply(32,retained),82U*mib),add(selection,add(association,plan)));
    } catch(const Capacity&) {return std::unexpected{Error::capacity};}
}
auto preparation_inputs(std::span<const std::size_t> inputs,std::size_t ceiling)
    -> std::expected<std::size_t,Error> {
    try {
        std::size_t sum{};
        for(const auto input:inputs){sum=add(sum,input);if(sum>ceiling)return std::unexpected{Error::capacity};}
        return sum;
    } catch(const Capacity&) {return std::unexpected{Error::capacity};}
}
auto producer_limits(std::size_t live) -> std::expected<ProducerLimits, Error> {
    try {
        const auto selection_stock=add(multiply(2,retained),results::implementation_selection::detail::fixed_stock());
        const auto association_stock=add(add(selection_stock,multiply(4,retained)),results::build_object_association::detail::fixed_stock());
        const auto plan_stock=add(add(association_stock,retained),results::execution_plan::detail::fixed_stock());
        const auto maximum_input=producer_input_limit();
        if(!maximum_input||live>*maximum_input)return std::unexpected{Error::capacity};
        ProducerLimits result{};
        result.serialization = results_validation_limits(results::event_profile::Version::compact_v3);
        auto& children = result.serialization.events.child;
        const auto& composition = *result.serialization.events.composition;
        result.resolution = {64,64,64,256,1024,64,nodes,bytes,bytes,
            {4096,4096,256,64,65536,retained},retained,1024,65536};
        result.source = {16U*mib,4095,0,0,64,bytes,nodes,retained};
        result.build = children.creation.base_build.manifest;
        result.host = {7,bytes,multiply(8,retained)};
        // These are the actual Host producer fields, also retained by the
        // serializer. Narrow the public inventory without changing lower owners.
        children.creation.host.frequency.source.policies = 1024;
        children.creation.host.frequency.source.uncovered = 1024;
        children.creation.host.frequency.source.memberships = 16384;
        children.creation.host.frequency.source.label_bytes = 4096;
        children.creation.host.frequency.source.total_label_bytes = bytes;
        result.report = composition.planning.report;
        result.report.input_bytes = 8U*mib;
        result.report.string_bytes = 8U*mib;
        result.report.integer_digits = 8U*mib;
        result.approval = composition.planning.approval;
        result.approval.input_bytes = mib;
        result.approval.string_bytes = mib;
        result.approval.integer_digits = mib;

        auto& selected = result.selection;
        selected.workloads=64; selected.bindings=64; selected.workload_text_bytes=bytes;
        selected.maximum_workload_text=4096; selected.child_retained_bytes=retained;
        selected.child_canonical_bytes=bytes; selected.canonical_bytes=bytes;
        selected.association_work=work;
        selected.retained_bytes=selection_stock;
        auto& associated = result.association;
        associated.workloads=64; associated.bindings=64; associated.approvals=2;
        associated.child_retained_bytes=add(selected.retained_bytes,multiply(3,retained));
        associated.child_evidence_bytes=multiply(5,bytes); associated.canonical_bytes=bytes;
        associated.association_work=work;
        associated.retained_bytes=association_stock;
        auto& plan = result.plan;
        plan.experiments=64; plan.variants=256; plan.pairs=64;
        plan.child_retained_bytes=associated.retained_bytes; plan.child_canonical_bytes=bytes;
        plan.index_bytes=multiply(256,sizeof(results::execution_plan::detail::NameIndex));
        plan.pair_bytes=multiply(64,sizeof(results::execution_plan::Pair));
        plan.semantic_bytes=bytes; plan.canonical_bytes=bytes; plan.planning_work=work;
        plan.retained_bytes=plan_stock;
        selected.consumer_simultaneous_bytes=phase(live,selected.retained_bytes,
            results::implementation_selection::detail::fixed_stock(),64,
            sizeof(results::implementation_selection::Workload)+sizeof(results::implementation_selection::Binding));
        associated.consumer_simultaneous_bytes=phase(live,associated.retained_bytes,
            results::build_object_association::detail::fixed_stock(),2,
            sizeof(results::build_object_association::FamilyFacts));
        plan.consumer_simultaneous_bytes=phase(live,plan.retained_bytes,
            results::execution_plan::detail::fixed_stock(),64,sizeof(results::execution_plan::Pair));

        auto& session=result.session;
        session.entries=1024; session.runner_instances=0;
        session.timestamp_bytes=multiply(30,records); session.canonical_child_bytes=multiply(records,bytes);
        session.retained_bytes=multiply(16,retained); session.failure_facts=1024;
        session.targets=65536; session.context_nodes=4096;
        session.private_string_bytes=bytes; session.private_binary_bytes=bytes;
        session.normalizations=1; session.source_nodes=nodes; session.source_canonical_bytes=bytes;
        session.source_outcomes=1; session.base_builds=1;
        auto& creation=session.run_creation;
        creation.records=1; creation.artifact_slots=16; creation.source_bytes=16U*mib;
        creation.source_name_bytes=4095; creation.failure_facts=1024;
        creation.targets=65536; creation.bindings=65536; creation.error_context_nodes=4096;
        creation.error_string_bytes=bytes; creation.error_binary_bytes=bytes;
        creation.canonical_child_bytes=bytes; creation.retained_bytes=session.retained_bytes;
        creation.consumer_simultaneous_bytes=phase(live,creation.retained_bytes,
            results::run_creation::detail::fixed_stock(),records,sizeof(results::run_creation::Binding));
        auto& planning=session.planning;
        planning.records=1; planning.pairs=64; planning.scope_links=64;
        planning.canonical_bytes=bytes; planning.child_retained_bytes=plan.retained_bytes;
        planning.comparison_bytes=bytes; planning.association_work=work; planning.association_bytes=retained;
        planning.planning_simultaneous_bytes=phase(live,plan.retained_bytes,
            results::planning::detail::append_fixed_stock(),64,sizeof(results::execution_plan::Pair));
        planning.association_preparation_bytes=phase(live,retained,
            results::planning::detail::map_fixed_stock(),64,sizeof(std::uint64_t));
        session.preflight=composition.preflight.evaluator;
        session.preflight.consumer_simultaneous_bytes=phase(live,session.preflight.retained_bytes,
            results::preflight::detail::fixed_stock(),session.preflight.checks,sizeof(results::preflight::Check));
        return result;
    } catch (const Capacity&) { return std::unexpected{Error::capacity}; }
}
auto run_limits() -> std::expected<benchmark_run_composition::RunLimits,Error> {
    try {
        auto policy=producer_limits();if(!policy)return std::unexpected{policy.error()};
        benchmark_run_composition::RunLimits out{};
        out.pair_reports=policy->plan.pairs;
        out.scenario_bytes=policy->source.canonical_bytes;
        out.final=std::move(policy->serialization);
        out.total_written_bytes=add(results_document_input_bytes,out.final.output_bytes);
        auto& file=out.journal.file;
        file.root_bytes=4095;file.root_components=256;file.component_bytes=255;
        file.files=4;file.descriptors=2;file.file_bytes=results_document_input_bytes;
        file.transfer_bytes=multiply(2,bytes);
        file.operations=add(multiply(2,file.transfer_bytes),add(multiply(4,file.root_components),32));
        auto writer=results::filesystem_journal::Writer::storage(out.final.events,out.journal,file.root_bytes);
        auto recovery=results::filesystem_journal::Recovery::storage(out.final.events,out.journal,file.root_bytes);
        if(!writer||!recovery)return std::unexpected{Error::capacity};
        out.journal.working_bytes=std::max(*writer,*recovery);
        const auto& context=out.final.events.composition->context;
        out.context={bytes,bytes,nodes,bytes,64,context.workers,nodes,bytes,retained,context.simultaneous};
        out.progress={out.final.events,retained,work};
        out.analysis=out.final.analysis;
        return out;
    }catch(...){return std::unexpected{Error::capacity};}
}

}
