#include "platform_linux/package_mapping.hpp"

#include "epycbench/platform_linux/numa_policy.hpp"
#include "epycbench/platform_linux/page_location.hpp"
#include "epycbench/platform_linux/task_stat.hpp"
#include "epycbench/platform_linux/thread_affinity.hpp"
#include "kernel/package_mapping.hpp"
#include "platform_linux/hugetlb_backing.hpp"
#include "platform_linux/package_worker_monitoring.hpp"
#include "platform_linux/timed_anonymous_mapping.hpp"

#include <atomic>
#include <cerrno>
#include <latch>
#include <map>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace epycbench::platform_linux::package_execution {
namespace {
using Json = nlohmann::json;
using U64 = std::uint64_t;
using ClockRead = detail::MappingClockRead;
using FaultRead = std::expected<LinuxTaskPageFaultCounters, core::Error>;

auto failure(std::string_view phase, std::string_view message) -> core::Error {
    return {core::ErrorCategory::runtime, core::StableErrorCode{"runtime.package.mapping." + std::string{phase}},
            std::string{message}, "Package.run_mapping", {}, Json::object()};
}

auto located_cpu(core::LinuxCpuId expected) -> std::expected<core::LinuxCpuId, core::Error> {
    auto affinity = get_current_thread_affinity();
    if (!affinity) return std::unexpected{std::move(affinity.error())};
    auto cpu = current_cpu();
    if (!cpu) return std::unexpected{std::move(cpu.error())};
    if (*affinity != core::CpuSet{expected})
        return std::unexpected{failure("affinity", "Mapping worker affinity does not match the admitted CPU")};
    return *cpu;
}

auto fault_evidence(const std::optional<FaultRead>& read) -> Json {
    if (!read) return nullptr;
    if (!*read) return {{"error", read->error().code.value()}};
    return {{"minor", (**read).minor_faults()}, {"major", (**read).major_faults()}};
}

auto backing_evidence(const std::optional<HugeTlbBacking>& backing) -> Json {
    if (!backing) return nullptr;
    return {{"extent_bytes", backing->extent_bytes}, {"kernel_page_bytes", backing->kernel_page_bytes},
            {"mmu_page_bytes", backing->mmu_page_bytes}, {"private_hugetlb_bytes", backing->private_hugetlb_bytes},
            {"shared_hugetlb_bytes", backing->shared_hugetlb_bytes}, {"hugetlb_vma", backing->hugetlb_vma}};
}

auto read_fixture_backing(const Json& row) -> HugeTlbBacking {
    return {row.at("extent_bytes").get<U64>(), row.at("kernel_page_bytes").get<U64>(),
            row.at("mmu_page_bytes").get<U64>(), row.at("private_hugetlb_bytes").get<U64>(),
            row.at("shared_hugetlb_bytes").get<U64>(), row.at("hugetlb_vma").get<bool>()};
}

struct SuppliedOperations {
    std::optional<U64> begin, end;
    bool map_error{}, release_error{};
    std::size_t clock_reads{}, releases{};

    static auto now(void* context, clockid_t) noexcept -> detail::RawClockResult {
        auto& fixture = *static_cast<SuppliedOperations*>(context);
        const auto time = fixture.clock_reads++ == 0 ? fixture.begin : fixture.end;
        if (!time) return {-1, EIO, {}};
        constexpr U64 billion = 1000000000U;
        return {0, 0, {static_cast<time_t>(*time / billion), static_cast<long>(*time % billion)}};
    }
    static auto map(void* context, std::size_t length, int protection, int flags, int descriptor,
                    std::int64_t offset) noexcept -> detail::RawMapResult {
        if (static_cast<SuppliedOperations*>(context)->map_error) return {MAP_FAILED, ENOMEM};
        const auto operations = detail::production_mapping_operations();
        return operations.map_anonymous(operations.context, length, protection, flags, descriptor, offset);
    }
    static auto release(void* context, void* address, std::size_t length) noexcept -> detail::RawMappingStatusResult {
        auto& fixture = *static_cast<SuppliedOperations*>(context);
        if (fixture.releases++ == 0 && fixture.release_error) return {-1, EIO};
        const auto operations = detail::production_mapping_operations();
        return operations.unmap(operations.context, address, length);
    }
    auto mapping() noexcept -> detail::MappingOperations {
        auto operations = detail::production_mapping_operations();
        operations.context = this;
        operations.map_anonymous = map;
        operations.unmap = release;
        return operations;
    }
    auto clock() noexcept -> detail::ClockOperations { return {this, now}; }
};

struct Attempt {
    EpycBenchPackageMappingExtentV1 extent;
    MappingCallbacks callbacks;
    bool huge;
    const Json* fixture;
    detail::MappingOperations mapping;
    detail::ClockOperations clock;
    std::optional<NativeNumaMask> mask{};
    std::vector<void*> pages{};
    std::map<std::uint32_t, U64> residency{};
    U64 unresolved{}, attempted_pages{}, verified_pages{}, requests{};
    bool corrupt_request{}, contained{}, worker_retired{};
    int execute_status{-1}, verify_status{-1};
    std::optional<ClockRead> begin{}, end{};
    std::optional<std::expected<void, core::Error>> mapped{}, binding{}, released{};
    std::optional<core::Error> placement_error{}, backing_error{}, worker_error{};
    std::optional<HugeTlbBacking> backing{};
    std::optional<core::LinuxCpuId> before{}, after{}, service{};
    std::optional<FaultRead> faults_before{}, faults_after{};
    std::optional<AnonymousMapping> owner{};
    Json monitoring = nullptr;
    PackageMonitorClock* monitor{};

