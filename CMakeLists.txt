cmake_minimum_required(VERSION 3.25)

project(
    EpycBench
    VERSION 0.1.0
    DESCRIPTION "Topology-linked benchmarking for AMD EPYC systems"
    LANGUAGES CXX
)

# This source distribution omits the private development test suite.
set(BUILD_TESTING OFF CACHE BOOL "Build the development test suite")
if(BUILD_TESTING)
    message(FATAL_ERROR "The public source distribution does not contain the development test suite")
endif()

include(CTest)

set(CMAKE_CXX_EXTENSIONS OFF)
set(EPYCBENCH_BUILD_PRESET "unknown" CACHE STRING "CMake preset used for this build")

message(STATUS "EpycBench build preset: ${EPYCBENCH_BUILD_PRESET}")

include(cmake/EpycBenchBuildOptions.cmake)
include(cmake/EpycBenchDependencies.cmake)
include(cmake/EpycBenchWarnings.cmake)

find_package(Threads REQUIRED)
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

include(CheckCXXSourceCompiles)
set(EPYCBENCH_SAVED_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES}")
set(EPYCBENCH_SAVED_CXX_STANDARD "${CMAKE_CXX_STANDARD}")
set(CMAKE_CXX_STANDARD 23)
list(APPEND CMAKE_REQUIRED_LIBRARIES Threads::Threads)
check_cxx_source_compiles(
    "#include <condition_variable>
     #include <mutex>
     #include <stop_token>
     #include <thread>
     int main() {
       std::mutex mutex;
       std::condition_variable condition;
       std::stop_source source;
       std::stop_token token = source.get_token();
       std::jthread worker([](std::stop_token) {}, token);
       source.request_stop();
       worker.join();
       (void)mutex;
       (void)condition;
       return 0;
     }"
    EPYCBENCH_HAS_STANDARD_WORKER_THREADS_CXX23
)
check_cxx_source_compiles(
    "#include <signal.h>
     #include <pthread.h>
     #include <atomic>
     static_assert(std::atomic<int>::is_always_lock_free);
     int main() {
       sigset_t mask{}; struct sigaction action{};
       (void)sigemptyset(&mask); (void)sigaddset(&mask, SIGINT);
       (void)sigismember(&mask, SIGTERM);
       (void)pthread_sigmask(SIG_SETMASK, nullptr, &mask);
       (void)pthread_equal(pthread_self(), pthread_self());
       (void)sigaction(SIGINT, nullptr, &action);
       return 0;
     }"
    EPYCBENCH_HAS_CONTROLLER_SIGNAL_SCOPE
)
if(NOT EPYCBENCH_HAS_CONTROLLER_SIGNAL_SCOPE)
    message(FATAL_ERROR "Required controller signal and lock-free atomic support unavailable")
endif()
set(CMAKE_REQUIRED_LIBRARIES "${EPYCBENCH_SAVED_REQUIRED_LIBRARIES}")
if(EPYCBENCH_SAVED_CXX_STANDARD STREQUAL "")
    unset(CMAKE_CXX_STANDARD)
else()
    set(CMAKE_CXX_STANDARD "${EPYCBENCH_SAVED_CXX_STANDARD}")
endif()
unset(EPYCBENCH_SAVED_CXX_STANDARD)
unset(EPYCBENCH_SAVED_REQUIRED_LIBRARIES)
if(NOT EPYCBENCH_HAS_STANDARD_WORKER_THREADS_CXX23)
    message(FATAL_ERROR "The C++ standard library lacks required persistent-worker threading support")
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "EpycBench requires Linux dynamic CPU affinity masks")
endif()

set(EPYCBENCH_SAVED_REQUIRED_DEFINITIONS "${CMAKE_REQUIRED_DEFINITIONS}")
list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_cxx_source_compiles(
    "#include <sched.h>
     int main() {
       const size_t size = CPU_ALLOC_SIZE(2048);
       cpu_set_t* set = CPU_ALLOC(2048);
       if (set == nullptr) return 0;
       CPU_ZERO_S(size, set);
       CPU_SET_S(1024, size, set);
       CPU_CLR_S(1024, size, set);
       const int present = CPU_ISSET_S(1024, size, set);
       CPU_FREE(set);
       return present;
     }"
    EPYCBENCH_HAS_DYNAMIC_CPU_SET
)
set(CMAKE_REQUIRED_DEFINITIONS "${EPYCBENCH_SAVED_REQUIRED_DEFINITIONS}")
unset(EPYCBENCH_SAVED_REQUIRED_DEFINITIONS)
if(NOT EPYCBENCH_HAS_DYNAMIC_CPU_SET)
    message(FATAL_ERROR "Linux libc lacks the required GNU dynamic CPU-set API")
endif()

set(EPYCBENCH_SAVED_REQUIRED_DEFINITIONS "${CMAKE_REQUIRED_DEFINITIONS}")
list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_cxx_source_compiles(
    "#include <sched.h>
     #include <unistd.h>
     int main() {
       cpu_set_t mask{};
       const size_t size = sizeof(mask);
       const pid_t tid = gettid();
       const int cpu = sched_getcpu();
       const int get_result = sched_getaffinity(0, size, &mask);
       const int set_result = sched_setaffinity(0, size, &mask);
       return static_cast<int>(tid) + cpu + get_result + set_result;
     }"
    EPYCBENCH_HAS_THREAD_IDENTITY_AND_AFFINITY
)
set(CMAKE_REQUIRED_DEFINITIONS "${EPYCBENCH_SAVED_REQUIRED_DEFINITIONS}")
unset(EPYCBENCH_SAVED_REQUIRED_DEFINITIONS)
if(NOT EPYCBENCH_HAS_THREAD_IDENTITY_AND_AFFINITY)
    message(
        FATAL_ERROR
        "Linux libc lacks required gettid, sched_getcpu, sched_getaffinity, or sched_setaffinity support"
    )
endif()

set(EPYCBENCH_SAVED_REQUIRED_DEFINITIONS "${CMAKE_REQUIRED_DEFINITIONS}")
list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_cxx_source_compiles(
    "#include <cstddef>
     #include <sys/mman.h>
     int main() {
       void* address = mmap(nullptr, std::size_t{4096}, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
       const int advice = madvise(address, std::size_t{4096}, MADV_NOHUGEPAGE);
       const int protection = mprotect(address, std::size_t{4096}, PROT_NONE);
       const int release = munmap(address, std::size_t{4096});
       return advice + protection + release;
     }"
    EPYCBENCH_HAS_ANONYMOUS_MEMORY_MAPPING
)
set(CMAKE_REQUIRED_DEFINITIONS "${EPYCBENCH_SAVED_REQUIRED_DEFINITIONS}")
unset(EPYCBENCH_SAVED_REQUIRED_DEFINITIONS)
if(NOT EPYCBENCH_HAS_ANONYMOUS_MEMORY_MAPPING)
    message(
        FATAL_ERROR
        "Linux libc lacks required mmap, munmap, mprotect, madvise, or MADV_NOHUGEPAGE support"
    )
endif()

set(EPYCBENCH_SAVED_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES}")
list(APPEND CMAKE_REQUIRED_LIBRARIES NUMA::NUMA)
check_cxx_source_compiles(
    "#include <linux/mempolicy.h>
     #include <numaif.h>
     int main() {
       void* pages[1]{nullptr};
       int status[1]{};
       const long bind_result = mbind(nullptr, 0UL, MPOL_BIND | MPOL_F_STATIC_NODES,
                                      nullptr, 0UL, 0U);
       const long query_result = move_pages(0, 1UL, pages, nullptr, status, 0);
       unsigned long allowed[1]{};
       const long allowed_result = get_mempolicy(nullptr, allowed, 1UL, nullptr,
                                                  MPOL_F_MEMS_ALLOWED);
       return static_cast<int>(bind_result + query_result + allowed_result);
     }"
    EPYCBENCH_HAS_NUMA_POLICY_API
)
set(CMAKE_REQUIRED_LIBRARIES "${EPYCBENCH_SAVED_REQUIRED_LIBRARIES}")
unset(EPYCBENCH_SAVED_REQUIRED_LIBRARIES)
if(NOT EPYCBENCH_HAS_NUMA_POLICY_API)
    message(FATAL_ERROR "Linux libnuma lacks required mbind, move_pages, get_mempolicy, MPOL_BIND, MPOL_F_STATIC_NODES, or MPOL_F_MEMS_ALLOWED support")
endif()

set(EPYCBENCH_SAVED_REQUIRED_DEFINITIONS "${CMAKE_REQUIRED_DEFINITIONS}")
list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_cxx_source_compiles(
    "#include <csignal>
     #include <fcntl.h>
     #include <poll.h>
     #include <spawn.h>
     #include <sys/socket.h>
     #include <sys/wait.h>
     #include <unistd.h>
     int main() {
       int descriptors[2]{};
       const int pair = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors);
       char byte{};
       const auto received = recv(descriptors[0], &byte, 1, 0);
       const auto sent = send(descriptors[0], &byte, 1, MSG_NOSIGNAL);
       const int read_shutdown = shutdown(descriptors[0], SHUT_RD);
       pollfd item{descriptors[0], static_cast<short>(POLLIN | POLLOUT),
                   static_cast<short>(POLLHUP | POLLERR | POLLNVAL)};
       const int ready = poll(&item, 1, 0);
       const int duplicated = fcntl(descriptors[0], F_DUPFD_CLOEXEC, 5);
       posix_spawn_file_actions_t actions{};
       int setup = posix_spawn_file_actions_init(&actions);
       setup += posix_spawn_file_actions_adddup2(&actions, descriptors[0], 3);
       setup += posix_spawn_file_actions_addclosefrom_np(&actions, 5);
       setup += posix_spawn_file_actions_destroy(&actions);
       pid_t pid{};
       int status{};
       char executable[] = \"/nonexistent\";
       char* arguments[]{executable, nullptr};
       char* environment[]{nullptr};
       const int spawned = posix_spawn(&pid, executable, nullptr, nullptr, arguments, environment);
       const auto waited = waitpid(pid, &status, WNOHANG);
       const int terminal = WIFEXITED(status) ? WEXITSTATUS(status)
                            : WIFSIGNALED(status) ? WTERMSIG(status) + WCOREDUMP(status) : 0;
       const int signaled = kill(pid, SIGTERM) + kill(pid, SIGKILL);
       return pair + static_cast<int>(received + sent) + read_shutdown + ready + duplicated + setup + spawned +
              static_cast<int>(waited) + terminal + signaled;
     }"
    EPYCBENCH_HAS_PROCESS_IPC_API
)
set(CMAKE_REQUIRED_DEFINITIONS "${EPYCBENCH_SAVED_REQUIRED_DEFINITIONS}")
unset(EPYCBENCH_SAVED_REQUIRED_DEFINITIONS)
if(NOT EPYCBENCH_HAS_PROCESS_IPC_API)
    message(FATAL_ERROR "Linux libc lacks required process and IPC primitives")
endif()

find_package(Git REQUIRED)
if(NOT IS_ABSOLUTE "${GIT_EXECUTABLE}")
    message(FATAL_ERROR "Q1 Git executable must be an absolute configured path")
endif()

set(
    EPYCBENCH_MODULE_TARGETS
    core
    platform_linux
    topology
    kernel_api
    scenario
    runtime
    memory
    kernel_bandwidth
    kernel_latency
    measurement
    monitoring
    statistics
    reporting
)

foreach(module IN LISTS EPYCBENCH_MODULE_TARGETS)
    if(module STREQUAL "core")
        add_library(
            epycbench_core
            STATIC
                src/core/canonical_json.cpp
                src/core/digest.cpp
                src/core/governed_repository_file.cpp
                src/core/governed_repository_root_authority_access.cpp
                src/core/seed.cpp
        )
        target_compile_features(epycbench_core PUBLIC cxx_std_23)
        target_include_directories(
            epycbench_core
            PUBLIC
                $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        )
        target_link_libraries(epycbench_core PUBLIC nlohmann_json::nlohmann_json)
        epycbench_enable_build_options(epycbench_core)
        epycbench_enable_warnings(epycbench_core)
    elseif(module STREQUAL "platform_linux")
        add_library(
            epycbench_platform_linux
            STATIC
                src/platform_linux/file.cpp
                src/platform_linux/bounded_sysfs_operations.cpp
                src/platform_linux/topology_source_io.cpp
                src/platform_linux/task_stat.cpp
                src/platform_linux/perf_context_switch.cpp
                src/platform_linux/q2_perf_self_diagnostic.cpp
                src/platform_linux/hwmon_temperature.cpp
                src/platform_linux/cpu_frequency.cpp
                src/platform_linux/dynamic_cpu_mask.cpp
                src/platform_linux/thread_identity.cpp
                src/platform_linux/thread_affinity.cpp
                src/platform_linux/benchmark_run_input.cpp
                src/platform_linux/raw_monotonic_clock.cpp
                src/platform_linux/thread_rusage.cpp
                src/platform_linux/memory_mapping.cpp
                src/platform_linux/numa_policy.cpp
                src/platform_linux/page_location.cpp
                src/platform_linux/file_descriptor.cpp
                src/platform_linux/host_baseline_file_export.cpp
                src/platform_linux/host_work_admission.cpp
                src/platform_linux/host_frozen_process_launch.cpp
                src/platform_linux/host_frozen_control_preparation.cpp
                src/platform_linux/host_work_exec_entry.cpp
                src/platform_linux/journal_file.cpp
                src/platform_linux/process_ipc_operations.cpp
                src/platform_linux/local_ipc.cpp
                src/platform_linux/child_process.cpp
                src/platform_linux/current_process_image.cpp
                src/platform_linux/controller_signal_scope.cpp
                src/platform_linux/run_start_identity.cpp
                src/platform_linux/q4_capacity_control.cpp
        )
        target_compile_features(epycbench_platform_linux PUBLIC cxx_std_23)
        target_compile_definitions(epycbench_platform_linux PRIVATE _GNU_SOURCE)
        target_include_directories(
            epycbench_platform_linux
            PUBLIC
                $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            PRIVATE
                "${PROJECT_SOURCE_DIR}/src"
        )
        target_link_libraries(epycbench_platform_linux PUBLIC EpycBench::core PRIVATE NUMA::NUMA Threads::Threads)
        epycbench_enable_build_options(epycbench_platform_linux)
        epycbench_enable_warnings(epycbench_platform_linux)
    elseif(module STREQUAL "topology")
        add_library(
            epycbench_topology
            STATIC
                src/topology/fingerprint.cpp
                src/topology/checked_topology.cpp
                src/topology/invariants.cpp
                src/topology/hwloc_source.cpp
                src/topology/model.cpp
                src/topology/reconciliation.cpp
                src/topology/stable_id.cpp
                src/topology/serialization.cpp
                src/topology/topology_json_parser.cpp
                src/topology/linux_source.cpp
                src/topology/reference_fixture.cpp
                src/topology/reference_fixture_sha256.cpp
        )
        target_compile_features(epycbench_topology PUBLIC cxx_std_23)
        target_include_directories(
            epycbench_topology
            PUBLIC
                $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        )
        target_link_libraries(
            epycbench_topology
            PUBLIC EpycBench::core
            PRIVATE HWLOC::HWLOC EpycBench::platform_linux
        )
        target_include_directories(epycbench_topology PRIVATE "${PROJECT_SOURCE_DIR}/src")
        epycbench_enable_build_options(epycbench_topology)
        epycbench_enable_warnings(epycbench_topology)
    elseif(module STREQUAL "scenario")
        add_library(
            epycbench_scenario
            STATIC
                src/scenario/parser.cpp
                src/scenario/default_expansion.cpp
                src/scenario/byte_quantity_parser.cpp
                src/scenario/exact_ratio.cpp
                src/scenario/kernel_dispatch.cpp
                src/scenario/kernel_binding_projection.cpp
                src/scenario/kernel_resolution.cpp
                src/scenario/identifier.cpp
                src/scenario/matrix_expansion.cpp
                src/scenario/memory_placement_resolution.cpp
                src/scenario/quantity_parser.cpp
                src/scenario/resolution.cpp
                src/scenario/resource_resolution.cpp
                src/scenario/resource_selector.cpp
                src/scenario/scenario_plan_digest.cpp
                src/scenario/retained_plan_projection.cpp
                src/scenario/retained_plan_projection_resources.cpp
                src/scenario/service_cpu_resolution.cpp
                src/scenario/restricted_yaml_parser.cpp
                src/scenario/scenario_v1_structure.cpp
                src/scenario/scenario_v1_defaults.cpp
                src/scenario/scenario_v1_memory.cpp
                src/scenario/scenario_v1_kernels.cpp
                src/scenario/scenario_v1_matrix.cpp
                src/scenario/scenario_v1_service_cpus.cpp
                src/scenario/scenario_v1_units.cpp
                src/scenario/scalar_resolver.cpp
                src/scenario/structural_validation.cpp
                src/scenario/strict_json_parser.cpp
                src/scenario/unit_normalization.cpp
        )
        target_compile_features(epycbench_scenario PUBLIC cxx_std_23)
        target_include_directories(
            epycbench_scenario
            PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            PRIVATE "${PROJECT_SOURCE_DIR}/src"
        )
        target_link_libraries(
            epycbench_scenario
            PUBLIC EpycBench::core EpycBench::topology EpycBench::kernel_api
                   nlohmann_json::nlohmann_json
            PRIVATE PkgConfig::LIBFYAML
        )
        epycbench_enable_build_options(epycbench_scenario)
        epycbench_enable_warnings(epycbench_scenario)
    elseif(module STREQUAL "runtime")
        add_library(
            epycbench_runtime
            STATIC
                src/runtime/ipc/protocol.cpp
                src/runtime/platform_ipc_adapters.cpp
                src/runtime/runner_control_reader.cpp
                src/runtime/runner_internal_mode.cpp
        )
        target_compile_features(epycbench_runtime PUBLIC cxx_std_23)
        target_include_directories(
            epycbench_runtime
            PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            PRIVATE "${PROJECT_SOURCE_DIR}/src"
        )
        target_link_libraries(
            epycbench_runtime PUBLIC EpycBench::core nlohmann_json::nlohmann_json
                              PRIVATE EpycBench::platform_linux
        )
        epycbench_enable_build_options(epycbench_runtime)
        epycbench_enable_warnings(epycbench_runtime)
    elseif(module STREQUAL "memory")
        add_library(
            epycbench_memory
            STATIC
                src/memory/placement_diagnostic.cpp
                src/memory/retained_region_cleanup.cpp
                src/memory/region_binding.cpp
                src/memory/region_finalization.cpp
                src/memory/region_finalization_diagnostic.cpp
                src/memory/region_initialization.cpp
                src/memory/region_mapping.cpp
                src/memory/region_placement_evaluation.cpp
                src/memory/region_placement_query.cpp
                src/memory/region_protection.cpp
                src/memory/region_request.cpp
        )
        target_compile_features(epycbench_memory PUBLIC cxx_std_23)
        target_include_directories(
            epycbench_memory
            PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            PRIVATE "${PROJECT_SOURCE_DIR}/src"
        )
        target_link_libraries(
            epycbench_memory
            PUBLIC EpycBench::core EpycBench::topology EpycBench::platform_linux
        )
        epycbench_enable_build_options(epycbench_memory)
        epycbench_enable_warnings(epycbench_memory)
    elseif(module STREQUAL "kernel_api")
        add_library(epycbench_kernel_api STATIC src/kernel/kernel_contracts.cpp)
        target_include_directories(epycbench_kernel_api PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_compile_features(epycbench_kernel_api PUBLIC cxx_std_23)
        target_include_directories(
            epycbench_kernel_api PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        )
        target_link_libraries(
            epycbench_kernel_api PUBLIC EpycBench::core nlohmann_json::nlohmann_json
        )
        epycbench_enable_build_options(epycbench_kernel_api)
        epycbench_enable_warnings(epycbench_kernel_api)
    elseif(module STREQUAL "kernel_bandwidth")
        add_library(
            epycbench_kernel_bandwidth STATIC
            src/kernel/sequential_read_bandwidth_validation.cpp
            src/kernel/sequential_read_bandwidth_preparation.cpp
            src/kernel/sequential_read_bandwidth_execute.cpp
            src/kernel/sequential_read_bandwidth_accounting.cpp
            src/kernel/sequential_read_bandwidth_checksum.cpp
        )
        set_property(
            SOURCE src/kernel/sequential_read_bandwidth_execute.cpp
            APPEND
            PROPERTY COMPILE_OPTIONS
                     "$<$<CXX_COMPILER_ID:GNU>:-fno-lto>"
                     "$<$<CXX_COMPILER_ID:GNU>:-fno-tree-loop-vectorize>"
                     "$<$<CXX_COMPILER_ID:GNU>:-fno-tree-slp-vectorize>"
                     "$<$<CXX_COMPILER_ID:GNU>:-fno-unroll-loops>"
                     "$<$<CXX_COMPILER_ID:Clang>:-fno-lto>"
                     "$<$<CXX_COMPILER_ID:Clang>:-fno-vectorize>"
                     "$<$<CXX_COMPILER_ID:Clang>:-fno-slp-vectorize>"
                     "$<$<CXX_COMPILER_ID:Clang>:-fno-unroll-loops>"
        )
        target_compile_features(epycbench_kernel_bandwidth PUBLIC cxx_std_23)
        target_include_directories(
            epycbench_kernel_bandwidth
            PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            PRIVATE "${PROJECT_SOURCE_DIR}/src"
        )
        target_link_libraries(
            epycbench_kernel_bandwidth PUBLIC EpycBench::core EpycBench::kernel_api
        )
        epycbench_enable_build_options(epycbench_kernel_bandwidth)
        epycbench_enable_warnings(epycbench_kernel_bandwidth)
    elseif(module STREQUAL "kernel_latency")
        add_library(
            epycbench_kernel_latency STATIC
            src/kernel/dependent_load_latency_validation.cpp
            src/kernel/dependent_load_latency_preparation.cpp
            src/kernel/dependent_load_latency_schedule.cpp
            src/kernel/dependent_load_latency_execute.cpp
            src/kernel/dependent_load_latency_verification.cpp
        )
        set_property(
            SOURCE src/kernel/dependent_load_latency_execute.cpp
            APPEND
            PROPERTY COMPILE_OPTIONS
                     "$<$<CXX_COMPILER_ID:GNU>:-fno-lto>"
                     "$<$<CXX_COMPILER_ID:GNU>:-fno-tree-loop-vectorize>"
                     "$<$<CXX_COMPILER_ID:GNU>:-fno-tree-slp-vectorize>"
                     "$<$<CXX_COMPILER_ID:GNU>:-fno-unroll-loops>"
                     "$<$<CXX_COMPILER_ID:Clang>:-fno-lto>"
                     "$<$<CXX_COMPILER_ID:Clang>:-fno-vectorize>"
                     "$<$<CXX_COMPILER_ID:Clang>:-fno-slp-vectorize>"
                     "$<$<CXX_COMPILER_ID:Clang>:-fno-unroll-loops>"
        )
        target_compile_features(epycbench_kernel_latency PUBLIC cxx_std_23)
        target_include_directories(
            epycbench_kernel_latency
            PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        )
        target_link_libraries(
            epycbench_kernel_latency PUBLIC EpycBench::core EpycBench::kernel_api
        )
        epycbench_enable_build_options(epycbench_kernel_latency)
        epycbench_enable_warnings(epycbench_kernel_latency)
    elseif(module STREQUAL "measurement")
        add_library(
            epycbench_measurement
            STATIC src/measurement/observation.cpp src/measurement/empty_harness.cpp
                   src/measurement/calibration.cpp src/measurement/warmup.cpp
                   src/measurement/block_scheduler.cpp
                   src/measurement/observation_execution.cpp
                   src/measurement/validity.cpp
                   src/measurement/repetition.cpp
        )
        target_compile_features(epycbench_measurement PUBLIC cxx_std_23)
        target_include_directories(
            epycbench_measurement PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                                  PRIVATE "${PROJECT_SOURCE_DIR}/src"
        )
        target_link_libraries(
            epycbench_measurement
            PUBLIC EpycBench::core EpycBench::kernel_bandwidth EpycBench::kernel_latency
                   EpycBench::platform_linux
        )
        epycbench_enable_build_options(epycbench_measurement)
        epycbench_enable_warnings(epycbench_measurement)
    elseif(module STREQUAL "monitoring")
        add_library(
            epycbench_monitoring STATIC src/monitoring/monitor_source.cpp
                                        src/monitoring/page_faults.cpp
                                        src/monitoring/context_switches.cpp
                                        src/monitoring/temperature.cpp
                                        src/monitoring/frequency.cpp
                                        src/monitoring/acquisition_driver.cpp
                                        src/monitoring/linux_interruptible_scheduler.cpp
                                        src/monitoring/linux_driver_operations_provider.cpp
        )
        target_compile_features(epycbench_monitoring PUBLIC cxx_std_23)
        target_include_directories(
            epycbench_monitoring PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        )
        target_link_libraries(
            epycbench_monitoring PUBLIC EpycBench::core EpycBench::platform_linux Threads::Threads
        )
        epycbench_enable_build_options(epycbench_monitoring)
        epycbench_enable_warnings(epycbench_monitoring)
    elseif(module STREQUAL "statistics")
        add_library(
            epycbench_statistics STATIC src/statistics/primary_metric.cpp
                                       src/statistics/descriptive_statistics.cpp
                                       src/statistics/outlier_flagging.cpp
                                       src/statistics/block_representatives.cpp
                                       src/statistics/log_ratio_comparison.cpp
                                       src/statistics/block_bootstrap.cpp
                                       src/statistics/prepared_statistics.cpp
                                       src/statistics/conclusion_classifier.cpp
                                       src/statistics/adaptive_stopping.cpp
        )
        target_compile_features(epycbench_statistics PUBLIC cxx_std_23)
        target_include_directories(
            epycbench_statistics
            PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            PRIVATE "${PROJECT_SOURCE_DIR}/src"
        )
        target_link_libraries(epycbench_statistics PUBLIC EpycBench::core)
        epycbench_enable_build_options(epycbench_statistics)
        epycbench_enable_warnings(epycbench_statistics)
    else()
        add_library(epycbench_${module} INTERFACE)
        target_compile_features(epycbench_${module} INTERFACE cxx_std_23)
    endif()
    add_library(EpycBench::${module} ALIAS epycbench_${module})
endforeach()

add_library(epycbench_kernel_registry STATIC src/kernel/registry.cpp)
add_library(EpycBench::kernel_registry ALIAS epycbench_kernel_registry)

add_library(epycbench_monitoring_host_capacity_authority STATIC
    src/platform_linux/authenticated_monitoring_capacity_authority.cpp)
add_library(EpycBench::monitoring_host_capacity_authority ALIAS
    epycbench_monitoring_host_capacity_authority)
target_compile_features(epycbench_monitoring_host_capacity_authority PUBLIC cxx_std_23)
target_include_directories(epycbench_monitoring_host_capacity_authority
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_monitoring_host_capacity_authority
    PUBLIC EpycBench::core EpycBench::topology)
epycbench_enable_build_options(epycbench_monitoring_host_capacity_authority)
epycbench_enable_warnings(epycbench_monitoring_host_capacity_authority)
target_compile_features(epycbench_kernel_registry PUBLIC cxx_std_23)
target_include_directories(
    epycbench_kernel_registry PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(epycbench_kernel_registry PUBLIC EpycBench::kernel_api)
epycbench_enable_build_options(epycbench_kernel_registry)
epycbench_enable_warnings(epycbench_kernel_registry)

add_library(epycbench_kernel_initialization STATIC src/kernel/region_initialization.cpp)
add_library(EpycBench::kernel_initialization ALIAS epycbench_kernel_initialization)
target_compile_features(epycbench_kernel_initialization PUBLIC cxx_std_23)
target_include_directories(
    epycbench_kernel_initialization
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(
    epycbench_kernel_initialization PUBLIC EpycBench::core EpycBench::kernel_api
)
epycbench_enable_build_options(epycbench_kernel_initialization)
epycbench_enable_warnings(epycbench_kernel_initialization)

add_library(EpycBench::runtime_ipc ALIAS epycbench_runtime)
add_library(
    epycbench_runtime_ipc_payloads
    STATIC
        src/runtime/ipc/payloads.cpp
        src/runtime/ipc/placement_request_construction_access.cpp
        src/runtime/placement_provenance.cpp
        src/runtime/controller_ipc_adapters.cpp
        src/runtime/controller_lifecycle.cpp
        src/runtime/controller_lifecycle_policy.cpp
)
add_library(EpycBench::runtime_ipc_payloads ALIAS epycbench_runtime_ipc_payloads)
target_compile_features(epycbench_runtime_ipc_payloads PUBLIC cxx_std_23)
target_include_directories(
    epycbench_runtime_ipc_payloads
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(
    epycbench_runtime_ipc_payloads
    PUBLIC EpycBench::runtime EpycBench::scenario EpycBench::topology EpycBench::kernel_api
    PRIVATE EpycBench::platform_linux
)
epycbench_enable_build_options(epycbench_runtime_ipc_payloads)
epycbench_enable_warnings(epycbench_runtime_ipc_payloads)

add_library(epycbench_runtime_scenario_monitoring_projection STATIC
    src/runtime/scenario_monitoring_policy_projection.cpp)
target_include_directories(epycbench_runtime_scenario_monitoring_projection PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_runtime_scenario_monitoring_projection PRIVATE EpycBench::core EpycBench::scenario)
epycbench_enable_build_options(epycbench_runtime_scenario_monitoring_projection)
epycbench_enable_warnings(epycbench_runtime_scenario_monitoring_projection)

add_library(epycbench_runtime_placement_request_projection STATIC
    src/runtime/placement_request_projection.cpp
)
add_library(EpycBench::runtime_placement_request_projection ALIAS
    epycbench_runtime_placement_request_projection
)
target_compile_features(epycbench_runtime_placement_request_projection PUBLIC cxx_std_23)
target_include_directories(epycbench_runtime_placement_request_projection
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(epycbench_runtime_placement_request_projection
    PUBLIC EpycBench::scenario EpycBench::runtime_ipc_payloads
    PRIVATE epycbench_runtime_scenario_monitoring_projection
)
epycbench_enable_build_options(epycbench_runtime_placement_request_projection)
epycbench_enable_warnings(epycbench_runtime_placement_request_projection)

add_library(epycbench_runner_context STATIC src/runtime/runner_context.cpp)
add_library(EpycBench::runner_context ALIAS epycbench_runner_context)
target_compile_features(epycbench_runner_context PUBLIC cxx_std_23)
target_include_directories(
    epycbench_runner_context
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(epycbench_runner_context PUBLIC EpycBench::topology)
epycbench_enable_build_options(epycbench_runner_context)
epycbench_enable_warnings(epycbench_runner_context)

add_library(epycbench_runner_control_monitor STATIC src/runtime/runner_control_monitor.cpp)
add_library(EpycBench::runner_control_monitor ALIAS epycbench_runner_control_monitor)
target_compile_features(epycbench_runner_control_monitor PUBLIC cxx_std_23)
target_include_directories(
    epycbench_runner_control_monitor
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(
    epycbench_runner_control_monitor PUBLIC EpycBench::runtime_ipc_payloads
)
epycbench_enable_build_options(epycbench_runner_control_monitor)
epycbench_enable_warnings(epycbench_runner_control_monitor)

add_library(epycbench_runner_state_machine STATIC
    src/runtime/runner_admission.cpp
    src/runtime/runner_state_machine.cpp
)
add_library(EpycBench::runner_state_machine ALIAS epycbench_runner_state_machine)
target_compile_features(epycbench_runner_state_machine PUBLIC cxx_std_23)
target_include_directories(
    epycbench_runner_state_machine
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(
    epycbench_runner_state_machine
    PUBLIC
        EpycBench::runner_context
        EpycBench::runner_control_monitor
        EpycBench::runtime
)
epycbench_enable_build_options(epycbench_runner_state_machine)
epycbench_enable_warnings(epycbench_runner_state_machine)

add_library(epycbench_persistent_worker_team STATIC
    src/runtime/persistent_worker_team.cpp
)
add_library(EpycBench::persistent_worker_team ALIAS epycbench_persistent_worker_team)
target_compile_features(epycbench_persistent_worker_team PUBLIC cxx_std_23)
target_include_directories(
    epycbench_persistent_worker_team
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(
    epycbench_persistent_worker_team
    PUBLIC EpycBench::runner_state_machine
    PRIVATE EpycBench::platform_linux Threads::Threads
)
epycbench_enable_build_options(epycbench_persistent_worker_team)
epycbench_enable_warnings(epycbench_persistent_worker_team)

add_library(epycbench_service_control_monitor STATIC
    src/runtime/service_placement.cpp
    src/runtime/production_control_monitor.cpp
)
add_library(EpycBench::service_control_monitor ALIAS epycbench_service_control_monitor)
target_compile_features(epycbench_service_control_monitor PUBLIC cxx_std_23)
target_include_directories(
    epycbench_service_control_monitor
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(
    epycbench_service_control_monitor
    PUBLIC EpycBench::runner_state_machine
    PRIVATE EpycBench::platform_linux Threads::Threads
)
epycbench_enable_build_options(epycbench_service_control_monitor)
epycbench_enable_warnings(epycbench_service_control_monitor)

add_library(epycbench_pairwise_finalized_error_profile STATIC
    src/runtime/finalized_evidence_error.cpp
    src/runtime/pairwise_finalized_error_profile.cpp
)
add_library(EpycBench::pairwise_finalized_error_profile ALIAS
    epycbench_pairwise_finalized_error_profile
)
target_compile_features(epycbench_pairwise_finalized_error_profile PUBLIC cxx_std_23)
target_include_directories(epycbench_pairwise_finalized_error_profile
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
)
target_link_libraries(epycbench_pairwise_finalized_error_profile PUBLIC EpycBench::core)
epycbench_enable_build_options(epycbench_pairwise_finalized_error_profile)
epycbench_enable_warnings(epycbench_pairwise_finalized_error_profile)

# Inactive logical-wire capacity values. No selected protocol, process,
# controller, runner, backend, or public command consumes this target.
add_library(epycbench_finalized_evidence_wire_capacity STATIC
    src/runtime/finalized_evidence_wire_capacity.cpp)
add_library(EpycBench::finalized_evidence_wire_capacity ALIAS
    epycbench_finalized_evidence_wire_capacity)
target_compile_features(epycbench_finalized_evidence_wire_capacity PUBLIC cxx_std_23)
target_include_directories(epycbench_finalized_evidence_wire_capacity
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
epycbench_enable_build_options(epycbench_finalized_evidence_wire_capacity)
epycbench_enable_warnings(epycbench_finalized_evidence_wire_capacity)

# Inactive bounded Core-canonical logical-body codec primitive. This target is
# deliberately disconnected from every protocol and production route.
add_library(epycbench_finalized_evidence_wire_logical_codec STATIC
    src/runtime/finalized_evidence_wire_logical_codec.cpp
    src/runtime/finalized_evidence_wire_json.cpp)
add_library(EpycBench::finalized_evidence_wire_logical_codec ALIAS
    epycbench_finalized_evidence_wire_logical_codec)
target_compile_features(epycbench_finalized_evidence_wire_logical_codec PUBLIC cxx_std_23)
target_include_directories(epycbench_finalized_evidence_wire_logical_codec
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_finalized_evidence_wire_logical_codec
    PRIVATE EpycBench::core EpycBench::finalized_evidence_wire_capacity)
epycbench_enable_build_options(epycbench_finalized_evidence_wire_logical_codec)
epycbench_enable_warnings(epycbench_finalized_evidence_wire_logical_codec)

# Pure private preparation-fact projection; no platform operation or live caller.
add_library(epycbench_pairwise_preparation_graph STATIC
    src/runtime/pairwise_preparation_graph.cpp)
target_include_directories(epycbench_pairwise_preparation_graph PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_pairwise_preparation_graph PRIVATE EpycBench::core EpycBench::scenario EpycBench::measurement EpycBench::monitoring)
epycbench_enable_build_options(epycbench_pairwise_preparation_graph)
epycbench_enable_warnings(epycbench_pairwise_preparation_graph)

add_library(epycbench_pairwise_monitoring_preparation_facts STATIC
    src/runtime/pairwise_monitoring_preparation_facts.cpp)
target_include_directories(epycbench_pairwise_monitoring_preparation_facts PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_pairwise_monitoring_preparation_facts PRIVATE EpycBench::core)
epycbench_enable_build_options(epycbench_pairwise_monitoring_preparation_facts)
epycbench_enable_warnings(epycbench_pairwise_monitoring_preparation_facts)

add_library(epycbench_runtime_production_backend STATIC
    src/runtime/pairwise_production_authority.cpp
    src/runtime/calibration_only_production.cpp
    src/runtime/pairwise_production_composition.cpp
    src/runtime/production_admission.cpp
    src/runtime/statistics_production_policy.cpp
    src/runtime/pairwise_run_evidence.cpp
    src/runtime/pairwise_runner_execution_backend.cpp
    src/runtime/pairwise_production_run_core.cpp
    src/runtime/pairwise_production_binding.cpp
    src/runtime/pairwise_typed_adapter_storage.cpp
    src/runtime/pairwise_typed_local_monitoring_capacity_producer.cpp
    src/runtime/pairwise_typed_monitoring_capacity_binding.cpp
    src/runtime/pairwise_prepared_typed_measurement_execution.cpp
    src/runtime/pairwise_typed_production_binding_preparation.cpp
    src/runtime/pairwise_typed_production_owner_transaction.cpp
    src/runtime/frozen_observation_run.cpp
    src/runtime/pairwise_production_operations_adapter.cpp
    src/runtime/pairwise_production_operations_support.cpp
    src/runtime/pairwise_platform_leaf_facade.cpp
    src/runtime/pairwise_kernel_leaf_facade.cpp
    src/runtime/pairwise_measurement_leaf_facade.cpp
    src/runtime/pairwise_statistics_leaf_facade.cpp
    src/runtime/pairwise_memory_owning_facade.cpp
    src/runtime/pairwise_worker_owning_facade.cpp
    src/runtime/pairwise_monitoring_host_capacity_admission.cpp
    src/runtime/pairwise_monitoring_capacity.cpp
    src/runtime/pairwise_monitoring_owning_facade.cpp
    src/runtime/linux_pairwise_production_host_operations_provider.cpp
    src/runtime/monitoring_q1_targeted_generation_bridge.cpp
)
add_library(EpycBench::runtime_production_backend ALIAS epycbench_runtime_production_backend)
target_compile_features(epycbench_runtime_production_backend PUBLIC cxx_std_23)
target_include_directories(
    epycbench_runtime_production_backend
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(
    epycbench_runtime_production_backend
    PUBLIC EpycBench::service_control_monitor EpycBench::monitoring EpycBench::statistics
           EpycBench::measurement EpycBench::persistent_worker_team
    PRIVATE epycbench_pairwise_preparation_graph epycbench_pairwise_monitoring_preparation_facts EpycBench::memory EpycBench::kernel_initialization
            EpycBench::runtime_placement_request_projection
            EpycBench::pairwise_finalized_error_profile
            EpycBench::monitoring_host_capacity_authority
            EpycBench::kernel_registry
)
epycbench_enable_build_options(epycbench_runtime_production_backend)
epycbench_enable_warnings(epycbench_runtime_production_backend)

# Inactive typed wire reconstruction slices. This target is intentionally not
# linked by a controller, runner, backend route, protocol, or public command.
add_library(epycbench_finalized_evidence_wire_typed_reconstruction STATIC
    src/runtime/finalized_evidence_wire_component.cpp
    src/runtime/finalized_evidence_wire_typed_stock.cpp
    src/runtime/finalized_evidence_wire_typed_reconstruction.cpp
    src/runtime/finalized_evidence_wire_memory_projection.cpp
    src/runtime/finalized_evidence_wire_monitoring_projection.cpp)
add_library(EpycBench::finalized_evidence_wire_typed_reconstruction ALIAS
    epycbench_finalized_evidence_wire_typed_reconstruction)
target_compile_features(epycbench_finalized_evidence_wire_typed_reconstruction PUBLIC cxx_std_23)
target_include_directories(epycbench_finalized_evidence_wire_typed_reconstruction
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_finalized_evidence_wire_typed_reconstruction
    PRIVATE EpycBench::runtime_production_backend
            EpycBench::finalized_evidence_wire_logical_codec)
epycbench_enable_build_options(epycbench_finalized_evidence_wire_typed_reconstruction)
epycbench_enable_warnings(epycbench_finalized_evidence_wire_typed_reconstruction)

# Inactive, source-private pairwise-v2 startup authority carriage.  This target
# intentionally has no selected controller/runner consumer until the atomic
# pairwise-v2 successor session is implemented and independently accepted.
add_library(epycbench_pairwise_monitoring_host_capacity_startup STATIC
    src/runtime/pairwise_monitoring_host_capacity_startup.cpp
    src/runtime/pairwise_v2_contextual_admission.cpp)
add_library(EpycBench::pairwise_monitoring_host_capacity_startup ALIAS
    epycbench_pairwise_monitoring_host_capacity_startup)
target_compile_features(epycbench_pairwise_monitoring_host_capacity_startup PUBLIC cxx_std_23)
target_include_directories(epycbench_pairwise_monitoring_host_capacity_startup
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(epycbench_pairwise_monitoring_host_capacity_startup
    PUBLIC EpycBench::runtime_production_backend)
epycbench_enable_build_options(epycbench_pairwise_monitoring_host_capacity_startup)
epycbench_enable_warnings(epycbench_pairwise_monitoring_host_capacity_startup)

# Inactive source-authenticated finalized-evidence capacity projection. This
# binds values only and is deliberately absent from Phase-A and selected IPC.
add_library(epycbench_finalized_evidence_wire_source_projection STATIC
    src/runtime/finalized_evidence_wire_source_projection.cpp)
add_library(EpycBench::finalized_evidence_wire_source_projection ALIAS
    epycbench_finalized_evidence_wire_source_projection)
target_compile_features(epycbench_finalized_evidence_wire_source_projection PUBLIC cxx_std_23)
target_include_directories(epycbench_finalized_evidence_wire_source_projection
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(epycbench_finalized_evidence_wire_source_projection
    PUBLIC EpycBench::pairwise_monitoring_host_capacity_startup
           EpycBench::finalized_evidence_wire_capacity)
epycbench_enable_build_options(epycbench_finalized_evidence_wire_source_projection)
epycbench_enable_warnings(epycbench_finalized_evidence_wire_source_projection)

# Inactive finalized-evidence capacity startup value and canonical offer/ack
# binding. It is deliberately absent from every selected IPC/session route.
add_library(epycbench_finalized_evidence_wire_startup_codec STATIC
    src/runtime/finalized_evidence_wire_startup_codec.cpp)
add_library(EpycBench::finalized_evidence_wire_startup_codec ALIAS
    epycbench_finalized_evidence_wire_startup_codec)
target_compile_features(epycbench_finalized_evidence_wire_startup_codec PUBLIC cxx_std_23)
target_include_directories(epycbench_finalized_evidence_wire_startup_codec
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(epycbench_finalized_evidence_wire_startup_codec
    PUBLIC EpycBench::finalized_evidence_wire_source_projection)
epycbench_enable_build_options(epycbench_finalized_evidence_wire_startup_codec)
epycbench_enable_warnings(epycbench_finalized_evidence_wire_startup_codec)

# Inactive prepared-resource value and canonical prepared offer/ack. It is not
# linked by any selected IPC/session route.
add_library(epycbench_finalized_evidence_prepared_resources STATIC
    src/runtime/finalized_evidence_prepared_resources.cpp)
add_library(EpycBench::finalized_evidence_prepared_resources ALIAS
    epycbench_finalized_evidence_prepared_resources)
target_compile_features(epycbench_finalized_evidence_prepared_resources PUBLIC cxx_std_23)
target_include_directories(epycbench_finalized_evidence_prepared_resources
    PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(epycbench_finalized_evidence_prepared_resources
    PUBLIC EpycBench::finalized_evidence_wire_startup_codec)
epycbench_enable_build_options(epycbench_finalized_evidence_prepared_resources)
epycbench_enable_warnings(epycbench_finalized_evidence_prepared_resources)

# Inactive allocation-bounded canonical cursor used by the prepared typed
# evidence codec. It is deliberately absent from selected IPC/session routes.
add_library(epycbench_finalized_evidence_prepared_direct_codec STATIC
    src/runtime/finalized_evidence_prepared_direct_codec.cpp)
add_library(EpycBench::finalized_evidence_prepared_direct_codec ALIAS
    epycbench_finalized_evidence_prepared_direct_codec)
target_compile_features(epycbench_finalized_evidence_prepared_direct_codec PUBLIC cxx_std_23)
target_include_directories(epycbench_finalized_evidence_prepared_direct_codec
    PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(epycbench_finalized_evidence_prepared_direct_codec
    PUBLIC EpycBench::finalized_evidence_prepared_resources)
epycbench_enable_build_options(epycbench_finalized_evidence_prepared_direct_codec)
epycbench_enable_warnings(epycbench_finalized_evidence_prepared_direct_codec)

add_library(epycbench_finalized_evidence_transport STATIC
    src/runtime/finalized_evidence_transport.cpp
    src/runtime/finalized_evidence_prepared_transport.cpp)
add_library(EpycBench::finalized_evidence_transport ALIAS
    epycbench_finalized_evidence_transport)
target_compile_features(epycbench_finalized_evidence_transport PUBLIC cxx_std_23)
target_include_directories(epycbench_finalized_evidence_transport
    PUBLIC ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(epycbench_finalized_evidence_transport
    PUBLIC EpycBench::core EpycBench::runtime_ipc)
epycbench_enable_build_options(epycbench_finalized_evidence_transport)
epycbench_enable_warnings(epycbench_finalized_evidence_transport)

# Inactive, source-private pairwise-v2 Phase-A protocol values.  No selected
# controller, runner, process, backend, or public IPC target consumes it.
add_library(epycbench_pairwise_v2_progress STATIC
    src/runtime/pairwise_v2_progress.cpp)
add_library(EpycBench::pairwise_v2_progress ALIAS epycbench_pairwise_v2_progress)
target_link_libraries(epycbench_runtime_production_backend PRIVATE EpycBench::pairwise_v2_progress)
target_link_libraries(epycbench_runtime_production_backend PRIVATE epycbench_stage_b_resource_policy)
# The existing backend/progress/prepared-codec/resources/startup/source-projection
# static component has late consumers of earlier archives (build190). A third
# component scan closes those references without changing its reachable owners.
set_property(TARGET epycbench_runtime_production_backend PROPERTY LINK_INTERFACE_MULTIPLICITY 3)
target_include_directories(epycbench_pairwise_v2_progress PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(epycbench_pairwise_v2_progress PUBLIC EpycBench::core
    EpycBench::finalized_evidence_prepared_direct_codec)
epycbench_enable_build_options(epycbench_pairwise_v2_progress)
epycbench_enable_warnings(epycbench_pairwise_v2_progress)

add_library(epycbench_pairwise_v2_phase_a_protocol STATIC
    src/runtime/pairwise_v2_phase_a_protocol.cpp)
add_library(EpycBench::pairwise_v2_phase_a_protocol ALIAS
    epycbench_pairwise_v2_phase_a_protocol)
target_compile_features(epycbench_pairwise_v2_phase_a_protocol PUBLIC cxx_std_23)
target_include_directories(epycbench_pairwise_v2_phase_a_protocol
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(epycbench_pairwise_v2_phase_a_protocol PRIVATE epycbench_stage_b_resource_policy)
target_link_libraries(epycbench_pairwise_v2_phase_a_protocol
    PUBLIC EpycBench::pairwise_monitoring_host_capacity_startup)
epycbench_enable_build_options(epycbench_pairwise_v2_phase_a_protocol)
epycbench_enable_warnings(epycbench_pairwise_v2_phase_a_protocol)

# Inactive runner-only composition of the accepted Phase-A startup, contextual
# admission, and event-state prerequisites.  It has no selected route.
add_library(epycbench_pairwise_v2_runner_session STATIC
    src/runtime/pairwise_v2_runner_session.cpp)
add_library(EpycBench::pairwise_v2_runner_session ALIAS
    epycbench_pairwise_v2_runner_session)
target_compile_features(epycbench_pairwise_v2_runner_session PUBLIC cxx_std_23)
target_include_directories(epycbench_pairwise_v2_runner_session
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(epycbench_pairwise_v2_runner_session
    PUBLIC EpycBench::pairwise_v2_phase_a_protocol)
epycbench_enable_build_options(epycbench_pairwise_v2_runner_session)
epycbench_enable_warnings(epycbench_pairwise_v2_runner_session)

# Inactive controller-side Phase-A deadline/process proof.  No selected route,
# backend, evidence, or public protocol consumes this target.
add_library(epycbench_pairwise_v2_controller_session STATIC
    src/runtime/pairwise_v2_controller_session.cpp)
add_library(EpycBench::pairwise_v2_controller_session ALIAS
    epycbench_pairwise_v2_controller_session)
target_compile_features(epycbench_pairwise_v2_controller_session PUBLIC cxx_std_23)
target_include_directories(epycbench_pairwise_v2_controller_session
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(epycbench_pairwise_v2_controller_session
    PUBLIC EpycBench::pairwise_v2_runner_session EpycBench::runtime)
epycbench_enable_build_options(epycbench_pairwise_v2_controller_session)
epycbench_enable_warnings(epycbench_pairwise_v2_controller_session)

# Inactive bridge from the accepted Phase-A event states to the prepared
# manifest/chunk/completion stream. No selected route consumes this target.
add_library(epycbench_pairwise_v2_prepared_evidence_session STATIC
    src/runtime/pairwise_v2_prepared_evidence_session.cpp)
add_library(EpycBench::pairwise_v2_prepared_evidence_session ALIAS
    epycbench_pairwise_v2_prepared_evidence_session)
target_compile_features(epycbench_pairwise_v2_prepared_evidence_session PUBLIC cxx_std_23)
target_include_directories(epycbench_pairwise_v2_prepared_evidence_session
    PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(epycbench_pairwise_v2_prepared_evidence_session
    PUBLIC EpycBench::pairwise_v2_phase_a_protocol
           EpycBench::finalized_evidence_transport
           EpycBench::pairwise_v2_prepared_resource_session)
epycbench_enable_build_options(epycbench_pairwise_v2_prepared_evidence_session)
epycbench_enable_warnings(epycbench_pairwise_v2_prepared_evidence_session)

add_library(epycbench_monitoring_qualification STATIC
    src/monitoring/current_revision_tracked_capture.cpp
    src/monitoring/governed_tracked_bundle.cpp
    src/monitoring/host_environment.cpp
    src/monitoring/host_input_template.cpp
    src/monitoring/host_runner_context_session.cpp
    src/monitoring/host_qualification_plan.cpp
    src/monitoring/host_q1_plan_v2_value.cpp
    src/monitoring/host_q2_plan_value.cpp
    src/monitoring/host_q3_plan_value.cpp
    src/monitoring/host_q4_plan_value.cpp
    src/monitoring/host_q4_schema_bundle.cpp
    src/monitoring/host_q4_governed_input_authority.cpp
    src/monitoring/host_q4_governed_plan_schema_binding.cpp
    src/monitoring/host_q4_composition_owner.cpp
    src/monitoring/host_q4_authority_session.cpp
    src/monitoring/host_q4_kernel_maximum_authority.cpp
    src/monitoring/host_q4_monitoring_capacity_adapter.cpp
    src/monitoring/host_q4_source_ready.cpp
    src/monitoring/host_q4_cpufreq_inventory_authority.cpp
    src/monitoring/host_q4_cpufreq_prepared_source_authority.cpp
    src/monitoring/host_q4_cpufreq_cycle_authority.cpp
    src/monitoring/host_q4_cpufreq_collector.cpp
    src/monitoring/host_q4_cpufreq_evidence_value.cpp
    src/monitoring/host_q4_cpufreq_evidence_json.cpp
    src/monitoring/host_q4_backend_transport.cpp
    src/monitoring/host_q4_backend.cpp
    src/monitoring/host_q4_cpufreq_semantic_agreement.cpp
    src/monitoring/host_q4_authority_failure_projection.cpp
    src/monitoring/host_q4_historical_git_authority.cpp
    src/monitoring/host_q4_complete_q3_predecessor_authenticator.cpp
    src/monitoring/host_q4_complete_q3_predecessor_authority.cpp
    src/monitoring/host_q4_structural_capacity.cpp
    src/monitoring/host_q4_collection_capacity_inputs.cpp
    src/monitoring/host_q4_interval_summary_capacity.cpp
    src/monitoring/host_q4_collection_capacity.cpp
    src/monitoring/governed_plan_input_core.cpp
    src/monitoring/host_q2_plan_public_input_authority.cpp
    src/monitoring/host_q3_plan_public_input_authority.cpp
    src/monitoring/host_q3_historical_git_authority.cpp
    src/monitoring/host_q3_complete_q2_predecessor_authenticator.cpp
    src/monitoring/host_q3_complete_q2_predecessor_authority.cpp
    src/monitoring/host_q3_composition_owner.cpp
    src/monitoring/host_q3_authority_session.cpp
    src/monitoring/host_q3_temperature_parser_agreement.cpp
    src/monitoring/host_q3_temperature_cycle_authority.cpp
    src/monitoring/host_q3_temperature_collector.cpp
    src/monitoring/host_q3_temperature_production_adapter.cpp
    src/monitoring/host_q3_temperature_evidence_value.cpp
    src/monitoring/host_q3_temperature_evidence_json.cpp
    src/monitoring/host_q3_backend.cpp
    src/monitoring/host_q1_git_command_owner.cpp
    src/monitoring/host_q1_git_bounded_child.cpp
    src/monitoring/host_q1_git_spawn.cpp
    src/monitoring/host_q1_git_repository_metadata.cpp
    src/monitoring/host_q1_git_fixed_results.cpp
    src/monitoring/host_q1_public_input_authority.cpp
    src/monitoring/host_q1_validated_public_values.cpp
    src/monitoring/host_q1_private_q0_value.cpp
    src/monitoring/host_q1_plan_v2_authority_session.cpp
    src/monitoring/host_q1_page_fault_collector.cpp
    src/monitoring/host_q1_backend.cpp
    src/monitoring/host_q2_complete_q1_predecessor_authority.cpp
    src/monitoring/host_q2_historical_q1_git_authority.cpp
    src/monitoring/host_q2_authority_session.cpp
    src/monitoring/host_q2_controlled_event_session.cpp
    src/monitoring/host_q2_context_switch_source.cpp
    src/monitoring/host_q2_context_switch_collector.cpp
    src/monitoring/host_q2_context_switch_evidence.cpp
    src/monitoring/host_q2_backend.cpp
    src/monitoring/host_q2_diagnostic_transport.cpp
    src/monitoring/host_q2_terminal_containment.cpp
    src/monitoring/host_q2_controlled_event_runtime_bridge.cpp
    src/monitoring/host_q2_helper_provider.cpp
    src/monitoring/host_q2_native_thread_owner.cpp
    src/monitoring/host_q2_native_worker_team.cpp
    src/monitoring/host_running_build_authority.cpp
    src/monitoring/host_running_build_attestation_value.cpp
    src/monitoring/host_q2_q1_authority.cpp
    src/monitoring/host_q2_composition_owner.cpp
    src/monitoring/host_qualification_composition.cpp
    src/monitoring/privacy_validation.cpp
    src/monitoring/production_admission_projection.cpp
    src/monitoring/q0b_base_environment_projection.cpp
    src/monitoring/host_q0b_v2_projection.cpp
    src/monitoring/host_q0b_c0_coordinator.cpp
    src/monitoring/host_q0b_backend_driver.cpp
    src/monitoring/host_baseline_preparation.cpp
    src/monitoring/host_baseline_work_admission.cpp
    src/monitoring/host_q0b_governed_work.cpp
    src/monitoring/host_q0b_governed_parent.cpp
    src/monitoring/host_frozen_observation_admission.cpp
    src/monitoring/host_frozen_collection.cpp
    src/monitoring/host_frozen_source_admission.cpp
    src/monitoring/host_frozen_source_launch.cpp
    src/monitoring/host_frozen_source_composition.cpp
    src/monitoring/host_frozen_process_control.cpp
    src/monitoring/host_production_authority.cpp
    src/monitoring/host_frozen_evidence.cpp
    src/monitoring/q0b_backend_result.cpp
    src/monitoring/q0b_backend_driver.cpp
    src/monitoring/q0b_c0_driver.cpp
    src/monitoring/q0b_family_characterization.cpp
    src/monitoring/q0b_production_inputs.cpp
)
add_library(EpycBench::monitoring_qualification ALIAS epycbench_monitoring_qualification)
target_compile_features(epycbench_monitoring_qualification PUBLIC cxx_std_23)
target_include_directories(epycbench_monitoring_qualification
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_compile_definitions(epycbench_monitoring_qualification
    PRIVATE EPYCBENCH_GIT_EXECUTABLE="${GIT_EXECUTABLE}"
            EPYCBENCH_SOURCE_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
            EPYCBENCH_SOURCE_TEST_PROCESS_SCRIPT="${PROJECT_SOURCE_DIR}/tools/monitoring_host_frozen_process.py")
target_link_libraries(epycbench_monitoring_qualification
    PUBLIC EpycBench::runtime_production_backend EpycBench::topology EpycBench::build_manifest
           EpycBench::kernel_registry
    PRIVATE EpycBench::platform_linux EpycBench::monitoring_host_capacity_authority
            EpycBench::finalized_evidence_wire_typed_reconstruction
)
epycbench_enable_build_options(epycbench_monitoring_qualification)
epycbench_enable_warnings(epycbench_monitoring_qualification)

# Keep Runtime placement provenance transport independent of Monitoring.  The
# calibration backend may use the established Monitoring execution policy, but
# it must not acquire qualification-layer authority.
foreach(runtime_foundation_target IN ITEMS
        epycbench_runtime_ipc_payloads
        epycbench_runner_context
        epycbench_runner_control_monitor
        epycbench_runner_state_machine)
    get_target_property(runtime_foundation_links ${runtime_foundation_target} LINK_LIBRARIES)
    if("${runtime_foundation_links}" MATCHES "monitoring")
        message(FATAL_ERROR
            "${runtime_foundation_target} must not depend on Monitoring")
    endif()
endforeach()
get_target_property(runtime_backend_links epycbench_runtime_production_backend LINK_LIBRARIES)
if("${runtime_backend_links}" MATCHES "monitoring_qualification")
    message(FATAL_ERROR
        "Runtime production backend must not depend on Monitoring qualification")
endif()
foreach(runtime_provenance_source IN ITEMS
        include/epycbench/runtime/placement_provenance.hpp
        include/epycbench/runtime/ipc/payloads.hpp
        include/epycbench/runtime/ipc/protocol.hpp
        include/epycbench/runtime/admitted_placement_request.hpp
        include/epycbench/runtime/controller_lifecycle.hpp
        include/epycbench/runtime/placement_request_projection.hpp
        include/epycbench/runtime/runner_state_machine.hpp
        src/runtime/placement_provenance.cpp
        src/runtime/placement_provenance_access.hpp
        src/runtime/ipc/payloads.cpp
        src/runtime/ipc/placement_request_construction_access.cpp
        src/runtime/ipc/placement_request_construction_access.hpp
        src/runtime/ipc/protocol.cpp
        src/runtime/runner_admission.cpp
        src/runtime/runner_state_machine.cpp
        src/runtime/controller_lifecycle.cpp
        src/runtime/placement_request_projection.cpp)
    file(READ "${PROJECT_SOURCE_DIR}/${runtime_provenance_source}" runtime_provenance_contents)
    if("${runtime_provenance_contents}" MATCHES
       "epycbench/monitoring|monitoring::|monitoring_qualification")
        message(FATAL_ERROR
            "${runtime_provenance_source} must not include Monitoring")
    endif()
endforeach()

# Higher calibration may use only the established Monitoring acquisition policy.
# Qualification inputs, host capture/session types, schemas, and target authority
# remain outside Runtime even though host-plan provenance is transported here.
set(runtime_calibration_forbidden_host_authority
    "MonitoringHostQualificationPlan|HostEnvironmentAuthority|HostEnvironmentEligibleNode|HostEnvironmentWorker|HostEnvironmentServiceCore|RunnerContextAcquisitionSession|MonitoringHostInputTemplate|MonitoringHostInputTemplateAuthority|MonitoringHostPrerequisiteCapabilities|HostRevalidationDisposition|monitoring::qualification|monitoring::host|monitoring_qualification")
foreach(runtime_calibration_source IN ITEMS
        include/epycbench/runtime/calibration_only_production.hpp
        src/runtime/calibration_only_production.cpp
        src/runtime/calibration_only_production_admission_access.hpp
        src/runtime/pairwise_runner_execution_backend.cpp)
    file(READ "${PROJECT_SOURCE_DIR}/${runtime_calibration_source}"
         runtime_calibration_contents)
    if("${runtime_calibration_contents}" MATCHES
       "${runtime_calibration_forbidden_host_authority}|epycbench/monitoring/(host_environment|host_input_template|host_runner_context|q0b)|epycbench\\.monitoring-|monitoring-host/v[0-9]")
        message(FATAL_ERROR
            "${runtime_calibration_source} exceeds the Monitoring calibration allowlist")
    endif()
    string(REGEX REPLACE
        "#include[ \t]+[<\"]epycbench/monitoring/acquisition_driver\\.hpp[>\"]"
        "" runtime_calibration_allowlisted "${runtime_calibration_contents}")
    string(REPLACE "monitoring::MonitoringExecutionPolicy" ""
        runtime_calibration_allowlisted "${runtime_calibration_allowlisted}")
    if("${runtime_calibration_allowlisted}" MATCHES
       "epycbench/monitoring|monitoring::")
        message(FATAL_ERROR
            "${runtime_calibration_source} uses non-allowlisted Monitoring API")
    endif()
endforeach()

# Pure memory-only Results journal prerequisite, independent of Runtime/CLI.
add_library(epycbench_results_journal_records STATIC src/results/journal_records.cpp)
add_library(EpycBench::results_journal_records ALIAS epycbench_results_journal_records)
target_compile_features(epycbench_results_journal_records PUBLIC cxx_std_23)
target_include_directories(epycbench_results_journal_records
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_link_libraries(epycbench_results_journal_records PUBLIC EpycBench::core)
epycbench_enable_build_options(epycbench_results_journal_records)
epycbench_enable_warnings(epycbench_results_journal_records)

add_library(epycbench_results STATIC src/results/pairwise_run_result.cpp
    src/results/prepared_pairwise_run_result.cpp)
add_library(EpycBench::results ALIAS epycbench_results)
target_compile_features(epycbench_results PUBLIC cxx_std_23)
target_include_directories(
    epycbench_results PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(
    epycbench_results
    PUBLIC EpycBench::runtime_production_backend EpycBench::statistics EpycBench::measurement
    PRIVATE EpycBench::finalized_evidence_prepared_direct_codec
)
epycbench_enable_build_options(epycbench_results)
epycbench_enable_warnings(epycbench_results)

# Source-private analysis representation; no journal or public route consumer.
add_library(epycbench_results_pairwise_analysis STATIC src/results/pairwise_analysis_serialization.cpp)
target_include_directories(epycbench_results_pairwise_analysis PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_pairwise_analysis PRIVATE EpycBench::results EpycBench::statistics EpycBench::core)
epycbench_enable_build_options(epycbench_results_pairwise_analysis)
epycbench_enable_warnings(epycbench_results_pairwise_analysis)

# Private supplied build metadata; never calls the generated manifest producer.
add_library(epycbench_results_validation_event STATIC src/results/validation_event_owner.cpp)
target_include_directories(epycbench_results_validation_event PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_validation_event PRIVATE EpycBench::core EpycBench::monitoring)
epycbench_enable_build_options(epycbench_results_validation_event)
epycbench_enable_warnings(epycbench_results_validation_event)

add_library(epycbench_results_kernel_report STATIC src/results/kernel_report_owner.cpp)
target_include_directories(epycbench_results_kernel_report PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_kernel_report PRIVATE EpycBench::core)
epycbench_enable_build_options(epycbench_results_kernel_report)
epycbench_enable_warnings(epycbench_results_kernel_report)

# Private supplied historical approval/report association; no acquisition or route.
add_library(epycbench_results_kernel_approval STATIC src/results/kernel_approval_owner.cpp)
target_include_directories(epycbench_results_kernel_approval PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_kernel_approval PRIVATE epycbench_results_kernel_report EpycBench::core)
epycbench_enable_build_options(epycbench_results_kernel_approval)
epycbench_enable_warnings(epycbench_results_kernel_approval)

# Private supplied Linux UTS identity; never an export or collection route.
add_library(epycbench_results_private_kernel_identity STATIC src/results/private_kernel_identity.cpp)
target_include_directories(epycbench_results_private_kernel_identity PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_kernel_identity PRIVATE epycbench_results_topology_evidence EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_kernel_identity)
epycbench_enable_warnings(epycbench_results_private_kernel_identity)

add_library(epycbench_results_anonymous_thp_policy_owner STATIC src/results/anonymous_thp_policy_owner.cpp)
target_include_directories(epycbench_results_anonymous_thp_policy_owner PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_anonymous_thp_policy_owner PRIVATE epycbench_results_topology_evidence EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_anonymous_thp_policy_owner)
epycbench_enable_warnings(epycbench_results_anonymous_thp_policy_owner)

add_library(epycbench_results_process_thp_restriction STATIC src/results/process_thp_restriction.cpp)
target_include_directories(epycbench_results_process_thp_restriction PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_process_thp_restriction PRIVATE epycbench_results_topology_evidence EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_process_thp_restriction)
epycbench_enable_warnings(epycbench_results_process_thp_restriction)

add_library(epycbench_results_pu_cpu_identity STATIC src/results/pu_cpu_identity.cpp)
target_include_directories(epycbench_results_pu_cpu_identity PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_pu_cpu_identity PRIVATE EpycBench::topology epycbench_results_topology_evidence EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_pu_cpu_identity)
epycbench_enable_warnings(epycbench_results_pu_cpu_identity)

add_library(epycbench_results_frequency_policy_owner STATIC src/results/frequency_policy_owner.cpp)
target_include_directories(epycbench_results_frequency_policy_owner PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_frequency_policy_owner PRIVATE EpycBench::monitoring EpycBench::topology epycbench_results_topology_evidence EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_frequency_policy_owner)
epycbench_enable_warnings(epycbench_results_frequency_policy_owner)

# Closed supplied system-control metadata; no acquisition or preflight evaluation.
add_library(epycbench_results_host_controls STATIC src/results/host_system_controls.cpp)
target_include_directories(epycbench_results_host_controls PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_host_controls PRIVATE epycbench_results_topology_evidence EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_host_controls)
epycbench_enable_warnings(epycbench_results_host_controls)

# Private supplied topology admission; no discovery or public route consumer.
add_library(epycbench_results_topology_evidence STATIC src/results/topology_evidence_owner.cpp)
target_include_directories(epycbench_results_topology_evidence PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_topology_evidence PRIVATE EpycBench::topology EpycBench::core)
epycbench_enable_build_options(epycbench_results_topology_evidence)
epycbench_enable_warnings(epycbench_results_topology_evidence)

add_library(epycbench_results_build_manifest STATIC src/results/build_manifest_owner.cpp)
target_include_directories(epycbench_results_build_manifest PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_build_manifest PRIVATE EpycBench::core)
epycbench_enable_build_options(epycbench_results_build_manifest)
epycbench_enable_warnings(epycbench_results_build_manifest)

# Pure supplied-memory authored source retention and Scenario association.
add_library(epycbench_results_authored_source STATIC src/results/authored_source.cpp)
target_include_directories(epycbench_results_authored_source PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_authored_source PRIVATE EpycBench::scenario EpycBench::core epycbench_results_private_error)
epycbench_enable_build_options(epycbench_results_authored_source)
epycbench_enable_warnings(epycbench_results_authored_source)

# Supplied-memory block component context; no selected Runtime route.
add_library(epycbench_results_incremental_block_evidence STATIC
    src/results/expected_pair_evidence_context.cpp
    src/results/incremental_block_evidence.cpp
    src/results/observation_timing_admission.cpp)
target_compile_features(epycbench_results_incremental_block_evidence PUBLIC cxx_std_23)
target_include_directories(epycbench_results_incremental_block_evidence PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_incremental_block_evidence PRIVATE EpycBench::results
    EpycBench::results_journal_records EpycBench::finalized_evidence_wire_typed_reconstruction
    epycbench_results_private_error)
epycbench_enable_build_options(epycbench_results_incremental_block_evidence)
epycbench_enable_warnings(epycbench_results_incremental_block_evidence)

# Source-private supplied pair-prefix composition; no selected Runtime route.
add_library(epycbench_results_incremental_pair_evidence STATIC src/results/incremental_pair_evidence.cpp)
target_compile_features(epycbench_results_incremental_pair_evidence PUBLIC cxx_std_23)
target_include_directories(epycbench_results_incremental_pair_evidence PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_incremental_pair_evidence PRIVATE epycbench_pairwise_preparation_graph epycbench_pairwise_monitoring_preparation_facts epycbench_results_incremental_block_evidence
    EpycBench::results EpycBench::results_journal_records EpycBench::finalized_evidence_wire_typed_reconstruction)
epycbench_enable_build_options(epycbench_results_incremental_pair_evidence)
epycbench_enable_warnings(epycbench_results_incremental_pair_evidence)

# Source-private post-finalization semantic snapshot composition; no selected route.
add_library(epycbench_results_journal_snapshot STATIC src/results/journal_snapshot.cpp)
target_compile_features(epycbench_results_journal_snapshot PUBLIC cxx_std_23)
target_include_directories(epycbench_results_journal_snapshot PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_journal_snapshot PRIVATE epycbench_results_incremental_block_evidence EpycBench::results
    EpycBench::results_journal_records EpycBench::finalized_evidence_wire_typed_reconstruction)
epycbench_enable_build_options(epycbench_results_journal_snapshot)
epycbench_enable_warnings(epycbench_results_journal_snapshot)

# Inactive Phase-A binding of accepted capacity and prepared-resource
# transcripts to one role-local prepared evidence session. It selects no IPC
# route and owns no descriptor, process, backend, host, or public authority.
add_library(epycbench_pairwise_v2_production_entry STATIC src/runtime/pairwise_v2_production_entry.cpp)
target_compile_features(epycbench_pairwise_v2_production_entry PUBLIC cxx_std_23)
target_include_directories(epycbench_pairwise_v2_production_entry PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(epycbench_pairwise_v2_production_entry PUBLIC
    EpycBench::finalized_evidence_wire_startup_codec EpycBench::pairwise_v2_native_lifecycle)
epycbench_enable_build_options(epycbench_pairwise_v2_production_entry)
epycbench_enable_warnings(epycbench_pairwise_v2_production_entry)

add_library(epycbench_stage_b_resource_policy STATIC src/runtime/stage_b_resource_policy.cpp)
target_compile_features(epycbench_stage_b_resource_policy PUBLIC cxx_std_23)
target_include_directories(epycbench_stage_b_resource_policy PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(epycbench_stage_b_resource_policy PUBLIC
    EpycBench::pairwise_v2_progress EpycBench::finalized_evidence_prepared_direct_codec)
epycbench_enable_build_options(epycbench_stage_b_resource_policy)
epycbench_enable_warnings(epycbench_stage_b_resource_policy)

add_library(epycbench_pairwise_v2_prepared_resource_session STATIC
    src/runtime/pairwise_v2_prepared_resource_session.cpp)
add_library(EpycBench::pairwise_v2_prepared_resource_session ALIAS
    epycbench_pairwise_v2_prepared_resource_session)
target_compile_features(epycbench_pairwise_v2_prepared_resource_session PUBLIC cxx_std_23)
target_include_directories(epycbench_pairwise_v2_prepared_resource_session
    PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(epycbench_pairwise_v2_prepared_resource_session
    PUBLIC epycbench_stage_b_resource_policy EpycBench::finalized_evidence_prepared_resources
           EpycBench::finalized_evidence_prepared_direct_codec
           EpycBench::finalized_evidence_transport
           EpycBench::results)
epycbench_enable_build_options(epycbench_pairwise_v2_prepared_resource_session)
epycbench_enable_warnings(epycbench_pairwise_v2_prepared_resource_session)

# Inactive four-control/four-ack composition, reachable only by fake tests.
add_library(epycbench_pairwise_v2_composed_startup STATIC
    src/runtime/pairwise_v2_composed_startup.cpp)
add_library(EpycBench::pairwise_v2_composed_startup ALIAS
    epycbench_pairwise_v2_composed_startup)
target_compile_features(epycbench_pairwise_v2_composed_startup PUBLIC cxx_std_23)
target_include_directories(epycbench_pairwise_v2_composed_startup
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(epycbench_pairwise_v2_composed_startup
    PUBLIC EpycBench::pairwise_v2_phase_a_protocol
           EpycBench::pairwise_v2_prepared_resource_session
           EpycBench::pairwise_v2_resource_startup_protocol
           EpycBench::pairwise_v2_progress)
epycbench_enable_build_options(epycbench_pairwise_v2_composed_startup)
epycbench_enable_warnings(epycbench_pairwise_v2_composed_startup)

# Inactive fake-backend to owning-Results composition. No selected route.
add_library(epycbench_pairwise_v2_active_evidence STATIC
    src/runtime/pairwise_v2_active_evidence.cpp)
add_library(EpycBench::pairwise_v2_active_evidence ALIAS
    epycbench_pairwise_v2_active_evidence)
target_compile_features(epycbench_pairwise_v2_active_evidence PUBLIC cxx_std_23)
target_include_directories(epycbench_pairwise_v2_active_evidence
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(epycbench_pairwise_v2_active_evidence
    PUBLIC EpycBench::pairwise_v2_composed_startup
           EpycBench::pairwise_v2_prepared_evidence_session
           EpycBench::runtime_production_backend)
epycbench_enable_build_options(epycbench_pairwise_v2_active_evidence)
epycbench_enable_warnings(epycbench_pairwise_v2_active_evidence)

# Inactive native component composition, with supplied synthetic backend facts only.
add_library(epycbench_pairwise_v2_native_lifecycle STATIC
    src/runtime/pairwise_v2_native_lifecycle.cpp
    src/runtime/pairwise_v2_production_control_monitor.cpp)
add_library(EpycBench::pairwise_v2_native_lifecycle ALIAS epycbench_pairwise_v2_native_lifecycle)
target_compile_features(epycbench_pairwise_v2_native_lifecycle PUBLIC cxx_std_23)
target_include_directories(epycbench_pairwise_v2_native_lifecycle PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(epycbench_pairwise_v2_native_lifecycle
    PUBLIC EpycBench::pairwise_v2_active_evidence EpycBench::platform_linux
           EpycBench::service_control_monitor)
epycbench_enable_build_options(epycbench_pairwise_v2_native_lifecycle)
epycbench_enable_warnings(epycbench_pairwise_v2_native_lifecycle)

# Inactive fake process composition; no public or real-process consumer.
add_library(epycbench_pairwise_v2_synthetic_lifecycle STATIC
    src/runtime/pairwise_v2_synthetic_lifecycle.cpp)
add_library(EpycBench::pairwise_v2_synthetic_lifecycle ALIAS
    epycbench_pairwise_v2_synthetic_lifecycle)
target_compile_features(epycbench_pairwise_v2_synthetic_lifecycle PUBLIC cxx_std_23)
target_include_directories(epycbench_pairwise_v2_synthetic_lifecycle
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(epycbench_pairwise_v2_synthetic_lifecycle
    PUBLIC EpycBench::pairwise_v2_active_evidence)
epycbench_enable_build_options(epycbench_pairwise_v2_synthetic_lifecycle)
epycbench_enable_warnings(epycbench_pairwise_v2_synthetic_lifecycle)




# Inactive framing/state for the synthetic finalized-evidence resource startup
# sub-prelude. It selects no process or public route.
add_library(epycbench_pairwise_v2_resource_startup_protocol STATIC
    src/runtime/pairwise_v2_resource_startup_protocol.cpp)
add_library(EpycBench::pairwise_v2_resource_startup_protocol ALIAS
    epycbench_pairwise_v2_resource_startup_protocol)
target_compile_features(epycbench_pairwise_v2_resource_startup_protocol PUBLIC cxx_std_23)
target_include_directories(epycbench_pairwise_v2_resource_startup_protocol
    PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(epycbench_pairwise_v2_resource_startup_protocol PRIVATE epycbench_stage_b_resource_policy)
target_link_libraries(epycbench_pairwise_v2_resource_startup_protocol
    PUBLIC EpycBench::finalized_evidence_prepared_resources
           EpycBench::runtime_ipc)
epycbench_enable_build_options(epycbench_pairwise_v2_resource_startup_protocol)
epycbench_enable_warnings(epycbench_pairwise_v2_resource_startup_protocol)

if(CMAKE_CXX_COMPILER_TARGET)
    set(EPYCBENCH_COMPILER_TARGET "${CMAKE_CXX_COMPILER_TARGET}")
else()
    set(EPYCBENCH_COMPILER_TARGET "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(CMAKE_CXX_COMPILER_LINKER_ID)
    set(EPYCBENCH_LINKER_ID "${CMAKE_CXX_COMPILER_LINKER_ID}")
else()
    set(EPYCBENCH_LINKER_ID "unknown")
endif()

if(CMAKE_CXX_COMPILER_LINKER_VERSION)
    set(EPYCBENCH_LINKER_VERSION "${CMAKE_CXX_COMPILER_LINKER_VERSION}")
else()
    set(EPYCBENCH_LINKER_VERSION "unknown")
endif()

string(TOUPPER "${CMAKE_BUILD_TYPE}" EPYCBENCH_BUILD_TYPE_UPPER)
set(
    EPYCBENCH_MANIFEST_COMPILER_FLAGS
    "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${EPYCBENCH_BUILD_TYPE_UPPER}} -std=c++23"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    string(
        APPEND
        EPYCBENCH_MANIFEST_COMPILER_FLAGS
        " -Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wshadow -Wformat=2 -Wundef"
    )
endif()
if(EPYCBENCH_ENABLE_ASAN_UBSAN)
    string(
        APPEND
        EPYCBENCH_MANIFEST_COMPILER_FLAGS
        " -fsanitize=address,undefined -fno-omit-frame-pointer"
    )
endif()
if(EPYCBENCH_ENABLE_TSAN)
    string(
        APPEND
        EPYCBENCH_MANIFEST_COMPILER_FLAGS
        " -fsanitize=thread -fno-omit-frame-pointer"
    )
endif()
if(EPYCBENCH_NATIVE_REFERENCE)
    string(APPEND EPYCBENCH_MANIFEST_COMPILER_FLAGS " -march=native")
endif()
string(STRIP "${EPYCBENCH_MANIFEST_COMPILER_FLAGS}" EPYCBENCH_MANIFEST_COMPILER_FLAGS)

foreach(feature IN ITEMS TESTING ASAN_UBSAN TSAN NATIVE_REFERENCE)
    set(EPYCBENCH_FEATURE_${feature} false)
endforeach()
# Private campaign launch entry; neither installed nor selected by the CLI/tool.
if(BUILD_TESTING)
    add_executable(epycbench_host_work_exec_entry tools/native/host-work-exec-entry.cpp)
    target_include_directories(epycbench_host_work_exec_entry PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_host_work_exec_entry PRIVATE EpycBench::platform_linux)
    epycbench_enable_build_options(epycbench_host_work_exec_entry)
    epycbench_enable_warnings(epycbench_host_work_exec_entry)
endif()

if(BUILD_TESTING)

    add_test(NAME EB-PUB-001.source-export
        COMMAND "${Python3_EXECUTABLE}" -I -B
                "${PROJECT_SOURCE_DIR}/tests/tools/public_source_export_test.py")
    add_test(NAME EB-PUB-001.pending-kernel-evidence
        COMMAND "${Python3_EXECUTABLE}" -I -B
                "${PROJECT_SOURCE_DIR}/tests/tools/public_kernel_evidence_test.py")
    add_test(NAME EB-PUB-001.linked-kernel-retention
        COMMAND "${Python3_EXECUTABLE}" -I -B
                "${PROJECT_SOURCE_DIR}/tests/tools/public_kernel_retention_test.py")
    set_tests_properties(EB-PUB-001.source-export EB-PUB-001.pending-kernel-evidence
        EB-PUB-001.linked-kernel-retention
        PROPERTIES LABELS "publication;synthetic;tooling;non-hardware")

    add_test(
        NAME EB-KRN-AUDIT-001.synthetic-framework
        COMMAND
            "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/tools/kernel_object_audit_test.py"
    )
    add_test(
        NAME EB-RT-ERROR-001.manifest-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_finalized_error_manifest_audit_test.py"
    )
    add_test(
        NAME EB-RT-ERROR-001.activation-inventory-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_finalized_error_activation_audit_test.py"
    )
    set_tests_properties(
        EB-RT-ERROR-001.activation-inventory-audit
        PROPERTIES LABELS "runtime;synthetic;tooling;non-hardware"
    )
    add_test(
        NAME EB-RT-ERROR-001.monitoring-failure-projector-activation-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_monitoring_failure_projector_activation_audit_test.py")
    set_tests_properties(EB-RT-ERROR-001.monitoring-failure-projector-activation-audit
        PROPERTIES LABELS "runtime;synthetic;tooling;non-hardware")
    add_test(
        NAME EB-RT-ERROR-001.two-stage-monitoring-activation-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_two_stage_monitoring_activation_audit_test.py")
    set_tests_properties(EB-RT-ERROR-001.two-stage-monitoring-activation-audit
        PROPERTIES LABELS "runtime;synthetic;tooling;non-hardware")
    add_test(
        NAME EB-RT-ERROR-001.prepared-measurement-activation-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_prepared_measurement_activation_audit_test.py")
    set_tests_properties(EB-RT-ERROR-001.prepared-measurement-activation-audit
        PROPERTIES LABELS "runtime;synthetic;tooling;non-hardware")
    add_test(
        NAME EB-RT-ERROR-001.production-binding-activation-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_production_binding_activation_audit_test.py")
    set_tests_properties(EB-RT-ERROR-001.production-binding-activation-audit
        PROPERTIES LABELS "runtime;synthetic;tooling;non-hardware")
    add_test(
        NAME EB-RT-ERROR-001.production-run-core-activation-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_production_run_core_activation_audit_test.py")
    set_tests_properties(EB-RT-ERROR-001.production-run-core-activation-audit
        PROPERTIES LABELS "runtime;synthetic;tooling;non-hardware")
    add_test(
        NAME EB-RT-ERROR-001.execution-backend-activation-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_execution_backend_activation_audit_test.py")
    set_tests_properties(EB-RT-ERROR-001.execution-backend-activation-audit
        PROPERTIES LABELS "runtime;synthetic;tooling;non-hardware")
    add_test(
        NAME EB-RT-ERROR-001.results-activation-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_results_activation_audit_test.py")
    set_tests_properties(EB-RT-ERROR-001.results-activation-audit
        PROPERTIES LABELS "runtime;synthetic;tooling;non-hardware")
    add_test(
        NAME EB-RT-ERROR-001.calibration-predecessor-activation-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_calibration_predecessor_activation_audit_test.py")
    set_tests_properties(EB-RT-ERROR-001.calibration-predecessor-activation-audit
        PROPERTIES LABELS "runtime;synthetic;tooling;non-hardware")
    add_test(
        NAME RT-prerequisite.pairwise-monitoring-host-capacity-startup-nonactivation-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_host_capacity_startup_nonactivation_audit_test.py"
    )
    set_tests_properties(
        RT-prerequisite.pairwise-monitoring-host-capacity-startup-nonactivation-audit
        PROPERTIES LABELS "runtime;synthetic;tooling;non-hardware"
    )
    add_test(
        NAME RT-prerequisite.pairwise-v2-phase-a-nonactivation-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_v2_phase_a_nonactivation_audit_test.py"
    )
    set_tests_properties(
        RT-prerequisite.pairwise-v2-phase-a-nonactivation-audit
        PROPERTIES LABELS "runtime;synthetic;tooling;non-hardware"
    )
    add_test(
        NAME RT-prerequisite.pairwise-typed-monitoring-capacity-binding-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_typed_monitoring_capacity_binding_audit_test.py"
    )
    set_tests_properties(
        RT-prerequisite.pairwise-typed-monitoring-capacity-binding-audit
        PROPERTIES LABELS "runtime;synthetic;tooling;non-hardware;audit"
    )
    set_tests_properties(
        EB-RT-ERROR-001.manifest-audit
        PROPERTIES LABELS "runtime;synthetic;tooling;non-hardware"
    )
    set_tests_properties(
        EB-KRN-AUDIT-001.synthetic-framework
        PROPERTIES LABELS "synthetic;tooling;non-hardware"
    )
    add_test(
        NAME EB-KRN-LAT-007.object-code-policy-synthetic
        COMMAND
            "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/tools/kernel_object_audit_latency_policy_test.py"
    )
    set_tests_properties(
        EB-KRN-LAT-007.object-code-policy-synthetic
        PROPERTIES LABELS "synthetic;tooling;kernel;non-hardware"
    )
    add_test(
        NAME EB-KRN-BW-006.object-code-policy-synthetic
        COMMAND
            "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/tools/kernel_object_audit_bandwidth_policy_test.py"
    )
    set_tests_properties(
        EB-KRN-BW-006.object-code-policy-synthetic
        PROPERTIES LABELS "synthetic;tooling;kernel;non-hardware"
    )

    if(EPYCBENCH_BUILD_PRESET STREQUAL "release" OR EPYCBENCH_BUILD_PRESET STREQUAL "release-clang")
        if(EPYCBENCH_BUILD_PRESET STREQUAL "release")
            set(EPYCBENCH_LATENCY_AUDIT_COMPILER_FAMILY GCC)
            set(EPYCBENCH_BANDWIDTH_AUDIT_COMPILER_FAMILY GCC)
        else()
            set(EPYCBENCH_LATENCY_AUDIT_COMPILER_FAMILY Clang)
            set(EPYCBENCH_BANDWIDTH_AUDIT_COMPILER_FAMILY Clang)
        endif()
        add_test(
            NAME EB-KRN-LAT-007.object-code-audit
            COMMAND
                "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tools/run-latency-object-audit.py"
                --source-root "${PROJECT_SOURCE_DIR}"
                --build-directory "${PROJECT_BINARY_DIR}"
                --compile-commands "${PROJECT_BINARY_DIR}/compile_commands.json"
                --timed-source src/kernel/dependent_load_latency_execute.cpp
                --implementation-id scalar-u64-random-cycle/v1
                --semantic-symbol "epycbench::kernel::execute_dependent_load_latency(epycbench::kernel::PreparedDependentLoadLatency const&, epycbench::kernel::DependentLoadLatencySchedule)"
                --compiler-family "${EPYCBENCH_LATENCY_AUDIT_COMPILER_FAMILY}"
                --qualification-preset "${EPYCBENCH_BUILD_PRESET}"
                --family-policy "${PROJECT_SOURCE_DIR}/tools/kernel-object-audit/policies/latency-v1.json"
                --machine-report "${PROJECT_BINARY_DIR}/qualification/EB-KRN-LAT-007/object-code-audit.json"
                --human-report "${PROJECT_BINARY_DIR}/qualification/EB-KRN-LAT-007/object-code-audit.txt"
        )
        set_tests_properties(
            EB-KRN-LAT-007.object-code-audit
            PROPERTIES LABELS "release;kernel;object-code;qualification"
        )
        add_test(
            NAME EB-KRN-BW-006.object-code-audit
            COMMAND
                "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tools/run-bandwidth-object-audit.py"
                --source-root "${PROJECT_SOURCE_DIR}"
                --build-directory "${PROJECT_BINARY_DIR}"
                --compile-commands "${PROJECT_BINARY_DIR}/compile_commands.json"
                --timed-source src/kernel/sequential_read_bandwidth_execute.cpp
                --implementation-id scalar-u64-sequential/v1
                --semantic-symbol "epycbench::kernel::execute_sequential_read_bandwidth(epycbench::kernel::PreparedSequentialReadBandwidth const&, epycbench::kernel::SequentialReadBandwidthPassCount)"
                --compiler-family "${EPYCBENCH_BANDWIDTH_AUDIT_COMPILER_FAMILY}"
                --qualification-preset "${EPYCBENCH_BUILD_PRESET}"
                --family-policy "${PROJECT_SOURCE_DIR}/tools/kernel-object-audit/policies/bandwidth-v1.json"
                --machine-report "${PROJECT_BINARY_DIR}/qualification/EB-KRN-BW-006/object-code-audit.json"
                --human-report "${PROJECT_BINARY_DIR}/qualification/EB-KRN-BW-006/object-code-audit.txt"
        )
        set_tests_properties(
            EB-KRN-BW-006.object-code-audit
            PROPERTIES LABELS "release;kernel;object-code;qualification"
        )
    endif()

    add_library(
        epycbench_runtime_test_support
        STATIC
            tests/support/fake_runner_backend.cpp
            tests/support/runner_proof_builder.cpp
            tests/support/runtime_process_fixture.cpp
    )
    target_compile_features(epycbench_runtime_test_support PUBLIC cxx_std_23)
    target_include_directories(
        epycbench_runtime_test_support
        PUBLIC "${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(
        epycbench_runtime_test_support PUBLIC EpycBench::runner_state_machine
    )
    target_include_directories(
        epycbench_runtime_test_support PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    epycbench_enable_build_options(epycbench_runtime_test_support)
    epycbench_enable_warnings(epycbench_runtime_test_support)

    function(epycbench_add_runtime_ipc_test target source test_name library)
        add_executable(${target} ${source})
        target_link_libraries(${target} PRIVATE ${library} Catch2::Catch2WithMain)
        epycbench_enable_build_options(${target})
        epycbench_enable_warnings(${target})
        add_test(NAME ${test_name} COMMAND ${target} ${ARGN})
    endfunction()
    epycbench_add_runtime_ipc_test(epycbench_runtime_ipc_envelope_tests tests/unit/runtime_ipc_envelope_test.cpp EB-RT-001.ipc-envelope EpycBench::runtime_ipc)
    epycbench_add_runtime_ipc_test(epycbench_runtime_ipc_framing_tests tests/unit/runtime_ipc_framing_test.cpp EB-RT-001.ipc-framing EpycBench::runtime_ipc)
    epycbench_add_runtime_ipc_test(epycbench_runtime_ipc_state_tests tests/unit/runtime_ipc_state_validation_test.cpp EB-RT-001.ipc-state-validation EpycBench::runtime_ipc)
    epycbench_add_runtime_ipc_test(epycbench_runtime_ipc_payload_tests tests/unit/runtime_ipc_payload_test.cpp EB-RT-001.ipc-payloads EpycBench::runtime_ipc_payloads)
    add_test(
        NAME EB-RT-006.typed-cancel-request-prerequisite
        COMMAND epycbench_runtime_ipc_payload_tests "[typed-cancel-request]"
    )
    epycbench_add_runtime_ipc_test(epycbench_runtime_typed_placement_request_tests tests/unit/runtime_typed_placement_request_test.cpp EB-RT-004.typed-placement-request-prerequisite EpycBench::runtime_ipc_payloads)
    epycbench_add_runtime_ipc_test(epycbench_runtime_typed_placement_request_construction_tests tests/unit/runtime_typed_placement_request_construction_test.cpp EB-RT-prerequisite.typed-placement-request-construction EpycBench::runtime_placement_request_projection)
    target_compile_definitions(epycbench_runtime_typed_placement_request_construction_tests
        PRIVATE EPYCBENCH_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
    target_include_directories(epycbench_runtime_typed_placement_request_construction_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    epycbench_add_runtime_ipc_test(epycbench_runtime_runner_context_tests tests/unit/runtime_runner_context_test.cpp EB-RT-004.runner-context-prerequisite EpycBench::runner_context)
    target_include_directories(epycbench_runtime_runner_context_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    epycbench_add_runtime_ipc_test(epycbench_runtime_runner_control_monitor_tests tests/unit/runtime_runner_control_monitor_test.cpp EB-RT-004.runner-control-monitor-prerequisite EpycBench::runner_control_monitor)
    target_include_directories(epycbench_runtime_runner_control_monitor_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    epycbench_add_runtime_ipc_test(epycbench_runtime_runner_state_machine_tests tests/unit/runtime_runner_state_machine_test.cpp EB-RT-004.runner-state-machine EpycBench::runner_state_machine)
    target_include_directories(epycbench_runtime_runner_state_machine_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/tests/unit")
    target_link_libraries(epycbench_runtime_runner_state_machine_tests PRIVATE epycbench_runtime_test_support)
    add_test(
        NAME EB-RT-006.placed-monitor-staging-prerequisite
        COMMAND epycbench_runtime_runner_state_machine_tests "[placed-monitor-staging]"
    )
    epycbench_add_runtime_ipc_test(epycbench_runtime_runner_admission_tests tests/unit/runtime_runner_admission_test.cpp EB-RT-004.runner-admission EpycBench::runner_state_machine)
    target_include_directories(epycbench_runtime_runner_admission_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests/unit")
    target_link_libraries(epycbench_runtime_runner_admission_tests PRIVATE epycbench_runtime_test_support)
    add_test(
        NAME RT-prerequisite.admitted-placement-identity
        COMMAND epycbench_runtime_runner_admission_tests "[admitted-placement-identity]"
    )
    epycbench_add_runtime_ipc_test(epycbench_runtime_admitted_execution_inputs_tests tests/unit/runtime_admitted_execution_inputs_test.cpp EB-RT-010.admitted-execution-inputs EpycBench::runtime_ipc_payloads)
    epycbench_add_runtime_ipc_test(epycbench_runtime_admitted_execution_inputs_validation_tests tests/unit/runtime_admitted_execution_inputs_validation_test.cpp EB-RT-010.admitted-execution-inputs-validation EpycBench::runtime_ipc_payloads)
    epycbench_add_runtime_ipc_test(epycbench_runtime_admitted_execution_inputs_integration_tests tests/unit/runtime_admitted_execution_inputs_integration_test.cpp EB-RT-010.admitted-execution-inputs-integration EpycBench::runner_state_machine)
    target_include_directories(epycbench_runtime_admitted_execution_inputs_integration_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/tests/unit")
    target_link_libraries(epycbench_runtime_admitted_execution_inputs_integration_tests PRIVATE epycbench_runtime_test_support)
    epycbench_add_runtime_ipc_test(epycbench_runtime_admitted_pairwise_production_request_tests tests/unit/runtime_admitted_pairwise_production_request_test.cpp RT-prerequisite.admitted-pairwise-production-request EpycBench::runner_state_machine)
    target_include_directories(epycbench_runtime_admitted_pairwise_production_request_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/tests/unit")
    target_link_libraries(epycbench_runtime_admitted_pairwise_production_request_tests PRIVATE epycbench_runtime_test_support)
    epycbench_add_runtime_ipc_test(
        epycbench_runtime_pairwise_production_authority_tests
        tests/unit/runtime_pairwise_production_authority_test.cpp
        EB-RT-prerequisite.pairwise-production-authority
        EpycBench::runtime_production_backend
        "[production-authority]"
    )
    target_include_directories(epycbench_runtime_pairwise_production_authority_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src"
                "${PROJECT_SOURCE_DIR}/tests/unit")
    target_link_libraries(epycbench_runtime_pairwise_production_authority_tests
        PRIVATE epycbench_runtime_test_support Threads::Threads
                EpycBench::runtime_production_backend)
    epycbench_add_runtime_ipc_test(
        epycbench_runtime_pairwise_production_composition_tests
        tests/unit/runtime_pairwise_production_composition_test.cpp
        EB-RT-prerequisite.scenario-pairwise-production-composition
        EpycBench::runtime_production_backend
    )
    target_compile_definitions(epycbench_runtime_pairwise_production_composition_tests
        PRIVATE EPYCBENCH_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
    target_include_directories(epycbench_runtime_pairwise_production_composition_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src"
                "${PROJECT_SOURCE_DIR}/tests/unit")
    target_link_libraries(epycbench_runtime_pairwise_production_composition_tests
        PRIVATE EpycBench::monitoring_qualification)
    epycbench_add_runtime_ipc_test(
        epycbench_runtime_monitoring_production_admission_tests
        tests/unit/runtime_monitoring_production_admission_test.cpp
        RT-prerequisite.monitoring-production-admission
        EpycBench::runtime_production_backend
        "~[pairwise-long]"
    )
    target_include_directories(
        epycbench_runtime_monitoring_production_admission_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src"
                "${PROJECT_SOURCE_DIR}/tests/unit"
    )
    target_link_libraries(
        epycbench_runtime_monitoring_production_admission_tests
        PRIVATE epycbench_runtime_test_support
                EpycBench::runtime_production_backend
                EpycBench::pairwise_v2_native_lifecycle
    )
    # Every late producer case has its own finite registration. The base
    # Monitoring registration excludes only these tags; imported authority
    # fixtures remain compiled but execute under their dedicated registrations.
    foreach(pairwise_long_case IN ITEMS
        prepared-native-retained-evidence
        prepared-native-progress
        producer-journal-inputs
        producer-requested-stock
        producer-journal-callbacks
        producer-operational-faults
        producer-failed-observation
        producer-journal-chronology
        producer-batch-stock
        producer-conflicting-failure
        producer-terminal-append
        producer-partial-write-sync
        producer-append-expiry
        producer-controller-stock-refusal
        producer-interrupted-progress
        prepared-native-offered-counts
        prepared-native-format-ceilings
        stage-b-zero-edge
        stage-b-entry-refusals
        stage-b-preflight
        stage-b-whole-cancellation
        stage-b-whole-publication
        public-run-publication
        public-run-preparation
        public-run-unavailable-frequency
        public-run-early-host-failure
        public-run-missing-host-callback
        public-run-final-identity-failure
        public-run-topology-retention
        public-run-preflight-temperature
        public-run-preparation-cancellation
        public-run-service-cancellation
        stage-b-pair-publication
        stage-b-pair-cancellation
        stage-b-signal-scope
        stage-b-publication-collision
    )
        add_test(
            NAME RT-prerequisite.monitoring-production-admission.${pairwise_long_case}
            COMMAND epycbench_runtime_monitoring_production_admission_tests
                "[pairwise-long-${pairwise_long_case}]"
        )
        set_tests_properties(
            RT-prerequisite.monitoring-production-admission.${pairwise_long_case}
            PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE
                LABELS "runtime;synthetic;non-hardware"
        )
    endforeach()

    # Realistic public composition uses honest GiB backing and supplied host
    # inputs. Keep each distinct profile finite and serial, outside the base
    # Monitoring registration; these remain software integration tests.
    set(public_realistic_timeout 900)
    if(EPYCBENCH_ENABLE_ASAN_UBSAN)
        # Instrumented parsing/publication and destruction need a separate
        # software supervisor bound; production/measurement deadlines do not change.
        set(public_realistic_timeout 1800)
    endif()
    # Dense retained context exercises the complete digest/recovery/publication
    # path. Use the existing software integration supervisor, including its
    # instrumented allowance; no production resource or deadline changes.
    add_test(
        NAME RT-prerequisite.monitoring-production-admission.compact-reconcile-stock
        COMMAND epycbench_runtime_monitoring_production_admission_tests
            "[compact-reconcile-stock]"
    )
    set_tests_properties(
        RT-prerequisite.monitoring-production-admission.compact-reconcile-stock
        PROPERTIES TIMEOUT ${public_realistic_timeout} RUN_SERIAL TRUE
            LABELS "runtime;results;synthetic;non-hardware"
    )
    foreach(public_realistic_profile IN ITEMS complete workers latency)
        add_test(
            NAME RT-prerequisite.monitoring-production-admission.public-realistic-${public_realistic_profile}
            COMMAND epycbench_runtime_monitoring_production_admission_tests
                "[public-realistic-${public_realistic_profile}]"
        )
        set_tests_properties(
            RT-prerequisite.monitoring-production-admission.public-realistic-${public_realistic_profile}
            PROPERTIES TIMEOUT ${public_realistic_timeout} RUN_SERIAL TRUE
                LABELS "runtime;synthetic;non-hardware"
        )
    endforeach()

    epycbench_add_runtime_ipc_test(
        epycbench_runtime_statistics_production_policy_tests
        tests/unit/runtime_statistics_production_policy_test.cpp
        RT-prerequisite.statistics-production-policy
        EpycBench::runtime_production_backend
    )
    target_include_directories(
        epycbench_runtime_statistics_production_policy_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/tests/unit"
    )
    epycbench_add_runtime_ipc_test(
        epycbench_runtime_pairwise_run_evidence_tests
        tests/unit/runtime_pairwise_run_evidence_test.cpp
        RT-prerequisite.pairwise-run-evidence-ledger
        EpycBench::runtime_production_backend
    )
    target_include_directories(
        epycbench_runtime_pairwise_run_evidence_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/tests/unit"
    )
    target_link_libraries(
        epycbench_runtime_pairwise_run_evidence_tests PRIVATE epycbench_runtime_test_support
    )
    add_executable(
        epycbench_pairwise_finalized_error_profile_tests
        tests/unit/runtime_pairwise_finalized_error_profile_test.cpp
    )
    target_link_libraries(
        epycbench_pairwise_finalized_error_profile_tests
        PRIVATE EpycBench::pairwise_finalized_error_profile Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_pairwise_finalized_error_profile_tests)
    epycbench_enable_warnings(epycbench_pairwise_finalized_error_profile_tests)
    add_test(
        NAME EB-RT-ERROR-001.pairwise-finalized-error-profile
        COMMAND epycbench_pairwise_finalized_error_profile_tests
    )
    add_executable(epycbench_runtime_pairwise_typed_adapter_storage_tests
        tests/unit/runtime_pairwise_typed_adapter_storage_test.cpp)
    target_include_directories(epycbench_runtime_pairwise_typed_adapter_storage_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_runtime_pairwise_typed_adapter_storage_tests
        PRIVATE EpycBench::runtime_production_backend Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_runtime_pairwise_typed_adapter_storage_tests)
    epycbench_enable_warnings(epycbench_runtime_pairwise_typed_adapter_storage_tests)
    add_test(NAME EB-RT-ERROR-001.typed-adapter-storage
        COMMAND epycbench_runtime_pairwise_typed_adapter_storage_tests)
    set_tests_properties(EB-RT-ERROR-001.typed-adapter-storage
        PROPERTIES LABELS "runtime;synthetic;non-hardware")
    add_executable(epycbench_runtime_pairwise_production_run_core_tests
        tests/unit/runtime_pairwise_production_run_core_test.cpp)
    target_include_directories(epycbench_runtime_pairwise_production_run_core_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_runtime_pairwise_production_run_core_tests
        PRIVATE EpycBench::runtime_production_backend Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_runtime_pairwise_production_run_core_tests)
    epycbench_enable_warnings(epycbench_runtime_pairwise_production_run_core_tests)
    add_test(NAME EB-RT-ERROR-001.production-run-core
        COMMAND epycbench_runtime_pairwise_production_run_core_tests)
    set_tests_properties(EB-RT-ERROR-001.production-run-core
        PROPERTIES LABELS "runtime;synthetic;non-hardware")
    add_test(NAME EB-RT-ERROR-001.production-run-core-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_production_run_core_audit_test.py")
    set_tests_properties(EB-RT-ERROR-001.production-run-core-audit
        PROPERTIES LABELS "runtime;synthetic;non-hardware;audit")
    add_test(NAME EB-RT-ERROR-001.typed-owner-sequence-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_typed_owner_sequence_audit_test.py")
    set_tests_properties(EB-RT-ERROR-001.typed-owner-sequence-audit
        PROPERTIES LABELS "runtime;synthetic;non-hardware;audit")
    add_executable(epycbench_runtime_pairwise_prepared_typed_measurement_execution_tests
        tests/unit/runtime_pairwise_prepared_typed_measurement_execution_test.cpp)
    target_include_directories(
        epycbench_runtime_pairwise_prepared_typed_measurement_execution_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        epycbench_runtime_pairwise_prepared_typed_measurement_execution_tests
        PRIVATE EpycBench::runtime_production_backend
                Catch2::Catch2WithMain)
    epycbench_enable_build_options(
        epycbench_runtime_pairwise_prepared_typed_measurement_execution_tests)
    epycbench_enable_warnings(
        epycbench_runtime_pairwise_prepared_typed_measurement_execution_tests)
    add_test(NAME EB-RT-ERROR-001.prepared-typed-measurement-execution
        COMMAND epycbench_runtime_pairwise_prepared_typed_measurement_execution_tests)
    set_tests_properties(EB-RT-ERROR-001.prepared-typed-measurement-execution
        PROPERTIES LABELS "runtime;synthetic;non-hardware")
    add_test(NAME EB-RT-ERROR-001.legacy-adapter-extraction-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_legacy_adapter_extraction_audit_test.py")
    set_tests_properties(EB-RT-ERROR-001.legacy-adapter-extraction-audit
        PROPERTIES LABELS "runtime;synthetic;non-hardware;audit")
    add_executable(
        epycbench_runtime_finalized_evidence_error_tests
        tests/unit/runtime_finalized_evidence_error_test.cpp
    )
    target_link_libraries(
        epycbench_runtime_finalized_evidence_error_tests
        PRIVATE EpycBench::pairwise_finalized_error_profile Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_runtime_finalized_evidence_error_tests)
    epycbench_enable_warnings(epycbench_runtime_finalized_evidence_error_tests)
    add_test(
        NAME RT-prerequisite.finalized-evidence-error
        COMMAND epycbench_runtime_finalized_evidence_error_tests
    )
    add_executable(epycbench_results_incremental_block_evidence_tests tests/unit/results_incremental_block_evidence_test.cpp)
    target_include_directories(epycbench_results_incremental_block_evidence_tests PRIVATE
        "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_incremental_block_evidence_tests PRIVATE
        epycbench_results_incremental_block_evidence EpycBench::results EpycBench::results_journal_records
        EpycBench::finalized_evidence_wire_typed_reconstruction epycbench_runtime_test_support Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_incremental_block_evidence_tests)
    epycbench_enable_warnings(epycbench_results_incremental_block_evidence_tests)
    add_test(NAME EB-RES-002.incremental-block-evidence COMMAND epycbench_results_incremental_block_evidence_tests)
    add_executable(epycbench_pairwise_monitoring_preparation_facts_tests
        tests/unit/runtime_pairwise_monitoring_preparation_facts_test.cpp)
    target_include_directories(epycbench_pairwise_monitoring_preparation_facts_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_pairwise_monitoring_preparation_facts_tests PRIVATE EPYCBENCH_MONITORING_FACT_TEST=1)
    target_link_libraries(epycbench_pairwise_monitoring_preparation_facts_tests PRIVATE
        epycbench_pairwise_monitoring_preparation_facts EpycBench::monitoring Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_pairwise_monitoring_preparation_facts_tests)
    epycbench_enable_warnings(epycbench_pairwise_monitoring_preparation_facts_tests)
    add_test(NAME EB-RES-002.monitoring-preparation-facts COMMAND epycbench_pairwise_monitoring_preparation_facts_tests)
    add_test(NAME EB-RES-002.monitoring-preparation-facts-audit
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_monitoring_preparation_facts_test.py")
    add_executable(epycbench_pairwise_preparation_graph_tests tests/unit/runtime_pairwise_preparation_graph_test.cpp)
    target_include_directories(epycbench_pairwise_preparation_graph_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_pairwise_preparation_graph_tests PRIVATE
        epycbench_pairwise_preparation_graph EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_pairwise_preparation_graph_tests)
    epycbench_enable_warnings(epycbench_pairwise_preparation_graph_tests)
    add_test(NAME EB-RES-002.preparation-graph COMMAND epycbench_pairwise_preparation_graph_tests)
    add_test(NAME EB-RES-002.preparation-graph-audit
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_preparation_graph_test.py")
    add_executable(epycbench_results_pair_analysis_tests tests/unit/results_pair_analysis_admission_test.cpp
        src/results/pairwise_run_result.cpp)
    target_compile_definitions(epycbench_results_pair_analysis_tests PRIVATE
        EPYCBENCH_MONITORING_FACT_TEST=1 EPYCBENCH_PREPARATION_GRAPH_TEST=1 EPYCBENCH_RESULTS_CALCULATION_TESTING)
    target_include_directories(epycbench_results_pair_analysis_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_pair_analysis_tests PRIVATE epycbench_results_process_subject
        epycbench_results_pair_analysis EpycBench::pairwise_v2_synthetic_lifecycle
        EpycBench::results_journal_records epycbench_runtime_test_support Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_pair_analysis_tests)
    epycbench_enable_warnings(epycbench_results_pair_analysis_tests)
    add_test(NAME EB-RES-002.pair-analysis COMMAND epycbench_results_pair_analysis_tests)
    add_test(NAME EB-RES-002.pair-analysis-nonactivation
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/tools/results_pair_analysis_admission_test.py)

    add_executable(epycbench_results_pair_lifecycle_tests tests/unit/results_pair_lifecycle_admission_test.cpp)
    add_executable(epycbench_results_observation_timing_tests tests/unit/results_observation_timing_admission_test.cpp)
    target_compile_definitions(epycbench_results_observation_timing_tests PRIVATE
        EPYCBENCH_MONITORING_FACT_TEST=1 EPYCBENCH_PREPARATION_GRAPH_TEST=1)
    target_include_directories(epycbench_results_observation_timing_tests PRIVATE
        "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_observation_timing_tests PRIVATE
        epycbench_results_process_subject epycbench_results_pair_lifecycle
        EpycBench::pairwise_v2_synthetic_lifecycle epycbench_results_incremental_pair_evidence
        EpycBench::results EpycBench::results_journal_records
        EpycBench::finalized_evidence_wire_typed_reconstruction epycbench_runtime_test_support Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_observation_timing_tests)
    epycbench_enable_warnings(epycbench_results_observation_timing_tests)
    add_test(NAME EB-RES-002.observation-timing COMMAND epycbench_results_observation_timing_tests)
    add_test(NAME EB-RES-002.observation-timing-nonactivation
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/tools/results_observation_timing_admission_test.py)
    target_compile_definitions(epycbench_results_pair_lifecycle_tests PRIVATE
        EPYCBENCH_MONITORING_FACT_TEST=1 EPYCBENCH_PREPARATION_GRAPH_TEST=1)
    target_include_directories(epycbench_results_pair_lifecycle_tests PRIVATE
        "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_pair_lifecycle_tests PRIVATE
        epycbench_results_process_subject epycbench_results_pair_lifecycle
        EpycBench::pairwise_v2_synthetic_lifecycle epycbench_results_incremental_pair_evidence
        EpycBench::results EpycBench::results_journal_records
        EpycBench::finalized_evidence_wire_typed_reconstruction epycbench_runtime_test_support Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_pair_lifecycle_tests)
    epycbench_enable_warnings(epycbench_results_pair_lifecycle_tests)
    add_test(NAME EB-RES-002.pair-lifecycle COMMAND epycbench_results_pair_lifecycle_tests)
    add_test(NAME EB-RES-002.pair-lifecycle-nonactivation
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/tools/results_pair_lifecycle_admission_test.py)
    add_executable(epycbench_results_incremental_pair_evidence_tests tests/unit/results_incremental_pair_evidence_test.cpp)
    target_compile_definitions(epycbench_results_incremental_pair_evidence_tests PRIVATE EPYCBENCH_MONITORING_FACT_TEST=1 EPYCBENCH_PREPARATION_GRAPH_TEST=1)
    target_include_directories(epycbench_results_incremental_pair_evidence_tests PRIVATE
        "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_incremental_pair_evidence_tests PRIVATE
        epycbench_results_incremental_pair_evidence EpycBench::results EpycBench::results_journal_records
        EpycBench::finalized_evidence_wire_typed_reconstruction epycbench_runtime_test_support Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_incremental_pair_evidence_tests)
    epycbench_enable_warnings(epycbench_results_incremental_pair_evidence_tests)
    add_test(NAME EB-RES-002.incremental-pair-evidence
        COMMAND epycbench_results_incremental_pair_evidence_tests "~[compact-finalization]")
    add_test(NAME EB-RES-002.compact-root
        COMMAND epycbench_results_incremental_pair_evidence_tests "[compact-finalization]")
    set_tests_properties(EB-RES-002.compact-root PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE LABELS "results;synthetic;non-hardware")
    add_test(NAME EB-RES-002.incremental-pair-evidence-audit
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/tools/results_incremental_pair_evidence_test.py)
    add_test(NAME EB-RES-002.preparation-outcomes-audit
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/tools/pairwise_preparation_outcomes_test.py)
    add_executable(epycbench_results_journal_snapshot_tests tests/unit/results_journal_snapshot_test.cpp)
    target_include_directories(epycbench_results_journal_snapshot_tests PRIVATE
        "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests/unit")
    target_link_libraries(epycbench_results_journal_snapshot_tests PRIVATE epycbench_results_journal_snapshot
        EpycBench::results EpycBench::results_journal_records EpycBench::finalized_evidence_wire_typed_reconstruction Catch2::Catch2WithMain
        epycbench_runtime_test_support)
    epycbench_enable_build_options(epycbench_results_journal_snapshot_tests)
    epycbench_enable_warnings(epycbench_results_journal_snapshot_tests)
    add_test(NAME EB-RES-002.semantic-snapshot COMMAND epycbench_results_journal_snapshot_tests)
    set_tests_properties(EB-RES-002.semantic-snapshot PROPERTIES LABELS "results;non-hardware")
    add_test(NAME EB-RES-002.semantic-snapshot-nonactivation
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_snapshot_nonactivation_test.py")
    set_tests_properties(EB-RES-002.semantic-snapshot-nonactivation PROPERTIES LABELS "results;non-hardware;audit")

    add_executable(epycbench_results_journal_records_tests
        tests/unit/results_journal_records_test.cpp)
    target_include_directories(epycbench_results_journal_records_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_journal_records_tests
        PRIVATE EpycBench::results_journal_records Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_journal_records_tests)
    epycbench_enable_warnings(epycbench_results_journal_records_tests)
    add_test(NAME EB-RES-002.memory-journal-records COMMAND epycbench_results_journal_records_tests)
    set_tests_properties(EB-RES-002.memory-journal-records PROPERTIES LABELS "results;non-hardware")
    add_test(NAME EB-RES-002.journal-records-nonactivation
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/results_journal_records_nonactivation_test.py")
    set_tests_properties(EB-RES-002.journal-records-nonactivation
        PROPERTIES LABELS "results;non-hardware;audit")

    add_executable(epycbench_results_private_kernel_identity_tests tests/unit/results_private_kernel_identity_test.cpp)
    target_include_directories(epycbench_results_private_kernel_identity_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_kernel_identity_tests PRIVATE epycbench_results_private_kernel_identity epycbench_results_topology_evidence EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_kernel_identity_tests)
    epycbench_enable_warnings(epycbench_results_private_kernel_identity_tests)
    add_test(NAME EB-RES-002.private-kernel-identity COMMAND epycbench_results_private_kernel_identity_tests)
    add_test(NAME EB-RES-002.private-kernel-identity-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_kernel_identity_nonactivation_test.py")

    add_executable(epycbench_results_pu_cpu_identity_tests tests/unit/results_pu_cpu_identity_test.cpp src/results/pu_cpu_identity.cpp)
    target_compile_definitions(epycbench_results_pu_cpu_identity_tests PRIVATE EPYCBENCH_PU_CPU_IDENTITY_TESTING)
    target_include_directories(epycbench_results_pu_cpu_identity_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_pu_cpu_identity_tests PRIVATE epycbench_results_topology_evidence EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_pu_cpu_identity_tests)
    epycbench_enable_warnings(epycbench_results_pu_cpu_identity_tests)
    add_test(NAME EB-RES-002.pu-cpu-identity COMMAND epycbench_results_pu_cpu_identity_tests)
    add_test(NAME EB-RES-002.pu-cpu-identity-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_pu_cpu_identity_nonactivation_test.py")

    add_executable(epycbench_results_anonymous_thp_policy_owner_tests tests/unit/results_anonymous_thp_policy_owner_test.cpp src/results/anonymous_thp_policy_owner.cpp)
    target_compile_definitions(epycbench_results_anonymous_thp_policy_owner_tests PRIVATE EPYCBENCH_ANONYMOUS_THP_POLICY_OWNER_TESTING)
    target_include_directories(epycbench_results_anonymous_thp_policy_owner_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_anonymous_thp_policy_owner_tests PRIVATE epycbench_results_topology_evidence EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_anonymous_thp_policy_owner_tests)
    epycbench_enable_warnings(epycbench_results_anonymous_thp_policy_owner_tests)
    add_test(NAME EB-RES-002.anonymous-thp-policy-owner COMMAND epycbench_results_anonymous_thp_policy_owner_tests)
    add_test(NAME EB-RES-002.anonymous-thp-policy-owner-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_anonymous_thp_policy_owner_nonactivation_test.py")

    add_executable(epycbench_results_private_comparison_experiments_serialization_tests tests/unit/results_private_comparison_experiments_serialization_test.cpp)
    target_include_directories(epycbench_results_private_comparison_experiments_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_comparison_experiments_serialization_tests PRIVATE epycbench_results_private_comparison_experiments_serialization EpycBench::scenario EpycBench::topology EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_comparison_experiments_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_comparison_experiments_serialization_tests)
    add_test(NAME EB-RES-002.private-comparison-experiments-serialization COMMAND epycbench_results_private_comparison_experiments_serialization_tests)
    add_test(NAME EB-RES-002.private-comparison-experiments-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_comparison_experiments_serialization_test.py")

    add_executable(epycbench_results_private_matrix_experiments_serialization_tests tests/unit/results_private_matrix_experiments_serialization_test.cpp)
    target_include_directories(epycbench_results_private_matrix_experiments_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_matrix_experiments_serialization_tests PRIVATE epycbench_results_private_matrix_experiments_serialization EpycBench::scenario EpycBench::topology EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_matrix_experiments_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_matrix_experiments_serialization_tests)
    add_test(NAME EB-RES-002.private-matrix-experiments-serialization COMMAND epycbench_results_private_matrix_experiments_serialization_tests)
    add_test(NAME EB-RES-002.private-matrix-experiments-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_matrix_experiments_serialization_test.py")

    add_executable(epycbench_results_private_kernel_dispatch_serialization_tests tests/unit/results_private_kernel_dispatch_serialization_test.cpp)
    target_include_directories(epycbench_results_private_kernel_dispatch_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_kernel_dispatch_serialization_tests PRIVATE epycbench_results_private_kernel_dispatch_serialization EpycBench::scenario EpycBench::kernel_api EpycBench::core EpycBench::topology Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_kernel_dispatch_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_kernel_dispatch_serialization_tests)
    add_test(NAME EB-RES-002.private-kernel-dispatch-serialization COMMAND epycbench_results_private_kernel_dispatch_serialization_tests)
    add_test(NAME EB-RES-002.private-kernel-dispatch-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_kernel_dispatch_serialization_test.py")

    add_executable(epycbench_results_private_memory_placement_serialization_tests tests/unit/results_private_memory_placement_serialization_test.cpp)
    target_include_directories(epycbench_results_private_memory_placement_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_memory_placement_serialization_tests PRIVATE epycbench_results_private_memory_placement_serialization EpycBench::scenario EpycBench::topology EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_memory_placement_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_memory_placement_serialization_tests)
    add_test(NAME EB-RES-002.private-memory-placement-serialization COMMAND epycbench_results_private_memory_placement_serialization_tests)
    add_test(NAME EB-RES-002.private-memory-placement-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_memory_placement_serialization_test.py")

    add_executable(epycbench_results_private_resource_service_serialization_tests tests/unit/results_private_resource_service_serialization_test.cpp)
    target_include_directories(epycbench_results_private_resource_service_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_resource_service_serialization_tests PRIVATE epycbench_results_private_resource_service_serialization EpycBench::scenario EpycBench::topology EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_resource_service_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_resource_service_serialization_tests)
    add_test(NAME EB-RES-002.private-resource-service-serialization COMMAND epycbench_results_private_resource_service_serialization_tests)
    add_test(NAME EB-RES-002.private-resource-service-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_resource_service_serialization_test.py")

    add_executable(epycbench_results_private_document_ancestry_serialization_tests tests/unit/results_private_document_ancestry_serialization_test.cpp)
    target_include_directories(epycbench_results_private_document_ancestry_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_document_ancestry_serialization_tests PRIVATE epycbench_results_private_document_ancestry_serialization EpycBench::scenario EpycBench::topology EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_document_ancestry_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_document_ancestry_serialization_tests)
    add_test(NAME EB-RES-002.private-document-ancestry-serialization COMMAND epycbench_results_private_document_ancestry_serialization_tests)
    add_test(NAME EB-RES-002.private-document-ancestry-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_document_ancestry_serialization_test.py")

    add_executable(epycbench_results_private_run_creation_serialization_tests tests/unit/results_private_run_creation_serialization_test.cpp)
    target_include_directories(epycbench_results_private_run_creation_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_run_creation_serialization_tests PRIVATE epycbench_results_process_subject EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_run_creation_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_run_creation_serialization_tests)
    add_test(NAME EB-RES-002.private-run-creation-serialization COMMAND epycbench_results_private_run_creation_serialization_tests)
    add_test(NAME EB-RES-002.private-run-creation-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_run_creation_serialization_test.py")
    add_executable(epycbench_results_private_host_serialization_tests tests/unit/results_private_host_serialization_test.cpp)
    target_include_directories(epycbench_results_private_host_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_host_serialization_tests PRIVATE epycbench_results_process_subject EpycBench::monitoring EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_host_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_host_serialization_tests)
    add_test(NAME EB-RES-002.private-host-serialization COMMAND epycbench_results_private_host_serialization_tests)
    add_test(NAME EB-RES-002.private-host-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_host_serialization_test.py")

    add_executable(epycbench_results_private_process_thp_restriction_serialization_tests tests/unit/results_private_process_thp_restriction_serialization_test.cpp)
    target_include_directories(epycbench_results_private_process_thp_restriction_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_process_thp_restriction_serialization_tests PRIVATE epycbench_results_process_subject EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_process_thp_restriction_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_process_thp_restriction_serialization_tests)
    add_test(NAME EB-RES-002.private-process-thp-restriction-serialization COMMAND epycbench_results_private_process_thp_restriction_serialization_tests)
    add_test(NAME EB-RES-002.private-process-thp-restriction-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_process_thp_restriction_serialization_test.py")

    add_executable(epycbench_results_private_anonymous_thp_policy_serialization_tests tests/unit/results_private_anonymous_thp_policy_serialization_test.cpp)
    target_include_directories(epycbench_results_private_anonymous_thp_policy_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_anonymous_thp_policy_serialization_tests PRIVATE epycbench_results_process_subject EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_anonymous_thp_policy_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_anonymous_thp_policy_serialization_tests)
    add_test(NAME EB-RES-002.private-anonymous-thp-policy-serialization COMMAND epycbench_results_private_anonymous_thp_policy_serialization_tests)
    add_test(NAME EB-RES-002.private-anonymous-thp-policy-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_anonymous_thp_policy_serialization_test.py")
    add_executable(epycbench_results_private_frequency_policy_serialization_tests tests/unit/results_private_frequency_policy_serialization_test.cpp)
    target_include_directories(epycbench_results_private_frequency_policy_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_frequency_policy_serialization_tests PRIVATE epycbench_results_process_subject EpycBench::monitoring EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_frequency_policy_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_frequency_policy_serialization_tests)
    add_test(NAME EB-RES-002.private-frequency-policy-serialization COMMAND epycbench_results_private_frequency_policy_serialization_tests)
    add_test(NAME EB-RES-002.private-frequency-policy-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_frequency_policy_serialization_test.py")
    add_executable(epycbench_results_private_pu_cpu_identity_serialization_tests tests/unit/results_private_pu_cpu_identity_serialization_test.cpp)
    target_include_directories(epycbench_results_private_pu_cpu_identity_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_pu_cpu_identity_serialization_tests PRIVATE epycbench_results_process_subject EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_pu_cpu_identity_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_pu_cpu_identity_serialization_tests)
    add_test(NAME EB-RES-002.private-pu-cpu-identity-serialization COMMAND epycbench_results_private_pu_cpu_identity_serialization_tests)
    add_test(NAME EB-RES-002.private-pu-cpu-identity-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_pu_cpu_identity_serialization_test.py")
    add_executable(epycbench_results_private_kernel_identity_serialization_tests tests/unit/results_private_kernel_identity_serialization_test.cpp)
    target_include_directories(epycbench_results_private_kernel_identity_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_kernel_identity_serialization_tests PRIVATE epycbench_results_process_subject EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_kernel_identity_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_kernel_identity_serialization_tests)
    add_test(NAME EB-RES-002.private-kernel-identity-serialization COMMAND epycbench_results_private_kernel_identity_serialization_tests)
    add_test(NAME EB-RES-002.private-kernel-identity-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_kernel_identity_serialization_test.py")

    add_executable(epycbench_results_private_system_controls_serialization_tests tests/unit/results_private_system_controls_serialization_test.cpp)
    target_include_directories(epycbench_results_private_system_controls_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_system_controls_serialization_tests PRIVATE epycbench_results_process_subject EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_system_controls_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_system_controls_serialization_tests)
    add_test(NAME EB-RES-002.private-system-controls-serialization COMMAND epycbench_results_private_system_controls_serialization_tests)
    add_test(NAME EB-RES-002.private-system-controls-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_system_controls_serialization_test.py")

    add_executable(epycbench_results_private_topology_serialization_tests tests/unit/results_private_topology_serialization_test.cpp)
    target_include_directories(epycbench_results_private_topology_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_topology_serialization_tests PRIVATE epycbench_results_process_subject EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_topology_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_topology_serialization_tests)
    add_test(NAME EB-RES-002.private-topology-serialization COMMAND epycbench_results_private_topology_serialization_tests)
    add_test(NAME EB-RES-002.private-topology-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_topology_serialization_test.py")

    add_executable(epycbench_results_private_base_build_serialization_tests tests/unit/results_private_base_build_serialization_test.cpp)
    target_include_directories(epycbench_results_private_base_build_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_base_build_serialization_tests PRIVATE epycbench_results_process_subject EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_base_build_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_base_build_serialization_tests)
    add_test(NAME EB-RES-002.private-base-build-serialization COMMAND epycbench_results_private_base_build_serialization_tests)
    add_test(NAME EB-RES-002.private-base-build-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_base_build_serialization_test.py")

    add_executable(epycbench_results_private_source_outcome_serialization_tests tests/unit/results_private_source_outcome_serialization_test.cpp)
    target_include_directories(epycbench_results_private_source_outcome_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_source_outcome_serialization_tests PRIVATE epycbench_results_process_subject EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_source_outcome_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_source_outcome_serialization_tests)
    add_test(NAME EB-RES-002.private-source-outcome-serialization COMMAND epycbench_results_private_source_outcome_serialization_tests)
    add_test(NAME EB-RES-002.private-source-outcome-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_source_outcome_serialization_test.py")
    add_executable(epycbench_results_private_source_serialization_tests tests/unit/results_private_source_serialization_test.cpp)
    target_include_directories(epycbench_results_private_source_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_source_serialization_tests PRIVATE epycbench_results_process_subject EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_source_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_source_serialization_tests)
    add_test(NAME EB-RES-002.private-source-serialization COMMAND epycbench_results_private_source_serialization_tests)
    add_test(NAME EB-RES-002.private-source-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_source_serialization_test.py")
    add_executable(epycbench_results_private_error_serialization_tests tests/unit/results_private_error_serialization_test.cpp)
    target_include_directories(epycbench_results_private_error_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_error_serialization_tests PRIVATE epycbench_results_process_subject EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_error_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_error_serialization_tests)
    add_test(NAME EB-RES-002.private-error-serialization COMMAND epycbench_results_private_error_serialization_tests)
    add_test(NAME EB-RES-002.private-error-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_error_serialization_test.py")

    add_executable(epycbench_results_private_error_tests tests/unit/results_private_error_owner_test.cpp)
    target_include_directories(epycbench_results_private_error_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_error_tests PRIVATE epycbench_results_private_error EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_error_tests)
    epycbench_enable_warnings(epycbench_results_private_error_tests)
    add_test(NAME EB-RES-002.private-error-owner COMMAND epycbench_results_private_error_tests)
    add_test(NAME EB-RES-002.private-error-owner-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_error_owner_nonactivation_test.py")

    add_executable(epycbench_results_preflight_tests tests/unit/results_preflight_admission_test.cpp)
    target_include_directories(epycbench_results_preflight_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_preflight_tests PRIVATE epycbench_results_process_subject
        epycbench_results_preflight epycbench_runtime_placement_request_projection epycbench_cli
        EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_preflight_tests)
    epycbench_enable_warnings(epycbench_results_preflight_tests)
    add_test(NAME EB-RES-002.preflight COMMAND epycbench_results_preflight_tests)
    add_test(NAME EB-RES-002.preflight-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_preflight_admission_test.py")

    add_executable(epycbench_results_run_creation_tests tests/unit/results_run_creation_admission_test.cpp)
    target_include_directories(epycbench_results_run_creation_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_run_creation_tests PRIVATE epycbench_results_process_subject EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_run_creation_tests)
    epycbench_enable_warnings(epycbench_results_run_creation_tests)
    add_test(NAME EB-RES-002.run-creation COMMAND epycbench_results_run_creation_tests)
    add_test(NAME EB-RES-002.run-creation-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_run_creation_admission_test.py")

    add_executable(epycbench_results_host_projection_tests tests/unit/results_host_projection_test.cpp)
    target_include_directories(epycbench_results_host_projection_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_host_projection_tests PRIVATE epycbench_results_process_subject EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_host_projection_tests)
    epycbench_enable_warnings(epycbench_results_host_projection_tests)
    add_test(NAME EB-RES-002.host-projection COMMAND epycbench_results_host_projection_tests)
    add_test(NAME EB-RES-002.host-projection-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_host_projection_test.py")

    add_executable(epycbench_results_build_projection_tests tests/unit/results_build_projection_test.cpp)
    target_include_directories(epycbench_results_build_projection_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_build_projection_tests PRIVATE epycbench_results_process_subject EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_build_projection_tests)
    epycbench_enable_warnings(epycbench_results_build_projection_tests)
    add_test(NAME EB-RES-002.build-projection COMMAND epycbench_results_build_projection_tests)
    add_test(NAME EB-RES-002.build-projection-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_build_projection_test.py")

    add_executable(epycbench_results_process_subject_tests tests/unit/results_process_subject_admission_test.cpp)
    target_include_directories(epycbench_results_process_subject_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_process_subject_tests PRIVATE epycbench_results_process_subject EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_process_subject_tests)
    epycbench_enable_warnings(epycbench_results_process_subject_tests)
    add_test(NAME EB-RES-002.process-subject COMMAND epycbench_results_process_subject_tests)
    add_test(NAME EB-RES-002.process-subject-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_process_subject_admission_nonactivation_test.py")

    add_executable(epycbench_results_host_assembly_tests tests/unit/results_host_evidence_assembly_test.cpp)
    target_include_directories(epycbench_results_host_assembly_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_host_assembly_tests PRIVATE epycbench_results_host_assembly EpycBench::results_journal_records EpycBench::topology EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_host_assembly_tests)
    epycbench_enable_warnings(epycbench_results_host_assembly_tests)
    add_test(NAME EB-RES-002.host-assembly COMMAND epycbench_results_host_assembly_tests)
    add_test(NAME EB-RES-002.host-assembly-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_host_evidence_assembly_nonactivation_test.py")

    add_executable(epycbench_results_process_thp_restriction_tests tests/unit/results_process_thp_restriction_test.cpp src/results/process_thp_restriction.cpp)
    target_compile_definitions(epycbench_results_process_thp_restriction_tests PRIVATE EPYCBENCH_PROCESS_THP_RESTRICTION_TESTING)
    target_include_directories(epycbench_results_process_thp_restriction_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_process_thp_restriction_tests PRIVATE epycbench_results_topology_evidence EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_process_thp_restriction_tests)
    epycbench_enable_warnings(epycbench_results_process_thp_restriction_tests)
    add_test(NAME EB-RES-002.process-thp-restriction COMMAND epycbench_results_process_thp_restriction_tests)
    add_test(NAME EB-RES-002.process-thp-restriction-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_process_thp_restriction_nonactivation_test.py")

    add_executable(epycbench_results_frequency_policy_owner_tests tests/unit/results_frequency_policy_owner_test.cpp src/results/frequency_policy_owner.cpp)
    target_compile_definitions(epycbench_results_frequency_policy_owner_tests PRIVATE EPYCBENCH_FREQUENCY_POLICY_OWNER_TESTING)
    target_include_directories(epycbench_results_frequency_policy_owner_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_frequency_policy_owner_tests PRIVATE EpycBench::monitoring epycbench_results_topology_evidence EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_frequency_policy_owner_tests)
    epycbench_enable_warnings(epycbench_results_frequency_policy_owner_tests)
    add_test(NAME EB-RES-002.frequency-policy-owner COMMAND epycbench_results_frequency_policy_owner_tests)
    add_test(NAME EB-RES-002.frequency-policy-owner-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_frequency_policy_owner_nonactivation_test.py")

    add_executable(epycbench_results_host_controls_tests tests/unit/results_host_system_controls_test.cpp)
    target_include_directories(epycbench_results_host_controls_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_host_controls_tests PRIVATE epycbench_results_host_controls epycbench_results_topology_evidence EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_host_controls_tests)
    epycbench_enable_warnings(epycbench_results_host_controls_tests)
    add_test(NAME EB-RES-002.host-controls COMMAND epycbench_results_host_controls_tests)
    add_test(NAME EB-RES-002.host-controls-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_host_system_controls_nonactivation_test.py")

    add_executable(epycbench_results_topology_evidence_tests tests/unit/results_topology_evidence_owner_test.cpp src/results/topology_evidence_owner.cpp)
    target_include_directories(epycbench_results_topology_evidence_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_results_topology_evidence_tests PRIVATE EPYCBENCH_TOPOLOGY_OWNER_TESTING)
    target_link_libraries(epycbench_results_topology_evidence_tests PRIVATE EpycBench::topology EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_topology_evidence_tests)
    epycbench_enable_warnings(epycbench_results_topology_evidence_tests)
    add_test(NAME EB-RES-002.topology-evidence COMMAND epycbench_results_topology_evidence_tests)
    add_test(NAME EB-RES-002.topology-evidence-nonactivation
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_topology_evidence_owner_nonactivation_test.py")

    add_executable(epycbench_results_validation_event_tests tests/unit/results_validation_event_owner_test.cpp)
    target_include_directories(epycbench_results_validation_event_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_validation_event_tests PRIVATE epycbench_results_validation_event EpycBench::monitoring epycbench_results_journal_records Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_validation_event_tests)
    epycbench_enable_warnings(epycbench_results_validation_event_tests)
    add_test(NAME EB-RES-002.validation-event COMMAND epycbench_results_validation_event_tests)
    add_test(NAME EB-RES-002.validation-event-nonactivation
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_validation_event_nonactivation_test.py")

    add_executable(epycbench_results_kernel_report_tests tests/unit/results_kernel_report_owner_test.cpp)
    target_include_directories(epycbench_results_kernel_report_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_kernel_report_tests PRIVATE epycbench_results_kernel_report EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_kernel_report_tests)
    epycbench_enable_warnings(epycbench_results_kernel_report_tests)
    add_test(NAME EB-RES-002.kernel-report COMMAND epycbench_results_kernel_report_tests)
    add_test(NAME EB-RES-002.kernel-report-nonactivation
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_kernel_report_nonactivation_test.py")

    add_executable(epycbench_results_kernel_approval_tests tests/unit/results_kernel_approval_owner_test.cpp)
    target_include_directories(epycbench_results_kernel_approval_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_kernel_approval_tests PRIVATE epycbench_results_kernel_approval epycbench_results_kernel_report EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_kernel_approval_tests)
    epycbench_enable_warnings(epycbench_results_kernel_approval_tests)
    add_test(NAME EB-RES-002.kernel-approval COMMAND epycbench_results_kernel_approval_tests)
    add_executable(epycbench_kernel_agent_admission_fixture tests/unit/kernel_agent_admission_fixture.cpp)
    target_include_directories(epycbench_kernel_agent_admission_fixture PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_kernel_agent_admission_fixture PRIVATE epycbench_results_kernel_approval EpycBench::core)
    epycbench_enable_build_options(epycbench_kernel_agent_admission_fixture)
    epycbench_enable_warnings(epycbench_kernel_agent_admission_fixture)
    add_test(NAME EB-RES-002.kernel-agent-supplied-files
        COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tests/tools/kernel_agent_admission_fixture_test.py" "$<TARGET_FILE:epycbench_kernel_agent_admission_fixture>")
    add_test(NAME EB-RES-002.kernel-approval-nonactivation
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_kernel_approval_nonactivation_test.py")

    add_executable(epycbench_results_build_manifest_tests tests/unit/results_build_manifest_owner_test.cpp)
    target_include_directories(epycbench_results_build_manifest_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_build_manifest_tests PRIVATE epycbench_results_build_manifest EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_build_manifest_tests)
    epycbench_enable_warnings(epycbench_results_build_manifest_tests)
    add_test(NAME EB-RES-002.build-manifest COMMAND epycbench_results_build_manifest_tests)
    add_test(NAME EB-RES-002.build-manifest-nonactivation
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_build_manifest_nonactivation_test.py")

    add_executable(epycbench_results_authored_source_tests tests/unit/results_authored_source_test.cpp)
    target_include_directories(epycbench_results_authored_source_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_authored_source_tests PRIVATE epycbench_results_authored_source EpycBench::scenario Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_authored_source_tests)
    epycbench_enable_warnings(epycbench_results_authored_source_tests)
    add_test(NAME EB-RES-002.authored-source COMMAND epycbench_results_authored_source_tests)
    add_test(NAME EB-RES-002.authored-source-nonactivation
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_authored_source_nonactivation_test.py")

    add_executable(epycbench_scenario_json_ownership_tests tests/unit/scenario_json_ownership_test.cpp)
    target_include_directories(epycbench_scenario_json_ownership_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_scenario_json_ownership_tests PRIVATE EpycBench::scenario Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_scenario_json_ownership_tests)
    epycbench_enable_warnings(epycbench_scenario_json_ownership_tests)
    add_test(NAME EB-SCN-011.json-ownership COMMAND epycbench_scenario_json_ownership_tests)
    set_tests_properties(EB-SCN-011.json-ownership PROPERTIES LABELS "scenario;non-hardware")
    add_test(NAME EB-SCN-011.json-ownership-nonactivation
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/scenario_json_ownership_nonactivation_test.py")
    set_tests_properties(EB-SCN-011.json-ownership-nonactivation PROPERTIES LABELS "scenario;non-hardware;audit")

    add_executable(epycbench_results_pairwise_analysis_tests
        tests/unit/results_pairwise_analysis_serialization_test.cpp
        src/results/pairwise_run_result.cpp src/results/prepared_pairwise_run_result.cpp)
    target_compile_definitions(epycbench_results_pairwise_analysis_tests PRIVATE EPYCBENCH_RESULTS_CALCULATION_TESTING)
    target_include_directories(epycbench_results_pairwise_analysis_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_pairwise_analysis_tests PRIVATE epycbench_results_pairwise_analysis
        EpycBench::finalized_evidence_wire_typed_reconstruction EpycBench::finalized_evidence_prepared_direct_codec Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_pairwise_analysis_tests)
    epycbench_enable_warnings(epycbench_results_pairwise_analysis_tests)
    add_test(NAME EB-RES-004.derived-analysis COMMAND epycbench_results_pairwise_analysis_tests)
    set_tests_properties(EB-RES-004.derived-analysis PROPERTIES LABELS "results;non-hardware")
    add_test(NAME EB-RES-004.derived-analysis-nonactivation
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_analysis_nonactivation_test.py")
    set_tests_properties(EB-RES-004.derived-analysis-nonactivation PROPERTIES LABELS "results;non-hardware;audit")

    # Fault controls exist only in this separately compiled, memory-only test executable.
    add_executable(epycbench_results_numerical_retention_tests
        tests/unit/results_numerical_retention_test.cpp
        src/results/pairwise_run_result.cpp src/results/prepared_pairwise_run_result.cpp)
    target_compile_definitions(epycbench_results_numerical_retention_tests PRIVATE
        EPYCBENCH_RESULTS_CALCULATION_TESTING)
    target_include_directories(epycbench_results_numerical_retention_tests PRIVATE
        "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_numerical_retention_tests PRIVATE
        EpycBench::runtime_production_backend EpycBench::statistics EpycBench::measurement
        EpycBench::finalized_evidence_prepared_direct_codec
        EpycBench::finalized_evidence_wire_typed_reconstruction Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_numerical_retention_tests)
    epycbench_enable_warnings(epycbench_results_numerical_retention_tests)
    add_test(NAME EB-RES-001.numerical-retention COMMAND epycbench_results_numerical_retention_tests)
    set_tests_properties(EB-RES-001.numerical-retention PROPERTIES LABELS "results;non-hardware")
    add_test(NAME EB-RES-001.numerical-retention-nonactivation
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_numerical_retention_nonactivation_test.py")
    set_tests_properties(EB-RES-001.numerical-retention-nonactivation PROPERTIES LABELS "results;non-hardware;audit")

    epycbench_add_runtime_ipc_test(
        epycbench_results_pairwise_run_result_tests
        tests/unit/results_pairwise_run_result_test.cpp
        EB-RES-001.typed-pairwise-result
        EpycBench::results
    )
    target_include_directories(
        epycbench_results_pairwise_run_result_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/tests/unit"
    )
    target_link_libraries(
        epycbench_results_pairwise_run_result_tests PRIVATE epycbench_runtime_test_support
    )
    epycbench_add_runtime_ipc_test(
        epycbench_runtime_pairwise_runner_execution_backend_tests
        tests/unit/runtime_pairwise_runner_execution_backend_test.cpp
        EB-RT-009.production-execution-backend
        EpycBench::results
    )
    target_include_directories(
        epycbench_runtime_pairwise_runner_execution_backend_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src"
                "${PROJECT_SOURCE_DIR}/tests/unit"
    )
    target_link_libraries(
        epycbench_runtime_pairwise_runner_execution_backend_tests
        PRIVATE epycbench_runtime_test_support
    )
    epycbench_add_runtime_ipc_test(
        epycbench_runtime_calibration_adapter_predecessor_tests
        tests/unit/runtime_calibration_adapter_predecessor_test.cpp
        EB-RT-009.calibration-adapter-predecessor
        EpycBench::results
    )
    target_include_directories(
        epycbench_runtime_calibration_adapter_predecessor_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src"
                "${PROJECT_SOURCE_DIR}/tests/unit")
    target_link_libraries(
        epycbench_runtime_calibration_adapter_predecessor_tests
        PRIVATE epycbench_runtime_test_support)
    add_test(
        NAME EB-RT-009.production-operations-adapter
        COMMAND epycbench_runtime_pairwise_production_composition_tests
    )
    epycbench_add_runtime_ipc_test(epycbench_runtime_runner_cancellation_tests tests/unit/runtime_runner_cancellation_test.cpp EB-RT-004.runner-cancellation EpycBench::runner_state_machine)
    target_include_directories(epycbench_runtime_runner_cancellation_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/tests/unit")
    target_link_libraries(epycbench_runtime_runner_cancellation_tests PRIVATE epycbench_runtime_test_support)
    epycbench_add_runtime_ipc_test(epycbench_runtime_runner_terminal_event_tests tests/unit/runtime_runner_terminal_event_test.cpp EB-RT-004.runner-terminal-events EpycBench::runner_state_machine)
    target_include_directories(epycbench_runtime_runner_terminal_event_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/tests/unit")
    target_link_libraries(epycbench_runtime_runner_terminal_event_tests PRIVATE epycbench_runtime_test_support)
    epycbench_add_runtime_ipc_test(
        epycbench_runtime_fake_runner_backend_tests
        tests/unit/runtime_fake_runner_backend_test.cpp
        EB-RT-007.fake-runner-backend
        epycbench_runtime_test_support
    )
    target_include_directories(
        epycbench_runtime_fake_runner_backend_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit"
    )
    epycbench_add_runtime_ipc_test(epycbench_runtime_ipc_failure_tests tests/unit/runtime_ipc_failure_projection_test.cpp EB-RT-001.failure-projection EpycBench::runtime_ipc)
    epycbench_add_runtime_ipc_test(
        epycbench_runtime_runner_control_reader_tests
        tests/unit/runtime_runner_control_reader_test.cpp
        EB-RT-006.stoppable-control-reader-prerequisite
        EpycBench::runtime
    )
    target_include_directories(
        epycbench_runtime_runner_control_reader_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        epycbench_runtime_runner_control_reader_tests PRIVATE EpycBench::platform_linux
    )

    add_executable(
        epycbench_runtime_service_control_monitor_tests
        tests/unit/runtime_service_control_monitor_test.cpp
    )
    target_include_directories(
        epycbench_runtime_service_control_monitor_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests/unit"
    )
    target_link_libraries(
        epycbench_runtime_service_control_monitor_tests
        PRIVATE EpycBench::service_control_monitor Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_runtime_service_control_monitor_tests)
    epycbench_enable_warnings(epycbench_runtime_service_control_monitor_tests)
    add_test(NAME EB-RT-006.service-assignment
             COMMAND epycbench_runtime_service_control_monitor_tests "[service-assignment]")
    add_test(NAME EB-RT-006.service-placement
             COMMAND epycbench_runtime_service_control_monitor_tests "[service-placement]")
    add_test(NAME EB-RT-006.control-monitor
             COMMAND epycbench_runtime_service_control_monitor_tests "[control-monitor]")
    add_test(NAME EB-RT-006.service-cleanup
             COMMAND epycbench_runtime_service_control_monitor_tests "[service-cleanup]")

    add_executable(
        epycbench_runtime_control_monitor_component_tests
        tests/component/runtime_control_monitor_component_test.cpp
    )
    target_include_directories(
        epycbench_runtime_control_monitor_component_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests/unit"
    )
    target_link_libraries(
        epycbench_runtime_control_monitor_component_tests
        PRIVATE EpycBench::service_control_monitor Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_runtime_control_monitor_component_tests)
    epycbench_enable_warnings(epycbench_runtime_control_monitor_component_tests)
    add_test(NAME EB-RT-006.control-monitor-component
             COMMAND epycbench_runtime_control_monitor_component_tests)

    function(epycbench_add_worker_team_test target source test_name)
        add_executable(${target} ${source})
        target_include_directories(
            ${target} PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src"
        )
        target_link_libraries(
            ${target} PRIVATE EpycBench::persistent_worker_team Catch2::Catch2WithMain
        )
        epycbench_enable_build_options(${target})
        epycbench_enable_warnings(${target})
        add_test(NAME ${test_name} COMMAND ${target})
    endfunction()
    epycbench_add_worker_team_test(
        epycbench_runtime_worker_team_lifecycle_tests
        tests/unit/runtime_persistent_worker_team_test.cpp
        EB-RT-005.worker-team-lifecycle
    )
    epycbench_add_worker_team_test(
        epycbench_runtime_worker_placement_tests
        tests/unit/runtime_worker_placement_test.cpp
        EB-RT-005.worker-placement
    )
    epycbench_add_worker_team_test(
        epycbench_runtime_worker_command_tests
        tests/unit/runtime_worker_command_test.cpp
        EB-RT-005.worker-commands
    )
    epycbench_add_worker_team_test(
        epycbench_runtime_worker_team_cleanup_tests
        tests/unit/runtime_worker_team_cleanup_test.cpp
        EB-RT-005.worker-team-cleanup
    )
    epycbench_add_worker_team_test(
        epycbench_runtime_worker_team_component_tests
        tests/component/runtime_persistent_worker_team_component_test.cpp
        EB-RT-005.worker-team-component
    )
    epycbench_add_worker_team_test(
        epycbench_runtime_staged_worker_generation_component_tests
        tests/component/runtime_staged_worker_generation_component_test.cpp
        RT-prerequisite.staged-worker-generation
    )
    target_link_libraries(epycbench_runtime_staged_worker_generation_component_tests
        PRIVATE EpycBench::runtime_production_backend)

    add_executable(
        epycbench_runtime_runner_internal_mode_tests
        tests/unit/runtime_runner_internal_mode_test.cpp
    )
    target_include_directories(
        epycbench_runtime_runner_internal_mode_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        epycbench_runtime_runner_internal_mode_tests
        PRIVATE EpycBench::runtime EpycBench::platform_linux Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_runtime_runner_internal_mode_tests)
    epycbench_enable_warnings(epycbench_runtime_runner_internal_mode_tests)
    add_test(
        NAME EB-RT-002.runner-internal-mode
        COMMAND epycbench_runtime_runner_internal_mode_tests
    )
    add_test(
        NAME EB-RT-008.runner-session-control-capability-prerequisite
        COMMAND epycbench_runtime_runner_internal_mode_tests
                "[runner-session-control-capability]"
    )

    function(epycbench_add_runtime_process_fixture target disposition test_name)
        add_executable(
            ${target}
            tests/integration/runtime_process_fixture_main.cpp
            tests/integration/runtime_process_integration_test.cpp
        )
        target_compile_definitions(${target} PRIVATE ${disposition})
        target_include_directories(
            ${target} PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src"
        )
        target_link_libraries(
            ${target}
            PRIVATE
                epycbench_runtime_test_support
                EpycBench::service_control_monitor
                EpycBench::runtime_ipc_payloads
                EpycBench::platform_linux
                Threads::Threads
                Catch2::Catch2
        )
        epycbench_enable_build_options(${target})
        epycbench_enable_warnings(${target})
        add_test(
            NAME ${test_name}
            COMMAND ${target} --fixture-executable $<TARGET_FILE:${target}>
        )
        set_tests_properties(
            ${test_name}
            PROPERTIES LABELS "live;process;ipc;integration;sandbox-sensitive"
        )
    endfunction()

    epycbench_add_runtime_process_fixture(
        epycbench_runtime_process_fixture_success
        EPYCBENCH_RUNTIME_PROCESS_FIXTURE_SUCCESS
        EB-RT-008.runtime-process-success
    )
    epycbench_add_runtime_process_fixture(
        epycbench_runtime_process_fixture_request_failure
        EPYCBENCH_RUNTIME_PROCESS_FIXTURE_REQUEST_FAILURE
        EB-RT-008.runtime-process-request-failure
    )

    add_executable(
        epycbench_runtime_platform_ipc_adapter_tests
        tests/unit/runtime_platform_ipc_adapters_test.cpp
    )
    target_include_directories(
        epycbench_runtime_platform_ipc_adapter_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        epycbench_runtime_platform_ipc_adapter_tests
        PRIVATE EpycBench::runtime EpycBench::platform_linux Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_runtime_platform_ipc_adapter_tests)
    epycbench_enable_warnings(epycbench_runtime_platform_ipc_adapter_tests)
    add_test(
        NAME EB-RT-002.platform-ipc-adapters
        COMMAND epycbench_runtime_platform_ipc_adapter_tests
    )

    function(epycbench_add_controller_test target source test_name)
        add_executable(${target} ${source})
        target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(
            ${target}
            PRIVATE EpycBench::runtime_ipc_payloads EpycBench::platform_linux
                    Catch2::Catch2WithMain
        )
        epycbench_enable_build_options(${target})
        epycbench_enable_warnings(${target})
        add_test(NAME ${test_name} COMMAND ${target})
    endfunction()
    epycbench_add_controller_test(
        epycbench_runtime_controller_lifecycle_policy_tests
        tests/unit/runtime_controller_lifecycle_policy_test.cpp
        P1.controller-lifecycle-policy
    )
    set_tests_properties(P1.controller-lifecycle-policy PROPERTIES TIMEOUT 300 LABELS "non-hardware")
    epycbench_add_controller_test(
        epycbench_runtime_controller_lifecycle_tests
        tests/unit/runtime_controller_lifecycle_test.cpp
        EB-RT-003.controller-lifecycle
    )
    epycbench_add_controller_test(
        epycbench_runtime_controller_deadline_tests
        tests/unit/runtime_controller_deadline_test.cpp
        EB-RT-003.controller-deadlines
    )
    epycbench_add_controller_test(
        epycbench_runtime_controller_cleanup_tests
        tests/unit/runtime_controller_cleanup_test.cpp
        EB-RT-003.controller-cleanup
    )
    set(EPYCBENCH_FEATURE_TESTING true)
endif()
if(EPYCBENCH_ENABLE_ASAN_UBSAN)
    set(EPYCBENCH_FEATURE_ASAN_UBSAN true)
endif()
if(EPYCBENCH_ENABLE_TSAN)
    set(EPYCBENCH_FEATURE_TSAN true)
endif()
if(EPYCBENCH_NATIVE_REFERENCE)
    set(EPYCBENCH_FEATURE_NATIVE_REFERENCE true)
endif()

set(
    EPYCBENCH_BUILD_INFO_HEADER
    "${PROJECT_BINARY_DIR}/generated/include/epycbench/build_info_generated.hpp"
)
configure_file(
    cmake/GenerateBuildInfo.cmake.in
    "${PROJECT_BINARY_DIR}/GenerateBuildInfo.cmake"
    @ONLY
)
add_custom_target(
    epycbench_build_info
    ALL
    COMMAND "${CMAKE_COMMAND}" -P "${PROJECT_BINARY_DIR}/GenerateBuildInfo.cmake"
    BYPRODUCTS "${EPYCBENCH_BUILD_INFO_HEADER}"
    COMMENT "Refreshing EpycBench build metadata"
    VERBATIM
)

add_library(epycbench_build_manifest STATIC src/build_manifest.cpp)
add_library(EpycBench::build_manifest ALIAS epycbench_build_manifest)
add_dependencies(epycbench_build_manifest epycbench_build_info)
target_compile_features(epycbench_build_manifest PUBLIC cxx_std_23)
target_include_directories(
    epycbench_build_manifest
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE
        "${PROJECT_BINARY_DIR}/generated/include"
)
target_link_libraries(epycbench_build_manifest PUBLIC nlohmann_json::nlohmann_json)
epycbench_enable_build_options(epycbench_build_manifest)
epycbench_enable_warnings(epycbench_build_manifest)

add_library(epycbench_topology_cli STATIC src/topology/topology_cli.cpp)
target_compile_features(epycbench_topology_cli PUBLIC cxx_std_23)
target_include_directories(epycbench_topology_cli PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(
    epycbench_topology_cli
    PUBLIC EpycBench::topology
    PRIVATE CLI11::CLI11
)
epycbench_enable_build_options(epycbench_topology_cli)
epycbench_enable_warnings(epycbench_topology_cli)

add_library(
    epycbench_cli
    STATIC
        src/cli/application.cpp
        src/cli/results_command.cpp
        src/cli/benchmark_run_command.cpp
        src/cli/monitoring_q0b_command.cpp
        src/cli/monitoring_q1_command.cpp
        src/cli/monitoring_q2_command.cpp
        src/cli/monitoring_q3_command.cpp
        src/cli/monitoring_q4_command.cpp
        src/cli/scenario_command.cpp
)
target_compile_features(epycbench_cli PUBLIC cxx_std_23)
set(EPYCBENCH_KERNEL_EVIDENCE_ROOT "" CACHE STRING
    "Reviewed Kernel evidence locator; empty leaves acquisition unconfigured")
set(EPYCBENCH_KERNEL_EVIDENCE_BANDWIDTH_SHA256
    "a3b7a9933a70d65a3d5ca95ab3c7db2c69f4be04262e03717027db378cab7725" CACHE STRING
    "Exact reviewed bandwidth Kernel approval manifest SHA-256")
set(EPYCBENCH_KERNEL_EVIDENCE_LATENCY_SHA256
    "865b713a6f54ba297af1871b3f814b7bd3fde89c78df8aa5090619b2efce0729" CACHE STRING
    "Exact reviewed latency Kernel approval manifest SHA-256")
set(EPYCBENCH_KERNEL_EVIDENCE_REVIEW_ROUTE "independent_agents" CACHE STRING
    "Kernel manifest admission route: independent_agents or human_approval")
set_property(CACHE EPYCBENCH_KERNEL_EVIDENCE_REVIEW_ROUTE PROPERTY STRINGS
    independent_agents human_approval)
# This is a code locator only. Configuration performs no evidence reads or copies.
# Reject compiler-definition delimiters instead of interpreting them as code.
if(EPYCBENCH_KERNEL_EVIDENCE_ROOT MATCHES "[;\"\\\\\n\r]")
    message(FATAL_ERROR "Kernel evidence locator contains an unsupported configuration delimiter")
endif()
foreach(kernel_evidence_digest IN ITEMS
        EPYCBENCH_KERNEL_EVIDENCE_BANDWIDTH_SHA256
        EPYCBENCH_KERNEL_EVIDENCE_LATENCY_SHA256)
    string(LENGTH "${${kernel_evidence_digest}}" kernel_evidence_digest_length)
    if(NOT kernel_evidence_digest_length EQUAL 64 OR
       NOT "${${kernel_evidence_digest}}" MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "${kernel_evidence_digest} must be exactly 64 lowercase hexadecimal digits")
    endif()
endforeach()
if(NOT EPYCBENCH_KERNEL_EVIDENCE_REVIEW_ROUTE STREQUAL "independent_agents" AND
   NOT EPYCBENCH_KERNEL_EVIDENCE_REVIEW_ROUTE STREQUAL "human_approval")
    message(FATAL_ERROR "Kernel evidence review route must be independent_agents or human_approval")
endif()
target_compile_definitions(epycbench_cli PRIVATE
    "EPYCBENCH_KERNEL_EVIDENCE_ROOT=\"${EPYCBENCH_KERNEL_EVIDENCE_ROOT}\""
    "EPYCBENCH_KERNEL_EVIDENCE_BANDWIDTH_SHA256=\"${EPYCBENCH_KERNEL_EVIDENCE_BANDWIDTH_SHA256}\""
    "EPYCBENCH_KERNEL_EVIDENCE_LATENCY_SHA256=\"${EPYCBENCH_KERNEL_EVIDENCE_LATENCY_SHA256}\""
    "EPYCBENCH_KERNEL_EVIDENCE_REVIEW_ROUTE=\"${EPYCBENCH_KERNEL_EVIDENCE_REVIEW_ROUTE}\"")
target_include_directories(epycbench_cli PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_include_directories(epycbench_cli PRIVATE "${PROJECT_BINARY_DIR}/generated/include")
add_dependencies(epycbench_cli epycbench_build_info)
target_link_libraries(
    epycbench_cli
    PUBLIC epycbench_topology_cli
    PRIVATE EpycBench::scenario epycbench_monitoring_qualification epycbench_results_final_document
            epycbench_results_producer_progress epycbench_benchmark_run_composition
            EpycBench::runtime_production_backend CLI11::CLI11 nlohmann_json::nlohmann_json
)
epycbench_enable_build_options(epycbench_cli)
epycbench_enable_warnings(epycbench_cli)

set(EPYCBENCH_STAGED_RESOURCE_ROOT "${PROJECT_BINARY_DIR}/resources")
option(EPYCBENCH_STAGE_REFERENCE_FIXTURE
    "Stage the development-only reference topology fixture" ${BUILD_TESTING})
if(EPYCBENCH_STAGE_REFERENCE_FIXTURE)
    file(MAKE_DIRECTORY "${EPYCBENCH_STAGED_RESOURCE_ROOT}/fixtures/topology")
    file(
        COPY "${PROJECT_SOURCE_DIR}/fixtures/topology/reference-epyc"
        DESTINATION "${EPYCBENCH_STAGED_RESOURCE_ROOT}/fixtures/topology"
    )
endif()

add_executable(epycbench src/main.cpp)
target_compile_features(epycbench PRIVATE cxx_std_23)
target_include_directories(epycbench PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench PRIVATE EpycBench::build_manifest epycbench_cli
    EpycBench::runtime_production_backend epycbench_pairwise_v2_production_entry)
epycbench_enable_build_options(epycbench)
epycbench_enable_warnings(epycbench)

if(BUILD_TESTING)
    find_program(EPYCBENCH_BASH_EXECUTABLE NAMES bash REQUIRED)
    find_program(EPYCBENCH_SHA256SUM_EXECUTABLE NAMES sha256sum REQUIRED)

    add_test(NAME epycbench.smoke COMMAND epycbench --help)

    add_executable(epycbench_platform_linux_authenticated_monitoring_capacity_authority_tests
        tests/unit/platform_linux_authenticated_monitoring_capacity_authority_test.cpp)
    target_include_directories(
        epycbench_platform_linux_authenticated_monitoring_capacity_authority_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        epycbench_platform_linux_authenticated_monitoring_capacity_authority_tests
        PRIVATE EpycBench::monitoring_host_capacity_authority Catch2::Catch2WithMain)
    epycbench_enable_build_options(
        epycbench_platform_linux_authenticated_monitoring_capacity_authority_tests)
    epycbench_enable_warnings(
        epycbench_platform_linux_authenticated_monitoring_capacity_authority_tests)
    add_test(NAME RT-prerequisite.authenticated-monitoring-host-capacity-authority
        COMMAND epycbench_platform_linux_authenticated_monitoring_capacity_authority_tests)

    add_executable(epycbench_statistics_descriptive_tests
                   tests/unit/statistics_descriptive_statistics_test.cpp)
    target_link_libraries(epycbench_statistics_descriptive_tests
                          PRIVATE EpycBench::statistics Catch2::Catch2WithMain)
    target_include_directories(epycbench_statistics_descriptive_tests
                               PRIVATE "${PROJECT_SOURCE_DIR}/src")
    epycbench_enable_build_options(epycbench_statistics_descriptive_tests)
    epycbench_enable_warnings(epycbench_statistics_descriptive_tests)
    add_test(NAME EB-STAT-001.descriptive-statistics
             COMMAND epycbench_statistics_descriptive_tests)

    add_executable(epycbench_statistics_outlier_tests
                   tests/unit/statistics_outlier_flagging_test.cpp)
    target_link_libraries(epycbench_statistics_outlier_tests
                          PRIVATE EpycBench::statistics Catch2::Catch2WithMain)
    target_include_directories(epycbench_statistics_outlier_tests
                               PRIVATE "${PROJECT_SOURCE_DIR}/src")
    epycbench_enable_build_options(epycbench_statistics_outlier_tests)
    epycbench_enable_warnings(epycbench_statistics_outlier_tests)
    add_test(NAME EB-STAT-002.outlier-flagging COMMAND epycbench_statistics_outlier_tests)

    add_executable(epycbench_statistics_block_representative_tests
                   tests/unit/statistics_block_representatives_test.cpp)
    target_link_libraries(epycbench_statistics_block_representative_tests
                          PRIVATE EpycBench::statistics Catch2::Catch2WithMain)
    target_include_directories(epycbench_statistics_block_representative_tests
                               PRIVATE "${PROJECT_SOURCE_DIR}/src")
    epycbench_enable_build_options(epycbench_statistics_block_representative_tests)
    epycbench_enable_warnings(epycbench_statistics_block_representative_tests)
    add_test(NAME EB-STAT-003.block-representatives
             COMMAND epycbench_statistics_block_representative_tests)

    add_executable(epycbench_statistics_log_ratio_tests
                   tests/unit/statistics_log_ratio_comparison_test.cpp)
    target_link_libraries(epycbench_statistics_log_ratio_tests
                          PRIVATE EpycBench::statistics Catch2::Catch2WithMain)
    target_include_directories(epycbench_statistics_log_ratio_tests
                               PRIVATE "${PROJECT_SOURCE_DIR}/src")
    epycbench_enable_build_options(epycbench_statistics_log_ratio_tests)
    epycbench_enable_warnings(epycbench_statistics_log_ratio_tests)
    add_test(NAME EB-STAT-004.log-ratio-comparison
             COMMAND epycbench_statistics_log_ratio_tests)

    add_executable(epycbench_statistics_block_bootstrap_tests
                   tests/unit/statistics_block_bootstrap_test.cpp)
    target_link_libraries(epycbench_statistics_block_bootstrap_tests
                          PRIVATE EpycBench::statistics Catch2::Catch2WithMain)
    target_include_directories(epycbench_statistics_block_bootstrap_tests
                               PRIVATE "${PROJECT_SOURCE_DIR}/src")
    epycbench_enable_build_options(epycbench_statistics_block_bootstrap_tests)
    epycbench_enable_warnings(epycbench_statistics_block_bootstrap_tests)
    add_test(NAME EB-STAT-005.block-bootstrap COMMAND epycbench_statistics_block_bootstrap_tests)

    add_executable(epycbench_statistics_conclusion_classifier_tests
                   tests/unit/statistics_conclusion_classifier_test.cpp)
    target_link_libraries(epycbench_statistics_conclusion_classifier_tests
                          PRIVATE EpycBench::statistics Catch2::Catch2WithMain)
    target_include_directories(epycbench_statistics_conclusion_classifier_tests
                               PRIVATE "${PROJECT_SOURCE_DIR}/src")
    epycbench_enable_build_options(epycbench_statistics_conclusion_classifier_tests)
    epycbench_enable_warnings(epycbench_statistics_conclusion_classifier_tests)
    add_test(NAME EB-STAT-006.conclusion-classifier
             COMMAND epycbench_statistics_conclusion_classifier_tests)

    add_executable(epycbench_statistics_adaptive_stopping_tests
                   tests/unit/statistics_adaptive_stopping_test.cpp)
    target_link_libraries(epycbench_statistics_adaptive_stopping_tests
                          PRIVATE EpycBench::statistics Catch2::Catch2WithMain)
    target_include_directories(epycbench_statistics_adaptive_stopping_tests
                               PRIVATE "${PROJECT_SOURCE_DIR}/src")
    epycbench_enable_build_options(epycbench_statistics_adaptive_stopping_tests)
    epycbench_enable_warnings(epycbench_statistics_adaptive_stopping_tests)
    add_test(NAME EB-STAT-007.adaptive-stopping
             COMMAND epycbench_statistics_adaptive_stopping_tests)

    add_executable(epycbench_statistics_golden_dataset_tests
                   tests/unit/statistics_golden_datasets_test.cpp)
    target_link_libraries(epycbench_statistics_golden_dataset_tests
                          PRIVATE EpycBench::statistics Catch2::Catch2WithMain
                                  nlohmann_json::nlohmann_json)
    target_include_directories(epycbench_statistics_golden_dataset_tests
                               PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_statistics_golden_dataset_tests
                               PRIVATE EPYCBENCH_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_statistics_golden_dataset_tests)
    epycbench_enable_warnings(epycbench_statistics_golden_dataset_tests)
    add_test(NAME EB-STAT-008.golden-datasets
             COMMAND epycbench_statistics_golden_dataset_tests)

    add_test(
        NAME EB-STAT-009.independent-oracle
        COMMAND
            "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/oracle/statistics_oracle.py"
            --fixture-root "${PROJECT_SOURCE_DIR}/fixtures/statistics/v0.1-golden"
    )
    set_tests_properties(
        EB-STAT-009.independent-oracle
        PROPERTIES LABELS "statistics;oracle;qualification;non-hardware"
    )

    add_executable(
        epycbench_topology_cli_tests
        tests/integration/topology_cli_test.cpp
    )
    target_include_directories(epycbench_topology_cli_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        epycbench_topology_cli_tests
        PRIVATE
            epycbench_topology_cli
            Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_topology_cli_tests)
    epycbench_enable_warnings(epycbench_topology_cli_tests)
    add_test(NAME EB-TOP-008.topology-cli COMMAND epycbench_topology_cli_tests)

    add_executable(epycbench_topology_checked_acquisition_tests tests/unit/topology_checked_topology_test.cpp)
    target_include_directories(epycbench_topology_checked_acquisition_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_topology_checked_acquisition_tests PRIVATE EpycBench::topology Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_topology_checked_acquisition_tests)
    epycbench_enable_warnings(epycbench_topology_checked_acquisition_tests)
    add_test(NAME EB-TOP.checked-topology-acquisition COMMAND epycbench_topology_checked_acquisition_tests)

    add_test(
        NAME EB-TOP-008.topology-cli-exit-gate
        COMMAND
            "${CMAKE_COMMAND}"
            "-DEPYCBENCH_EXECUTABLE=$<TARGET_FILE:epycbench>"
            "-DEPYCBENCH_TEST_ROOT=${PROJECT_BINARY_DIR}/topology-cli-exit-gate"
            -P
            "${PROJECT_SOURCE_DIR}/tests/cmake/TopologyCliExitGate.cmake"
    )

    add_executable(epycbench_core_id_tests tests/unit/core_id_test.cpp)
    target_link_libraries(
        epycbench_core_id_tests
        PRIVATE
            EpycBench::core
            Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_core_id_tests)
    epycbench_enable_warnings(epycbench_core_id_tests)

    add_test(NAME EB-CORE-001.strong-identifiers COMMAND epycbench_core_id_tests)

    add_executable(epycbench_core_id_set_tests tests/unit/core_id_set_test.cpp)
    target_link_libraries(
        epycbench_core_id_set_tests
        PRIVATE
            EpycBench::core
            Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_core_id_set_tests)
    epycbench_enable_warnings(epycbench_core_id_set_tests)

    add_test(NAME EB-CORE-002.cpu-node-sets COMMAND epycbench_core_id_set_tests)

    add_executable(epycbench_core_quantity_tests tests/unit/core_quantity_test.cpp)
    target_link_libraries(
        epycbench_core_quantity_tests
        PRIVATE
            EpycBench::core
            Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_core_quantity_tests)
    epycbench_enable_warnings(epycbench_core_quantity_tests)

    add_test(NAME EB-CORE-003.checked-quantities COMMAND epycbench_core_quantity_tests)

    add_executable(epycbench_core_error_tests tests/unit/core_error_test.cpp)
    target_link_libraries(
        epycbench_core_error_tests
        PRIVATE
            EpycBench::core
            Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_core_error_tests)
    epycbench_enable_warnings(epycbench_core_error_tests)

    add_test(NAME EB-CORE-004.structured-errors COMMAND epycbench_core_error_tests)

    add_executable(
        epycbench_core_canonical_json_hash_tests
        tests/unit/core_canonical_json_hash_test.cpp
    )
    target_link_libraries(
        epycbench_core_canonical_json_hash_tests
        PRIVATE
            EpycBench::core
            Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_core_canonical_json_hash_tests)
    epycbench_enable_warnings(epycbench_core_canonical_json_hash_tests)

    add_test(
        NAME EB-CORE-005.canonical-json-hashing
        COMMAND epycbench_core_canonical_json_hash_tests
    )

    add_executable(epycbench_core_jcs_json_tests tests/unit/core_jcs_json_test.cpp)
    target_include_directories(epycbench_core_jcs_json_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_core_jcs_json_tests PRIVATE EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_core_jcs_json_tests)
    epycbench_enable_warnings(epycbench_core_jcs_json_tests)
    add_test(NAME EB-CORE-005.additive-jcs-json COMMAND epycbench_core_jcs_json_tests)
    set_tests_properties(EB-CORE-005.additive-jcs-json PROPERTIES LABELS "core;non-hardware")
    add_test(NAME EB-CORE-005.jcs-json-nonactivation
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/jcs_json_nonactivation_test.py")
    set_tests_properties(EB-CORE-005.jcs-json-nonactivation PROPERTIES LABELS "core;non-hardware;audit")

    add_executable(epycbench_core_seed_tests tests/unit/core_seed_test.cpp)
    target_link_libraries(
        epycbench_core_seed_tests
        PRIVATE
            EpycBench::core
            Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_core_seed_tests)
    epycbench_enable_warnings(epycbench_core_seed_tests)
    add_test(NAME EB-CORE-006.seed-derivation COMMAND epycbench_core_seed_tests)

    add_executable(
        epycbench_core_governed_repository_file_tests
        tests/unit/core_governed_repository_file_test.cpp
    )
    target_include_directories(
        epycbench_core_governed_repository_file_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/src/core
    )
    target_link_libraries(
        epycbench_core_governed_repository_file_tests
        PRIVATE EpycBench::core Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_core_governed_repository_file_tests)
    epycbench_enable_warnings(epycbench_core_governed_repository_file_tests)
    add_test(
        NAME EB-CORE.repository-governed-file-read
        COMMAND epycbench_core_governed_repository_file_tests
    )

    add_executable(
        epycbench_memory_region_request_tests
        tests/unit/memory_region_request_test.cpp
    )
    target_link_libraries(
        epycbench_memory_region_request_tests
        PRIVATE EpycBench::memory Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_memory_region_request_tests)
    epycbench_enable_warnings(epycbench_memory_region_request_tests)
    add_test(
        NAME EB-MEM-001.region-request
        COMMAND epycbench_memory_region_request_tests
    )

    add_executable(
        epycbench_memory_region_request_validation_tests
        tests/unit/memory_region_request_validation_test.cpp
    )
    target_link_libraries(
        epycbench_memory_region_request_validation_tests
        PRIVATE EpycBench::memory Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_memory_region_request_validation_tests)
    epycbench_enable_warnings(epycbench_memory_region_request_validation_tests)
    add_test(
        NAME EB-MEM-001.region-request-validation
        COMMAND epycbench_memory_region_request_validation_tests
    )

    foreach(mapping_test IN ITEMS region_mapping region_mapping_validation region_mapping_ownership)
        add_executable(
            epycbench_memory_${mapping_test}_tests
            tests/unit/memory_${mapping_test}_test.cpp
        )
        target_include_directories(
            epycbench_memory_${mapping_test}_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
        )
        target_link_libraries(
            epycbench_memory_${mapping_test}_tests
            PRIVATE EpycBench::memory Catch2::Catch2WithMain
        )
        epycbench_enable_build_options(epycbench_memory_${mapping_test}_tests)
        epycbench_enable_warnings(epycbench_memory_${mapping_test}_tests)
    endforeach()
    add_test(
        NAME EB-MEM-002.region-mapping
        COMMAND epycbench_memory_region_mapping_tests
    )
    add_test(
        NAME EB-MEM-002.region-mapping-validation
        COMMAND epycbench_memory_region_mapping_validation_tests
    )
    add_test(
        NAME EB-MEM-002.region-mapping-ownership
        COMMAND epycbench_memory_region_mapping_ownership_tests
    )

    add_executable(
        epycbench_memory_region_binding_tests
        tests/unit/memory_region_binding_test.cpp
    )
    target_include_directories(
        epycbench_memory_region_binding_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        epycbench_memory_region_binding_tests
        PRIVATE EpycBench::memory Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_memory_region_binding_tests)
    epycbench_enable_warnings(epycbench_memory_region_binding_tests)
    add_test(
        NAME EB-MEM-003.region-binding
        COMMAND epycbench_memory_region_binding_tests "[region-binding]"
    )
    add_test(
        NAME EB-MEM-003.region-binding-validation
        COMMAND epycbench_memory_region_binding_tests "[region-binding-validation]"
    )
    add_test(
        NAME EB-MEM-003.region-binding-ownership
        COMMAND epycbench_memory_region_binding_tests "[region-binding-ownership]"
    )

    add_executable(
        epycbench_memory_region_initialization_tests
        tests/unit/memory_region_initialization_test.cpp
    )
    target_include_directories(
        epycbench_memory_region_initialization_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        epycbench_memory_region_initialization_tests
        PRIVATE EpycBench::memory Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_memory_region_initialization_tests)
    epycbench_enable_warnings(epycbench_memory_region_initialization_tests)
    add_test(
        NAME EB-MEM-004.region-initialization
        COMMAND epycbench_memory_region_initialization_tests "[region-initialization]"
    )
    add_test(
        NAME EB-MEM-004.region-initialization-validation
        COMMAND epycbench_memory_region_initialization_tests
                "[region-initialization-validation]"
    )
    add_test(
        NAME EB-MEM-004.region-initialization-ownership
        COMMAND epycbench_memory_region_initialization_tests
                "[region-initialization-ownership]"
    )

    add_executable(
        epycbench_memory_region_placement_query_tests
        tests/unit/memory_region_placement_query_test.cpp
    )
    target_include_directories(
        epycbench_memory_region_placement_query_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        epycbench_memory_region_placement_query_tests
        PRIVATE EpycBench::memory Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_memory_region_placement_query_tests)
    epycbench_enable_warnings(epycbench_memory_region_placement_query_tests)
    add_test(
        NAME EB-MEM-005.region-placement-query
        COMMAND epycbench_memory_region_placement_query_tests "[region-placement-query]"
    )
    add_test(
        NAME EB-MEM-005.region-placement-query-validation
        COMMAND epycbench_memory_region_placement_query_tests
                "[region-placement-query-validation]"
    )
    add_test(
        NAME EB-MEM-005.region-placement-query-ownership
        COMMAND epycbench_memory_region_placement_query_tests
                "[region-placement-query-ownership]"
    )

    add_executable(
        epycbench_memory_region_placement_evaluation_tests
        tests/unit/memory_region_placement_evaluation_test.cpp
    )
    target_include_directories(
        epycbench_memory_region_placement_evaluation_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        epycbench_memory_region_placement_evaluation_tests
        PRIVATE EpycBench::memory Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_memory_region_placement_evaluation_tests)
    epycbench_enable_warnings(epycbench_memory_region_placement_evaluation_tests)
    add_test(
        NAME EB-MEM-006.region-placement-evaluation
        COMMAND epycbench_memory_region_placement_evaluation_tests
                "[region-placement-evaluation]"
    )
    add_test(
        NAME EB-MEM-006.region-placement-evaluation-validation
        COMMAND epycbench_memory_region_placement_evaluation_tests
                "[region-placement-evaluation-validation]"
    )
    add_test(
        NAME EB-MEM-006.region-placement-evaluation-ownership
        COMMAND epycbench_memory_region_placement_evaluation_tests
                "[region-placement-evaluation-ownership]"
    )

    add_executable(
        epycbench_memory_region_protection_tests
        tests/unit/memory_region_protection_test.cpp
    )
    target_include_directories(
        epycbench_memory_region_protection_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        epycbench_memory_region_protection_tests
        PRIVATE EpycBench::memory Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_memory_region_protection_tests)
    epycbench_enable_warnings(epycbench_memory_region_protection_tests)
    add_test(
        NAME EB-MEM-007.region-read-only
        COMMAND epycbench_memory_region_protection_tests "[region-read-only]"
    )
    add_test(
        NAME EB-MEM-007.region-read-only-validation
        COMMAND epycbench_memory_region_protection_tests
                "[region-read-only-validation]"
    )
    add_test(
        NAME EB-MEM-007.region-read-only-ownership
        COMMAND epycbench_memory_region_protection_tests
                "[region-read-only-ownership]"
    )

    add_executable(
        epycbench_memory_region_finalization_tests
        tests/unit/memory_region_finalization_test.cpp
        tests/unit/memory_region_finalization_validation_test.cpp
        tests/unit/memory_region_finalization_ownership_test.cpp
    )
    target_include_directories(
        epycbench_memory_region_finalization_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        epycbench_memory_region_finalization_tests
        PRIVATE EpycBench::memory Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_memory_region_finalization_tests)
    epycbench_enable_warnings(epycbench_memory_region_finalization_tests)
    add_test(
        NAME EB-MEM-008.region-finalization
        COMMAND epycbench_memory_region_finalization_tests "[region-finalization]"
    )
    add_test(
        NAME EB-MEM-008.region-finalization-validation
        COMMAND epycbench_memory_region_finalization_tests
                "[region-finalization-validation]"
    )
    add_test(
        NAME EB-MEM-008.region-finalization-ownership
        COMMAND epycbench_memory_region_finalization_tests
                "[region-finalization-ownership]"
    )

    add_executable(
        epycbench_memory_placement_diagnostic_tests
        tests/unit/memory_placement_diagnostic_test.cpp
        tests/unit/memory_placement_diagnostic_validation_test.cpp
        tests/unit/memory_placement_diagnostic_ownership_test.cpp
    )
    target_include_directories(
        epycbench_memory_placement_diagnostic_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        epycbench_memory_placement_diagnostic_tests
        PRIVATE EpycBench::memory Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_memory_placement_diagnostic_tests)
    epycbench_enable_warnings(epycbench_memory_placement_diagnostic_tests)
    add_test(
        NAME EB-MEM-009.placement-diagnostic
        COMMAND epycbench_memory_placement_diagnostic_tests "[placement-diagnostic]"
    )
    add_test(
        NAME EB-MEM-009.placement-diagnostic-validation
        COMMAND epycbench_memory_placement_diagnostic_tests
                "[placement-diagnostic-validation]"
    )
    add_test(
        NAME EB-MEM-009.placement-diagnostic-ownership
        COMMAND epycbench_memory_placement_diagnostic_tests
                "[placement-diagnostic-ownership]"
    )
    add_test(
        NAME Memory.region-finalization-diagnostic
        COMMAND epycbench_memory_placement_diagnostic_tests
                "[region-finalization-diagnostic]"
    )

    add_executable(
        epycbench_memory_retained_region_cleanup_tests
        tests/unit/memory_retained_region_cleanup_test.cpp
        tests/unit/memory_retained_region_cleanup_validation_test.cpp
        tests/unit/memory_retained_region_cleanup_ownership_test.cpp
    )
    target_include_directories(
        epycbench_memory_retained_region_cleanup_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests/unit"
    )
    target_link_libraries(
        epycbench_memory_retained_region_cleanup_tests
        PRIVATE EpycBench::memory Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_memory_retained_region_cleanup_tests)
    epycbench_enable_warnings(epycbench_memory_retained_region_cleanup_tests)
    add_test(
        NAME EB-MEM-010.retained-region-cleanup
        COMMAND epycbench_memory_retained_region_cleanup_tests
                "[retained-region-cleanup]"
    )
    add_test(
        NAME EB-MEM-010.retained-region-cleanup-validation
        COMMAND epycbench_memory_retained_region_cleanup_tests
                "[retained-region-cleanup-validation]"
    )
    add_test(
        NAME EB-MEM-010.retained-region-cleanup-ownership
        COMMAND epycbench_memory_retained_region_cleanup_tests
                "[retained-region-cleanup-ownership]"
    )

    add_executable(
        epycbench_platform_linux_dynamic_cpu_mask_tests
        tests/unit/platform_linux_dynamic_cpu_mask_test.cpp
    )
    target_include_directories(
        epycbench_platform_linux_dynamic_cpu_mask_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_definitions(
        epycbench_platform_linux_dynamic_cpu_mask_tests PRIVATE _GNU_SOURCE
    )
    target_link_libraries(
        epycbench_platform_linux_dynamic_cpu_mask_tests
        PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_platform_linux_dynamic_cpu_mask_tests)
    epycbench_enable_warnings(epycbench_platform_linux_dynamic_cpu_mask_tests)
    add_test(
        NAME EB-LNX-002.dynamic-affinity-mask
        COMMAND epycbench_platform_linux_dynamic_cpu_mask_tests
    )

    add_executable(
        epycbench_platform_linux_thread_identity_tests
        tests/unit/platform_linux_thread_identity_test.cpp
    )
    target_include_directories(
        epycbench_platform_linux_thread_identity_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_definitions(epycbench_platform_linux_thread_identity_tests PRIVATE _GNU_SOURCE)
    target_link_libraries(
        epycbench_platform_linux_thread_identity_tests
        PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_platform_linux_thread_identity_tests)
    epycbench_enable_warnings(epycbench_platform_linux_thread_identity_tests)
    add_test(
        NAME EB-LNX-003.thread-identity
        COMMAND epycbench_platform_linux_thread_identity_tests
    )

    add_executable(
        epycbench_platform_linux_raw_monotonic_clock_tests
        tests/unit/platform_linux_raw_monotonic_clock_test.cpp
    )
    target_include_directories(
        epycbench_platform_linux_raw_monotonic_clock_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        epycbench_platform_linux_raw_monotonic_clock_tests
        PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_platform_linux_raw_monotonic_clock_tests)
    epycbench_enable_warnings(epycbench_platform_linux_raw_monotonic_clock_tests)
    add_test(
        NAME EB-LNX-004.raw-monotonic-clock
        COMMAND epycbench_platform_linux_raw_monotonic_clock_tests [raw-monotonic-clock]
    )
    add_test(
        NAME EB-LNX-004.raw-monotonic-clock-validation
        COMMAND epycbench_platform_linux_raw_monotonic_clock_tests
                [raw-monotonic-clock-validation]
    )
    add_test(
        NAME EB-LNX-004.raw-monotonic-clock-ownership
        COMMAND epycbench_platform_linux_raw_monotonic_clock_tests
                [raw-monotonic-clock-ownership]
    )

    add_executable(
        epycbench_platform_linux_thread_rusage_tests
        tests/unit/platform_linux_thread_rusage_test.cpp
    )
    target_include_directories(
        epycbench_platform_linux_thread_rusage_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        epycbench_platform_linux_thread_rusage_tests
        PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_platform_linux_thread_rusage_tests)
    epycbench_enable_warnings(epycbench_platform_linux_thread_rusage_tests)
    add_test(
        NAME EB-MON-007.q1-thread-rusage-oracle
        COMMAND epycbench_platform_linux_thread_rusage_tests
    )

    add_executable(
        epycbench_platform_linux_thread_affinity_tests
        tests/unit/platform_linux_thread_affinity_test.cpp
    )
    target_include_directories(
        epycbench_platform_linux_thread_affinity_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_definitions(epycbench_platform_linux_thread_affinity_tests PRIVATE _GNU_SOURCE)
    target_link_libraries(
        epycbench_platform_linux_thread_affinity_tests
        PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_platform_linux_thread_affinity_tests)
    epycbench_enable_warnings(epycbench_platform_linux_thread_affinity_tests)
    add_test(
        NAME EB-LNX-003.thread-affinity
        COMMAND epycbench_platform_linux_thread_affinity_tests
    )

    add_executable(
        epycbench_platform_linux_memory_mapping_tests
        tests/unit/platform_linux_memory_mapping_test.cpp
    )
    target_include_directories(
        epycbench_platform_linux_memory_mapping_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_definitions(
        epycbench_platform_linux_memory_mapping_tests PRIVATE _GNU_SOURCE
    )
    target_link_libraries(
        epycbench_platform_linux_memory_mapping_tests
        PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_platform_linux_memory_mapping_tests)
    epycbench_enable_warnings(epycbench_platform_linux_memory_mapping_tests)
    add_test(
        NAME EB-LNX-006.memory-mapping-wrapper
        COMMAND epycbench_platform_linux_memory_mapping_tests
    )

    add_executable(
        epycbench_platform_linux_memory_mapping_component_tests
        tests/component/platform_linux_memory_mapping_component_test.cpp
    )
    target_compile_definitions(
        epycbench_platform_linux_memory_mapping_component_tests PRIVATE _GNU_SOURCE
    )
    target_link_libraries(
        epycbench_platform_linux_memory_mapping_component_tests
        PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_platform_linux_memory_mapping_component_tests)
    epycbench_enable_warnings(epycbench_platform_linux_memory_mapping_component_tests)
    add_test(
        NAME EB-LNX-006.memory-mapping-component
        COMMAND epycbench_platform_linux_memory_mapping_component_tests
    )

    foreach(numa_test IN ITEMS numa_mask numa_bind page_location)
        add_executable(
            epycbench_platform_linux_${numa_test}_tests
            tests/unit/platform_linux_${numa_test}_test.cpp
        )
        target_include_directories(
            epycbench_platform_linux_${numa_test}_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
        )
        target_link_libraries(
            epycbench_platform_linux_${numa_test}_tests
            PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain
        )
        epycbench_enable_build_options(epycbench_platform_linux_${numa_test}_tests)
        epycbench_enable_warnings(epycbench_platform_linux_${numa_test}_tests)
    endforeach()
    add_test(NAME EB-LNX-007.numa-mask COMMAND epycbench_platform_linux_numa_mask_tests)
    add_test(NAME EB-LNX-007.numa-bind COMMAND epycbench_platform_linux_numa_bind_tests)
    add_test(NAME EB-LNX-007.page-query COMMAND epycbench_platform_linux_page_location_tests)

    add_executable(
        epycbench_platform_linux_local_ipc_tests
        tests/unit/platform_linux_local_ipc_test.cpp
    )
    target_include_directories(
        epycbench_platform_linux_local_ipc_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_definitions(epycbench_platform_linux_local_ipc_tests PRIVATE _GNU_SOURCE)
    target_link_libraries(
        epycbench_platform_linux_local_ipc_tests
        PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_platform_linux_local_ipc_tests)
    epycbench_enable_warnings(epycbench_platform_linux_local_ipc_tests)
    add_test(NAME EB-LNX-005.local-ipc COMMAND epycbench_platform_linux_local_ipc_tests)
    add_test(
        NAME EB-RT-006.control-endpoint-read-shutdown-prerequisite
        COMMAND epycbench_platform_linux_local_ipc_tests
                "[control-endpoint-read-shutdown]"
    )

    add_executable(epycbench_controller_signal_scope_tests
        tests/unit/platform_linux_controller_signal_scope_test.cpp)
    target_include_directories(epycbench_controller_signal_scope_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_controller_signal_scope_tests PRIVATE _GNU_SOURCE)
    target_link_libraries(epycbench_controller_signal_scope_tests PRIVATE
        EpycBench::platform_linux EpycBench::runtime_ipc_payloads Catch2::Catch2WithMain Threads::Threads)
    epycbench_enable_build_options(epycbench_controller_signal_scope_tests)
    epycbench_enable_warnings(epycbench_controller_signal_scope_tests)
    add_test(NAME P1.controller-signal-scope COMMAND epycbench_controller_signal_scope_tests)
    set_tests_properties(P1.controller-signal-scope PROPERTIES TIMEOUT 300 LABELS "non-hardware")

    add_executable(epycbench_controller_signal_scope_native
        tests/component/controller_signal_scope_native.cpp)
    target_include_directories(epycbench_controller_signal_scope_native PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_controller_signal_scope_native PRIVATE _GNU_SOURCE)
    target_link_libraries(epycbench_controller_signal_scope_native PRIVATE EpycBench::platform_linux Threads::Threads)
    epycbench_enable_build_options(epycbench_controller_signal_scope_native)
    epycbench_enable_warnings(epycbench_controller_signal_scope_native)
    # Native signal execution is opt-in after independent source/plan acceptance.
    add_test(NAME P1.controller-signal-scope-harness
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/controller_signal_scope_native_test.py" --self-test)
    set_tests_properties(P1.controller-signal-scope-harness PROPERTIES TIMEOUT 300 LABELS "non-hardware")

    add_executable(epycbench_run_start_identity_tests
        tests/unit/platform_linux_run_start_identity_test.cpp)
    target_include_directories(epycbench_run_start_identity_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_run_start_identity_tests PRIVATE
        EpycBench::platform_linux EpycBench::results_journal_records Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_run_start_identity_tests)
    epycbench_enable_warnings(epycbench_run_start_identity_tests)
    add_test(NAME P1.run-start-identity COMMAND epycbench_run_start_identity_tests)
    set_tests_properties(P1.run-start-identity PROPERTIES TIMEOUT 300 LABELS "non-hardware")

    add_executable(epycbench_current_process_image_tests
        tests/unit/platform_linux_current_process_image_test.cpp)
    target_include_directories(epycbench_current_process_image_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_current_process_image_tests PRIVATE _GNU_SOURCE)
    target_link_libraries(epycbench_current_process_image_tests PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_current_process_image_tests)
    epycbench_enable_warnings(epycbench_current_process_image_tests)
    add_executable(epycbench_current_process_image_native
        tests/component/current_process_image_native.cpp)
    target_include_directories(epycbench_current_process_image_native PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_current_process_image_native PRIVATE _GNU_SOURCE)
    target_link_libraries(epycbench_current_process_image_native PRIVATE EpycBench::platform_linux)
    epycbench_enable_build_options(epycbench_current_process_image_native)
    epycbench_enable_warnings(epycbench_current_process_image_native)
    # Native execution is explicit only: no implicit CTest registration before
    # independent source/finite-plan acceptance and the governed command report.

    add_test(NAME P1.current-process-image COMMAND epycbench_current_process_image_tests)
    set_tests_properties(P1.current-process-image PROPERTIES TIMEOUT 300)

    add_executable(
        epycbench_platform_linux_child_process_tests
        tests/unit/platform_linux_child_process_test.cpp
    )
    target_include_directories(
        epycbench_platform_linux_child_process_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_definitions(epycbench_platform_linux_child_process_tests PRIVATE _GNU_SOURCE)
    target_link_libraries(
        epycbench_platform_linux_child_process_tests
        PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_platform_linux_child_process_tests)
    epycbench_enable_warnings(epycbench_platform_linux_child_process_tests)
    add_test(NAME EB-LNX-005.child-process COMMAND epycbench_platform_linux_child_process_tests)

    add_executable(epycbench_platform_linux_host_frozen_process_launch_tests
        tests/unit/platform_linux_host_frozen_process_launch_test.cpp)
    target_include_directories(epycbench_platform_linux_host_frozen_process_launch_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_platform_linux_host_frozen_process_launch_tests PRIVATE _GNU_SOURCE)
    target_link_libraries(epycbench_platform_linux_host_frozen_process_launch_tests
        PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_platform_linux_host_frozen_process_launch_tests)
    epycbench_enable_warnings(epycbench_platform_linux_host_frozen_process_launch_tests)
    add_test(NAME H3.source-launch-owner
        COMMAND epycbench_platform_linux_host_frozen_process_launch_tests "[source-startup-owner]")
    set_tests_properties(H3.source-launch-owner PROPERTIES TIMEOUT 60 LABELS "non-hardware")

    add_executable(
        epycbench_platform_linux_local_ipc_component_tests
        tests/component/platform_linux_local_ipc_component_test.cpp
    )
    target_link_libraries(
        epycbench_platform_linux_local_ipc_component_tests
        PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_platform_linux_local_ipc_component_tests)
    epycbench_enable_warnings(epycbench_platform_linux_local_ipc_component_tests)
    add_test(
        NAME EB-LNX-005.local-ipc-component
        COMMAND epycbench_platform_linux_local_ipc_component_tests
    )

    add_executable(epycbench_scenario_parser_tests tests/unit/scenario_parser_test.cpp)
    target_link_libraries(
        epycbench_scenario_parser_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    target_include_directories(epycbench_scenario_parser_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    epycbench_enable_build_options(epycbench_scenario_parser_tests)
    epycbench_enable_warnings(epycbench_scenario_parser_tests)
    add_test(NAME EB-SCN-001.restricted-parser COMMAND epycbench_scenario_parser_tests)

    add_executable(epycbench_scenario_identifier_tests tests/unit/scenario_identifier_test.cpp)
    target_link_libraries(
        epycbench_scenario_identifier_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_scenario_identifier_tests)
    epycbench_enable_warnings(epycbench_scenario_identifier_tests)
    add_test(NAME EB-SCN-002.common-identifier COMMAND epycbench_scenario_identifier_tests)

    add_executable(
        epycbench_scenario_structural_validation_tests
        tests/unit/scenario_structural_validation_test.cpp
    )
    target_link_libraries(
        epycbench_scenario_structural_validation_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    target_compile_definitions(
        epycbench_scenario_structural_validation_tests
        PRIVATE EPYCBENCH_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    )
    target_include_directories(
        epycbench_scenario_structural_validation_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    epycbench_enable_build_options(epycbench_scenario_structural_validation_tests)
    epycbench_enable_warnings(epycbench_scenario_structural_validation_tests)
    add_test(
        NAME EB-SCN-002.structural-validation
        COMMAND epycbench_scenario_structural_validation_tests
    )

    add_executable(
        epycbench_scenario_unit_normalization_tests
        tests/unit/scenario_unit_normalization_test.cpp
    )
    target_link_libraries(
        epycbench_scenario_unit_normalization_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    target_include_directories(
        epycbench_scenario_unit_normalization_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_definitions(
        epycbench_scenario_unit_normalization_tests
        PRIVATE EPYCBENCH_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    )
    epycbench_enable_build_options(epycbench_scenario_unit_normalization_tests)
    epycbench_enable_warnings(epycbench_scenario_unit_normalization_tests)
    add_test(
        NAME EB-SCN-003.unit-normalization
        COMMAND epycbench_scenario_unit_normalization_tests
    )

    add_executable(
        epycbench_scenario_default_expansion_tests
        tests/unit/scenario_default_expansion_test.cpp
    )
    target_link_libraries(
        epycbench_scenario_default_expansion_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    target_include_directories(
        epycbench_scenario_default_expansion_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    epycbench_enable_build_options(epycbench_scenario_default_expansion_tests)
    epycbench_enable_warnings(epycbench_scenario_default_expansion_tests)
    add_test(
        NAME EB-SCN-004.default-expansion
        COMMAND epycbench_scenario_default_expansion_tests
    )

    add_executable(
        epycbench_scenario_resource_resolution_tests
        tests/unit/scenario_resource_resolution_test.cpp
    )
    target_link_libraries(
        epycbench_scenario_resource_resolution_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    target_include_directories(
        epycbench_scenario_resource_resolution_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    epycbench_enable_build_options(epycbench_scenario_resource_resolution_tests)
    epycbench_enable_warnings(epycbench_scenario_resource_resolution_tests)
    add_test(
        NAME EB-SCN-005.resource-resolution
        COMMAND epycbench_scenario_resource_resolution_tests
    )

    add_executable(
        epycbench_scenario_resource_fixture_tests
        tests/component/scenario_resource_fixture_test.cpp
    )
    target_link_libraries(
        epycbench_scenario_resource_fixture_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    target_compile_definitions(
        epycbench_scenario_resource_fixture_tests
        PRIVATE EPYCBENCH_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    )
    epycbench_enable_build_options(epycbench_scenario_resource_fixture_tests)
    epycbench_enable_warnings(epycbench_scenario_resource_fixture_tests)
    add_test(
        NAME EB-SCN-005.reference-fixture-resolution
        COMMAND epycbench_scenario_resource_fixture_tests
    )

    add_executable(
        epycbench_scenario_service_cpu_resolution_tests
        tests/unit/scenario_service_cpu_resolution_test.cpp
    )
    target_link_libraries(
        epycbench_scenario_service_cpu_resolution_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    target_include_directories(
        epycbench_scenario_service_cpu_resolution_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    epycbench_enable_build_options(epycbench_scenario_service_cpu_resolution_tests)
    epycbench_enable_warnings(epycbench_scenario_service_cpu_resolution_tests)
    add_test(
        NAME EB-SCN-006.service-cpu-resolution
        COMMAND epycbench_scenario_service_cpu_resolution_tests
    )

    add_executable(
        epycbench_scenario_service_cpu_fixture_tests
        tests/component/scenario_service_cpu_fixture_test.cpp
    )
    target_link_libraries(
        epycbench_scenario_service_cpu_fixture_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    target_compile_definitions(
        epycbench_scenario_service_cpu_fixture_tests
        PRIVATE EPYCBENCH_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    )
    epycbench_enable_build_options(epycbench_scenario_service_cpu_fixture_tests)
    epycbench_enable_warnings(epycbench_scenario_service_cpu_fixture_tests)
    add_test(
        NAME EB-SCN-006.reference-fixture-service-cpus
        COMMAND epycbench_scenario_service_cpu_fixture_tests
    )

    add_executable(
        epycbench_scenario_memory_placement_resolution_tests
        tests/unit/scenario_memory_placement_resolution_test.cpp
    )
    target_link_libraries(
        epycbench_scenario_memory_placement_resolution_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    target_include_directories(
        epycbench_scenario_memory_placement_resolution_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    epycbench_enable_build_options(epycbench_scenario_memory_placement_resolution_tests)
    epycbench_enable_warnings(epycbench_scenario_memory_placement_resolution_tests)
    add_test(
        NAME EB-SCN-007.memory-placement-resolution
        COMMAND epycbench_scenario_memory_placement_resolution_tests
    )

    add_executable(
        epycbench_scenario_memory_fixture_tests
        tests/component/scenario_memory_fixture_test.cpp
    )
    target_link_libraries(
        epycbench_scenario_memory_fixture_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    target_compile_definitions(
        epycbench_scenario_memory_fixture_tests
        PRIVATE EPYCBENCH_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    )
    epycbench_enable_build_options(epycbench_scenario_memory_fixture_tests)
    epycbench_enable_warnings(epycbench_scenario_memory_fixture_tests)
    add_test(
        NAME EB-SCN-007.reference-fixture-memory
        COMMAND epycbench_scenario_memory_fixture_tests
    )

    add_executable(epycbench_kernel_contract_tests tests/unit/kernel_contract_test.cpp)
    target_link_libraries(
        epycbench_kernel_contract_tests
        PRIVATE EpycBench::kernel_api Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_kernel_contract_tests)
    epycbench_enable_warnings(epycbench_kernel_contract_tests)
    add_test(NAME EB-SCN-008.kernel-contracts COMMAND epycbench_kernel_contract_tests)

    add_executable(
        epycbench_kernel_descriptor_tests tests/unit/kernel_descriptor_test.cpp
    )
    target_link_libraries(
        epycbench_kernel_descriptor_tests
        PRIVATE EpycBench::kernel_registry Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_kernel_descriptor_tests)
    epycbench_enable_warnings(epycbench_kernel_descriptor_tests)
    add_test(
        NAME EB-KRN-001.kernel-descriptor
        COMMAND epycbench_kernel_descriptor_tests
    )

    add_executable(
        epycbench_kernel_registry_tests tests/unit/kernel_registry_test.cpp
    )
    target_link_libraries(
        epycbench_kernel_registry_tests
        PRIVATE EpycBench::kernel_registry Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_kernel_registry_tests)
    epycbench_enable_warnings(epycbench_kernel_registry_tests)
    add_test(
        NAME EB-KRN-001.kernel-registry
        COMMAND epycbench_kernel_registry_tests
    )

    add_executable(
        epycbench_kernel_registry_validation_tests
        tests/unit/kernel_registry_validation_test.cpp
    )
    target_link_libraries(
        epycbench_kernel_registry_validation_tests
        PRIVATE EpycBench::kernel_registry Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_kernel_registry_validation_tests)
    epycbench_enable_warnings(epycbench_kernel_registry_validation_tests)
    add_test(
        NAME EB-KRN-001.kernel-registry-validation
        COMMAND epycbench_kernel_registry_validation_tests
    )

    add_executable(
        epycbench_kernel_bandwidth_parameter_validation_tests
        tests/unit/kernel_bandwidth_parameter_validation_test.cpp
        tests/unit/kernel_bandwidth_parameter_validation_validation_test.cpp
        tests/unit/kernel_bandwidth_parameter_validation_ownership_test.cpp
    )
    target_link_libraries(
        epycbench_kernel_bandwidth_parameter_validation_tests
        PRIVATE EpycBench::kernel_bandwidth Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_kernel_bandwidth_parameter_validation_tests)
    epycbench_enable_warnings(epycbench_kernel_bandwidth_parameter_validation_tests)
    add_test(
        NAME EB-KRN-BW-001.parameter-validation
        COMMAND epycbench_kernel_bandwidth_parameter_validation_tests
                "[bandwidth-parameter-validation]"
    )
    add_test(
        NAME EB-KRN-BW-001.parameter-validation-validation
        COMMAND epycbench_kernel_bandwidth_parameter_validation_tests
                "[bandwidth-parameter-validation-validation]"
    )
    add_test(
        NAME EB-KRN-BW-001.parameter-validation-ownership
        COMMAND epycbench_kernel_bandwidth_parameter_validation_tests
                "[bandwidth-parameter-validation-ownership]"
    )

    add_executable(
        epycbench_kernel_latency_parameter_validation_tests
        tests/unit/kernel_latency_parameter_validation_test.cpp
        tests/unit/kernel_latency_parameter_validation_validation_test.cpp
        tests/unit/kernel_latency_parameter_validation_ownership_test.cpp
    )
    target_link_libraries(
        epycbench_kernel_latency_parameter_validation_tests
        PRIVATE EpycBench::kernel_latency Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_kernel_latency_parameter_validation_tests)
    epycbench_enable_warnings(epycbench_kernel_latency_parameter_validation_tests)
    add_test(
        NAME EB-KRN-LAT-001.parameter-validation
        COMMAND epycbench_kernel_latency_parameter_validation_tests
                "[latency-parameter-validation]"
    )
    add_test(
        NAME EB-KRN-LAT-001.parameter-validation-validation
        COMMAND epycbench_kernel_latency_parameter_validation_tests
                "[latency-parameter-validation-validation]"
    )
    add_test(
        NAME EB-KRN-LAT-001.parameter-validation-ownership
        COMMAND epycbench_kernel_latency_parameter_validation_tests
                "[latency-parameter-validation-ownership]"
    )

    add_executable(
        epycbench_kernel_latency_start_state_schedule_tests
        tests/unit/kernel_latency_preparation_test.cpp
        tests/unit/kernel_latency_start_state_schedule_test.cpp
        tests/unit/kernel_latency_start_state_schedule_ownership_test.cpp
    )
    target_link_libraries(
        epycbench_kernel_latency_start_state_schedule_tests
        PRIVATE EpycBench::kernel_latency Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_kernel_latency_start_state_schedule_tests)
    epycbench_enable_warnings(epycbench_kernel_latency_start_state_schedule_tests)
    add_test(
        NAME EB-KRN-LAT-003.preparation
        COMMAND epycbench_kernel_latency_start_state_schedule_tests
                "[latency-preparation]"
    )
    add_test(
        NAME EB-KRN-LAT-003.start-state-schedule
        COMMAND epycbench_kernel_latency_start_state_schedule_tests
                "[latency-start-state-schedule]"
    )
    add_test(
        NAME EB-KRN-LAT-003.start-state-schedule-ownership
        COMMAND epycbench_kernel_latency_start_state_schedule_tests
                "[latency-start-state-schedule-ownership]"
    )

    add_executable(
        epycbench_kernel_latency_timed_function_tests
        tests/unit/kernel_latency_timed_function_test.cpp
        tests/unit/kernel_latency_timed_function_semantics_test.cpp
        tests/unit/kernel_latency_timed_function_ownership_test.cpp
    )
    target_link_libraries(
        epycbench_kernel_latency_timed_function_tests
        PRIVATE EpycBench::kernel_latency Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_kernel_latency_timed_function_tests)
    epycbench_enable_warnings(epycbench_kernel_latency_timed_function_tests)
    add_test(
        NAME EB-KRN-LAT-004.timed-function
        COMMAND epycbench_kernel_latency_timed_function_tests
                "[latency-timed-function]"
    )
    add_test(
        NAME EB-KRN-LAT-004.timed-function-semantics
        COMMAND epycbench_kernel_latency_timed_function_tests
                "[latency-timed-function-semantics]"
    )
    add_test(
        NAME EB-KRN-LAT-004.timed-function-ownership
        COMMAND epycbench_kernel_latency_timed_function_tests
                "[latency-timed-function-ownership]"
    )

    add_executable(
        epycbench_kernel_latency_terminal_verification_tests
        tests/unit/kernel_latency_terminal_verification_test.cpp
        tests/unit/kernel_latency_terminal_verification_mismatch_test.cpp
        tests/unit/kernel_latency_terminal_verification_ownership_test.cpp
    )
    target_link_libraries(
        epycbench_kernel_latency_terminal_verification_tests
        PRIVATE EpycBench::kernel_latency EpycBench::kernel_initialization
                Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_kernel_latency_terminal_verification_tests)
    epycbench_enable_warnings(epycbench_kernel_latency_terminal_verification_tests)
    add_test(
        NAME EB-KRN-LAT-006.terminal-verification
        COMMAND epycbench_kernel_latency_terminal_verification_tests
                "[latency-terminal-verification]"
    )
    add_test(
        NAME EB-KRN-LAT-006.terminal-verification-mismatch
        COMMAND epycbench_kernel_latency_terminal_verification_tests
                "[latency-terminal-verification-mismatch]"
    )
    add_test(
        NAME EB-KRN-LAT-006.terminal-verification-ownership
        COMMAND epycbench_kernel_latency_terminal_verification_tests
                "[latency-terminal-verification-ownership]"
    )

    add_executable(
        epycbench_kernel_bandwidth_preparation_tests
        tests/unit/kernel_bandwidth_preparation_test.cpp
        tests/unit/kernel_bandwidth_preparation_validation_test.cpp
        tests/unit/kernel_bandwidth_preparation_ownership_test.cpp
    )
    target_link_libraries(
        epycbench_kernel_bandwidth_preparation_tests
        PRIVATE EpycBench::kernel_bandwidth Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_kernel_bandwidth_preparation_tests)
    epycbench_enable_warnings(epycbench_kernel_bandwidth_preparation_tests)
    add_test(
        NAME EB-KRN-BW-002.preparation
        COMMAND epycbench_kernel_bandwidth_preparation_tests "[bandwidth-preparation]"
    )
    add_test(
        NAME EB-KRN-BW-002.preparation-validation
        COMMAND epycbench_kernel_bandwidth_preparation_tests
                "[bandwidth-preparation-validation]"
    )
    add_test(
        NAME EB-KRN-BW-002.preparation-ownership
        COMMAND epycbench_kernel_bandwidth_preparation_tests
                "[bandwidth-preparation-ownership]"
    )

    add_executable(
        epycbench_kernel_bandwidth_timed_function_tests
        tests/unit/kernel_bandwidth_timed_function_test.cpp
        tests/unit/kernel_bandwidth_timed_function_semantics_test.cpp
        tests/unit/kernel_bandwidth_timed_function_ownership_test.cpp
    )
    target_link_libraries(
        epycbench_kernel_bandwidth_timed_function_tests
        PRIVATE EpycBench::kernel_bandwidth Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_kernel_bandwidth_timed_function_tests)
    epycbench_enable_warnings(epycbench_kernel_bandwidth_timed_function_tests)
    add_test(
        NAME EB-KRN-BW-003.timed-function
        COMMAND epycbench_kernel_bandwidth_timed_function_tests
                "[bandwidth-timed-function]"
    )
    add_test(
        NAME EB-KRN-BW-003.timed-function-semantics
        COMMAND epycbench_kernel_bandwidth_timed_function_tests
                "[bandwidth-timed-function-semantics]"
    )
    add_test(
        NAME EB-KRN-BW-003.timed-function-ownership
        COMMAND epycbench_kernel_bandwidth_timed_function_tests
                "[bandwidth-timed-function-ownership]"
    )

    add_executable(
        epycbench_kernel_bandwidth_work_accounting_tests
        tests/unit/kernel_bandwidth_work_accounting_test.cpp
        tests/unit/kernel_bandwidth_work_accounting_overflow_test.cpp
        tests/unit/kernel_bandwidth_work_accounting_ownership_test.cpp
    )
    target_link_libraries(
        epycbench_kernel_bandwidth_work_accounting_tests
        PRIVATE EpycBench::kernel_bandwidth Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_kernel_bandwidth_work_accounting_tests)
    epycbench_enable_warnings(epycbench_kernel_bandwidth_work_accounting_tests)
    add_test(
        NAME EB-KRN-BW-004.work-accounting
        COMMAND epycbench_kernel_bandwidth_work_accounting_tests
                "[bandwidth-work-accounting]"
    )
    add_test(
        NAME EB-KRN-BW-004.work-accounting-overflow
        COMMAND epycbench_kernel_bandwidth_work_accounting_tests
                "[bandwidth-work-accounting-overflow]"
    )
    add_test(
        NAME EB-KRN-BW-004.work-accounting-ownership
        COMMAND epycbench_kernel_bandwidth_work_accounting_tests
                "[bandwidth-work-accounting-ownership]"
    )

    add_executable(
        epycbench_kernel_bandwidth_checksum_tests
        tests/unit/kernel_bandwidth_checksum_test.cpp
        tests/unit/kernel_bandwidth_checksum_verification_test.cpp
        tests/unit/kernel_bandwidth_checksum_ownership_test.cpp
    )
    target_link_libraries(
        epycbench_kernel_bandwidth_checksum_tests
        PRIVATE EpycBench::kernel_bandwidth Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_kernel_bandwidth_checksum_tests)
    epycbench_enable_warnings(epycbench_kernel_bandwidth_checksum_tests)
    add_test(
        NAME EB-KRN-BW-005.bandwidth-checksum
        COMMAND epycbench_kernel_bandwidth_checksum_tests "[bandwidth-checksum]"
    )
    add_test(
        NAME EB-KRN-BW-005.bandwidth-checksum-verification
        COMMAND epycbench_kernel_bandwidth_checksum_tests
                "[bandwidth-checksum-verification]"
    )
    add_test(
        NAME EB-KRN-BW-005.bandwidth-checksum-ownership
        COMMAND epycbench_kernel_bandwidth_checksum_tests
                "[bandwidth-checksum-ownership]"
    )

    add_executable(
        epycbench_kernel_region_initialization_tests
        tests/unit/kernel_region_initialization_test.cpp
        tests/unit/kernel_region_initialization_validation_test.cpp
        tests/unit/kernel_region_initialization_determinism_test.cpp
    )
    target_include_directories(
        epycbench_kernel_region_initialization_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        epycbench_kernel_region_initialization_tests
        PRIVATE EpycBench::kernel_initialization Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_kernel_region_initialization_tests)
    epycbench_enable_warnings(epycbench_kernel_region_initialization_tests)
    add_test(
        NAME EB-KRN-002.region-initializer
        COMMAND epycbench_kernel_region_initialization_tests "[region-initializer]"
    )
    add_test(
        NAME EB-KRN-002.region-initializer-validation
        COMMAND epycbench_kernel_region_initialization_tests "[region-initializer-validation]"
    )
    add_test(
        NAME EB-KRN-002.region-initializer-determinism
        COMMAND epycbench_kernel_region_initialization_tests "[region-initializer-determinism]"
    )

    add_executable(
        epycbench_g4_kernel_known_answer_fixture_tests
        tests/unit/g4_kernel_known_answer_fixture_test.cpp
    )
    target_compile_definitions(
        epycbench_g4_kernel_known_answer_fixture_tests
        PRIVATE EPYCBENCH_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(
        epycbench_g4_kernel_known_answer_fixture_tests
        PRIVATE EpycBench::kernel_bandwidth EpycBench::kernel_latency
                EpycBench::kernel_initialization nlohmann_json::nlohmann_json
                Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_g4_kernel_known_answer_fixture_tests)
    epycbench_enable_warnings(epycbench_g4_kernel_known_answer_fixture_tests)
    add_test(
        NAME G4.kernel-work-correctness-fixture
        COMMAND epycbench_g4_kernel_known_answer_fixture_tests "[g4-kernel-known-answer]"
    )

    add_executable(
        epycbench_measurement_observation_tests
        tests/unit/measurement_observation_test.cpp
    )
    target_link_libraries(
        epycbench_measurement_observation_tests
        PRIVATE EpycBench::measurement Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_measurement_observation_tests)
    epycbench_enable_warnings(epycbench_measurement_observation_tests)
    add_test(
        NAME EB-MSR-001.observation-model
        COMMAND epycbench_measurement_observation_tests
    )

    add_executable(
        epycbench_measurement_empty_harness_tests
        tests/unit/measurement_empty_harness_test.cpp
    )
    target_link_libraries(
        epycbench_measurement_empty_harness_tests
        PRIVATE EpycBench::measurement Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_measurement_empty_harness_tests)
    epycbench_enable_warnings(epycbench_measurement_empty_harness_tests)
    add_test(
        NAME EB-MSR-002.empty-harness
        COMMAND epycbench_measurement_empty_harness_tests
    )

    add_executable(
        epycbench_measurement_calibration_tests
        tests/unit/measurement_calibration_test.cpp
    )
    target_link_libraries(
        epycbench_measurement_calibration_tests
        PRIVATE EpycBench::measurement Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_measurement_calibration_tests)
    epycbench_enable_warnings(epycbench_measurement_calibration_tests)
    add_test(
        NAME EB-MSR-003.calibration-engine
        COMMAND epycbench_measurement_calibration_tests
    )

    add_executable(
        epycbench_measurement_warmup_tests
        tests/unit/measurement_warmup_test.cpp
    )
    target_link_libraries(
        epycbench_measurement_warmup_tests
        PRIVATE EpycBench::measurement Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_measurement_warmup_tests)
    epycbench_enable_warnings(epycbench_measurement_warmup_tests)
    add_test(
        NAME EB-MSR-004.warmup-engine
        COMMAND epycbench_measurement_warmup_tests
    )

    add_executable(
        epycbench_measurement_block_scheduler_tests
        tests/unit/measurement_block_scheduler_test.cpp
    )
    target_link_libraries(
        epycbench_measurement_block_scheduler_tests
        PRIVATE EpycBench::measurement Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_measurement_block_scheduler_tests)
    epycbench_enable_warnings(epycbench_measurement_block_scheduler_tests)
    add_test(
        NAME EB-MSR-005.block-scheduler
        COMMAND epycbench_measurement_block_scheduler_tests
    )

    add_executable(
        epycbench_measurement_observation_execution_tests
        tests/unit/measurement_observation_execution_test.cpp
    )
    target_link_libraries(
        epycbench_measurement_observation_execution_tests
        PRIVATE EpycBench::measurement Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_measurement_observation_execution_tests)
    epycbench_enable_warnings(epycbench_measurement_observation_execution_tests)
    add_test(
        NAME EB-MSR-006.observation-execution
        COMMAND epycbench_measurement_observation_execution_tests
    )

    add_executable(
        epycbench_measurement_validity_tests
        tests/unit/measurement_validity_test.cpp
    )
    target_link_libraries(
        epycbench_measurement_validity_tests
        PRIVATE EpycBench::measurement Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_measurement_validity_tests)
    epycbench_enable_warnings(epycbench_measurement_validity_tests)
    add_test(
        NAME EB-MSR-007.validity-engine
        COMMAND epycbench_measurement_validity_tests
    )

    add_executable(
        epycbench_measurement_repetition_tests
        tests/unit/measurement_repetition_test.cpp
    )
    target_link_libraries(
        epycbench_measurement_repetition_tests
        PRIVATE EpycBench::measurement Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_measurement_repetition_tests)
    epycbench_enable_warnings(epycbench_measurement_repetition_tests)
    add_test(
        NAME EB-MSR-008.repetition-engine
        COMMAND epycbench_measurement_repetition_tests
    )

    add_executable(
        epycbench_monitoring_monitor_source_tests
        tests/unit/monitoring_monitor_source_test.cpp
    )
    target_link_libraries(
        epycbench_monitoring_monitor_source_tests
        PRIVATE EpycBench::monitoring Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_monitoring_monitor_source_tests)
    epycbench_enable_warnings(epycbench_monitoring_monitor_source_tests)
    add_test(
        NAME EB-MON-001.monitor-source-interface
        COMMAND epycbench_monitoring_monitor_source_tests
    )

    add_executable(
        epycbench_monitoring_page_fault_tests
        tests/unit/monitoring_page_faults_test.cpp
    )
    target_include_directories(
        epycbench_monitoring_page_fault_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        epycbench_monitoring_page_fault_tests
        PRIVATE EpycBench::monitoring Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_monitoring_page_fault_tests)
    epycbench_enable_warnings(epycbench_monitoring_page_fault_tests)
    add_test(
        NAME EB-MON-002.page-faults
        COMMAND epycbench_monitoring_page_fault_tests
    )

    add_executable(
        epycbench_monitoring_context_switch_tests
        tests/unit/monitoring_context_switches_test.cpp
    )
    target_include_directories(
        epycbench_monitoring_context_switch_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        epycbench_monitoring_context_switch_tests
        PRIVATE EpycBench::monitoring Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_monitoring_context_switch_tests)
    epycbench_enable_warnings(epycbench_monitoring_context_switch_tests)
    add_test(
        NAME EB-MON-003.context-switches
        COMMAND epycbench_monitoring_context_switch_tests
    )
    add_executable(epycbench_monitoring_host_q2_context_switch_source_tests
        tests/unit/monitoring_host_q2_context_switch_source_test.cpp)
    target_include_directories(epycbench_monitoring_host_q2_context_switch_source_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_monitoring_host_q2_context_switch_source_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q2_context_switch_source_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q2_context_switch_source_tests)
    if(EPYCBENCH_ENABLE_TSAN)
        # TSan supplies allocation interceptors; fault injection stays in the
        # other compiler/sanitizer lanes. Select only supported changed seams.
        target_compile_definitions(epycbench_monitoring_host_q2_context_switch_source_tests
            PRIVATE EPYCBENCH_Q2_CONCURRENCY_TSAN=1)
        add_test(NAME EB-MON-003.host-q2-context-switch-source
            COMMAND epycbench_monitoring_host_q2_context_switch_source_tests "[q2-voluntary-event]")
    else()
        add_test(NAME EB-MON-003.host-q2-context-switch-source COMMAND epycbench_monitoring_host_q2_context_switch_source_tests)
    endif()
    add_executable(epycbench_monitoring_host_q2_context_switch_collector_tests
        tests/unit/monitoring_host_q2_context_switch_source_test.cpp)
    target_compile_definitions(epycbench_monitoring_host_q2_context_switch_collector_tests PRIVATE EPYCBENCH_Q2_COLLECTOR_TESTS=1)
    target_include_directories(epycbench_monitoring_host_q2_context_switch_collector_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_monitoring_host_q2_context_switch_collector_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q2_context_switch_collector_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q2_context_switch_collector_tests)
    if(EPYCBENCH_ENABLE_TSAN)
        target_compile_definitions(epycbench_monitoring_host_q2_context_switch_collector_tests
            PRIVATE EPYCBENCH_Q2_CONCURRENCY_TSAN=1)
        add_test(NAME EB-MON-003.host-q2-context-switch-collector
            COMMAND epycbench_monitoring_host_q2_context_switch_collector_tests "[q2-voluntary-event]")
    else()
        add_test(NAME EB-MON-003.host-q2-context-switch-collector COMMAND epycbench_monitoring_host_q2_context_switch_collector_tests)
    endif()

    add_executable(epycbench_monitoring_temperature_tests tests/unit/monitoring_temperature_test.cpp)
    target_include_directories(epycbench_monitoring_temperature_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_monitoring_temperature_tests PRIVATE EpycBench::monitoring Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_temperature_tests)
    epycbench_enable_warnings(epycbench_monitoring_temperature_tests)
    add_test(NAME EB-MON-004.temperature COMMAND epycbench_monitoring_temperature_tests)

    add_executable(epycbench_monitoring_frequency_tests tests/unit/monitoring_frequency_test.cpp)
    target_include_directories(epycbench_monitoring_frequency_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_monitoring_frequency_tests PRIVATE EpycBench::monitoring Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_frequency_tests)
    epycbench_enable_warnings(epycbench_monitoring_frequency_tests)
    add_test(NAME EB-MON-005.frequency COMMAND epycbench_monitoring_frequency_tests)

    add_test(
        NAME EB-MON-007.qualification-framework
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_qualification_framework_test.py"
    )
    add_test(
        NAME EB-MON-007.host-frozen-q7
        COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_host_frozen_q7_test.py"
    )
    add_test(
        NAME EB-MON-007.host-v2-input-schema
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_host_v2_inputs_test.py"
                "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(
        NAME EB-MON-007.q1-stage-acceptance
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q1_stage_acceptance_test.py"
    )
    add_test(
        NAME EB-MON-007.q2-governed-plan
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q2_plan_test.py"
    )
    add_test(
        NAME EB-MON-007.q3-governed-plan
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q3_plan_test.py"
    )
    add_test(
        NAME EB-MON-007.q3-orchestration
        COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q3_orchestration_test.py"
    )
    add_test(
        NAME EB-MON-007.q3-public-route
        COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q3_public_route_test.py"
    )
    add_test(
        NAME EB-MON-005.host-q4-schemas
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q4_schema_test.py"
    )
    add_test(
        NAME EB-MON-007.q4-transport
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q4_transport_test.py"
    )
    add_test(
        NAME EB-MON-007.q4-orchestration
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q4_orchestration_test.py"
    )
    add_test(
        NAME EB-MON-007.q4-plan-authority
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q4_plan_authority_test.py"
    )
    add_test(
        NAME EB-MON-007.q4-stage-acceptance
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q4_stage_acceptance_test.py"
    )
    add_test(
        NAME EB-MON-007.q4-publication-authority
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q4_publication_authority_test.py"
    )
    add_test(
        NAME EB-MON-007.q4-snapshot-update
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q4_snapshot_update_test.py"
    )
    add_test(
        NAME EB-MON-007.q4-public-route
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q4_public_route_test.py"
    )
    add_test(
        NAME EB-MON-007.q2-stage-acceptance
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q2_stage_acceptance_test.py"
    )
    add_test(
        NAME EB-MON-007.q1-orchestration
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q1_orchestration_test.py"
    )
    add_test(
        NAME EB-MON-007.q2-orchestration
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q2_orchestration_test.py"
    )
    add_test(
        NAME EB-MON-007.q1-public-route
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q1_public_route_test.py"
    )
    add_test(
        NAME EB-MON-007.q2-public-route
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q2_public_route_test.py"
    )
    add_test(
        NAME EB-MON-007.snapshot-update
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_snapshot_update_test.py"
    )

    # Test/repository-config qualification only; never install or use as a production helper.
    add_executable(epycbench_scenario_v1_validator
        tests/tools/scenario_v1_validator.cpp
    )
    target_link_libraries(epycbench_scenario_v1_validator PRIVATE EpycBench::scenario)
    epycbench_enable_build_options(epycbench_scenario_v1_validator)
    epycbench_enable_warnings(epycbench_scenario_v1_validator)
    set_tests_properties(EB-MON-007.qualification-framework EB-MON-007.host-frozen-q7 PROPERTIES
        ENVIRONMENT "EPYCBENCH_SCENARIO_V1_VALIDATOR=$<TARGET_FILE:epycbench_scenario_v1_validator>;EPYCBENCH_HOST_PRODUCTION_AUTHORITY_FIXTURE=$<TARGET_FILE:epycbench_monitoring_host_frozen_observation_admission_tests>;EPYCBENCH_HOST_FROZEN_EVIDENCE_FIXTURE=$<TARGET_FILE:epycbench_runtime_frozen_observation_run_tests>"
    )
    set_tests_properties(EB-MON-007.host-frozen-q7 PROPERTIES TIMEOUT 300)

    add_executable(epycbench_monitoring_production_admission_projection_tests
        tests/unit/monitoring_production_admission_projection_test.cpp
    )
    target_include_directories(epycbench_monitoring_production_admission_projection_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/tests/unit"
    )
    target_compile_definitions(epycbench_monitoring_production_admission_projection_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(epycbench_monitoring_production_admission_projection_tests
        PRIVATE EpycBench::monitoring_qualification Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_monitoring_production_admission_projection_tests)
    epycbench_enable_warnings(epycbench_monitoring_production_admission_projection_tests)
    add_test(NAME EB-MON-007.production-admission-projection
        COMMAND epycbench_monitoring_production_admission_projection_tests)

    add_executable(epycbench_monitoring_q0b_base_environment_projection_tests
        tests/unit/monitoring_q0b_base_environment_projection_test.cpp
    )
    target_include_directories(epycbench_monitoring_q0b_base_environment_projection_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests/unit"
    )
    target_compile_definitions(epycbench_monitoring_q0b_base_environment_projection_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(epycbench_monitoring_q0b_base_environment_projection_tests
        PRIVATE EpycBench::monitoring_qualification Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_monitoring_q0b_base_environment_projection_tests)
    epycbench_enable_warnings(epycbench_monitoring_q0b_base_environment_projection_tests)
    add_test(NAME EB-MON-007.q0b-base-environment-projection
        COMMAND epycbench_monitoring_q0b_base_environment_projection_tests)

    add_executable(epycbench_monitoring_q0b_command_tests
        tests/unit/monitoring_q0b_command_test.cpp)
    target_include_directories(epycbench_monitoring_q0b_command_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_monitoring_q0b_command_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(epycbench_monitoring_q0b_command_tests
        PRIVATE epycbench_cli Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_q0b_command_tests)
    epycbench_enable_warnings(epycbench_monitoring_q0b_command_tests)
    add_test(NAME EB-MON-007.q0b-hidden-command
        COMMAND epycbench_monitoring_q0b_command_tests)

    add_executable(epycbench_monitoring_q0b_production_input_authority_tests
        tests/unit/monitoring_q0b_production_inputs_test.cpp)
    target_include_directories(epycbench_monitoring_q0b_production_input_authority_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_monitoring_q0b_production_input_authority_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(epycbench_monitoring_q0b_production_input_authority_tests
        PRIVATE EpycBench::monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_q0b_production_input_authority_tests)
    epycbench_enable_warnings(epycbench_monitoring_q0b_production_input_authority_tests)
    add_test(NAME EB-MON-007.q0b-production-input-authority
        COMMAND epycbench_monitoring_q0b_production_input_authority_tests)

    add_executable(epycbench_monitoring_host_input_template_tests
        tests/unit/monitoring_host_input_template_test.cpp)
    target_include_directories(epycbench_monitoring_host_input_template_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_monitoring_host_input_template_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(epycbench_monitoring_host_input_template_tests
        PRIVATE EpycBench::monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_input_template_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_input_template_tests)
    add_test(NAME EB-MON-007.host-v2-input-authority
        COMMAND epycbench_monitoring_host_input_template_tests)

    add_executable(epycbench_monitoring_host_runner_context_session_tests
        tests/unit/monitoring_host_runner_context_session_test.cpp)
    target_include_directories(epycbench_monitoring_host_runner_context_session_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_monitoring_host_runner_context_session_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(epycbench_monitoring_host_runner_context_session_tests
        PRIVATE EpycBench::monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_runner_context_session_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_runner_context_session_tests)
    add_test(NAME EB-MON-007.host-v2-runner-context-session
        COMMAND epycbench_monitoring_host_runner_context_session_tests)

    add_executable(epycbench_monitoring_host_environment_tests
        tests/unit/monitoring_host_environment_test.cpp)
    target_include_directories(epycbench_monitoring_host_environment_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_monitoring_host_environment_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(epycbench_monitoring_host_environment_tests
        PRIVATE EpycBench::monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_environment_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_environment_tests)
    add_test(NAME EB-MON-007.host-v2-environment
        COMMAND epycbench_monitoring_host_environment_tests)

    add_executable(epycbench_monitoring_host_baseline_preparation_tests
        tests/unit/monitoring_host_baseline_preparation_test.cpp)
    target_include_directories(epycbench_monitoring_host_baseline_preparation_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_monitoring_host_baseline_preparation_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(epycbench_monitoring_host_baseline_preparation_tests
        PRIVATE EpycBench::monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_baseline_preparation_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_baseline_preparation_tests)
    add_test(NAME EB-MON-007.host-baseline-preparation
        COMMAND epycbench_monitoring_host_baseline_preparation_tests)
    set_tests_properties(EB-MON-007.host-baseline-preparation PROPERTIES TIMEOUT 120)

    add_executable(epycbench_monitoring_host_baseline_file_export_tests
        tests/unit/monitoring_host_baseline_file_export_test.cpp)
    target_include_directories(epycbench_monitoring_host_baseline_file_export_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_monitoring_host_baseline_file_export_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(epycbench_monitoring_host_baseline_file_export_tests
        PRIVATE EpycBench::platform_linux EpycBench::monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_baseline_file_export_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_baseline_file_export_tests)
    add_test(NAME EB-MON-007.host-baseline-file-export
        COMMAND epycbench_monitoring_host_baseline_file_export_tests)
    set_tests_properties(EB-MON-007.host-baseline-file-export PROPERTIES TIMEOUT 30)

    add_executable(epycbench_monitoring_host_baseline_native_fixture
        tests/unit/monitoring_host_baseline_native_fixture.cpp)
    target_include_directories(epycbench_monitoring_host_baseline_native_fixture
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_monitoring_host_baseline_native_fixture
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(epycbench_monitoring_host_baseline_native_fixture
        PRIVATE EpycBench::platform_linux EpycBench::monitoring_qualification epycbench_cli Catch2::Catch2)
    epycbench_enable_build_options(epycbench_monitoring_host_baseline_native_fixture)
    epycbench_enable_warnings(epycbench_monitoring_host_baseline_native_fixture)
    add_test(NAME EB-MON-007.host-baseline-retention
        COMMAND Python3::Interpreter -B
            "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_host_baseline_retention_test.py"
            "${PROJECT_SOURCE_DIR}" "$<TARGET_FILE:epycbench_monitoring_host_baseline_native_fixture>")
    set_tests_properties(EB-MON-007.host-baseline-retention PROPERTIES TIMEOUT 60)

    add_test(NAME EB-MON-007.host-baseline-migration
        COMMAND Python3::Interpreter -B
            "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_host_baseline_migration_test.py"
            "${PROJECT_SOURCE_DIR}" "$<TARGET_FILE:epycbench_monitoring_host_baseline_native_fixture>")
    set_tests_properties(EB-MON-007.host-baseline-migration PROPERTIES TIMEOUT 60)

    if(NOT EPYCBENCH_ENABLE_ASAN_UBSAN AND NOT EPYCBENCH_ENABLE_TSAN)
        foreach(baseline_native_case IN ITEMS publication occupied)
            add_test(NAME EB-MON-007.governed-baseline-native-${baseline_native_case}
                COMMAND Python3::Interpreter -I -B
                    "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_host_frozen_workflow_test.py"
                    --baseline-native --source-root "${PROJECT_SOURCE_DIR}"
                    --native-binary "$<TARGET_FILE:epycbench_monitoring_host_baseline_native_fixture>"
                    --entry-binary "$<TARGET_FILE:epycbench_host_work_exec_entry>"
                    --test-script "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_host_baseline_retention_test.py"
                    --artifact-parent "${PROJECT_BINARY_DIR}"
                    --case "${baseline_native_case}" --timeout 180)
            set_tests_properties(EB-MON-007.governed-baseline-native-${baseline_native_case}
                PROPERTIES TIMEOUT 210 RUN_SERIAL TRUE
                LABELS "synthetic;non-hardware;source-workflow;native-owners;contained-native")
        endforeach()
        add_test(NAME EB-MON-007.campaign-baseline-commit-driver-owners
            COMMAND Python3::Interpreter -I -B
                "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_host_baseline_migration_test.py"
                "${PROJECT_SOURCE_DIR}" "$<TARGET_FILE:epycbench_monitoring_host_baseline_native_fixture>"
                -k commit_driver)
        set_tests_properties(EB-MON-007.campaign-baseline-commit-driver-owners
            PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
            LABELS "synthetic;non-hardware;source-workflow;native-owners")
    endif()

    add_executable(epycbench_monitoring_host_work_admission_tests
        tests/unit/monitoring_host_work_admission_test.cpp)
    target_include_directories(epycbench_monitoring_host_work_admission_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_monitoring_host_work_admission_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(epycbench_monitoring_host_work_admission_tests
        PRIVATE EpycBench::monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_work_admission_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_work_admission_tests)
    add_test(NAME EB-MON-007.host-work-admission
        COMMAND epycbench_monitoring_host_work_admission_tests)
    set_tests_properties(EB-MON-007.host-work-admission PROPERTIES TIMEOUT 60)

    add_executable(epycbench_monitoring_host_q0b_governed_work_tests
        tests/unit/monitoring_host_q0b_governed_work_test.cpp)
    target_include_directories(epycbench_monitoring_host_q0b_governed_work_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_monitoring_host_q0b_governed_work_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(epycbench_monitoring_host_q0b_governed_work_tests
        PRIVATE EpycBench::monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q0b_governed_work_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q0b_governed_work_tests)
    add_test(NAME EB-MON-007.host-q0b-governed-work
        COMMAND epycbench_monitoring_host_q0b_governed_work_tests)
    set_tests_properties(EB-MON-007.host-q0b-governed-work PROPERTIES TIMEOUT 60)

    # Includes the actual parent body once to replace only fixed native leaves.
    # No child, discovery or host policy is acquired by this unit scope.
    add_executable(epycbench_monitoring_host_q0b_governed_parent_tests
        tests/unit/monitoring_host_q0b_governed_parent_test.cpp)
    target_include_directories(epycbench_monitoring_host_q0b_governed_parent_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_monitoring_host_q0b_governed_parent_tests
        PRIVATE EpycBench::monitoring_qualification)
    epycbench_enable_build_options(epycbench_monitoring_host_q0b_governed_parent_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q0b_governed_parent_tests)
    add_test(NAME EB-MON-007.host-q0b-governed-parent-injected
        COMMAND epycbench_monitoring_host_q0b_governed_parent_tests)
    set_tests_properties(EB-MON-007.host-q0b-governed-parent-injected PROPERTIES
        TIMEOUT 30 LABELS "synthetic;non-hardware;source-workflow")

    add_executable(epycbench_monitoring_host_frozen_source_composition_tests
        tests/unit/monitoring_host_frozen_source_composition_test.cpp)
    target_include_directories(epycbench_monitoring_host_frozen_source_composition_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src"
                "${PROJECT_SOURCE_DIR}/tests/unit")
    target_compile_definitions(epycbench_monitoring_host_frozen_source_composition_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(epycbench_monitoring_host_frozen_source_composition_tests
        PRIVATE EpycBench::monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_frozen_source_composition_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_frozen_source_composition_tests)
    add_test(NAME EB-MON-007.source-prerequisite-composition
        COMMAND epycbench_monitoring_host_frozen_source_composition_tests)
    set_tests_properties(EB-MON-007.source-prerequisite-composition PROPERTIES TIMEOUT 120)

    add_executable(epycbench_monitoring_host_frozen_source_launch_tests
        tests/unit/monitoring_host_frozen_source_launch_test.cpp)
    target_include_directories(epycbench_monitoring_host_frozen_source_launch_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_monitoring_host_frozen_source_launch_tests
        PRIVATE EpycBench::monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_frozen_source_launch_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_frozen_source_launch_tests)
    add_test(NAME H3.source-launch-binding
        COMMAND epycbench_monitoring_host_frozen_source_launch_tests "[source-launch-binding]")
    set_tests_properties(H3.source-launch-binding PROPERTIES TIMEOUT 60 LABELS "non-hardware")
    add_test(NAME H3.source-governed-handoff
        COMMAND epycbench_monitoring_host_frozen_source_launch_tests "[source-governed-handoff]")
    set_tests_properties(H3.source-governed-handoff PROPERTIES TIMEOUT 60 LABELS "non-hardware;synthetic")
    add_test(NAME EB-MON-007.owned-source-workflow-supervisor-injected
        COMMAND Python3::Interpreter -I -B
            "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_host_frozen_workflow_test.py" --self-test)
    set_tests_properties(EB-MON-007.owned-source-workflow-supervisor-injected
        PROPERTIES TIMEOUT 30 LABELS "runtime;synthetic;non-hardware")

    add_executable(epycbench_monitoring_host_frozen_observation_admission_tests
        tests/unit/monitoring_host_frozen_observation_admission_test.cpp)
    target_include_directories(epycbench_monitoring_host_frozen_observation_admission_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_monitoring_host_frozen_observation_admission_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
                EPYCBENCH_HOST_FROZEN_COLLECTION_TEST=1)
    target_link_libraries(epycbench_monitoring_host_frozen_observation_admission_tests
        PRIVATE EpycBench::monitoring_qualification EpycBench::runtime_production_backend
                epycbench_stage_b_resource_policy
                Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_frozen_observation_admission_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_frozen_observation_admission_tests)
    if(EPYCBENCH_ENABLE_TSAN)
        # TSan supplies allocation interceptors; the allocation-fault cases keep
        # their independent GCC/Clang/ASan/leak gates. Select only native probes.
        target_compile_definitions(epycbench_monitoring_host_frozen_observation_admission_tests
            PRIVATE EPYCBENCH_HOST_FROZEN_NATIVE_TSAN=1)
        add_test(NAME EB-MON-007.host-frozen-observation-admission
            COMMAND epycbench_monitoring_host_frozen_observation_admission_tests
                    "[source-main-control-native]" --order decl)
    else()
        add_test(NAME EB-MON-007.host-frozen-observation-admission
            COMMAND epycbench_monitoring_host_frozen_observation_admission_tests)
    endif()
    set_tests_properties(EB-MON-007.host-frozen-observation-admission PROPERTIES TIMEOUT 60)

    add_executable(epycbench_runtime_frozen_observation_run_tests
        tests/unit/runtime_frozen_observation_run_test.cpp)
    target_include_directories(epycbench_runtime_frozen_observation_run_tests
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src"
                "${PROJECT_SOURCE_DIR}/tests/unit")
    target_link_libraries(epycbench_runtime_frozen_observation_run_tests
        PRIVATE EpycBench::runtime_production_backend epycbench_runtime_test_support
                epycbench_stage_b_resource_policy EpycBench::monitoring_qualification
                Catch2::Catch2)
    target_compile_definitions(epycbench_runtime_frozen_observation_run_tests
        PRIVATE EPYCBENCH_HOST_FROZEN_COLLECTION_TEST=1
                EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_runtime_frozen_observation_run_tests)
    epycbench_enable_warnings(epycbench_runtime_frozen_observation_run_tests)
    add_test(NAME EB-MON-007.frozen-observation-run
        COMMAND epycbench_runtime_frozen_observation_run_tests)
    # The complete synthetic collector fixture includes two 384-cell Q6
    # traversals; its instrumented aggregate exceeds the former 60s limit.
    set_tests_properties(EB-MON-007.frozen-observation-run
        PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE LABELS "runtime;synthetic;non-hardware")

    add_executable(epycbench_host_work_exec_entry_tests
        tests/unit/platform_linux_host_work_exec_entry_test.cpp)
    target_include_directories(epycbench_host_work_exec_entry_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_host_work_exec_entry_tests PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_host_work_exec_entry_tests)
    epycbench_enable_warnings(epycbench_host_work_exec_entry_tests)
    add_test(NAME EB-MON-007.host-preexec-entry COMMAND epycbench_host_work_exec_entry_tests)
    set_tests_properties(EB-MON-007.host-preexec-entry PROPERTIES TIMEOUT 30)

    # The exact 20GiB native ceiling cannot enclose ASan's virtual reservation.
    # Fake/pure production-algorithm coverage above remains sanitizer-enabled.
    if(NOT EPYCBENCH_ENABLE_ASAN_UBSAN AND NOT EPYCBENCH_ENABLE_TSAN)
        add_executable(epycbench_host_work_exec_probe tests/component/host_work_exec_probe.cpp)
        target_compile_features(epycbench_host_work_exec_probe PRIVATE cxx_std_23)
        target_compile_definitions(epycbench_host_work_exec_probe PRIVATE _GNU_SOURCE)
        target_link_libraries(epycbench_host_work_exec_probe PRIVATE Threads::Threads)
        epycbench_enable_build_options(epycbench_host_work_exec_probe)
        epycbench_enable_warnings(epycbench_host_work_exec_probe)
        add_test(NAME EB-MON-007.host-preexec-native COMMAND Python3::Interpreter -B
            "${PROJECT_SOURCE_DIR}/tests/tools/host_work_exec_entry_test.py"
            "$<TARGET_FILE:epycbench_host_work_exec_entry>" "$<TARGET_FILE:epycbench_host_work_exec_probe>")
        set_tests_properties(EB-MON-007.host-preexec-native PROPERTIES TIMEOUT 60)
        add_executable(epycbench_host_work_limits_native
            tests/component/host_work_limits_native.cpp)
        target_include_directories(epycbench_host_work_limits_native PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(epycbench_host_work_limits_native PRIVATE EpycBench::platform_linux)
        epycbench_enable_build_options(epycbench_host_work_limits_native)
        epycbench_enable_warnings(epycbench_host_work_limits_native)
        add_test(NAME EB-MON-007.host-work-limits-native COMMAND Python3::Interpreter -B
            "${PROJECT_SOURCE_DIR}/tests/tools/host_work_limits_native_test.py"
            "$<TARGET_FILE:epycbench_host_work_limits_native>")
        set_tests_properties(EB-MON-007.host-work-limits-native PROPERTIES TIMEOUT 25)
        foreach(source_workflow_mode IN ITEMS complete interrupt interrupt-after-q5 leader-first pre-drain cleanup-refusal)
            # The complete graph includes all384 actual Q6 cells and serial
            # publication checks. This supervisor bound is separate from the
            # unchanged production phase, PERMIT and original launch ends.
            set(source_workflow_timeout 180)
            set(source_workflow_ctest_timeout 210)
            if(source_workflow_mode STREQUAL "complete" OR source_workflow_mode STREQUAL "cleanup-refusal")
                set(source_workflow_timeout 300)
                set(source_workflow_ctest_timeout 330)
            endif()
            add_test(NAME EB-MON-007.owned-source-workflow-${source_workflow_mode}
                COMMAND Python3::Interpreter -I -B
                    "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_host_frozen_workflow_test.py"
                    --setup-binary "$<TARGET_FILE:epycbench_monitoring_host_q4_complete_q3_predecessor_authenticator_tests>"
                    --workflow-binary "$<TARGET_FILE:epycbench_runtime_frozen_observation_run_tests>"
                    --entry-binary "$<TARGET_FILE:epycbench_host_work_exec_entry>"
                    --artifact-parent "${PROJECT_BINARY_DIR}"
                    --mode "${source_workflow_mode}" --timeout "${source_workflow_timeout}")
            set_tests_properties(EB-MON-007.owned-source-workflow-${source_workflow_mode}
                PROPERTIES TIMEOUT "${source_workflow_ctest_timeout}" RUN_SERIAL TRUE
                LABELS "runtime;synthetic;non-hardware;contained-native;source-workflow")
        endforeach()
        foreach(governed_source_mode IN ITEMS complete interrupt cleanup-refusal)
            set(governed_source_timeout 300)
            set(governed_source_ctest_timeout 330)
            if(governed_source_mode STREQUAL "interrupt")
                set(governed_source_timeout 180)
                set(governed_source_ctest_timeout 210)
            endif()
            add_test(NAME EB-MON-007.governed-source-workflow-${governed_source_mode}
                COMMAND Python3::Interpreter -I -B
                    "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_host_frozen_workflow_test.py"
                    --setup-binary "$<TARGET_FILE:epycbench_monitoring_host_q4_complete_q3_predecessor_authenticator_tests>"
                    --workflow-binary "$<TARGET_FILE:epycbench_runtime_frozen_observation_run_tests>"
                    --entry-binary "$<TARGET_FILE:epycbench_host_work_exec_entry>"
                    --artifact-parent "${PROJECT_BINARY_DIR}"
                    --governed-window --mode "${governed_source_mode}" --timeout "${governed_source_timeout}")
            set_tests_properties(EB-MON-007.governed-source-workflow-${governed_source_mode}
                PROPERTIES TIMEOUT "${governed_source_ctest_timeout}" RUN_SERIAL TRUE
                LABELS "runtime;synthetic;non-hardware;contained-native;source-workflow;injected-policy")
        endforeach()
        add_test(NAME EB-MON-007.post-reboot-governed-source-workflow-complete
            COMMAND Python3::Interpreter -I -B
                "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_host_frozen_workflow_test.py"
                --setup-binary "$<TARGET_FILE:epycbench_monitoring_host_q4_complete_q3_predecessor_authenticator_tests>"
                --workflow-binary "$<TARGET_FILE:epycbench_runtime_frozen_observation_run_tests>"
                --entry-binary "$<TARGET_FILE:epycbench_host_work_exec_entry>"
                --artifact-parent "${PROJECT_BINARY_DIR}" --post-reboot-baseline
                --governed-window --mode complete --timeout 300)
        set_tests_properties(EB-MON-007.post-reboot-governed-source-workflow-complete
            PROPERTIES TIMEOUT 330 RUN_SERIAL TRUE
            LABELS "runtime;synthetic;non-hardware;contained-native;source-workflow;injected-policy")
        add_test(NAME EB-MON-007.governed-reboot-source-workflow-complete
            COMMAND Python3::Interpreter -I -B
                "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_host_frozen_workflow_test.py"
                --setup-binary "$<TARGET_FILE:epycbench_monitoring_host_q4_complete_q3_predecessor_authenticator_tests>"
                --workflow-binary "$<TARGET_FILE:epycbench_runtime_frozen_observation_run_tests>"
                --entry-binary "$<TARGET_FILE:epycbench_host_work_exec_entry>"
                --artifact-parent "${PROJECT_BINARY_DIR}" --governed-reboot-baseline
                --governed-window --mode complete --timeout 300)
        set_tests_properties(EB-MON-007.governed-reboot-source-workflow-complete
            PROPERTIES TIMEOUT 330 RUN_SERIAL TRUE
            LABELS "runtime;synthetic;non-hardware;contained-native;source-workflow;injected-policy")
        add_test(NAME EB-MON-007.environment-recovery-source-workflow-complete
            COMMAND Python3::Interpreter -I -B
                "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_host_frozen_workflow_test.py"
                --setup-binary "$<TARGET_FILE:epycbench_monitoring_host_q4_complete_q3_predecessor_authenticator_tests>"
                --workflow-binary "$<TARGET_FILE:epycbench_runtime_frozen_observation_run_tests>"
                --entry-binary "$<TARGET_FILE:epycbench_host_work_exec_entry>"
                --artifact-parent "${PROJECT_BINARY_DIR}" --environment-recovery-baseline
                --governed-window --mode complete --timeout 300)
        set_tests_properties(EB-MON-007.environment-recovery-source-workflow-complete
            PROPERTIES TIMEOUT 330 RUN_SERIAL TRUE
            LABELS "runtime;synthetic;non-hardware;contained-native;source-workflow;injected-policy")
        foreach(campaign_bridge_mode IN ITEMS complete readiness-interrupt pre-ack-interruption continuation-interrupt q4-continuation-interrupt q5-continuation-complete q5-continuation-interrupt q5-fresh-allowance-complete q5-fresh-allowance-interrupt)
            set(campaign_bridge_timeout 180)
            set(campaign_bridge_ctest_timeout 210)
            if(campaign_bridge_mode STREQUAL "complete" OR campaign_bridge_mode STREQUAL "q5-continuation-complete" OR campaign_bridge_mode STREQUAL "q5-fresh-allowance-complete")
                set(campaign_bridge_timeout 300)
                set(campaign_bridge_ctest_timeout 330)
            endif()
            add_test(NAME EB-MON-007.campaign-bridge-${campaign_bridge_mode}
                COMMAND Python3::Interpreter -I -B
                    "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_host_campaign_bridge_test.py"
                    --source-root "${PROJECT_SOURCE_DIR}"
                    --baseline-binary "$<TARGET_FILE:epycbench_monitoring_host_baseline_native_fixture>"
                    --workflow-binary "$<TARGET_FILE:epycbench_runtime_frozen_observation_run_tests>"
                    --entry-binary "$<TARGET_FILE:epycbench_host_work_exec_entry>"
                    --setup-binary "$<TARGET_FILE:epycbench_monitoring_host_q4_complete_q3_predecessor_authenticator_tests>"
                    --artifact-parent /tmp
                    --mode "${campaign_bridge_mode}" --timeout "${campaign_bridge_timeout}")
            set_tests_properties(EB-MON-007.campaign-bridge-${campaign_bridge_mode}
                PROPERTIES TIMEOUT "${campaign_bridge_ctest_timeout}" RUN_SERIAL TRUE
                LABELS "runtime;synthetic;non-hardware;contained-native;source-workflow;injected-policy")
        endforeach()
    endif()

    add_executable(epycbench_monitoring_host_q1_plan_v2_value_tests
        tests/unit/monitoring_host_q1_plan_v2_value_test.cpp)
    target_include_directories(epycbench_monitoring_host_q1_plan_v2_value_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_monitoring_host_q1_plan_v2_value_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(epycbench_monitoring_host_q1_plan_v2_value_tests
        PRIVATE EpycBench::monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q1_plan_v2_value_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q1_plan_v2_value_tests)
    add_test(NAME EB-MON-007.host-q1-plan-v2-value
        COMMAND epycbench_monitoring_host_q1_plan_v2_value_tests)

    add_executable(epycbench_monitoring_host_q1_git_command_owner_tests
        tests/unit/monitoring_host_q1_git_command_owner_test.cpp
        src/monitoring/host_q1_git_command_owner.cpp)
    target_include_directories(epycbench_monitoring_host_q1_git_command_owner_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_monitoring_host_q1_git_command_owner_tests
        PRIVATE EpycBench::core Catch2::Catch2WithMain)
    target_compile_definitions(epycbench_monitoring_host_q1_git_command_owner_tests
        PRIVATE EPYCBENCH_GIT_EXECUTABLE="$<TARGET_FILE:epycbench_monitoring_host_q1_git_command_owner_tests>")
    epycbench_enable_build_options(epycbench_monitoring_host_q1_git_command_owner_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q1_git_command_owner_tests)
    add_test(NAME EB-MON-007.host-q1-git-command-owner
        COMMAND epycbench_monitoring_host_q1_git_command_owner_tests)

    add_executable(epycbench_monitoring_host_q1_git_bounded_child_tests
        tests/unit/monitoring_host_q1_git_bounded_child_test.cpp)
    add_executable(epycbench_monitoring_host_q1_git_spawn_probe
        tests/support/monitoring_q1_git_spawn_probe.cpp)
    epycbench_enable_build_options(epycbench_monitoring_host_q1_git_spawn_probe)
    epycbench_enable_warnings(epycbench_monitoring_host_q1_git_spawn_probe)
    add_dependencies(epycbench_monitoring_host_q1_git_bounded_child_tests
        epycbench_monitoring_host_q1_git_spawn_probe)
    target_compile_definitions(epycbench_monitoring_host_q1_git_bounded_child_tests PRIVATE
        Q1_SPAWN_PROBE="$<TARGET_FILE:epycbench_monitoring_host_q1_git_spawn_probe>")
    target_include_directories(epycbench_monitoring_host_q1_git_bounded_child_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_monitoring_host_q1_git_bounded_child_tests
        PRIVATE EpycBench::monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q1_git_bounded_child_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q1_git_bounded_child_tests)
    add_test(NAME EB-MON-007.host-q1-git-bounded-child
        COMMAND epycbench_monitoring_host_q1_git_bounded_child_tests)

    add_executable(epycbench_monitoring_host_q1_git_repository_metadata_tests
        tests/unit/monitoring_host_q1_git_repository_metadata_test.cpp)
    target_include_directories(epycbench_monitoring_host_q1_git_repository_metadata_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_monitoring_host_q1_git_repository_metadata_tests
        PRIVATE EpycBench::monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q1_git_repository_metadata_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q1_git_repository_metadata_tests)
    add_test(NAME EB-MON-007.host-q1-git-repository-metadata
        COMMAND epycbench_monitoring_host_q1_git_repository_metadata_tests)

    add_executable(epycbench_monitoring_host_q1_git_fixed_results_tests
        tests/unit/monitoring_host_q1_git_fixed_results_test.cpp)
    target_include_directories(epycbench_monitoring_host_q1_git_fixed_results_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_monitoring_host_q1_git_fixed_results_tests
        PRIVATE EpycBench::monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q1_git_fixed_results_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q1_git_fixed_results_tests)
    add_test(NAME EB-MON-007.host-q1-git-fixed-results
        COMMAND epycbench_monitoring_host_q1_git_fixed_results_tests)
    add_executable(epycbench_monitoring_host_q1_public_input_authority_tests
        tests/unit/monitoring_host_q1_public_input_authority_test.cpp)
    target_include_directories(epycbench_monitoring_host_q1_public_input_authority_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q1_public_input_authority_tests
        PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q1_public_input_authority_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q1_public_input_authority_tests)
    add_test(NAME EB-MON-007.host-q1-public-input-authority
        COMMAND epycbench_monitoring_host_q1_public_input_authority_tests)
    add_executable(epycbench_monitoring_host_q1_validated_public_values_tests
        tests/unit/monitoring_host_q1_validated_public_values_test.cpp)
    target_include_directories(epycbench_monitoring_host_q1_validated_public_values_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q1_validated_public_values_tests
        PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    target_compile_definitions(epycbench_monitoring_host_q1_validated_public_values_tests
        PRIVATE Q1_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_monitoring_host_q1_validated_public_values_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q1_validated_public_values_tests)
    add_test(NAME EB-MON-007.host-q1-validated-public-values
        COMMAND epycbench_monitoring_host_q1_validated_public_values_tests)

    add_executable(epycbench_monitoring_host_q1_private_q0_value_tests
        tests/unit/monitoring_host_q1_private_q0_value_test.cpp)
    target_include_directories(epycbench_monitoring_host_q1_private_q0_value_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q1_private_q0_value_tests
        PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    target_compile_definitions(epycbench_monitoring_host_q1_private_q0_value_tests
        PRIVATE Q1_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_monitoring_host_q1_private_q0_value_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q1_private_q0_value_tests)
    add_test(NAME EB-MON-007.host-q1-private-q0-value
        COMMAND epycbench_monitoring_host_q1_private_q0_value_tests)

    set(EPYCBENCH_Q1_TEST_GIT "${CMAKE_CURRENT_BINARY_DIR}/epycbench-q1-test-git")
    configure_file("${GIT_EXECUTABLE}" "${EPYCBENCH_Q1_TEST_GIT}" COPYONLY)
    file(CHMOD "${EPYCBENCH_Q1_TEST_GIT}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    add_executable(epycbench_monitoring_host_q1_plan_v2_authority_session_tests
        tests/unit/monitoring_host_q1_plan_v2_authority_session_test.cpp
        src/monitoring/host_q1_git_command_owner.cpp
        src/monitoring/host_q1_plan_v2_authority_session.cpp)
    target_include_directories(epycbench_monitoring_host_q1_plan_v2_authority_session_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q1_plan_v2_authority_session_tests
        PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    target_compile_definitions(epycbench_monitoring_host_q1_plan_v2_authority_session_tests
        PRIVATE Q1_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                EPYCBENCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                EPYCBENCH_GIT_EXECUTABLE="${EPYCBENCH_Q1_TEST_GIT}")
    epycbench_enable_build_options(epycbench_monitoring_host_q1_plan_v2_authority_session_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q1_plan_v2_authority_session_tests)
    add_test(NAME EB-MON-007.host-q1-plan-v2-authority-session
        COMMAND epycbench_monitoring_host_q1_plan_v2_authority_session_tests)

    add_executable(epycbench_monitoring_host_q2_plan_value_tests
        tests/unit/monitoring_host_q2_plan_value_test.cpp)
    target_include_directories(epycbench_monitoring_host_q2_plan_value_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q2_plan_value_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    target_compile_definitions(epycbench_monitoring_host_q2_plan_value_tests PRIVATE Q2_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_monitoring_host_q2_plan_value_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q2_plan_value_tests)
    add_test(NAME EB-MON-003.host-q2-plan-value COMMAND epycbench_monitoring_host_q2_plan_value_tests)

    add_executable(epycbench_monitoring_host_q3_plan_value_tests
        tests/unit/monitoring_host_q3_plan_value_test.cpp)
    target_include_directories(epycbench_monitoring_host_q3_plan_value_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q3_plan_value_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    target_compile_definitions(epycbench_monitoring_host_q3_plan_value_tests PRIVATE Q3_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_monitoring_host_q3_plan_value_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q3_plan_value_tests)
    add_test(NAME EB-MON-004.host-q3-plan-value COMMAND epycbench_monitoring_host_q3_plan_value_tests)
    add_executable(epycbench_monitoring_host_q4_plan_value_tests
        tests/unit/monitoring_host_q4_plan_value_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_plan_value_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_plan_value_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    target_compile_definitions(epycbench_monitoring_host_q4_plan_value_tests PRIVATE Q4_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_monitoring_host_q4_plan_value_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_plan_value_tests)
    add_test(NAME EB-MON-005.host-q4-plan-value COMMAND epycbench_monitoring_host_q4_plan_value_tests)
    add_executable(epycbench_monitoring_host_q4_schema_bundle_tests
        tests/unit/monitoring_host_q4_schema_bundle_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_schema_bundle_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_schema_bundle_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    target_compile_definitions(epycbench_monitoring_host_q4_schema_bundle_tests PRIVATE Q4_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_monitoring_host_q4_schema_bundle_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_schema_bundle_tests)
    add_test(NAME EB-MON-005.host-q4-schema-bundle COMMAND epycbench_monitoring_host_q4_schema_bundle_tests)
    add_executable(epycbench_monitoring_host_q4_governed_plan_schema_binding_tests
        tests/unit/monitoring_host_q4_governed_plan_schema_binding_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_governed_plan_schema_binding_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_governed_plan_schema_binding_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    target_compile_definitions(epycbench_monitoring_host_q4_governed_plan_schema_binding_tests PRIVATE Q4_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_monitoring_host_q4_governed_plan_schema_binding_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_governed_plan_schema_binding_tests)
    add_test(NAME EB-MON-005.host-q4-governed-plan-schema-binding COMMAND epycbench_monitoring_host_q4_governed_plan_schema_binding_tests)
    add_executable(epycbench_monitoring_host_q4_composition_owner_tests
        tests/unit/monitoring_host_q4_composition_owner_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_composition_owner_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_composition_owner_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q4_composition_owner_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_composition_owner_tests)
    add_test(NAME EB-MON-005.host-q4-composition-owner COMMAND epycbench_monitoring_host_q4_composition_owner_tests)
    add_executable(epycbench_monitoring_host_q4_authority_failure_projection_tests
        tests/unit/monitoring_host_q4_authority_failure_projection_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_authority_failure_projection_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_authority_failure_projection_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q4_authority_failure_projection_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_authority_failure_projection_tests)
    add_test(NAME EB-MON-005.host-q4-authority-failure-projection COMMAND epycbench_monitoring_host_q4_authority_failure_projection_tests)
    add_executable(epycbench_monitoring_host_q4_authority_session_tests
        tests/unit/monitoring_host_q4_authority_session_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_authority_session_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_authority_session_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q4_authority_session_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_authority_session_tests)
    add_test(NAME EB-MON-005.host-q4-authority-session COMMAND epycbench_monitoring_host_q4_authority_session_tests)
    add_executable(epycbench_monitoring_host_q4_kernel_maximum_authority_tests
        tests/unit/monitoring_host_q4_kernel_maximum_authority_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_kernel_maximum_authority_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_kernel_maximum_authority_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q4_kernel_maximum_authority_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_kernel_maximum_authority_tests)
    add_test(NAME EB-MON-005.host-q4-kernel-maximum-authority COMMAND epycbench_monitoring_host_q4_kernel_maximum_authority_tests)
    add_executable(epycbench_monitoring_host_q4_source_ready_tests
        tests/unit/monitoring_host_q4_source_ready_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_source_ready_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_source_ready_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    target_compile_definitions(epycbench_monitoring_host_q4_source_ready_tests PRIVATE Q4_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_monitoring_host_q4_source_ready_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_source_ready_tests)
    add_test(NAME EB-MON-005.host-q4-source-ready COMMAND epycbench_monitoring_host_q4_source_ready_tests)
    add_executable(epycbench_monitoring_host_q4_cpufreq_inventory_authority_tests
        tests/unit/monitoring_host_q4_cpufreq_inventory_authority_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_cpufreq_inventory_authority_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_cpufreq_inventory_authority_tests
        PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q4_cpufreq_inventory_authority_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_cpufreq_inventory_authority_tests)
    add_test(NAME EB-MON-005.host-q4-cpufreq-inventory-authority
        COMMAND epycbench_monitoring_host_q4_cpufreq_inventory_authority_tests)
    add_executable(epycbench_monitoring_host_q4_cpufreq_semantic_agreement_tests
        tests/unit/monitoring_host_q4_cpufreq_semantic_agreement_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_cpufreq_semantic_agreement_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_cpufreq_semantic_agreement_tests
        PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q4_cpufreq_semantic_agreement_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_cpufreq_semantic_agreement_tests)
    add_test(NAME EB-MON-005.host-q4-cpufreq-semantic-agreement
        COMMAND epycbench_monitoring_host_q4_cpufreq_semantic_agreement_tests)
    add_executable(epycbench_monitoring_host_q4_cpufreq_prepared_source_authority_tests
        tests/unit/monitoring_host_q4_cpufreq_prepared_source_authority_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_cpufreq_prepared_source_authority_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_cpufreq_prepared_source_authority_tests
        PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    target_compile_definitions(epycbench_monitoring_host_q4_cpufreq_prepared_source_authority_tests
        PRIVATE Q4_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_monitoring_host_q4_cpufreq_prepared_source_authority_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_cpufreq_prepared_source_authority_tests)
    add_test(NAME EB-MON-005.host-q4-cpufreq-prepared-source-authority
        COMMAND epycbench_monitoring_host_q4_cpufreq_prepared_source_authority_tests)
    add_executable(epycbench_monitoring_current_revision_tracked_capture_tests
        tests/unit/monitoring_current_revision_tracked_capture_test.cpp)
    target_include_directories(epycbench_monitoring_current_revision_tracked_capture_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_current_revision_tracked_capture_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_current_revision_tracked_capture_tests)
    epycbench_enable_warnings(epycbench_monitoring_current_revision_tracked_capture_tests)
    add_test(NAME EB-MON-005.current-revision-tracked-capture COMMAND epycbench_monitoring_current_revision_tracked_capture_tests)
    add_executable(epycbench_monitoring_governed_tracked_bundle_tests
        tests/unit/monitoring_governed_tracked_bundle_test.cpp)
    target_include_directories(epycbench_monitoring_governed_tracked_bundle_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_governed_tracked_bundle_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_governed_tracked_bundle_tests)
    epycbench_enable_warnings(epycbench_monitoring_governed_tracked_bundle_tests)
    add_test(NAME EB-MON-005.governed-tracked-bundle COMMAND epycbench_monitoring_governed_tracked_bundle_tests)
    add_executable(epycbench_monitoring_host_q4_governed_input_authority_tests
        tests/unit/monitoring_host_q4_governed_input_authority_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_governed_input_authority_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_governed_input_authority_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q4_governed_input_authority_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_governed_input_authority_tests)
    add_test(NAME EB-MON-005.host-q4-governed-input-authority COMMAND epycbench_monitoring_host_q4_governed_input_authority_tests)
    add_executable(epycbench_monitoring_host_q4_historical_git_authority_tests
        tests/unit/monitoring_host_q4_historical_git_authority_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_historical_git_authority_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_historical_git_authority_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q4_historical_git_authority_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_historical_git_authority_tests)
    add_test(NAME EB-MON-005.host-q4-historical-git-authority COMMAND epycbench_monitoring_host_q4_historical_git_authority_tests)
    add_executable(epycbench_monitoring_host_q4_complete_q3_predecessor_authenticator_tests
        tests/unit/monitoring_host_q4_complete_q3_predecessor_authenticator_test.cpp
        tests/unit/monitoring_host_frozen_source_admission_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_complete_q3_predecessor_authenticator_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(epycbench_monitoring_host_q4_complete_q3_predecessor_authenticator_tests
        PRIVATE Q2_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" Q1_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" EPYCBENCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(epycbench_monitoring_host_q4_complete_q3_predecessor_authenticator_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q4_complete_q3_predecessor_authenticator_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_complete_q3_predecessor_authenticator_tests)
    add_test(NAME EB-MON-005.host-q4-complete-q3-predecessor-authenticator COMMAND epycbench_monitoring_host_q4_complete_q3_predecessor_authenticator_tests)
    set_tests_properties(EB-MON-007.q4-plan-authority PROPERTIES
        ENVIRONMENT "EPYCBENCH_CAMPAIGN_FIXTURE_BINARY=$<TARGET_FILE:epycbench_monitoring_host_q4_complete_q3_predecessor_authenticator_tests>")
    set_tests_properties(EB-MON-005.host-q4-complete-q3-predecessor-authenticator
        PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(epycbench_monitoring_host_q4_capacity_reasons_tests
        tests/unit/monitoring_host_q4_capacity_reasons_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_capacity_reasons_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_capacity_reasons_tests
        PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q4_capacity_reasons_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_capacity_reasons_tests)
    add_test(NAME EB-MON-005.host-q4-capacity-reasons
        COMMAND epycbench_monitoring_host_q4_capacity_reasons_tests)
    add_test(NAME EB-MON-005.host-q4-capacity-reasons-audit
        COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_q4_capacity_reasons_test.py")

    add_executable(epycbench_monitoring_host_q4_structural_capacity_tests
        tests/unit/monitoring_host_q4_structural_capacity_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_structural_capacity_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_structural_capacity_tests
        PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q4_structural_capacity_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_structural_capacity_tests)
    add_test(NAME EB-MON-005.host-q4-structural-capacity
        COMMAND epycbench_monitoring_host_q4_structural_capacity_tests)
    add_executable(epycbench_monitoring_host_q4_collection_capacity_inputs_tests
        tests/unit/monitoring_host_q4_collection_capacity_inputs_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_collection_capacity_inputs_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_collection_capacity_inputs_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q4_collection_capacity_inputs_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_collection_capacity_inputs_tests)
    add_test(NAME EB-MON-005.host-q4-collection-capacity-inputs COMMAND epycbench_monitoring_host_q4_collection_capacity_inputs_tests)
    add_executable(epycbench_monitoring_host_q4_interval_summary_capacity_tests
        tests/unit/monitoring_host_q4_interval_summary_capacity_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_interval_summary_capacity_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_interval_summary_capacity_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q4_interval_summary_capacity_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_interval_summary_capacity_tests)
    add_test(NAME EB-MON-005.host-q4-interval-summary-capacity COMMAND epycbench_monitoring_host_q4_interval_summary_capacity_tests)
    add_executable(epycbench_monitoring_host_q4_collection_capacity_tests
        tests/unit/monitoring_host_q4_collection_capacity_test.cpp)
    target_include_directories(epycbench_monitoring_host_q4_collection_capacity_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q4_collection_capacity_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q4_collection_capacity_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q4_collection_capacity_tests)
    add_test(NAME EB-MON-005.host-q4-collection-capacity COMMAND epycbench_monitoring_host_q4_collection_capacity_tests)

    add_executable(epycbench_monitoring_host_q3_temperature_parser_agreement_tests
        tests/unit/monitoring_host_q3_temperature_parser_agreement_test.cpp)
    target_include_directories(epycbench_monitoring_host_q3_temperature_parser_agreement_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q3_temperature_parser_agreement_tests
        PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q3_temperature_parser_agreement_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q3_temperature_parser_agreement_tests)
    add_test(NAME EB-MON-004.host-q3-temperature-parser-agreement
        COMMAND epycbench_monitoring_host_q3_temperature_parser_agreement_tests)

    add_executable(epycbench_monitoring_host_q3_temperature_cycle_authority_tests
        tests/unit/monitoring_host_q3_temperature_cycle_authority_test.cpp)
    target_include_directories(epycbench_monitoring_host_q3_temperature_cycle_authority_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q3_temperature_cycle_authority_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q3_temperature_cycle_authority_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q3_temperature_cycle_authority_tests)
    add_test(NAME EB-MON-004.host-q3-temperature-cycle-authority COMMAND epycbench_monitoring_host_q3_temperature_cycle_authority_tests)
    add_executable(epycbench_monitoring_host_q3_temperature_collector_tests tests/unit/monitoring_host_q3_temperature_collector_test.cpp)
    target_include_directories(epycbench_monitoring_host_q3_temperature_collector_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q3_temperature_collector_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q3_temperature_collector_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q3_temperature_collector_tests)
    add_test(NAME EB-MON-004.host-q3-temperature-collector COMMAND epycbench_monitoring_host_q3_temperature_collector_tests)
    add_executable(epycbench_monitoring_host_q3_temperature-production-adapter-tests tests/unit/monitoring_host_q3_temperature_production_adapter_test.cpp)
    target_include_directories(epycbench_monitoring_host_q3_temperature-production-adapter-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q3_temperature-production-adapter-tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q3_temperature-production-adapter-tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q3_temperature-production-adapter-tests)
    add_test(NAME EB-MON-004.host-q3-temperature-production-adapter COMMAND epycbench_monitoring_host_q3_temperature-production-adapter-tests)
    add_executable(epycbench_monitoring_host_q3_temperature_evidence_value_tests tests/unit/monitoring_host_q3_temperature_evidence_value_test.cpp)
    target_include_directories(epycbench_monitoring_host_q3_temperature_evidence_value_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q3_temperature_evidence_value_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q3_temperature_evidence_value_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q3_temperature_evidence_value_tests)
    add_test(NAME EB-MON-004.host-q3-temperature-evidence-value COMMAND epycbench_monitoring_host_q3_temperature_evidence_value_tests)
    add_executable(epycbench_monitoring_host_q3_backend_tests tests/unit/monitoring_host_q3_backend_test.cpp)
    target_include_directories(epycbench_monitoring_host_q3_backend_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q3_backend_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q3_backend_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q3_backend_tests)
    add_test(NAME EB-MON-004.host-q3-backend COMMAND epycbench_monitoring_host_q3_backend_tests)

    add_executable(epycbench_monitoring_host_q2_plan_public_input_authority_tests
        tests/unit/monitoring_host_q2_plan_public_input_authority_test.cpp)
    target_include_directories(epycbench_monitoring_host_q2_plan_public_input_authority_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q2_plan_public_input_authority_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q2_plan_public_input_authority_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q2_plan_public_input_authority_tests)
    add_test(NAME EB-MON-003.host-q2-plan-public-input-authority COMMAND epycbench_monitoring_host_q2_plan_public_input_authority_tests)

    add_executable(epycbench_monitoring_host_q3_plan_public_input_authority_tests
        tests/unit/monitoring_host_q3_plan_public_input_authority_test.cpp)
    target_include_directories(epycbench_monitoring_host_q3_plan_public_input_authority_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(epycbench_monitoring_host_q3_plan_public_input_authority_tests
        PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    target_compile_definitions(epycbench_monitoring_host_q3_plan_public_input_authority_tests
        PRIVATE Q3_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_monitoring_host_q3_plan_public_input_authority_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q3_plan_public_input_authority_tests)
    add_test(NAME EB-MON-004.host-q3-plan-public-input-authority
        COMMAND epycbench_monitoring_host_q3_plan_public_input_authority_tests)

    add_executable(epycbench_monitoring_host_q3_historical_git_authority_tests
        tests/unit/monitoring_host_q3_historical_git_authority_test.cpp)
    target_include_directories(epycbench_monitoring_host_q3_historical_git_authority_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q3_historical_git_authority_tests
        PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q3_historical_git_authority_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q3_historical_git_authority_tests)
    add_test(NAME EB-MON-004.host-q3-historical-git-authority
        COMMAND epycbench_monitoring_host_q3_historical_git_authority_tests)

    add_executable(epycbench_monitoring_host_q3_complete_q2_predecessor_authenticator_tests
        tests/unit/monitoring_host_q3_complete_q2_predecessor_authenticator_test.cpp)
    target_include_directories(epycbench_monitoring_host_q3_complete_q2_predecessor_authenticator_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(epycbench_monitoring_host_q3_complete_q2_predecessor_authenticator_tests
        PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    target_compile_definitions(epycbench_monitoring_host_q3_complete_q2_predecessor_authenticator_tests
        PRIVATE Q2_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" Q1_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_monitoring_host_q3_complete_q2_predecessor_authenticator_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q3_complete_q2_predecessor_authenticator_tests)
    add_test(NAME EB-MON-004.host-q3-complete-q2-predecessor-authenticator
        COMMAND epycbench_monitoring_host_q3_complete_q2_predecessor_authenticator_tests)

    add_executable(epycbench_monitoring_host_q3_composition_owner_tests
        tests/unit/monitoring_host_q3_composition_owner_test.cpp)
    target_include_directories(epycbench_monitoring_host_q3_composition_owner_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q3_composition_owner_tests
        PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q3_composition_owner_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q3_composition_owner_tests)
    add_test(NAME EB-MON-004.host-q3-composition-owner
        COMMAND epycbench_monitoring_host_q3_composition_owner_tests)

    add_executable(epycbench_monitoring_host_q2_authority_session_tests
        tests/unit/monitoring_host_q2_authority_session_test.cpp)
    target_include_directories(epycbench_monitoring_host_q2_authority_session_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(epycbench_monitoring_host_q2_authority_session_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q2_authority_session_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q2_authority_session_tests)
    target_compile_definitions(epycbench_monitoring_host_q2_authority_session_tests
        PRIVATE Q1_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" EPYCBENCH_GIT_EXECUTABLE="${EPYCBENCH_Q1_TEST_GIT}")
    if(EPYCBENCH_ENABLE_TSAN)
        target_compile_definitions(epycbench_monitoring_host_q2_authority_session_tests
            PRIVATE EPYCBENCH_Q2_CONCURRENCY_TSAN=1)
        add_test(NAME EB-MON-003.host-q2-authority-session
            COMMAND epycbench_monitoring_host_q2_authority_session_tests "[q2-cancellation-borrow]")
    else()
        add_test(NAME EB-MON-003.host-q2-authority-session COMMAND epycbench_monitoring_host_q2_authority_session_tests)
    endif()
    add_executable(epycbench_monitoring_host_q2_controlled_event_session_tests tests/unit/monitoring_host_q2_controlled_event_session_test.cpp)
    target_include_directories(epycbench_monitoring_host_q2_controlled_event_session_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q2_controlled_event_session_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q2_controlled_event_session_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q2_controlled_event_session_tests)
    if(EPYCBENCH_ENABLE_TSAN)
        # The configured TSan runtime owns allocation interceptors. Keep the
        # allocation-fault scopes in GCC/Clang/ASan and select actual helper waits here.
        target_compile_definitions(epycbench_monitoring_host_q2_controlled_event_session_tests
            PRIVATE EPYCBENCH_Q2_HELPER_TSAN=1)
        add_test(NAME EB-MON-003.host-q2-controlled-event-session
            COMMAND epycbench_monitoring_host_q2_controlled_event_session_tests "[q2-actual-helper]")
    else()
        add_test(NAME EB-MON-003.host-q2-controlled-event-session
            COMMAND epycbench_monitoring_host_q2_controlled_event_session_tests)
    endif()

    add_executable(epycbench_monitoring_host_q1_page_fault_collector_tests
        tests/unit/monitoring_host_q1_page_fault_collector_test.cpp)
    target_include_directories(epycbench_monitoring_host_q1_page_fault_collector_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(epycbench_monitoring_host_q1_page_fault_collector_tests
        PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q1_page_fault_collector_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q1_page_fault_collector_tests)
    add_test(NAME EB-MON-007.host-q1-page-fault-collector
        COMMAND epycbench_monitoring_host_q1_page_fault_collector_tests)
    add_executable(epycbench_monitoring_host_q2_complete_q1_predecessor_authority_tests
        tests/unit/monitoring_host_q2_complete_q1_predecessor_authority_test.cpp)
    target_include_directories(epycbench_monitoring_host_q2_complete_q1_predecessor_authority_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_monitoring_host_q2_complete_q1_predecessor_authority_tests
        PRIVATE Q2_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(epycbench_monitoring_host_q2_complete_q1_predecessor_authority_tests
        PRIVATE EpycBench::monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q2_complete_q1_predecessor_authority_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q2_complete_q1_predecessor_authority_tests)
    add_test(NAME monitoring.host_q2_complete_q1_predecessor_authority
        COMMAND epycbench_monitoring_host_q2_complete_q1_predecessor_authority_tests)

    add_executable(epycbench_monitoring_host_q1_backend_tests
        tests/unit/monitoring_host_q1_backend_test.cpp
        src/monitoring/host_q1_git_command_owner.cpp
        src/monitoring/host_q1_plan_v2_authority_session.cpp)
    target_include_directories(epycbench_monitoring_host_q1_backend_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q1_backend_tests
        PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    target_compile_definitions(epycbench_monitoring_host_q1_backend_tests
        PRIVATE Q1_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                EPYCBENCH_GIT_EXECUTABLE="${EPYCBENCH_Q1_TEST_GIT}")
    epycbench_enable_build_options(epycbench_monitoring_host_q1_backend_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q1_backend_tests)
    add_test(NAME EB-MON-007.host-q1-backend
        COMMAND epycbench_monitoring_host_q1_backend_tests)
    add_test(NAME EB-MON-007.host-q1-cpp-python-acceptance
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/monitoring_q1_cpp_backend_bridge_test.py"
                "$<TARGET_FILE:epycbench_monitoring_host_q1_backend_tests>"
                "${CMAKE_CURRENT_SOURCE_DIR}")

    add_executable(epycbench_monitoring_q1_command_tests
        tests/unit/monitoring_q1_command_test.cpp)
    target_include_directories(epycbench_monitoring_q1_command_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_q1_command_tests
        PRIVATE epycbench_cli Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_q1_command_tests)
    epycbench_enable_warnings(epycbench_monitoring_q1_command_tests)
    add_test(NAME EB-MON-007.host-q1-command
        COMMAND epycbench_monitoring_q1_command_tests)

    add_executable(epycbench_monitoring_q2_command_tests tests/unit/monitoring_q2_command_test.cpp)
    target_include_directories(epycbench_monitoring_q2_command_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_q2_command_tests PRIVATE epycbench_cli Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_q2_command_tests)
    epycbench_enable_warnings(epycbench_monitoring_q2_command_tests)
    add_test(NAME EB-MON-007.host-q2-command COMMAND epycbench_monitoring_q2_command_tests)

    add_executable(epycbench_monitoring_q3_command_tests tests/unit/monitoring_q3_command_test.cpp)
    target_include_directories(epycbench_monitoring_q3_command_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_q3_command_tests PRIVATE epycbench_cli Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_q3_command_tests)
    epycbench_enable_warnings(epycbench_monitoring_q3_command_tests)
    add_test(NAME EB-MON-007.host-q3-command COMMAND epycbench_monitoring_q3_command_tests)

    add_executable(epycbench_monitoring_q4_command_tests tests/unit/monitoring_q4_command_test.cpp)
    target_include_directories(epycbench_monitoring_q4_command_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_q4_command_tests PRIVATE epycbench_cli Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_q4_command_tests)
    epycbench_enable_warnings(epycbench_monitoring_q4_command_tests)
    add_test(NAME EB-MON-007.host-q4-command COMMAND epycbench_monitoring_q4_command_tests)

    add_executable(epycbench_monitoring_host_q2_backend_tests
        tests/unit/monitoring_host_q2_backend_test.cpp)
    target_include_directories(epycbench_monitoring_host_q2_backend_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_host_q2_backend_tests PRIVATE epycbench_monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_host_q2_backend_tests)
    epycbench_enable_warnings(epycbench_monitoring_host_q2_backend_tests)
    add_test(NAME EB-MON-007.host-q2-backend COMMAND epycbench_monitoring_host_q2_backend_tests)

    add_executable(epycbench_monitoring_q0b_backend_result_tests
        tests/unit/monitoring_q0b_backend_result_test.cpp)
    target_link_libraries(epycbench_monitoring_q0b_backend_result_tests
        PRIVATE EpycBench::monitoring_qualification Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_q0b_backend_result_tests)
    epycbench_enable_warnings(epycbench_monitoring_q0b_backend_result_tests)
    add_test(NAME EB-MON-007.q0b-backend-result
        COMMAND epycbench_monitoring_q0b_backend_result_tests)

    add_executable(
        epycbench_monitoring_acquisition_driver_tests
        tests/unit/monitoring_acquisition_driver_test.cpp
        tests/component/monitoring_acquisition_driver_component_test.cpp
    )
    target_include_directories(epycbench_monitoring_acquisition_driver_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        epycbench_monitoring_acquisition_driver_tests
        PRIVATE EpycBench::monitoring Catch2::Catch2WithMain Threads::Threads
    )
    epycbench_enable_build_options(epycbench_monitoring_acquisition_driver_tests)
    epycbench_enable_warnings(epycbench_monitoring_acquisition_driver_tests)
    add_test(
        NAME MON-prerequisite.acquisition-driver
        COMMAND epycbench_monitoring_acquisition_driver_tests
    )

    add_executable(epycbench_monitoring_linux_interruptible_scheduler_tests
        tests/unit/monitoring_linux_interruptible_scheduler_test.cpp)
    target_include_directories(epycbench_monitoring_linux_interruptible_scheduler_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_monitoring_linux_interruptible_scheduler_tests
        PRIVATE EpycBench::monitoring Catch2::Catch2WithMain Threads::Threads)
    epycbench_enable_build_options(epycbench_monitoring_linux_interruptible_scheduler_tests)
    epycbench_enable_warnings(epycbench_monitoring_linux_interruptible_scheduler_tests)
    add_test(NAME MON-prerequisite.interruptible-raw-monotonic-scheduler
        COMMAND epycbench_monitoring_linux_interruptible_scheduler_tests)

    add_executable(epycbench_monitoring_linux_driver_operations_provider_tests
        tests/unit/monitoring_linux_driver_operations_provider_test.cpp)
    target_include_directories(epycbench_monitoring_linux_driver_operations_provider_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_monitoring_linux_driver_operations_provider_tests
        PRIVATE EpycBench::monitoring Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_monitoring_linux_driver_operations_provider_tests)
    epycbench_enable_warnings(epycbench_monitoring_linux_driver_operations_provider_tests)
    add_test(NAME MON-prerequisite.linux-driver-operations-provider
        COMMAND epycbench_monitoring_linux_driver_operations_provider_tests)

    add_executable(epycbench_runtime_linux_pairwise_host_operations_provider_tests
        tests/unit/runtime_linux_pairwise_production_host_operations_provider_test.cpp)
    target_include_directories(epycbench_runtime_linux_pairwise_host_operations_provider_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_runtime_linux_pairwise_host_operations_provider_tests
        PRIVATE EpycBench::runtime_production_backend Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_runtime_linux_pairwise_host_operations_provider_tests)
    epycbench_enable_warnings(epycbench_runtime_linux_pairwise_host_operations_provider_tests)
    add_test(NAME RT-prerequisite.linux-pairwise-production-host-operations-provider
        COMMAND epycbench_runtime_linux_pairwise_host_operations_provider_tests)

    add_executable(epycbench_runtime_pairwise_platform_leaf_facade_tests
        tests/unit/runtime_pairwise_platform_leaf_facade_test.cpp)
    target_include_directories(epycbench_runtime_pairwise_platform_leaf_facade_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_runtime_pairwise_platform_leaf_facade_tests
        PRIVATE EpycBench::runtime_production_backend Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_runtime_pairwise_platform_leaf_facade_tests)
    epycbench_enable_warnings(epycbench_runtime_pairwise_platform_leaf_facade_tests)
    add_test(NAME RT-prerequisite.pairwise-platform-leaf-facades
        COMMAND epycbench_runtime_pairwise_platform_leaf_facade_tests)
    add_test(NAME RT-prerequisite.prepared-bounded-hwmon-discovery-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/prepared_bounded_hwmon_discovery_audit_test.py")
    set_tests_properties(RT-prerequisite.prepared-bounded-hwmon-discovery-audit
        PROPERTIES LABELS "runtime;synthetic;non-hardware;audit")
    add_test(NAME RT-prerequisite.prepared-bounded-cpu-frequency-discovery-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/prepared_bounded_cpu_frequency_discovery_audit_test.py")
    set_tests_properties(
        RT-prerequisite.prepared-bounded-cpu-frequency-discovery-audit
        PROPERTIES LABELS "runtime;synthetic;non-hardware;audit")
    add_test(NAME RT-prerequisite.prepared-platform-monitoring-discovery-audit
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/tools/prepared_platform_monitoring_discovery_audit_test.py")
    set_tests_properties(
        RT-prerequisite.prepared-platform-monitoring-discovery-audit
        PROPERTIES LABELS "runtime;synthetic;non-hardware;audit")
    add_executable(epycbench_runtime_pairwise_statistics_leaf_facade_tests
        tests/unit/runtime_pairwise_statistics_leaf_facade_test.cpp)
    target_include_directories(epycbench_runtime_pairwise_statistics_leaf_facade_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_runtime_pairwise_statistics_leaf_facade_tests
        PRIVATE EpycBench::runtime_production_backend Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_runtime_pairwise_statistics_leaf_facade_tests)
    epycbench_enable_warnings(epycbench_runtime_pairwise_statistics_leaf_facade_tests)
    add_test(NAME RT-prerequisite.pairwise-statistics-leaf-facades
        COMMAND epycbench_runtime_pairwise_statistics_leaf_facade_tests)

    add_executable(epycbench_runtime_pairwise_kernel_leaf_facade_tests
        tests/unit/runtime_pairwise_kernel_leaf_facade_test.cpp)
    target_include_directories(epycbench_runtime_pairwise_kernel_leaf_facade_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_runtime_pairwise_kernel_leaf_facade_tests
        PRIVATE EpycBench::runtime_production_backend Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_runtime_pairwise_kernel_leaf_facade_tests)
    epycbench_enable_warnings(epycbench_runtime_pairwise_kernel_leaf_facade_tests)
    add_test(NAME RT-prerequisite.pairwise-kernel-leaf-facades
        COMMAND epycbench_runtime_pairwise_kernel_leaf_facade_tests)

    add_executable(epycbench_runtime_pairwise_measurement_leaf_facade_tests
        tests/unit/runtime_pairwise_measurement_leaf_facade_test.cpp)
    target_include_directories(epycbench_runtime_pairwise_measurement_leaf_facade_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_runtime_pairwise_measurement_leaf_facade_tests
        PRIVATE EpycBench::runtime_production_backend Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_runtime_pairwise_measurement_leaf_facade_tests)
    epycbench_enable_warnings(epycbench_runtime_pairwise_measurement_leaf_facade_tests)
    add_test(NAME RT-prerequisite.pairwise-measurement-leaf-facades
        COMMAND epycbench_runtime_pairwise_measurement_leaf_facade_tests)

    add_executable(epycbench_runtime_pairwise_memory_owning_facade_tests
        tests/unit/runtime_pairwise_memory_owning_facade_test.cpp)
    target_include_directories(epycbench_runtime_pairwise_memory_owning_facade_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_runtime_pairwise_memory_owning_facade_tests
        PRIVATE EpycBench::runtime_production_backend Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_runtime_pairwise_memory_owning_facade_tests)
    epycbench_enable_warnings(epycbench_runtime_pairwise_memory_owning_facade_tests)
    add_test(NAME RT-prerequisite.pairwise-memory-owning-facades
        COMMAND epycbench_runtime_pairwise_memory_owning_facade_tests)

    add_executable(epycbench_runtime_pairwise_worker_owning_facade_tests
        tests/unit/runtime_pairwise_worker_owning_facade_test.cpp)
    target_include_directories(epycbench_runtime_pairwise_worker_owning_facade_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}")
    target_link_libraries(epycbench_runtime_pairwise_worker_owning_facade_tests
        PRIVATE EpycBench::runtime_production_backend Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_runtime_pairwise_worker_owning_facade_tests)
    epycbench_enable_warnings(epycbench_runtime_pairwise_worker_owning_facade_tests)
    add_test(NAME RT-prerequisite.pairwise-worker-owning-facades
        COMMAND epycbench_runtime_pairwise_worker_owning_facade_tests)

    add_executable(epycbench_runtime_pairwise_monitoring_owning_facade_tests
        tests/unit/runtime_pairwise_monitoring_owning_facade_test.cpp)
    target_include_directories(epycbench_runtime_pairwise_monitoring_owning_facade_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_runtime_pairwise_monitoring_owning_facade_tests
        PRIVATE epycbench_runtime_production_backend Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_runtime_pairwise_monitoring_owning_facade_tests)
    epycbench_enable_warnings(epycbench_runtime_pairwise_monitoring_owning_facade_tests)
    add_test(NAME RT-prerequisite.pairwise-monitoring-owning-facades
        COMMAND epycbench_runtime_pairwise_monitoring_owning_facade_tests)

    add_executable(epycbench_runtime_pairwise_monitoring_host_capacity_startup_tests
        tests/unit/runtime_pairwise_monitoring_host_capacity_startup_test.cpp)
    target_include_directories(epycbench_runtime_pairwise_monitoring_host_capacity_startup_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(epycbench_runtime_pairwise_monitoring_host_capacity_startup_tests
        PRIVATE EpycBench::pairwise_monitoring_host_capacity_startup
                EpycBench::finalized_evidence_wire_source_projection
                EpycBench::finalized_evidence_wire_startup_codec
                EpycBench::finalized_evidence_prepared_resources
                EpycBench::pairwise_v2_prepared_resource_session
                EpycBench::pairwise_v2_resource_startup_protocol
                EpycBench::pairwise_v2_composed_startup
                EpycBench::pairwise_v2_active_evidence
                EpycBench::pairwise_v2_synthetic_lifecycle
                epycbench_results_process_subject
                Catch2::Catch2WithMain)
    epycbench_enable_build_options(
        epycbench_runtime_pairwise_monitoring_host_capacity_startup_tests)
    epycbench_enable_warnings(
        epycbench_runtime_pairwise_monitoring_host_capacity_startup_tests)
    add_test(NAME RT-prerequisite.pairwise-monitoring-host-capacity-startup
        COMMAND epycbench_runtime_pairwise_monitoring_host_capacity_startup_tests)

    add_executable(epycbench_finalized_evidence_wire_capacity_tests
        tests/unit/runtime_finalized_evidence_wire_capacity_test.cpp)
    target_include_directories(epycbench_finalized_evidence_wire_capacity_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_finalized_evidence_wire_capacity_tests
        PRIVATE EpycBench::finalized_evidence_wire_capacity Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_finalized_evidence_wire_capacity_tests)
    epycbench_enable_warnings(epycbench_finalized_evidence_wire_capacity_tests)
    add_test(NAME RT-prerequisite.finalized-evidence-wire-capacity
        COMMAND epycbench_finalized_evidence_wire_capacity_tests)
    add_test(NAME RT-prerequisite.finalized-evidence-wire-schema-generation
        COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tools/generate-finalized-evidence-wire-schema.py"
            --check)
    add_test(NAME RT-prerequisite.finalized-evidence-wire-source-projection-audit
        COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/tools/finalized_evidence_wire_source_projection_test.py")
    add_test(NAME RT-prerequisite.finalized-evidence-prepared-resources-generation
        COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/tools/finalized_evidence_prepared_resources_generator_test.py")
    add_test(NAME RT-prerequisite.finalized-evidence-typed-traversal-generation
        COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/tools/finalized_pairwise_evidence_typed_traversal_test.py")

    add_executable(epycbench_pairwise_v2_progress_tests
        tests/unit/runtime_pairwise_v2_progress_test.cpp)
    target_include_directories(epycbench_pairwise_v2_progress_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_pairwise_v2_progress_tests
        PRIVATE EpycBench::pairwise_v2_progress EpycBench::pairwise_v2_composed_startup Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_pairwise_v2_progress_tests)
    epycbench_enable_warnings(epycbench_pairwise_v2_progress_tests)
    add_test(NAME P1.pairwise-v2-progress-pure COMMAND epycbench_pairwise_v2_progress_tests)

    add_executable(epycbench_finalized_evidence_prepared_direct_codec_tests
        tests/unit/runtime_finalized_evidence_prepared_direct_codec_test.cpp)
    target_include_directories(epycbench_finalized_evidence_prepared_direct_codec_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR})
    target_compile_definitions(epycbench_finalized_evidence_prepared_direct_codec_tests
        PRIVATE EPYCBENCH_MONITORING_FACT_TEST=1 EPYCBENCH_PREPARATION_GRAPH_TEST=1)
    target_link_libraries(epycbench_finalized_evidence_prepared_direct_codec_tests
        PRIVATE EpycBench::finalized_evidence_prepared_direct_codec EpycBench::results
                EpycBench::finalized_evidence_wire_typed_reconstruction
                Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_finalized_evidence_prepared_direct_codec_tests)
    epycbench_enable_warnings(epycbench_finalized_evidence_prepared_direct_codec_tests)
    add_test(NAME RT-prerequisite.finalized-evidence-prepared-direct-codec
        COMMAND epycbench_finalized_evidence_prepared_direct_codec_tests)

    add_executable(epycbench_pairwise_v2_native_lifecycle_component
        tests/component/pairwise_v2_native_lifecycle.cpp)
    target_include_directories(epycbench_pairwise_v2_native_lifecycle_component
        PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests/unit)
    target_link_libraries(epycbench_pairwise_v2_native_lifecycle_component
        PRIVATE EpycBench::pairwise_v2_native_lifecycle EpycBench::runtime_ipc_payloads Threads::Threads)
    epycbench_enable_build_options(epycbench_pairwise_v2_native_lifecycle_component)
    epycbench_enable_warnings(epycbench_pairwise_v2_native_lifecycle_component)
    # Native execution remains an explicit reviewed component operation, not default CTest.

    add_test(NAME P1.pairwise-v2-native-lifecycle-harness
        COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tests/tools/pairwise_v2_native_lifecycle_test.py" --self-test)
    set_tests_properties(P1.pairwise-v2-native-lifecycle-harness PROPERTIES TIMEOUT 300 LABELS non-hardware)

    add_executable(epycbench_pairwise_v2_prepared_production_execution_tests
        tests/unit/runtime_pairwise_v2_prepared_production_execution_test.cpp)
    target_include_directories(epycbench_pairwise_v2_prepared_production_execution_tests
        PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_pairwise_v2_prepared_production_execution_tests
        PRIVATE EpycBench::pairwise_v2_native_lifecycle Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_pairwise_v2_prepared_production_execution_tests)
    epycbench_enable_warnings(epycbench_pairwise_v2_prepared_production_execution_tests)
    add_test(NAME P1.pairwise-v2-prepared-production-execution
        COMMAND epycbench_pairwise_v2_prepared_production_execution_tests)
    set_tests_properties(P1.pairwise-v2-prepared-production-execution PROPERTIES TIMEOUT 300 LABELS non-hardware)

    add_executable(epycbench_benchmark_run_composition_tests tests/unit/cli_benchmark_run_composition_test.cpp)
    target_include_directories(epycbench_benchmark_run_composition_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_benchmark_run_composition_tests PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_benchmark_run_composition_tests)
    epycbench_enable_warnings(epycbench_benchmark_run_composition_tests)
    add_test(NAME P1.benchmark-run-composition-pure COMMAND epycbench_benchmark_run_composition_tests)
    set_tests_properties(P1.benchmark-run-composition-pure PROPERTIES TIMEOUT 300)
    add_executable(epycbench_pairwise_v2_production_entry_tests
        tests/unit/runtime_pairwise_v2_production_entry_test.cpp)
    target_include_directories(epycbench_pairwise_v2_production_entry_tests
        PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests ${PROJECT_SOURCE_DIR}/tests/unit)
    target_link_libraries(epycbench_pairwise_v2_production_entry_tests
        PRIVATE epycbench_stage_b_resource_policy epycbench_pairwise_v2_production_entry EpycBench::pairwise_v2_native_lifecycle Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_pairwise_v2_production_entry_tests)
    epycbench_enable_warnings(epycbench_pairwise_v2_production_entry_tests)
    add_test(NAME P1.pairwise-v2-production-entry-pure
        COMMAND epycbench_pairwise_v2_production_entry_tests)
    set_tests_properties(P1.pairwise-v2-production-entry-pure PROPERTIES
        TIMEOUT 300 LABELS non-hardware)

    add_executable(epycbench_pairwise_v2_native_lifecycle_tests
        tests/unit/runtime_pairwise_v2_native_lifecycle_test.cpp)
    target_include_directories(epycbench_pairwise_v2_native_lifecycle_tests
        PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests ${PROJECT_SOURCE_DIR}/tests/unit)
    target_link_libraries(epycbench_pairwise_v2_native_lifecycle_tests
        PRIVATE EpycBench::pairwise_v2_native_lifecycle Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_pairwise_v2_native_lifecycle_tests)
    epycbench_enable_warnings(epycbench_pairwise_v2_native_lifecycle_tests)
    add_test(NAME P1.pairwise-v2-native-lifecycle-pure COMMAND epycbench_pairwise_v2_native_lifecycle_tests)
    set_tests_properties(P1.pairwise-v2-native-lifecycle-pure PROPERTIES TIMEOUT 300 LABELS non-hardware)

    add_executable(epycbench_finalized_evidence_transport_tests
        tests/unit/runtime_finalized_evidence_transport_test.cpp)
    target_include_directories(epycbench_finalized_evidence_transport_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_finalized_evidence_transport_tests
        PRIVATE EpycBench::finalized_evidence_transport Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_finalized_evidence_transport_tests)
    epycbench_enable_warnings(epycbench_finalized_evidence_transport_tests)
    add_test(NAME RT-prerequisite.finalized-evidence-transport
        COMMAND epycbench_finalized_evidence_transport_tests)

    add_test(NAME EB-RES-002.incremental-block-evidence-audit
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_incremental_block_evidence_test.py")
    add_test(NAME RT-prerequisite.finalized-evidence-root-reconstruction-audit
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/finalized_evidence_root_reconstruction_test.py)
    add_test(NAME RT-prerequisite.finalized-evidence-root-projection-audit
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/finalized_evidence_root_projection_test.py")
    add_test(NAME RT-prerequisite.finalized-evidence-component-extraction-audit
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/finalized_evidence_component_extraction_test.py")
    set_tests_properties(RT-prerequisite.finalized-evidence-component-extraction-audit
        PROPERTIES LABELS "runtime;non-hardware;audit")
    add_executable(epycbench_finalized_evidence_wire_json_ownership_tests
        tests/unit/runtime_finalized_evidence_wire_json_ownership_test.cpp)
    target_include_directories(epycbench_finalized_evidence_wire_json_ownership_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(epycbench_finalized_evidence_wire_json_ownership_tests
        PRIVATE EpycBench::finalized_evidence_wire_typed_reconstruction
                EpycBench::results Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_finalized_evidence_wire_json_ownership_tests)
    epycbench_enable_warnings(epycbench_finalized_evidence_wire_json_ownership_tests)
    add_test(NAME RT-prerequisite.finalized-evidence-wire-json-ownership
        COMMAND epycbench_finalized_evidence_wire_json_ownership_tests)

    add_executable(epycbench_finalized_evidence_wire_logical_codec_tests
        tests/unit/runtime_finalized_evidence_wire_logical_codec_test.cpp)
    target_include_directories(epycbench_finalized_evidence_wire_logical_codec_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_finalized_evidence_wire_logical_codec_tests
        PRIVATE EpycBench::finalized_evidence_wire_logical_codec
                EpycBench::finalized_evidence_wire_typed_reconstruction
                EpycBench::results Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_finalized_evidence_wire_logical_codec_tests)
    epycbench_enable_warnings(epycbench_finalized_evidence_wire_logical_codec_tests)
    add_test(NAME RT-prerequisite.finalized-evidence-wire-logical-codec
        COMMAND epycbench_finalized_evidence_wire_logical_codec_tests)

    add_executable(epycbench_finalized_evidence_wire_typed_reconstruction_tests
        tests/unit/runtime_finalized_evidence_wire_typed_reconstruction_test.cpp)
    target_include_directories(epycbench_finalized_evidence_wire_typed_reconstruction_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(epycbench_finalized_evidence_wire_typed_reconstruction_tests
        PRIVATE EpycBench::finalized_evidence_wire_typed_reconstruction
                EpycBench::finalized_evidence_prepared_direct_codec
                EpycBench::results
                Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_finalized_evidence_wire_typed_reconstruction_tests)
    epycbench_enable_warnings(epycbench_finalized_evidence_wire_typed_reconstruction_tests)
    add_test(NAME RT-prerequisite.finalized-evidence-wire-typed-reconstruction
        COMMAND epycbench_finalized_evidence_wire_typed_reconstruction_tests)

    add_executable(epycbench_runtime_pairwise_v2_phase_a_protocol_tests
        tests/unit/runtime_pairwise_v2_phase_a_protocol_test.cpp)
    target_include_directories(epycbench_runtime_pairwise_v2_phase_a_protocol_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(epycbench_runtime_pairwise_v2_phase_a_protocol_tests
        PRIVATE EpycBench::pairwise_v2_controller_session
                EpycBench::pairwise_v2_prepared_evidence_session
                EpycBench::runtime_production_backend Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_runtime_pairwise_v2_phase_a_protocol_tests)
    epycbench_enable_warnings(epycbench_runtime_pairwise_v2_phase_a_protocol_tests)
    add_test(NAME RT-prerequisite.pairwise-v2-phase-a-protocol
        COMMAND epycbench_runtime_pairwise_v2_phase_a_protocol_tests)

    add_executable(
        epycbench_scenario_kernel_dispatch_tests
        tests/unit/scenario_kernel_dispatch_test.cpp
    )
    target_link_libraries(
        epycbench_scenario_kernel_dispatch_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    target_include_directories(
        epycbench_scenario_kernel_dispatch_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    epycbench_enable_build_options(epycbench_scenario_kernel_dispatch_tests)
    epycbench_enable_warnings(epycbench_scenario_kernel_dispatch_tests)
    add_test(
        NAME EB-SCN-008.kernel-dispatch
        COMMAND epycbench_scenario_kernel_dispatch_tests
    )

    add_executable(
        epycbench_scenario_kernel_binding_projection_tests
        tests/unit/scenario_kernel_binding_projection_test.cpp
    )
    target_link_libraries(
        epycbench_scenario_kernel_binding_projection_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_scenario_kernel_binding_projection_tests)
    epycbench_enable_warnings(epycbench_scenario_kernel_binding_projection_tests)
    add_test(
        NAME EB-SCN-008.kernel-binding-projection
        COMMAND epycbench_scenario_kernel_binding_projection_tests
    )

    add_executable(
        epycbench_scenario_matrix_expansion_tests
        tests/unit/scenario_matrix_expansion_test.cpp
    )
    target_link_libraries(
        epycbench_scenario_matrix_expansion_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    target_include_directories(
        epycbench_scenario_matrix_expansion_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    epycbench_enable_build_options(epycbench_scenario_matrix_expansion_tests)
    epycbench_enable_warnings(epycbench_scenario_matrix_expansion_tests)
    add_test(
        NAME EB-SCN-009.matrix-expansion
        COMMAND epycbench_scenario_matrix_expansion_tests
    )

    add_executable(
        epycbench_scenario_matrix_fixture_tests
        tests/component/scenario_matrix_fixture_test.cpp
    )
    target_link_libraries(
        epycbench_scenario_matrix_fixture_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    target_compile_definitions(
        epycbench_scenario_matrix_fixture_tests
        PRIVATE EPYCBENCH_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    )
    epycbench_enable_build_options(epycbench_scenario_matrix_fixture_tests)
    epycbench_enable_warnings(epycbench_scenario_matrix_fixture_tests)
    add_test(
        NAME EB-SCN-009.reference-fixture-matrix
        COMMAND epycbench_scenario_matrix_fixture_tests
    )

    add_executable(epycbench_results_private_plan_composition_tests
        tests/unit/results_private_plan_composition_test.cpp)
    target_link_libraries(epycbench_results_private_plan_composition_tests
        PRIVATE epycbench_results_private_plan_composition EpycBench::scenario Catch2::Catch2WithMain)
    target_include_directories(epycbench_results_private_plan_composition_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_results_private_plan_composition_tests)
    epycbench_enable_warnings(epycbench_results_private_plan_composition_tests)
    add_test(NAME EB-RES-002.private-plan-composition COMMAND epycbench_results_private_plan_composition_tests)
    add_test(NAME EB-RES-002.private-plan-composition-nonactivation
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_plan_composition_nonactivation_test.py")

    add_executable(epycbench_scenario_retained_plan_projection_resources_tests
        tests/unit/scenario_retained_plan_projection_resources_test.cpp)
    target_link_libraries(epycbench_scenario_retained_plan_projection_resources_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain)
    target_include_directories(epycbench_scenario_retained_plan_projection_resources_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_scenario_retained_plan_projection_resources_tests)
    epycbench_enable_warnings(epycbench_scenario_retained_plan_projection_resources_tests)
    add_test(NAME EB-SCN-010.retained-plan-projection-resources
        COMMAND epycbench_scenario_retained_plan_projection_resources_tests)

    add_executable(
        epycbench_scenario_retained_plan_projection_tests
        tests/unit/scenario_retained_plan_projection_test.cpp
    )
    target_link_libraries(epycbench_scenario_retained_plan_projection_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain)
    target_include_directories(epycbench_scenario_retained_plan_projection_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests/support" "${PROJECT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_scenario_retained_plan_projection_tests)
    epycbench_enable_warnings(epycbench_scenario_retained_plan_projection_tests)
    add_test(NAME EB-SCN-010.retained-plan-projection
        COMMAND epycbench_scenario_retained_plan_projection_tests)
    add_test(NAME EB-SCN-010.retained-plan-projection-nonactivation
        COMMAND "${Python3_EXECUTABLE}" -B
                "${PROJECT_SOURCE_DIR}/tests/tools/scenario_retained_plan_projection_nonactivation_test.py")

    add_executable(
        epycbench_scenario_plan_digest_tests
        tests/unit/scenario_plan_digest_test.cpp
    )
    target_link_libraries(
        epycbench_scenario_plan_digest_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_scenario_plan_digest_tests)
    epycbench_enable_warnings(epycbench_scenario_plan_digest_tests)
    add_test(
        NAME EB-SCN-010.scenario-plan-digests
        COMMAND epycbench_scenario_plan_digest_tests
    )

    add_executable(
        epycbench_scenario_plan_digest_fixture_tests
        tests/component/scenario_plan_digest_fixture_test.cpp
    )
    target_link_libraries(
        epycbench_scenario_plan_digest_fixture_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    target_compile_definitions(
        epycbench_scenario_plan_digest_fixture_tests
        PRIVATE EPYCBENCH_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    )
    epycbench_enable_build_options(epycbench_scenario_plan_digest_fixture_tests)
    epycbench_enable_warnings(epycbench_scenario_plan_digest_fixture_tests)
    add_test(
        NAME EB-SCN-010.reference-fixture-digests
        COMMAND epycbench_scenario_plan_digest_fixture_tests
    )

    add_executable(
        epycbench_scenario_resolution_tests
        tests/unit/scenario_resolution_test.cpp
    )
    target_link_libraries(
        epycbench_scenario_resolution_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain
    )
    target_compile_definitions(
        epycbench_scenario_resolution_tests
        PRIVATE EPYCBENCH_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    )
    epycbench_enable_build_options(epycbench_scenario_resolution_tests)
    epycbench_enable_warnings(epycbench_scenario_resolution_tests)
    add_test(
        NAME EB-SCN-011.scenario-resolution
        COMMAND epycbench_scenario_resolution_tests
    )

    add_executable(epycbench_results_preparation_observation_tests tests/unit/results_preparation_observation_test.cpp)
    target_compile_definitions(epycbench_results_preparation_observation_tests PRIVATE EPYCBENCH_MONITORING_FACT_TEST=1 EPYCBENCH_PREPARATION_GRAPH_TEST=1)
    target_include_directories(epycbench_results_preparation_observation_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_preparation_observation_tests PRIVATE epycbench_results_producer_progress epycbench_cli epycbench_benchmark_run_composition Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_preparation_observation_tests)
    epycbench_enable_warnings(epycbench_results_preparation_observation_tests)
    add_test(NAME EB-RES-002.preparation-observation COMMAND epycbench_results_preparation_observation_tests)
    add_executable(epycbench_benchmark_run_cli_tests tests/component/benchmark_run_cli_test.cpp)
    add_executable(epycbench_platform_benchmark_run_input_tests tests/unit/platform_benchmark_run_input_test.cpp)
    target_include_directories(epycbench_platform_benchmark_run_input_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_platform_benchmark_run_input_tests PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_platform_benchmark_run_input_tests)
    epycbench_enable_warnings(epycbench_platform_benchmark_run_input_tests)
    add_test(NAME EB-CLI-001.public-run-platform COMMAND epycbench_platform_benchmark_run_input_tests)
    target_include_directories(epycbench_benchmark_run_cli_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_benchmark_run_cli_tests PRIVATE epycbench_cli Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_benchmark_run_cli_tests)
    epycbench_enable_warnings(epycbench_benchmark_run_cli_tests)
    add_test(NAME EB-CLI-001.public-run COMMAND epycbench_benchmark_run_cli_tests)

    add_executable(epycbench_scenario_public_resolution_limits_tests
        tests/unit/scenario_public_resolution_limits_test.cpp)
    target_link_libraries(epycbench_scenario_public_resolution_limits_tests
        PRIVATE EpycBench::scenario Catch2::Catch2WithMain)
    target_compile_definitions(epycbench_scenario_public_resolution_limits_tests
        PRIVATE EPYCBENCH_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_scenario_public_resolution_limits_tests)
    epycbench_enable_warnings(epycbench_scenario_public_resolution_limits_tests)
    add_test(NAME EB-CLI-001.public-resolution-limits
        COMMAND epycbench_scenario_public_resolution_limits_tests)

    add_executable(epycbench_scenario_cli_tests tests/component/scenario_cli_test.cpp)
    target_include_directories(epycbench_scenario_cli_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        epycbench_scenario_cli_tests
        PRIVATE epycbench_cli Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_scenario_cli_tests)
    epycbench_enable_warnings(epycbench_scenario_cli_tests)
    add_test(NAME EB-SCN-011.scenario-cli COMMAND epycbench_scenario_cli_tests "~[reader_fifo]")
    add_test(NAME EB-RES-007.reader-fifo COMMAND epycbench_scenario_cli_tests "[reader_fifo]")
    set_tests_properties(EB-RES-007.reader-fifo PROPERTIES TIMEOUT 5)

    add_executable(
        epycbench_scenario_cli_fixture_tests
        tests/component/scenario_cli_fixture_test.cpp
    )
    target_include_directories(
        epycbench_scenario_cli_fixture_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        epycbench_scenario_cli_fixture_tests
        PRIVATE epycbench_cli EpycBench::scenario Catch2::Catch2WithMain
    )
    target_compile_definitions(
        epycbench_scenario_cli_fixture_tests
        PRIVATE EPYCBENCH_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    )
    epycbench_enable_build_options(epycbench_scenario_cli_fixture_tests)
    epycbench_enable_warnings(epycbench_scenario_cli_fixture_tests)
    add_test(
        NAME EB-SCN-011.reference-fixture-cli
        COMMAND epycbench_scenario_cli_fixture_tests
    )

    add_executable(epycbench_topology_model_tests tests/unit/topology_model_test.cpp)
    target_link_libraries(
        epycbench_topology_model_tests
        PRIVATE
            EpycBench::topology
            Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_topology_model_tests)
    epycbench_enable_warnings(epycbench_topology_model_tests)

    add_test(NAME EB-TOP-001.owned-topology-model COMMAND epycbench_topology_model_tests)

    add_executable(epycbench_topology_invariants_tests tests/unit/topology_invariants_test.cpp)
    target_link_libraries(
        epycbench_topology_invariants_tests
        PRIVATE
            EpycBench::topology
            Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_topology_invariants_tests)
    epycbench_enable_warnings(epycbench_topology_invariants_tests)

    add_test(NAME EB-TOP-005.topology-invariants COMMAND epycbench_topology_invariants_tests)

    add_executable(
        epycbench_topology_page_size_projection_tests
        tests/unit/topology_page_size_projection_test.cpp
    )
    target_link_libraries(
        epycbench_topology_page_size_projection_tests
        PRIVATE EpycBench::topology Catch2::Catch2WithMain
    )
    target_include_directories(
        epycbench_topology_page_size_projection_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    epycbench_enable_build_options(epycbench_topology_page_size_projection_tests)
    epycbench_enable_warnings(epycbench_topology_page_size_projection_tests)
    add_test(
        NAME EB-TOP-005.page-size-projection
        COMMAND epycbench_topology_page_size_projection_tests
    )

    add_executable(
        epycbench_topology_stable_id_tests
        tests/unit/topology_stable_id_test.cpp
    )
    target_link_libraries(
        epycbench_topology_stable_id_tests
        PRIVATE EpycBench::topology Catch2::Catch2WithMain
    )
    target_compile_definitions(
        epycbench_topology_stable_id_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    epycbench_enable_build_options(epycbench_topology_stable_id_tests)
    epycbench_enable_warnings(epycbench_topology_stable_id_tests)
    add_test(
        NAME EB-TOP-006.stable-topology-ids
        COMMAND epycbench_topology_stable_id_tests
    )

    add_executable(
        epycbench_topology_fingerprint_tests
        tests/unit/topology_fingerprint_test.cpp
    )
    target_link_libraries(
        epycbench_topology_fingerprint_tests
        PRIVATE EpycBench::topology Catch2::Catch2WithMain
    )
    target_include_directories(
        epycbench_topology_fingerprint_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    epycbench_enable_build_options(epycbench_topology_fingerprint_tests)
    epycbench_enable_warnings(epycbench_topology_fingerprint_tests)
    add_test(
        NAME EB-TOP-007.topology-fingerprints
        COMMAND epycbench_topology_fingerprint_tests
    )

    add_executable(
        epycbench_topology_fingerprint_fixture_tests
        tests/component/topology_fingerprint_fixture_test.cpp
    )
    target_link_libraries(
        epycbench_topology_fingerprint_fixture_tests
        PRIVATE EpycBench::topology Catch2::Catch2WithMain
    )
    target_compile_definitions(
        epycbench_topology_fingerprint_fixture_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    epycbench_enable_build_options(epycbench_topology_fingerprint_fixture_tests)
    epycbench_enable_warnings(epycbench_topology_fingerprint_fixture_tests)
    add_test(
        NAME EB-TOP-007.reference-fixture-fingerprints
        COMMAND epycbench_topology_fingerprint_fixture_tests
    )

    add_executable(
        epycbench_topology_serialization_fixture_tests
        tests/component/topology_serialization_fixture_test.cpp
    )
    target_link_libraries(
        epycbench_topology_serialization_fixture_tests
        PRIVATE EpycBench::topology Catch2::Catch2WithMain
    )
    target_compile_definitions(
        epycbench_topology_serialization_fixture_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    epycbench_enable_build_options(epycbench_topology_serialization_fixture_tests)
    epycbench_enable_warnings(epycbench_topology_serialization_fixture_tests)
    add_test(
        NAME EB-TOP-008.reference-fixture-serialization
        COMMAND epycbench_topology_serialization_fixture_tests
    )

    add_executable(epycbench_topology_serialization_ownership_tests tests/unit/topology_serialization_ownership_test.cpp)
    target_include_directories(epycbench_topology_serialization_ownership_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_topology_serialization_ownership_tests PRIVATE EpycBench::topology Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_topology_serialization_ownership_tests)
    epycbench_enable_warnings(epycbench_topology_serialization_ownership_tests)
    add_test(NAME EB-TOP-008.serialization-ownership COMMAND epycbench_topology_serialization_ownership_tests)
    add_test(NAME EB-TOP-008.serialization-ownership-nonactivation
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/topology_serialization_ownership_nonactivation_test.py")

    add_executable(
        epycbench_topology_serialization_tests
        tests/unit/topology_serialization_test.cpp
    )
    target_link_libraries(
        epycbench_topology_serialization_tests
        PRIVATE EpycBench::topology Catch2::Catch2WithMain
    )
    target_include_directories(epycbench_topology_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    epycbench_enable_build_options(epycbench_topology_serialization_tests)
    epycbench_enable_warnings(epycbench_topology_serialization_tests)
    add_test(
        NAME EB-TOP-008.topology-serialization
        COMMAND epycbench_topology_serialization_tests
    )

    add_executable(epycbench_hwloc_source_tests tests/component/hwloc_source_test.cpp)
    target_link_libraries(
        epycbench_hwloc_source_tests
        PRIVATE EpycBench::topology Catch2::Catch2WithMain
    )
    target_include_directories(epycbench_hwloc_source_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    epycbench_enable_build_options(epycbench_hwloc_source_tests)
    epycbench_enable_warnings(epycbench_hwloc_source_tests)
    add_test(NAME EB-TOP-002.hwloc-adapter COMMAND epycbench_hwloc_source_tests)
    set_tests_properties(
        EB-TOP-002.hwloc-adapter
        PROPERTIES LABELS "synthetic;unprivileged;non-hardware"
    )

    add_executable(
        epycbench_linux_topology_source_tests
        tests/component/linux_topology_source_test.cpp
    )
    target_link_libraries(
        epycbench_linux_topology_source_tests
        PRIVATE EpycBench::topology Catch2::Catch2WithMain
    )
    target_include_directories(
        epycbench_linux_topology_source_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_definitions(
        epycbench_linux_topology_source_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    epycbench_enable_build_options(epycbench_linux_topology_source_tests)
    epycbench_enable_warnings(epycbench_linux_topology_source_tests)
    add_test(
        NAME EB-TOP-003.linux-topology-source
        COMMAND epycbench_linux_topology_source_tests
    )
    set_tests_properties(
        EB-TOP-003.linux-topology-source
        PROPERTIES LABELS "simulated-filesystem;unprivileged;non-hardware"
    )

    add_executable(
        epycbench_topology_source_io_tests
        tests/unit/platform_linux_topology_source_io_test.cpp
    )
    target_link_libraries(
        epycbench_topology_source_io_tests
        PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain
    )
    target_include_directories(
        epycbench_topology_source_io_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    epycbench_enable_build_options(epycbench_topology_source_io_tests)
    epycbench_enable_warnings(epycbench_topology_source_io_tests)
    add_test(
        NAME EB-TOP-003.bounded-source-io
        COMMAND epycbench_topology_source_io_tests
    )
    set_tests_properties(
        EB-TOP-003.bounded-source-io
        PROPERTIES LABELS "synthetic;unprivileged;non-hardware"
    )

    add_executable(
        epycbench_topology_reconciliation_tests
        tests/component/topology_reconciliation_test.cpp
    )
    target_link_libraries(
        epycbench_topology_reconciliation_tests
        PRIVATE EpycBench::topology Catch2::Catch2WithMain
    )
    target_include_directories(
        epycbench_topology_reconciliation_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    epycbench_enable_build_options(epycbench_topology_reconciliation_tests)
    epycbench_enable_warnings(epycbench_topology_reconciliation_tests)
    add_test(
        NAME EB-TOP-004.source-reconciliation
        COMMAND epycbench_topology_reconciliation_tests
    )
    set_tests_properties(
        EB-TOP-004.source-reconciliation
        PROPERTIES LABELS "synthetic;unprivileged;non-hardware"
    )

    add_executable(
        epycbench_topology_discovery_tests
        tests/synthetic/topology_discovery_test.cpp
    )
    target_link_libraries(
        epycbench_topology_discovery_tests
        PRIVATE EpycBench::topology Catch2::Catch2WithMain
    )
    target_compile_definitions(
        epycbench_topology_discovery_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    epycbench_enable_build_options(epycbench_topology_discovery_tests)
    epycbench_enable_warnings(epycbench_topology_discovery_tests)
    add_test(
        NAME EB-TOP-004.public-fixture-reconciliation
        COMMAND epycbench_topology_discovery_tests
    )
    set_tests_properties(
        EB-TOP-004.public-fixture-reconciliation
        PROPERTIES LABELS "repository;synthetic;unprivileged;non-hardware"
    )

    add_executable(
        epycbench_reference_fixture_loader_tests
        tests/component/reference_fixture_loader_test.cpp
    )
    target_link_libraries(
        epycbench_reference_fixture_loader_tests
        PRIVATE EpycBench::topology Catch2::Catch2WithMain
    )
    target_include_directories(
        epycbench_reference_fixture_loader_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_definitions(
        epycbench_reference_fixture_loader_tests
        PRIVATE EPYCBENCH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    epycbench_enable_build_options(epycbench_reference_fixture_loader_tests)
    epycbench_enable_warnings(epycbench_reference_fixture_loader_tests)
    add_test(NAME EB-TOP-010.reference-fixture-loader COMMAND epycbench_reference_fixture_loader_tests)
    set_tests_properties(
        EB-TOP-010.reference-fixture-loader
        PROPERTIES LABELS "repository;unprivileged;non-hardware"
    )

    add_executable(
        epycbench_platform_linux_file_tests
        tests/unit/platform_linux_file_test.cpp
    )
    target_link_libraries(
        epycbench_platform_linux_file_tests
        PRIVATE
            EpycBench::platform_linux
            Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_platform_linux_file_tests)
    epycbench_enable_warnings(epycbench_platform_linux_file_tests)

    add_test(NAME EB-LNX-001.safe-linux-readers COMMAND epycbench_platform_linux_file_tests)

    # Permission-gated private diagnostic. No CTest entry invokes this active event.
    add_executable(epycbench_q2_perf_self_diagnostic EXCLUDE_FROM_ALL tools/q2_perf_self_diagnostic.cpp)
    target_include_directories(epycbench_q2_perf_self_diagnostic PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_q2_perf_self_diagnostic PRIVATE EpycBench::platform_linux)
    epycbench_enable_build_options(epycbench_q2_perf_self_diagnostic)
    epycbench_enable_warnings(epycbench_q2_perf_self_diagnostic)

    add_executable(epycbench_platform_linux_q2_perf_self_diagnostic_tests
        tests/unit/platform_linux_q2_perf_self_diagnostic_test.cpp)
    target_include_directories(epycbench_platform_linux_q2_perf_self_diagnostic_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_platform_linux_q2_perf_self_diagnostic_tests PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_platform_linux_q2_perf_self_diagnostic_tests)
    epycbench_enable_warnings(epycbench_platform_linux_q2_perf_self_diagnostic_tests)
    add_test(NAME EB-MON-003.q2-perf-self-diagnostic-synthetic
        COMMAND epycbench_platform_linux_q2_perf_self_diagnostic_tests)
    add_test(NAME EB-MON-003.q2-perf-self-wrapper-synthetic
        COMMAND "${Python3_EXECUTABLE}" -I -B "${PROJECT_SOURCE_DIR}/tests/tools/q2_perf_self_diagnostic_test.py")
    set_tests_properties(EB-MON-003.q2-perf-self-wrapper-synthetic PROPERTIES TIMEOUT 15)

    add_test(NAME EB-VAL-007.release-evidence-synthetic
        COMMAND "${Python3_EXECUTABLE}" -I -B
            "${PROJECT_SOURCE_DIR}/tests/tools/release_evidence_test.py")
    set_tests_properties(EB-VAL-007.release-evidence-synthetic PROPERTIES
        TIMEOUT 30 LABELS "synthetic;nonhardware;release-evidence")

    add_test(NAME EB-MON-007.host-perf-access-window-injected
        COMMAND "${Python3_EXECUTABLE}" -I -B
            "${PROJECT_SOURCE_DIR}/tests/tools/host_perf_access_window_test.py")
    set_tests_properties(EB-MON-007.host-perf-access-window-injected PROPERTIES
        TIMEOUT 60 LABELS "synthetic;nonhardware;policy-window")

    add_test(NAME EB-MON-007.source-controller-injected
        COMMAND "${Python3_EXECUTABLE}" -I -B
            "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_host_frozen_process_test.py"
            --source-controller-tests)
    set_tests_properties(EB-MON-007.source-controller-injected PROPERTIES
        TIMEOUT 60 LABELS "synthetic;nonhardware;source-workflow")

    add_test(NAME EB-MON-007.campaign-baseline-commit-driver-injected
        COMMAND "${Python3_EXECUTABLE}" -I -B
            "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_host_campaign_baseline_driver_test.py"
            "${PROJECT_SOURCE_DIR}")
    set_tests_properties(EB-MON-007.campaign-baseline-commit-driver-injected PROPERTIES
        TIMEOUT 60 LABELS "synthetic;nonhardware;source-workflow")

    add_test(NAME EB-MON-007.campaign-invoker-injected
        COMMAND "${Python3_EXECUTABLE}" -I -B
            "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_host_campaign_invoker_test.py"
            "${PROJECT_SOURCE_DIR}")
    set_tests_properties(EB-MON-007.campaign-invoker-injected PROPERTIES
        TIMEOUT 60 LABELS "synthetic;nonhardware;source-workflow")

    add_test(NAME EB-MON-007.governed-baseline-injected
        COMMAND "${Python3_EXECUTABLE}" -I -B
            "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_host_governed_baseline_test.py"
            "${PROJECT_SOURCE_DIR}")
    set_tests_properties(EB-MON-007.governed-baseline-injected PROPERTIES
        TIMEOUT 60 LABELS "synthetic;nonhardware;source-workflow")

    add_executable(epycbench_platform_linux_q4_capacity_control_tests tests/unit/platform_linux_q4_capacity_control_test.cpp)
    target_include_directories(epycbench_platform_linux_q4_capacity_control_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_platform_linux_q4_capacity_control_tests PRIVATE EpycBench::platform_linux Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_platform_linux_q4_capacity_control_tests)
    epycbench_enable_warnings(epycbench_platform_linux_q4_capacity_control_tests)
    add_test(NAME EB-MON-005.host-q4-capacity-control COMMAND epycbench_platform_linux_q4_capacity_control_tests)

    # Test-only native peer; never linked into a production executable.
    add_executable(epycbench_monitoring_q4_capacity_peer tests/support/monitoring_q4_capacity_peer.cpp)
    target_include_directories(epycbench_monitoring_q4_capacity_peer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(epycbench_monitoring_q4_capacity_peer PRIVATE EpycBench::platform_linux EpycBench::monitoring_qualification)
    epycbench_enable_build_options(epycbench_monitoring_q4_capacity_peer)
    epycbench_enable_warnings(epycbench_monitoring_q4_capacity_peer)
    add_test(NAME EB-MON-005.host-q4-capacity-integration
        COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tests/tools/monitoring_q4_capacity_integration_test.py"
                --peer "$<TARGET_FILE:epycbench_monitoring_q4_capacity_peer>")
    set_tests_properties(EB-MON-005.host-q4-capacity-integration PROPERTIES TIMEOUT 120)

    add_executable(epycbench_build_manifest_tests tests/unit/build_manifest_test.cpp)
    target_link_libraries(
        epycbench_build_manifest_tests
        PRIVATE
            EpycBench::build_manifest
            Catch2::Catch2WithMain
    )
    epycbench_enable_build_options(epycbench_build_manifest_tests)
    epycbench_enable_warnings(epycbench_build_manifest_tests)

    add_test(NAME EB-BLD-005.build-manifest COMMAND epycbench_build_manifest_tests)
    add_test(
        NAME EB-BLD-005.version-json
        COMMAND
            "${CMAKE_COMMAND}"
            "-DEPYCBENCH_EXECUTABLE=$<TARGET_FILE:epycbench>"
            -P
            "${PROJECT_SOURCE_DIR}/tests/cmake/ValidateVersionJson.cmake"
    )

    set(
        EPYCBENCH_REFERENCE_FIXTURE
        "${PROJECT_SOURCE_DIR}/fixtures/topology/reference-epyc"
    )
    add_test(
        NAME reference-fixture-integrity
        COMMAND "${EPYCBENCH_SHA256SUM_EXECUTABLE}" -c checksums.sha256
        WORKING_DIRECTORY "${EPYCBENCH_REFERENCE_FIXTURE}"
    )
    add_test(
        NAME reference-fixture-privacy
        COMMAND
            "${EPYCBENCH_BASH_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/tools/reference_fixture_privacy_test.sh"
            "${EPYCBENCH_REFERENCE_FIXTURE}"
    )
    add_test(
        NAME reference-fixture-completeness
        COMMAND
            "${EPYCBENCH_BASH_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/tools/reference_fixture_completeness_test.sh"
            "${EPYCBENCH_REFERENCE_FIXTURE}"
    )
    set_tests_properties(
        reference-fixture-integrity
        reference-fixture-privacy
        reference-fixture-completeness
        PROPERTIES LABELS "repository;unprivileged;non-hardware"
    )
endif()

# Private supplied Host owner composition; no route or exported schema.
add_library(epycbench_results_host_assembly STATIC src/results/host_evidence_assembly.cpp)
target_include_directories(epycbench_results_host_assembly PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_host_assembly PRIVATE epycbench_results_topology_evidence epycbench_results_host_controls epycbench_results_private_kernel_identity epycbench_results_pu_cpu_identity epycbench_results_frequency_policy_owner epycbench_results_anonymous_thp_policy_owner epycbench_results_process_thp_restriction EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_host_assembly)
epycbench_enable_warnings(epycbench_results_host_assembly)

add_library(epycbench_results_pair_lifecycle STATIC src/results/pair_lifecycle_admission.cpp)
target_include_directories(epycbench_results_pair_lifecycle PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_pair_lifecycle PRIVATE
    epycbench_results_incremental_pair_evidence EpycBench::pairwise_v2_synthetic_lifecycle)
epycbench_enable_build_options(epycbench_results_pair_lifecycle)
epycbench_enable_warnings(epycbench_results_pair_lifecycle)

add_library(epycbench_results_pair_analysis STATIC src/results/pair_analysis_admission.cpp)
target_include_directories(epycbench_results_pair_analysis PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_pair_analysis PRIVATE epycbench_results_pairwise_analysis
    epycbench_results_pair_lifecycle EpycBench::finalized_evidence_wire_typed_reconstruction EpycBench::results EpycBench::core)
epycbench_enable_build_options(epycbench_results_pair_analysis)
epycbench_enable_warnings(epycbench_results_pair_analysis)

add_library(epycbench_results_preflight STATIC src/results/preflight_admission.cpp)
target_include_directories(epycbench_results_preflight PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_preflight PRIVATE epycbench_results_host_assembly
    epycbench_results_authored_source epycbench_results_build_manifest epycbench_results_validation_event
    epycbench_runtime_scenario_monitoring_projection epycbench_runtime_production_backend
    EpycBench::topology EpycBench::core)
epycbench_enable_build_options(epycbench_results_preflight)
epycbench_enable_warnings(epycbench_results_preflight)

add_library(epycbench_results_host_projection STATIC src/results/host_projection.cpp)
target_include_directories(epycbench_results_host_projection PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_host_projection PRIVATE epycbench_results_host_assembly EpycBench::topology EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_host_projection)
epycbench_enable_warnings(epycbench_results_host_projection)

add_library(epycbench_results_build_projection STATIC src/results/build_projection.cpp)
target_include_directories(epycbench_results_build_projection PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_build_projection PRIVATE epycbench_results_build_manifest EpycBench::core)
epycbench_enable_build_options(epycbench_results_build_projection)
epycbench_enable_warnings(epycbench_results_build_projection)

add_library(epycbench_results_process_subject STATIC src/results/process_subject_admission.cpp src/results/run_creation_admission.cpp src/results/execution_plan_admission.cpp src/results/execution_request_association.cpp)
target_include_directories(epycbench_results_process_subject PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_process_subject PRIVATE epycbench_results_private_execution_plan epycbench_results_private_normalization_serialization epycbench_results_private_run_creation_serialization epycbench_results_private_host_serialization epycbench_results_private_process_thp_restriction_serialization epycbench_results_private_anonymous_thp_policy_serialization epycbench_results_private_frequency_policy_serialization epycbench_results_private_pu_cpu_identity_serialization epycbench_results_private_kernel_identity_serialization epycbench_results_private_system_controls_serialization epycbench_results_private_topology_serialization epycbench_results_private_base_build_serialization epycbench_results_private_source_outcome_serialization epycbench_results_private_source_serialization epycbench_results_private_error_serialization epycbench_results_build_projection epycbench_results_host_projection epycbench_results_preflight epycbench_results_pair_analysis epycbench_results_pair_lifecycle epycbench_results_build_manifest epycbench_results_authored_source epycbench_results_private_error epycbench_results_host_assembly EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_process_subject)
epycbench_enable_warnings(epycbench_results_process_subject)

add_library(epycbench_results_private_source_outcome_serialization STATIC src/results/private_source_outcome_serialization.cpp)
target_include_directories(epycbench_results_private_source_outcome_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_source_outcome_serialization PRIVATE epycbench_results_authored_source epycbench_results_private_source_serialization epycbench_results_private_error_serialization EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_source_outcome_serialization)
epycbench_enable_warnings(epycbench_results_private_source_outcome_serialization)

add_library(epycbench_results_private_source_serialization STATIC src/results/private_source_serialization.cpp)
target_include_directories(epycbench_results_private_source_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_source_serialization PRIVATE epycbench_results_authored_source epycbench_results_private_error_serialization EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_source_serialization)
epycbench_enable_warnings(epycbench_results_private_source_serialization)

add_library(epycbench_results_private_error_serialization STATIC src/results/private_error_serialization.cpp)
target_include_directories(epycbench_results_private_error_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_error_serialization PRIVATE epycbench_results_private_error EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_error_serialization)
epycbench_enable_warnings(epycbench_results_private_error_serialization)

add_library(epycbench_results_private_error STATIC src/results/private_error_owner.cpp)
target_include_directories(epycbench_results_private_error PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_error PRIVATE EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_error)
epycbench_enable_warnings(epycbench_results_private_error)

add_library(epycbench_results_private_base_build_serialization STATIC src/results/private_base_build_serialization.cpp)
target_include_directories(epycbench_results_private_base_build_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_base_build_serialization PRIVATE epycbench_results_build_manifest epycbench_results_private_error_serialization EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_base_build_serialization)
epycbench_enable_warnings(epycbench_results_private_base_build_serialization)

add_library(epycbench_results_private_topology_serialization STATIC src/results/private_topology_serialization.cpp)
target_include_directories(epycbench_results_private_topology_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_topology_serialization PRIVATE epycbench_results_topology_evidence epycbench_results_private_error_serialization EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_topology_serialization)
epycbench_enable_warnings(epycbench_results_private_topology_serialization)

add_library(epycbench_results_private_system_controls_serialization STATIC src/results/private_system_controls_serialization.cpp)
target_include_directories(epycbench_results_private_system_controls_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_system_controls_serialization PRIVATE epycbench_results_host_controls epycbench_results_private_topology_serialization epycbench_results_private_error_serialization EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_system_controls_serialization)
epycbench_enable_warnings(epycbench_results_private_system_controls_serialization)

add_library(epycbench_results_private_kernel_identity_serialization STATIC src/results/private_kernel_identity_serialization.cpp)
target_include_directories(epycbench_results_private_kernel_identity_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_kernel_identity_serialization PRIVATE epycbench_results_private_kernel_identity epycbench_results_private_topology_serialization epycbench_results_private_error_serialization EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_kernel_identity_serialization)
epycbench_enable_warnings(epycbench_results_private_kernel_identity_serialization)

add_library(epycbench_results_private_pu_cpu_identity_serialization STATIC src/results/private_pu_cpu_identity_serialization.cpp)
target_include_directories(epycbench_results_private_pu_cpu_identity_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_pu_cpu_identity_serialization PRIVATE epycbench_results_pu_cpu_identity epycbench_results_private_topology_serialization epycbench_results_private_error_serialization EpycBench::topology EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_pu_cpu_identity_serialization)
epycbench_enable_warnings(epycbench_results_private_pu_cpu_identity_serialization)

add_library(epycbench_results_private_frequency_policy_serialization STATIC src/results/private_frequency_policy_serialization.cpp)
target_include_directories(epycbench_results_private_frequency_policy_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_frequency_policy_serialization PRIVATE epycbench_results_frequency_policy_owner epycbench_results_private_topology_serialization epycbench_results_private_error_serialization EpycBench::monitoring EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_frequency_policy_serialization)
epycbench_enable_warnings(epycbench_results_private_frequency_policy_serialization)

add_library(epycbench_results_private_process_thp_restriction_serialization STATIC src/results/private_process_thp_restriction_serialization.cpp)
target_include_directories(epycbench_results_private_process_thp_restriction_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_process_thp_restriction_serialization PRIVATE epycbench_results_process_thp_restriction epycbench_results_private_topology_serialization epycbench_results_private_error_serialization EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_process_thp_restriction_serialization)
epycbench_enable_warnings(epycbench_results_private_process_thp_restriction_serialization)


add_library(epycbench_results_private_anonymous_thp_policy_serialization STATIC src/results/private_anonymous_thp_policy_serialization.cpp)
target_include_directories(epycbench_results_private_anonymous_thp_policy_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_anonymous_thp_policy_serialization PRIVATE epycbench_results_anonymous_thp_policy_owner epycbench_results_private_topology_serialization epycbench_results_private_error_serialization EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_anonymous_thp_policy_serialization)
epycbench_enable_warnings(epycbench_results_private_anonymous_thp_policy_serialization)

add_library(epycbench_results_private_host_serialization STATIC src/results/private_host_serialization.cpp)
target_include_directories(epycbench_results_private_host_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_host_serialization PRIVATE epycbench_results_private_topology_serialization epycbench_results_private_system_controls_serialization epycbench_results_private_kernel_identity_serialization epycbench_results_private_pu_cpu_identity_serialization epycbench_results_private_frequency_policy_serialization epycbench_results_private_anonymous_thp_policy_serialization epycbench_results_private_process_thp_restriction_serialization epycbench_results_private_error_serialization EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_host_serialization)
epycbench_enable_warnings(epycbench_results_private_host_serialization)

add_library(epycbench_results_private_document_ancestry_serialization STATIC src/results/private_document_ancestry_serialization.cpp)
target_include_directories(epycbench_results_private_document_ancestry_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_document_ancestry_serialization PRIVATE EpycBench::scenario EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_document_ancestry_serialization)
epycbench_enable_warnings(epycbench_results_private_document_ancestry_serialization)

add_library(epycbench_results_private_run_creation_serialization STATIC src/results/private_run_creation_serialization.cpp)
target_include_directories(epycbench_results_private_run_creation_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_run_creation_serialization PRIVATE epycbench_results_private_source_serialization epycbench_results_private_base_build_serialization epycbench_results_private_host_serialization epycbench_results_private_error_serialization EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_run_creation_serialization)
epycbench_enable_warnings(epycbench_results_private_run_creation_serialization)

add_library(epycbench_results_private_resource_service_serialization STATIC src/results/private_resource_service_serialization.cpp)
target_include_directories(epycbench_results_private_resource_service_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_resource_service_serialization PRIVATE EpycBench::scenario EpycBench::topology EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_resource_service_serialization)
epycbench_enable_warnings(epycbench_results_private_resource_service_serialization)

add_library(epycbench_results_private_memory_placement_serialization STATIC src/results/private_memory_placement_serialization.cpp)
target_include_directories(epycbench_results_private_memory_placement_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_memory_placement_serialization PRIVATE EpycBench::scenario EpycBench::topology EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_memory_placement_serialization)
epycbench_enable_warnings(epycbench_results_private_memory_placement_serialization)

add_library(epycbench_results_private_kernel_dispatch_serialization STATIC src/results/private_kernel_dispatch_serialization.cpp)
target_include_directories(epycbench_results_private_kernel_dispatch_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_kernel_dispatch_serialization PRIVATE EpycBench::scenario EpycBench::kernel_api EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_kernel_dispatch_serialization)
epycbench_enable_warnings(epycbench_results_private_kernel_dispatch_serialization)

add_library(epycbench_results_private_comparison_experiments_serialization STATIC src/results/private_comparison_experiments_serialization.cpp)
target_include_directories(epycbench_results_private_comparison_experiments_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_comparison_experiments_serialization PRIVATE EpycBench::scenario EpycBench::topology EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_comparison_experiments_serialization)
epycbench_enable_warnings(epycbench_results_private_comparison_experiments_serialization)

add_library(epycbench_results_private_matrix_experiments_serialization STATIC src/results/private_matrix_experiments_serialization.cpp)
target_include_directories(epycbench_results_private_matrix_experiments_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_matrix_experiments_serialization PRIVATE EpycBench::scenario EpycBench::topology EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_matrix_experiments_serialization)
epycbench_enable_warnings(epycbench_results_private_matrix_experiments_serialization)

add_library(epycbench_results_private_plan_composition STATIC src/results/private_plan_composition.cpp)
target_include_directories(epycbench_results_private_plan_composition PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_plan_composition PRIVATE
    epycbench_results_private_document_ancestry_serialization
    epycbench_results_private_resource_service_serialization
    epycbench_results_private_memory_placement_serialization
    epycbench_results_private_kernel_dispatch_serialization
    epycbench_results_private_comparison_experiments_serialization
    epycbench_results_private_matrix_experiments_serialization
    EpycBench::scenario EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_plan_composition)
epycbench_enable_warnings(epycbench_results_private_plan_composition)

add_library(epycbench_results_private_normalization_serialization STATIC src/results/private_normalization_serialization.cpp)
target_include_directories(epycbench_results_private_normalization_serialization PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_normalization_serialization PRIVATE
    epycbench_results_private_source_serialization epycbench_results_private_plan_composition
    epycbench_results_private_topology_serialization epycbench_results_authored_source EpycBench::topology EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_normalization_serialization)
epycbench_enable_warnings(epycbench_results_private_normalization_serialization)

if(BUILD_TESTING)
    add_executable(epycbench_results_private_normalization_serialization_tests tests/unit/results_private_normalization_serialization_test.cpp)
    target_include_directories(epycbench_results_private_normalization_serialization_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_normalization_serialization_tests PRIVATE epycbench_results_process_subject EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_normalization_serialization_tests)
    epycbench_enable_warnings(epycbench_results_private_normalization_serialization_tests)
    add_test(NAME EB-RES-002.private-normalization-serialization COMMAND epycbench_results_private_normalization_serialization_tests)
    add_test(NAME EB-RES-002.private-normalization-serialization-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_normalization_serialization_test.py")
endif()

add_library(epycbench_results_private_implementation_selection STATIC src/results/private_implementation_selection.cpp)
target_include_directories(epycbench_results_private_implementation_selection PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_implementation_selection PRIVATE epycbench_results_private_normalization_serialization EpycBench::kernel_registry EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_implementation_selection)
epycbench_enable_warnings(epycbench_results_private_implementation_selection)
if(BUILD_TESTING)
    add_executable(epycbench_results_private_implementation_selection_tests tests/unit/results_private_implementation_selection_test.cpp)
    target_include_directories(epycbench_results_private_implementation_selection_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_implementation_selection_tests PRIVATE epycbench_results_private_implementation_selection epycbench_results_process_subject EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_implementation_selection_tests)
    epycbench_enable_warnings(epycbench_results_private_implementation_selection_tests)
    add_test(NAME EB-RES-002.private-implementation-selection COMMAND epycbench_results_private_implementation_selection_tests)
    add_test(NAME EB-RES-002.private-implementation-selection-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_implementation_selection_test.py")
endif()

add_library(epycbench_results_private_build_object_association STATIC src/results/private_build_object_association.cpp)
target_include_directories(epycbench_results_private_build_object_association PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_build_object_association PRIVATE epycbench_results_private_implementation_selection epycbench_results_private_base_build_serialization epycbench_results_kernel_approval EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_build_object_association)
epycbench_enable_warnings(epycbench_results_private_build_object_association)
if(BUILD_TESTING)
    add_executable(epycbench_results_private_build_object_association_tests tests/unit/results_private_build_object_association_test.cpp)
    target_include_directories(epycbench_results_private_build_object_association_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_build_object_association_tests PRIVATE epycbench_results_private_build_object_association epycbench_results_process_subject EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_build_object_association_tests)
    epycbench_enable_warnings(epycbench_results_private_build_object_association_tests)
    add_test(NAME EB-RES-002.private-build-object-association COMMAND epycbench_results_private_build_object_association_tests)
    add_test(NAME EB-RES-002.private-build-object-association-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_build_object_association_test.py")
endif()

add_library(epycbench_results_private_execution_plan STATIC src/results/private_execution_plan.cpp)
target_include_directories(epycbench_results_private_execution_plan PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_private_execution_plan PRIVATE epycbench_results_private_build_object_association EpycBench::core)
epycbench_enable_build_options(epycbench_results_private_execution_plan)
epycbench_enable_warnings(epycbench_results_private_execution_plan)
if(BUILD_TESTING)
    add_executable(epycbench_results_private_execution_plan_tests tests/unit/results_private_execution_plan_test.cpp)
    target_include_directories(epycbench_results_private_execution_plan_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_private_execution_plan_tests PRIVATE epycbench_results_private_execution_plan epycbench_results_process_subject EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_private_execution_plan_tests)
    epycbench_enable_warnings(epycbench_results_private_execution_plan_tests)
    add_test(NAME EB-RES-002.private-execution-plan COMMAND epycbench_results_private_execution_plan_tests)
    add_test(NAME EB-RES-002.private-execution-plan-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_private_execution_plan_test.py")
endif()

if(BUILD_TESTING)
    add_executable(epycbench_results_execution_request_association_tests tests/unit/results_execution_request_association_test.cpp)
    target_include_directories(epycbench_results_execution_request_association_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_results_execution_request_association_tests PRIVATE EPYCBENCH_MONITORING_FACT_TEST=1 EPYCBENCH_PREPARATION_GRAPH_TEST=1)
    target_link_libraries(epycbench_results_execution_request_association_tests PRIVATE epycbench_results_process_subject EpycBench::pairwise_v2_synthetic_lifecycle epycbench_runtime_test_support EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_execution_request_association_tests)
    epycbench_enable_warnings(epycbench_results_execution_request_association_tests)
    add_test(NAME EB-RES-002.execution-request-association COMMAND epycbench_results_execution_request_association_tests)
    add_test(NAME EB-RES-002.execution-request-association-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_execution_request_association_test.py")

    add_executable(epycbench_results_execution_plan_admission_tests tests/unit/results_execution_plan_admission_test.cpp)
    target_include_directories(epycbench_results_execution_plan_admission_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(epycbench_results_execution_plan_admission_tests PRIVATE EPYCBENCH_MONITORING_FACT_TEST=1 EPYCBENCH_PREPARATION_GRAPH_TEST=1)
    target_link_libraries(epycbench_results_execution_plan_admission_tests PRIVATE epycbench_results_process_subject EpycBench::pairwise_v2_synthetic_lifecycle epycbench_runtime_test_support EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_execution_plan_admission_tests)
    epycbench_enable_warnings(epycbench_results_execution_plan_admission_tests)
    add_test(NAME EB-RES-002.execution-plan-admission COMMAND epycbench_results_execution_plan_admission_tests)
    add_test(NAME EB-RES-002.execution-plan-admission-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_execution_plan_admission_test.py")
endif()

add_library(epycbench_results_incremental_event STATIC
    src/results/incremental_event_profile.cpp
    src/results/incremental_event_preparation.cpp
    src/results/incremental_event_pair.cpp
    src/results/incremental_event_transaction.cpp)
target_include_directories(epycbench_results_incremental_event PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_incremental_event PRIVATE
    epycbench_results_pair_analysis
    epycbench_results_preflight
    epycbench_results_private_execution_plan
    epycbench_results_private_run_creation_serialization
    epycbench_results_private_normalization_serialization
    epycbench_results_private_source_outcome_serialization
    epycbench_results_private_topology_serialization
    epycbench_results_private_error_serialization EpycBench::results_journal_records EpycBench::core)
epycbench_enable_build_options(epycbench_results_incremental_event)
epycbench_enable_warnings(epycbench_results_incremental_event)
# These private Results archives call Session stock definitions (and request
# comparison) owned by process_subject. Express the existing static archive cycle
# so standalone readers link without relying on a test's Session construction.
foreach(results_session_consumer IN ITEMS
    epycbench_results_incremental_event
    epycbench_results_private_anonymous_thp_policy_serialization
    epycbench_results_private_base_build_serialization
    epycbench_results_private_frequency_policy_serialization
    epycbench_results_private_host_serialization
    epycbench_results_private_kernel_identity_serialization
    epycbench_results_private_normalization_serialization
    epycbench_results_private_process_thp_restriction_serialization
    epycbench_results_private_pu_cpu_identity_serialization
    epycbench_results_private_run_creation_serialization
    epycbench_results_private_source_outcome_serialization
    epycbench_results_private_system_controls_serialization
    epycbench_results_private_topology_serialization)
    target_link_libraries(${results_session_consumer} PRIVATE epycbench_results_process_subject)
endforeach()
if(BUILD_TESTING)
    add_executable(epycbench_results_producer_progress_tests tests/unit/results_producer_progress_test.cpp)
    target_compile_definitions(epycbench_results_producer_progress_tests PRIVATE EPYCBENCH_MONITORING_FACT_TEST=1 EPYCBENCH_PREPARATION_GRAPH_TEST=1)
    target_include_directories(epycbench_results_producer_progress_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_producer_progress_tests PRIVATE
        epycbench_results_incremental_event epycbench_results_process_subject
        EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_producer_progress_tests)
    epycbench_enable_warnings(epycbench_results_producer_progress_tests)
    add_test(NAME P1.producer-progress-results COMMAND epycbench_results_producer_progress_tests)
    set_tests_properties(P1.producer-progress-results PROPERTIES TIMEOUT 300)
    add_executable(epycbench_results_incremental_event_pair_tests tests/unit/results_incremental_event_pair_test.cpp)
    target_compile_definitions(epycbench_results_incremental_event_pair_tests PRIVATE EPYCBENCH_MONITORING_FACT_TEST=1 EPYCBENCH_PREPARATION_GRAPH_TEST=1)
    target_include_directories(epycbench_results_incremental_event_pair_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_incremental_event_pair_tests PRIVATE
        epycbench_results_incremental_event epycbench_results_process_subject
        EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_incremental_event_pair_tests)
    epycbench_enable_warnings(epycbench_results_incremental_event_pair_tests)
    add_test(NAME EB-RES-002.incremental-event-pair
        COMMAND epycbench_results_incremental_event_pair_tests "~[compact-finalization]~[compact-work]")
    set(compact_finalization_timeout 600)
    if(EPYCBENCH_ENABLE_ASAN_UBSAN)
        # This software replay/mutation supervisor does not change Native deadlines.
        set(compact_finalization_timeout 1800)
    endif()
    add_test(NAME EB-RES-002.compact-finalization
        COMMAND epycbench_results_incremental_event_pair_tests "[compact-finalization]")
    set_tests_properties(EB-RES-002.compact-finalization PROPERTIES
        TIMEOUT ${compact_finalization_timeout} RUN_SERIAL TRUE
        LABELS "results;synthetic;non-hardware")
    add_test(NAME EB-RES-002.compact-work
        COMMAND epycbench_results_incremental_event_pair_tests "[compact-work]")
    set_tests_properties(EB-RES-002.compact-work PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE LABELS "results;synthetic;non-hardware")
    add_test(NAME EB-RES-002.planning-approval-route
        COMMAND epycbench_results_incremental_event_pair_tests "[planning-approval-route]")
    add_test(NAME EB-RES-002.precontext-stock
        COMMAND epycbench_results_incremental_event_pair_tests "[precontext-stock]")
    set_tests_properties(EB-RES-002.precontext-stock PROPERTIES
        TIMEOUT 120 LABELS "results;synthetic;non-hardware")
    add_executable(epycbench_results_incremental_event_tests tests/unit/results_incremental_event_profile_test.cpp)
    target_include_directories(epycbench_results_incremental_event_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_incremental_event_tests PRIVATE
        epycbench_results_incremental_event epycbench_results_process_subject
        EpycBench::topology EpycBench::results_journal_records EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_incremental_event_tests)
    epycbench_enable_warnings(epycbench_results_incremental_event_tests)
    add_test(NAME EB-RES-002.incremental-event-profile COMMAND epycbench_results_incremental_event_tests)
    add_test(NAME EB-RES-002.incremental-event-profile-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_incremental_event_profile_test.py")
    add_test(NAME EB-RES-002.incremental-event-pair-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_incremental_event_pair_test.py")
endif()

# Private C3 filesystem composition; no public route selects this target.
add_library(epycbench_results_filesystem_journal STATIC src/results/filesystem_journal.cpp)
target_include_directories(epycbench_results_filesystem_journal PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_filesystem_journal PRIVATE epycbench_results_incremental_event EpycBench::platform_linux)
epycbench_enable_build_options(epycbench_results_filesystem_journal)
epycbench_enable_warnings(epycbench_results_filesystem_journal)
add_library(epycbench_results_producer_progress STATIC src/results/producer_progress.cpp src/results/native_lifecycle.cpp
    src/results/preparation_observation.cpp)
target_include_directories(epycbench_results_producer_progress PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_producer_progress PRIVATE
    epycbench_results_filesystem_journal epycbench_results_incremental_event
    epycbench_pairwise_v2_native_lifecycle epycbench_results_process_subject EpycBench::core)
epycbench_enable_build_options(epycbench_results_producer_progress)
epycbench_enable_warnings(epycbench_results_producer_progress)
add_library(epycbench_benchmark_run_composition STATIC src/cli/benchmark_run_composition.cpp)
target_include_directories(epycbench_benchmark_run_composition PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_benchmark_run_composition PRIVATE
    epycbench_results_producer_progress epycbench_results_process_subject epycbench_results_final_document
    epycbench_pairwise_v2_production_entry EpycBench::platform_linux)
epycbench_enable_build_options(epycbench_benchmark_run_composition)
epycbench_enable_warnings(epycbench_benchmark_run_composition)
if(BUILD_TESTING)
    target_link_libraries(epycbench_benchmark_run_composition_tests PRIVATE epycbench_benchmark_run_composition)
    target_link_libraries(epycbench_runtime_pairwise_production_authority_tests PRIVATE epycbench_benchmark_run_composition epycbench_cli)
    target_compile_definitions(epycbench_runtime_pairwise_production_authority_tests PRIVATE EPYCBENCH_MONITORING_FACT_TEST=1 EPYCBENCH_PREPARATION_GRAPH_TEST=1)
    target_link_libraries(epycbench_results_producer_progress_tests PRIVATE epycbench_results_producer_progress)
    target_link_libraries(epycbench_results_incremental_event_pair_tests PRIVATE epycbench_results_producer_progress epycbench_results_final_document epycbench_benchmark_run_composition)
    target_link_libraries(epycbench_runtime_monitoring_production_admission_tests PRIVATE epycbench_results_producer_progress epycbench_results_process_subject epycbench_results_final_document epycbench_benchmark_run_composition epycbench_cli)
    target_compile_definitions(epycbench_runtime_monitoring_production_admission_tests PRIVATE EPYCBENCH_MONITORING_FACT_TEST=1 EPYCBENCH_PREPARATION_GRAPH_TEST=1)
endif()
if(BUILD_TESTING)
    add_executable(epycbench_results_filesystem_journal_tests tests/unit/results_filesystem_journal_test.cpp)
    target_compile_definitions(epycbench_results_filesystem_journal_tests PRIVATE EPYCBENCH_MONITORING_FACT_TEST=1 EPYCBENCH_PREPARATION_GRAPH_TEST=1)
    target_include_directories(epycbench_results_filesystem_journal_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_filesystem_journal_tests PRIVATE epycbench_results_filesystem_journal epycbench_results_incremental_event epycbench_results_process_subject EpycBench::topology EpycBench::results_journal_records EpycBench::platform_linux EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_filesystem_journal_tests)
    epycbench_enable_warnings(epycbench_results_filesystem_journal_tests)
    add_test(NAME EB-RES-002.filesystem-journal COMMAND epycbench_results_filesystem_journal_tests)
    add_test(NAME EB-RES-002.filesystem-journal-nonactivation COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/results_filesystem_journal_test.py")
    set_tests_properties(EB-RES-002.filesystem-journal PROPERTIES TIMEOUT 300)
endif()

# Private self-contained supplied Results; no public route or acquisition.
add_library(epycbench_results_final_document STATIC src/results/final_document.cpp)
target_include_directories(epycbench_results_final_document PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_final_document PRIVATE epycbench_results_filesystem_journal epycbench_results_incremental_event epycbench_results_pairwise_analysis EpycBench::results EpycBench::core)
epycbench_enable_build_options(epycbench_results_final_document)
epycbench_enable_warnings(epycbench_results_final_document)

# Offline projection consumes an already validated document; no acquisition.
add_library(epycbench_results_export STATIC src/results/result_export.cpp)
target_include_directories(epycbench_results_export PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_results_export PRIVATE epycbench_results_final_document EpycBench::core)
epycbench_enable_build_options(epycbench_results_export)
epycbench_enable_warnings(epycbench_results_export)
target_link_libraries(epycbench_cli PRIVATE epycbench_results_export)

if(BUILD_TESTING)
    add_executable(epycbench_results_export_tests tests/unit/results_export_test.cpp)
    target_include_directories(epycbench_results_export_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_export_tests PRIVATE epycbench_results_export epycbench_cli EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_export_tests)
    epycbench_enable_warnings(epycbench_results_export_tests)
    add_test(NAME EB-RES-007.results-export COMMAND epycbench_results_export_tests)
    set_tests_properties(EB-RES-007.results-export PROPERTIES TIMEOUT 300)
endif()

if(BUILD_TESTING)
    add_executable(epycbench_results_cli_tests tests/component/results_cli_test.cpp)
    target_compile_definitions(epycbench_results_cli_tests PRIVATE EPYCBENCH_MONITORING_FACT_TEST=1 EPYCBENCH_PREPARATION_GRAPH_TEST=1)
    target_include_directories(epycbench_results_cli_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_cli_tests PRIVATE epycbench_cli epycbench_results_final_document
        epycbench_results_filesystem_journal epycbench_results_incremental_event
        epycbench_results_process_subject EpycBench::topology EpycBench::results_journal_records
        EpycBench::platform_linux EpycBench::core Catch2::Catch2WithMain)
    epycbench_enable_build_options(epycbench_results_cli_tests)
    epycbench_enable_warnings(epycbench_results_cli_tests)
    add_test(NAME EB-RES-007.results-cli COMMAND epycbench_results_cli_tests "~[completed_profile]~[external_profile]~[external_semantics]~[export_complete]~[export_limits]~[export_sink]")
    foreach(export_case IN ITEMS "v1 completed" "v1 interrupted" "v2 completed" "v2 interrupted")
        string(REPLACE " " "-" export_case_name "${export_case}")
        add_test(NAME EB-RES-007.results-export-${export_case_name} COMMAND epycbench_results_cli_tests "Results export preserves genuine ${export_case} profile")
        set_tests_properties(EB-RES-007.results-export-${export_case_name} PROPERTIES TIMEOUT 300)
    endforeach()
    add_test(NAME EB-RES-007.results-export-sinks COMMAND epycbench_results_cli_tests "[export_sink]")
    set_tests_properties(EB-RES-007.results-export-sinks PROPERTIES TIMEOUT 300)
    add_test(NAME EB-RES-007.results-export-schema COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tests/tools/results_export_test.py")
    set_tests_properties(EB-RES-007.results-export-schema PROPERTIES TIMEOUT 120)
    add_test(NAME EB-RES-007.results-export-limits COMMAND epycbench_results_cli_tests "[export_limits]")
    set_tests_properties(EB-RES-007.results-export-limits PROPERTIES TIMEOUT 300)
    add_test(NAME EB-RES-007.results-cli-completed-profiles COMMAND epycbench_results_cli_tests "[completed_profile]")
    set_tests_properties(EB-RES-007.results-cli PROPERTIES TIMEOUT 300)
    set_tests_properties(EB-RES-007.results-cli-completed-profiles PROPERTIES TIMEOUT 300)
    add_executable(epycbench_results_final_document_tests tests/unit/results_final_document_test.cpp)
    target_compile_definitions(epycbench_results_final_document_tests PRIVATE EPYCBENCH_MONITORING_FACT_TEST=1 EPYCBENCH_PREPARATION_GRAPH_TEST=1)
    target_include_directories(epycbench_results_final_document_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(epycbench_results_final_document_tests PRIVATE epycbench_results_final_document epycbench_results_filesystem_journal epycbench_results_incremental_event epycbench_results_process_subject EpycBench::topology EpycBench::results_journal_records EpycBench::platform_linux EpycBench::core Catch2::Catch2WithMain)
    target_link_libraries(epycbench_results_final_document_tests PRIVATE epycbench_benchmark_run_composition epycbench_cli)
    epycbench_enable_build_options(epycbench_results_final_document_tests)
    epycbench_enable_warnings(epycbench_results_final_document_tests)
    add_test(NAME EB-RES-002.final-document
        COMMAND epycbench_results_final_document_tests
            "~[final_schema]~[compact-matrix-56]~[compact-matrix-56-maximum]~[compact-prefix]~[compact-matrix-preflight]~[compact-projection]")
    add_test(NAME EB-RES-002.compact-projection
        COMMAND epycbench_results_final_document_tests "[compact-projection]")
    set_tests_properties(EB-RES-002.compact-projection PROPERTIES
        TIMEOUT ${compact_finalization_timeout} RUN_SERIAL TRUE LABELS "results;synthetic;non-hardware")
    add_test(NAME EB-RES-002.compact-prefix
        COMMAND epycbench_results_final_document_tests "[compact-prefix]")
    add_test(NAME EB-RES-002.compact-matrix-preflight
        COMMAND epycbench_results_final_document_tests "[compact-matrix-preflight]")
    set_tests_properties(EB-RES-002.compact-prefix EB-RES-002.compact-matrix-preflight PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE LABELS "results;synthetic;non-hardware")
    add_test(NAME EB-RES-002.compact-matrix-56 COMMAND epycbench_results_final_document_tests "[compact-matrix-56]")
    set_tests_properties(EB-RES-002.compact-matrix-56 PROPERTIES TIMEOUT 1800 RUN_SERIAL TRUE LABELS "results;synthetic;non-hardware")
    # The complete max20 publication proof uses the committed optimized preset;
    # the same case/helper still compiles in every ordinary/sanitizer preset.
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_test(NAME EB-RES-002.compact-matrix-56-maximum COMMAND epycbench_results_final_document_tests "[compact-matrix-56-maximum]")
        set_tests_properties(EB-RES-002.compact-matrix-56-maximum PROPERTIES TIMEOUT 3600 RUN_SERIAL TRUE LABELS "results;synthetic;non-hardware")
    endif()
    add_test(NAME EB-RES-002.final-document-nonactivation COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tests/tools/results_final_document_test.py")
    set_tests_properties(EB-RES-002.final-document PROPERTIES TIMEOUT 300)
    foreach(case completed interrupted)
        add_test(NAME EB-RES-002.synthetic-integration.${case}
            COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tests/tools/results_final_document_test.py"
                --demo $<TARGET_FILE:epycbench_results_final_document_tests> "${case}")
        set_tests_properties(EB-RES-002.synthetic-integration.${case} PROPERTIES TIMEOUT 300)
    endforeach()
    foreach(case preparation bandwidth latency zero-edge partial-next invalid operational exact-numbers planned postclosure)
        add_test(NAME EB-RES-002.final-document-schema.${case}
            COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tests/tools/results_final_document_test.py"
                --case $<TARGET_FILE:epycbench_results_final_document_tests> "${case}")
        set_tests_properties(EB-RES-002.final-document-schema.${case} PROPERTIES TIMEOUT 300)
    endforeach()
endif()
