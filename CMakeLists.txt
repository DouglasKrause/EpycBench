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
if(EPYCBENCH_STAGE_REFERENCE_FIXTURE)
    message(FATAL_ERROR "The public source distribution does not contain the reference topology fixture")
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
    message(FATAL_ERROR "Build provenance Git executable must be an absolute configured path")
endif()


# The executable and its modules share only the public package ABI and core types.
function(epycbench_library component)
    add_library(epycbench_${component} STATIC ${ARGN})
    add_library(EpycBench::${component} ALIAS epycbench_${component})
    target_compile_features(epycbench_${component} PUBLIC cxx_std_23)
    target_include_directories(epycbench_${component}
        PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        PRIVATE "${PROJECT_SOURCE_DIR}/src")
    epycbench_enable_build_options(epycbench_${component})
    epycbench_enable_warnings(epycbench_${component})
endfunction()

epycbench_library(core
    src/core/binary_digest.cpp
    src/core/canonical_json.cpp
    src/core/digest.cpp
    src/core/seed.cpp
)
target_link_libraries(epycbench_core PUBLIC nlohmann_json::nlohmann_json)
find_package(OpenSSL 3 QUIET COMPONENTS Crypto)
if(TARGET OpenSSL::Crypto)
    target_compile_definitions(epycbench_core PRIVATE EPYCBENCH_BINARY_DIGEST_OPENSSL)
    target_link_libraries(epycbench_core PRIVATE OpenSSL::Crypto)
endif()

epycbench_library(platform_linux
    src/platform_linux/file.cpp
    src/platform_linux/bounded_sysfs_operations.cpp
    src/platform_linux/kernel_modules.cpp
    src/platform_linux/topology_source_io.cpp
    src/platform_linux/task_stat.cpp
    src/platform_linux/perf_context_switch.cpp
    src/platform_linux/hardware_counters.cpp
    src/platform_linux/package_energy.cpp
    src/platform_linux/hwmon_temperature.cpp
    src/platform_linux/cpu_frequency.cpp
    src/platform_linux/dynamic_cpu_mask.cpp
    src/platform_linux/thread_identity.cpp
    src/platform_linux/controller_affinity.cpp
    src/platform_linux/thread_affinity.cpp
    src/platform_linux/presentation_files.cpp
    src/platform_linux/raw_monotonic_clock.cpp
    src/platform_linux/thread_rusage.cpp
    src/platform_linux/memory_mapping.cpp
    src/platform_linux/hugetlb_backing.cpp
    src/platform_linux/numa_policy.cpp
    src/platform_linux/page_location.cpp
    src/platform_linux/file_descriptor.cpp
    src/platform_linux/journal_file.cpp
    src/platform_linux/host_work_admission.cpp
    src/platform_linux/local_ipc.cpp
    src/platform_linux/child_process.cpp
    src/platform_linux/current_process_image.cpp
    src/platform_linux/process_ipc_operations.cpp
    src/platform_linux/controller_signal_scope.cpp
    src/platform_linux/experiment_operations.cpp
    src/platform_linux/run_start_identity.cpp
)
target_link_libraries(epycbench_platform_linux PUBLIC EpycBench::core PRIVATE NUMA::NUMA Threads::Threads ${CMAKE_DL_LIBS})

target_compile_definitions(epycbench_platform_linux PRIVATE _GNU_SOURCE)

epycbench_library(topology
    src/topology/fingerprint.cpp
    src/topology/checked_topology.cpp
    src/topology/invariants.cpp
    src/topology/hwloc_source.cpp
    src/topology/model.cpp
    src/topology/worker_locality.cpp
    src/topology/reconciliation.cpp
    src/topology/stable_id.cpp
    src/topology/serialization.cpp
    src/topology/topology_json_parser.cpp
    src/topology/linux_source.cpp
    src/topology/reference_fixture.cpp
    src/topology/reference_fixture_sha256.cpp
)
target_link_libraries(epycbench_topology PUBLIC EpycBench::core PRIVATE HWLOC::HWLOC EpycBench::platform_linux)

epycbench_library(statistics
    src/statistics/adaptive_stopping.cpp
    src/statistics/block_bootstrap.cpp
    src/statistics/block_representatives.cpp
    src/statistics/conclusion_classifier.cpp
    src/statistics/descriptive_statistics.cpp
    src/statistics/log_ratio_comparison.cpp
    src/statistics/outlier_flagging.cpp
    src/statistics/prepared_statistics.cpp
    src/statistics/primary_metric.cpp
)
target_link_libraries(epycbench_statistics PUBLIC EpycBench::core)

epycbench_library(monitoring
    src/monitoring/package_energy_evidence.cpp
    src/monitoring/hardware_counter_evidence.cpp
)
target_link_libraries(epycbench_monitoring PUBLIC EpycBench::core EpycBench::platform_linux)

add_library(epycbench_kernel_task_domain STATIC src/kernel/task.cpp src/kernel/affine_f64_task.cpp
    src/kernel/dot_f64_task.cpp
    src/kernel/max_u64_task.cpp
    src/kernel/min_u64_task.cpp
    src/kernel/norm_l1_f64_task.cpp
    src/kernel/sum_f64_task.cpp
    src/kernel/copy_bytes_task.cpp
    src/kernel/serialize_u64_task.cpp
    src/kernel/chase4_u64_task.cpp
    src/kernel/compact_u64_task.cpp
    src/kernel/pack_u4_task.cpp
    src/kernel/varint14_task.cpp
    src/kernel/gather_u64_task.cpp
    src/kernel/shared_atomic_counter.cpp
    src/kernel/false_sharing_counter.cpp
    src/kernel/lock_counter.cpp
    src/kernel/two_rank_barrier.cpp
    src/kernel/spsc_queue.cpp)