    static int allocate(void* context, U64 bytes) noexcept {
        auto& self = *static_cast<Attempt*>(context);
        if (self.requests++ != 0 || bytes != self.extent.bytes) {
            self.corrupt_request = true;
            return 1;
        }
        try {
            if (self.monitor) self.monitor->begin_bracket();
            auto timed = detail::time_anonymous_mapping(static_cast<std::size_t>(bytes), self.mapping, self.clock);
            if (self.monitor) self.monitor->end_bracket();
            self.begin.emplace(std::move(timed.start));
            self.end = std::move(timed.end);
            if (!timed.request) return 1;
            if (!*timed.request) {
                self.mapped.emplace(std::unexpected{std::move(timed.request->error())});
                return 1;
            }
            self.mapped.emplace();
            self.owner.emplace(std::move(**timed.request));
            return 0;
        } catch (...) {
            self.contained = true;
            return 1;
        }
    }

    void observe_pages() {
        if (fixture) {
            if (fixture->contains("residency_error")) {
                placement_error = failure("residency", "Supplied page query failed");
                return;
            }
            const auto& rows = fixture->at("residency");
            if (!rows.is_array() || rows.size() > pages.size()) {
                placement_error = failure("residency", "Expected a bounded supplied NUMA histogram");
                return;
            }
            for (const auto& row : rows)
                if (!residency.emplace(row.at("node").get<std::uint32_t>(), row.at("pages").get<U64>()).second) {
                    placement_error = failure("residency", "Duplicate supplied NUMA histogram entry");
                    return;
                }
            unresolved = fixture->value("unresolved_pages", U64{});
            return;
        }
        const auto bytes = owner->writable_bytes();
        for (std::size_t page{}; page < pages.size(); ++page)
            pages[page] = bytes.data() + page * extent.page_bytes;
        auto observed = query_page_locations(pages);
        if (!observed) {
            placement_error = std::move(observed.error());
            return;
        }
        if (observed->size() != pages.size()) {
            placement_error = failure("residency", "NUMA query did not report every page");
            return;
        }
        for (const auto& page : *observed) {
            if (page.kind() == PageQueryStatusKind::resident && page.node())
                ++residency[page.node()->value()];
            else
                ++unresolved;
        }
    }

    void first_touch() {
        const auto length = static_cast<std::size_t>(extent.bytes);
        auto fresh = huge && !fixture ? detail::AnonymousMappingAccess::create_explicit_hugetlb_2m(length)
                                      : detail::AnonymousMappingAccess::create(length, mapping);
        if (!fresh) {
            mapped.emplace(std::unexpected{std::move(fresh.error())});
            return;
        }
        mapped.emplace();
        owner.emplace(std::move(*fresh));
        if (!huge) {
            auto advice = owner->disable_transparent_huge_pages();
            if (!advice) {
                binding.emplace(std::unexpected{std::move(advice.error())});
                return;
            }
        }
        if (fixture) {
            if (fixture->value("bind_error", false))
                binding.emplace(std::unexpected{failure("bind", "Supplied policy bind failed")});
            else
                binding.emplace();
        } else {
            binding.emplace(bind_memory(*owner, *mask));
        }
        if (!*binding) return;
        auto* const stores = reinterpret_cast<unsigned char*>(owner->data());
        if (monitor) monitor->begin_bracket();
        begin.emplace(detail::RawMonotonicClockAccess::now(clock));
        if (!*begin) {
            if (monitor) monitor->end_bracket();
            return;
        }
        std::atomic_signal_fence(std::memory_order_seq_cst);
        const auto status = callbacks.implementation->first_touch(&extent, stores);
        std::atomic_signal_fence(std::memory_order_seq_cst);
        end.emplace(detail::RawMonotonicClockAccess::now(clock));
        if (monitor) monitor->end_bracket();
        execute_status = status;
        if (status == 0) attempted_pages = extent.pages;
        if (huge) {
            if (fixture) {
                if (fixture->contains("backing")) backing = read_fixture_backing(fixture->at("backing"));
            } else {
                auto observed = observe_hugetlb_backing(owner->bytes());
                if (observed) backing = *observed;
                else backing_error = std::move(observed.error());
            }
        }
        observe_pages();
        if (fixture && fixture->contains("corrupt_page")) {
            const auto page = fixture->at("corrupt_page").get<U64>();
            if (page < extent.pages) stores[page * extent.page_bytes] ^= 0xffU;
        }
        verify_status = callbacks.task->verify(&extent, stores, &verified_pages);
    }

    void run() noexcept {
        try {
            if (callbacks.implementation->first_touch) {
                first_touch();
            } else {
                execute_status = callbacks.implementation->allocate(&extent, {this, allocate});
                if (mapped && *mapped)
                    verify_status = callbacks.task->verify(&extent, nullptr, &verified_pages);
            }
        } catch (...) {
            contained = true;
        }
        if (owner) {
            try { released.emplace(owner->release()); }
            catch (...) { contained = true; }
        }
    }
};

auto clock_value(const std::optional<ClockRead>& read) -> std::optional<U64> {
    if (read && *read) return (**read).nanoseconds();
    return std::nullopt;
}

auto fixture_clock(const Json& row, const char* name) -> std::optional<U64> {
    const auto& value = row.at(name);
    if (value.is_null()) return std::nullopt;
    if (!value.is_number_unsigned()) throw std::invalid_argument("Expected an unsigned supplied timestamp");
    return value.get<U64>();
}

auto fixture_fault(const Json& row) -> FaultRead {
    if (row.contains("error")) {
        auto error = failure("faults", "Supplied page-fault read failed");
        error.code = core::StableErrorCode{row.at("error").get<std::string>()};
        return std::unexpected{std::move(error)};
    }
    return LinuxTaskPageFaultCounters::create(row.at("minor").get<U64>(), row.at("major").get<U64>());
}

void run_attempt(Attempt& attempt, core::LinuxCpuId worker_cpu, core::LinuxCpuId service_cpu,
                 bool touch, PackageMonitor* monitor) {
    const auto* supplied = attempt.fixture;
    if (supplied) {
        attempt.service = core::LinuxCpuId{supplied->value("service_cpu", service_cpu.value())};
    } else {
        const auto cpu = set_current_thread_affinity(core::CpuSet{service_cpu}).and_then(
            [&] { return located_cpu(service_cpu); });
        if (!cpu) {
            attempt.worker_error = cpu.error();
            return;
        }
        attempt.service = *cpu;
    }
    if (attempt.service != service_cpu) return;
    // Decode fixture faults before launching to keep malformed input outside the
    // dispatch/retirement protocol. Native reads use the same command bracket.
    std::optional<FaultRead> supplied_before, supplied_after;
    if (supplied && touch) {
        supplied_before.emplace(fixture_fault(supplied->at("faults_before")));
        supplied_after.emplace(fixture_fault(supplied->at("faults_after")));
    }
    std::latch ready{1}, dispatch{1}, done{1}, retire{1};
    std::optional<LinuxThreadId> tid;
    std::jthread worker{[&] {
        try {
            const auto placed = supplied
                ? std::expected<core::LinuxCpuId, core::Error>{core::LinuxCpuId{supplied->value("cpu_before", worker_cpu.value())}}
                : set_current_thread_affinity(core::CpuSet{worker_cpu}).and_then([&] { return located_cpu(worker_cpu); });
            if (placed) attempt.before = *placed;
            else attempt.worker_error = placed.error();
            if (touch && !supplied && attempt.before == worker_cpu) {
                auto identity = current_thread_id();
                if (identity) tid = *identity;
                else attempt.worker_error = std::move(identity.error());
            }
        } catch (...) { attempt.contained = true; }
        ready.count_down();
        dispatch.wait();
        if (!attempt.contained && !attempt.worker_error && attempt.before == worker_cpu) {
            try {
                const auto* supplied_monitoring = supplied && supplied->contains("monitoring")
                    ? &supplied->at("monitoring") : nullptr;
                PackageMonitorClock monitoring{{}, monitor, worker_cpu, supplied_monitoring,
                                               attempt.monitoring};
                if (monitor) attempt.monitor = &monitoring;
                attempt.run();
                auto placed = supplied
                    ? std::expected<core::LinuxCpuId, core::Error>{core::LinuxCpuId{supplied->value("cpu_after", worker_cpu.value())}}
                    : located_cpu(worker_cpu);
                if (placed) attempt.after = *placed;
                else attempt.worker_error = std::move(placed.error());
            } catch (...) { attempt.contained = true; }
        }
        done.count_down();
        retire.wait();
    }};
    ready.wait();
    if (supplied_before) attempt.faults_before = std::move(supplied_before);
    else if (tid) attempt.faults_before.emplace(read_task_page_fault_counters(*tid));
    dispatch.count_down();
    done.wait();
    if (supplied_after) attempt.faults_after = std::move(supplied_after);
    else if (tid) attempt.faults_after.emplace(read_task_page_fault_counters(*tid));
    retire.count_down();
    worker.join();
    attempt.worker_retired = true;
}

auto status(const std::optional<std::expected<void, core::Error>>& result) -> Json {
    return result ? Json(result->has_value()) : Json(nullptr);
}

auto observed_cpu(std::optional<core::LinuxCpuId> cpu) -> Json {
    return cpu ? Json(cpu->value()) : Json(nullptr);
}

} // namespace