add_library(EpycBench::kernel_task_domain ALIAS epycbench_kernel_task_domain)
target_compile_features(epycbench_kernel_task_domain PUBLIC cxx_std_23)
target_include_directories(epycbench_kernel_task_domain
    PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench_kernel_task_domain PUBLIC EpycBench::core)
epycbench_enable_build_options(epycbench_kernel_task_domain)
epycbench_enable_warnings(epycbench_kernel_task_domain)
target_sources(epycbench_kernel_task_domain PRIVATE src/kernel/gemm_u64_task.cpp src/kernel/transpose_u64_task.cpp src/kernel/stencil_u64_task.cpp src/kernel/scatter_u64_task.cpp src/kernel/csr_u64_task.cpp src/kernel/bfs_u64_task.cpp src/kernel/convolution_u64_task.cpp src/kernel/histogram_u64_task.cpp src/kernel/stable_key_sort_u64_task.cpp src/kernel/search_tree_u64_task.cpp
    src/kernel/prefix_scan_u64_task.cpp src/kernel/merge_u64_task.cpp)

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
if(BUILD_TESTING)
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

epycbench_library(build_manifest
    src/build_manifest.cpp
    src/results/build_manifest_owner.cpp
    src/results/current_build_manifest.cpp
)
target_link_libraries(epycbench_build_manifest PUBLIC EpycBench::core)

add_dependencies(epycbench_build_manifest epycbench_build_info)
target_include_directories(epycbench_build_manifest PRIVATE "${PROJECT_BINARY_DIR}/generated/include")
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

set(EPYCBENCH_STAGED_RESOURCE_ROOT "${PROJECT_BINARY_DIR}/resources")

function(epycbench_add_kernel_module target source output directory)
    add_library(${target} MODULE "${source}")
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/include")
    set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME "${output}"
        LIBRARY_OUTPUT_DIRECTORY "${directory}")
    target_compile_options(${target} PRIVATE
        "$<$<CXX_COMPILER_ID:GNU>:-fno-lto;-fno-tree-loop-vectorize;-fno-tree-slp-vectorize;-fno-unroll-loops>"
        "$<$<CXX_COMPILER_ID:Clang>:-fno-lto;-fno-vectorize;-fno-slp-vectorize;-fno-unroll-loops>")
    epycbench_enable_build_options(${target})
    epycbench_enable_warnings(${target})
endfunction()
include(cmake/KernelPackages.cmake)
include(cmake/PackageExecution.cmake)
epycbench_library(scenario
    src/scenario/parser.cpp
    src/scenario/strict_json_parser.cpp
    src/scenario/identifier.cpp
    src/scenario/configuration_space.cpp
    src/scenario/package_scenario.cpp
)
target_link_libraries(epycbench_scenario PUBLIC EpycBench::topology epycbench_kernel_package_manifest PRIVATE epycbench_restricted_yaml)

epycbench_library(experiment
    src/experiment/analysis.cpp
    src/experiment/coordinator.cpp
    src/experiment/genetic.cpp
    src/experiment/search.cpp
    src/experiment/tradeoffs.cpp
    src/experiment/local_search.cpp
    src/experiment/annealing.cpp
    src/experiment/bayesian_search.cpp
    src/experiment/resolved_population.cpp
    src/experiment/genetic_history.cpp
    src/experiment/ledger_store.cpp
)
target_link_libraries(epycbench_experiment PUBLIC EpycBench::scenario PRIVATE EpycBench::platform_linux)

epycbench_library(cli
    src/cli/application.cpp
    src/cli/terminal_menu.cpp
    src/cli/workbench_actions.cpp
    src/cli/tui_screen.cpp
    src/cli/tui_session.cpp
    src/cli/tui_scenario.cpp
    src/cli/tui_placement.cpp
    src/cli/tui_sweep.cpp
    src/cli/tui_plan.cpp
    src/cli/tui_study.cpp
    src/platform_linux/workbench_terminal.cpp
    src/cli/machine_commands.cpp
    src/cli/json_csv.cpp
    src/cli/results_command.cpp
    src/cli/run_energy.cpp
    src/cli/run_energy_qualification.cpp
    src/cli/benchmark_run_command.cpp
    src/cli/experiment_command.cpp
    src/cli/scenario_command.cpp
    src/cli/package_authoring.cpp
    src/cli/package_run.cpp
    src/runtime/package_admission.cpp
    src/runtime/hardware_counter_wire.cpp
    src/runtime/controller_lifecycle_policy.cpp
    src/runtime/package_runner.cpp
    src/platform_linux/package_process.cpp
    src/results/journal_records.cpp
    src/results/result_export.cpp
    src/results/package_result.cpp
    src/results/package_observation.cpp
    src/results/package_monitoring_wire.cpp
    src/results/package_stream.cpp
    src/results/package_worker_counter_wire.cpp
)
target_link_libraries(epycbench_cli PUBLIC epycbench_topology_cli PRIVATE EpycBench::build_manifest EpycBench::experiment epycbench_package_execution EpycBench::statistics EpycBench::monitoring CLI11::CLI11)

add_executable(epycbench src/main.cpp)
target_link_libraries(epycbench_cli PRIVATE ftxui::component)
target_include_directories(epycbench PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(epycbench PRIVATE epycbench_cli EpycBench::build_manifest)
epycbench_enable_build_options(epycbench)
epycbench_enable_warnings(epycbench)
target_sources(epycbench_package_execution PRIVATE src/platform_linux/package_worker_counters.cpp)
target_link_libraries(epycbench_package_execution PRIVATE EpycBench::monitoring)
include(cmake/BundledKernelPackages.cmake)


include(cmake/GeneratedSumPackages.cmake)