auto run_mapping(const Role& role, const Json& admitted, const Json* supplied, PackageMonitor* monitor)
    -> std::expected<MappingRank, core::Error> {
    const auto callbacks = role.module.mapping();
    if (!callbacks.task || role.cpus.size() != 1U || role.request.repetitions != 1U)
        return std::unexpected{failure("admission", "Invalid fresh mapping worker or module")};
    const bool touch = callbacks.implementation->first_touch;
    const bool huge = admitted.at("page_mode") == "hugetlb-2m";
    const EpycBenchPackageMappingExtentV1 extent{admitted.at("requested_bytes").get<U64>(),
        admitted.at("page_bytes").get<U64>(), admitted.at("pages").get<U64>()};
    const auto bytes = core::ByteCount::from_base_units(extent.bytes);
    using namespace kernel::packages;
    static_assert(huge_page_bytes == hugetlb_2m_bytes);
    if (touch ? (first_touch_extent(core::ByteCount::from_base_units(extent.page_bytes), extent.pages) != bytes ||
                 admitted.at("linux_numa_node") != role.node.value() ||
                 admitted.at("operation") != mapping_scope(MappingResource::first_touch) ||
                 admitted.at("pages") != role.request.parameters.at("pages") ||
                 admitted.at("page_mode") != role.request.parameters.at("page_mode") ||
                 (huge ? extent.page_bytes != hugetlb_2m_bytes || admitted.at("base_page_bytes") != 4096U
                       : admitted.at("page_mode") != "base" || admitted.at("base_page_bytes") != extent.page_bytes))
              : (!valid_allocation_extent(bytes) || extent.pages != 0 || extent.page_bytes != 0 ||
                 !admitted.at("linux_numa_node").is_null() || !admitted.at("page_mode").is_null() ||
                 !admitted.at("base_page_bytes").is_null() ||
                 admitted.at("operation") != mapping_scope(MappingResource::allocation) ||
                 admitted.at("requested_bytes") != role.request.parameters.at("requested_bytes")))
        return std::unexpected{failure("admission", "Invalid admitted fresh mapping geometry")};
    const auto cpu = role.cpus.front();
    const core::LinuxCpuId service{admitted.at("service_cpu").get<std::uint32_t>()};
    if (cpu == service)
        return std::unexpected{failure("admission", "Worker and service CPUs must be distinct")};
    if (touch && !supplied) {
        const auto native_page = ::sysconf(_SC_PAGESIZE);
        if (native_page <= 0 || admitted.at("base_page_bytes") != static_cast<U64>(native_page))
            return std::unexpected{failure("admission", "Native base page size differs from the admitted topology")};
    }
    SuppliedOperations operations;
    if (supplied) {
        if (supplied->at("role") != role.id || supplied->at("rank") != 0 || supplied->at("cpu") != cpu.value())
            return std::unexpected{failure("fixture", "Supplied mapping worker does not match admitted role or CPU")};
        operations = {fixture_clock(*supplied, "begin_ns"), fixture_clock(*supplied, "end_ns"),
                      supplied->value("map_error", false), supplied->value("release_error", false)};
    }
    Attempt attempt{extent, callbacks, huge, supplied,
        supplied ? operations.mapping() : detail::production_mapping_operations(),
        supplied ? operations.clock() : detail::production_clock_operations()};
    if (touch) {
        attempt.pages.resize(static_cast<std::size_t>(extent.pages));
        if (!supplied) {
            auto mask = NativeNumaMask::from_node_set(core::LinuxNumaNodeSet{role.node});
            if (!mask) return std::unexpected{std::move(mask.error())};
            attempt.mask = std::move(*mask);
        }
    }
    run_attempt(attempt, cpu, service, touch, monitor);

    const auto here = attempt.before == cpu && attempt.after == cpu && attempt.service == service;
    MappingRank result{{role.id, 0U, 1U, Trial{},
        {cpu, role.node, attempt.before, attempt.after, extent.page_bytes, {}, here}, {}, {}, {}}, admitted};
    result.rank.monitoring = std::move(attempt.monitoring);
    auto& trial = *result.rank.trial;
    trial.begin_ns = clock_value(attempt.begin);
    trial.end_ns = clock_value(attempt.end);
    trial.execute_status = attempt.execute_status;
    trial.verify_status = attempt.verify_status;
    trial.expected = {1U, touch ? extent.pages : 1U, 0U};
    trial.actual = {attempt.execute_status == 0 ? 1U : 0U,
                    touch ? attempt.attempted_pages : attempt.mapped && *attempt.mapped ? 1U : 0U, 0U};
    trial.input_unchanged = true;
    trial.output_matches = attempt.verify_status == 0;
    trial.work_matches = trial.expected.repetitions == trial.actual.repetitions && trial.expected.work_units == trial.actual.work_units;
    const auto failed_phase = [](const auto& phase) -> const core::Error* {
        return phase && !*phase ? &phase->error() : nullptr;
    };
    auto failures = Json::array();
    const auto record = [&](const char* phase, const core::Error* error) {
        if (!error) return;
        failures.push_back({{"phase", phase}, {"error", *error}});
        if (!result.rank.error && std::string_view{phase} != "release") result.rank.error = *error;
    };
    record("mapping", failed_phase(attempt.mapped));
    record("binding", failed_phase(attempt.binding));
    if (const auto* error = failed_phase(attempt.begin)) trial.timing_error = *error;
    if (const auto* error = failed_phase(attempt.end)) trial.timing_error = *error;
    record("start", failed_phase(attempt.begin));
    record("end", failed_phase(attempt.end));
    record("backing", attempt.backing_error ? &*attempt.backing_error : nullptr);
    record("residency", attempt.placement_error ? &*attempt.placement_error : nullptr);
    record("worker", attempt.worker_error ? &*attempt.worker_error : nullptr);
    if (const auto* error = failed_phase(attempt.released)) result.rank.cleanup_error = *error;
    record("release", failed_phase(attempt.released));
    if (attempt.contained)
        result.rank.error = failure("worker", "Mapping operation contained an exception");
    auto residency = Json::array();
    for (const auto& [node, pages] : attempt.residency)
        residency.push_back({{"node", node}, {"pages", pages}});
    result.mapping.update(Json{{"mapping_succeeded", status(attempt.mapped)}, {"binding_succeeded", status(attempt.binding)},
        {"release_succeeded", status(attempt.released)}, {"requests", attempt.requests}, {"invalid_request", attempt.corrupt_request},
        {"worker_retired", attempt.worker_retired},
        {"attempted_pages", attempt.attempted_pages}, {"verified_pages", attempt.verified_pages},
        {"service_cpu_observed", observed_cpu(attempt.service)}, {"residency", std::move(residency)}, {"unresolved_pages", attempt.unresolved},
        {"hugetlb_backing", backing_evidence(attempt.backing)}, {"faults_before", fault_evidence(attempt.faults_before)},
        {"faults_after", fault_evidence(attempt.faults_after)}, {"failures", std::move(failures)}});
    return result;
}

} // namespace epycbench::platform_linux::package_execution
