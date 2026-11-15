#pragma once

#include "epycbench/platform_linux/task_stat.hpp"

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace epycbench::platform_linux::detail {

struct TaskStatOperations {
    void* context;
    std::expected<std::string, core::Error> (*read)(void*, LinuxThreadId) noexcept;
};

enum class BoundedTaskStatFailureKind : unsigned char {
    invalid_operations,
    open_failed,
    read_failed,
    close_failed,
    contained_exception,
    contained_unknown_failure,
    task_id,
    task_identity_mismatch,
    command_delimiter,
    truncated_record,
    minor_faults,
    minor_faults_out_of_range,
    major_faults,
    major_faults_out_of_range,
};

struct BoundedTaskStatFailure final {
    BoundedTaskStatFailureKind kind{};
    int error_number{};
};

struct BoundedTaskStatReadFailure final {
    BoundedTaskStatFailure primary{};
    std::optional<BoundedTaskStatFailure> cleanup;
};

struct BoundedTaskStatOperations final {
    struct IntegerResult final { long value{}; int error_number{}; };
    void* context{};
    IntegerResult (*open)(void*, LinuxThreadId) noexcept{};
    IntegerResult (*read)(void*, int, void*, std::size_t) noexcept{};
    IntegerResult (*close)(void*, int) noexcept{};
    void (*before)(void*){};
};

class TaskStatAccess {
  public:
    [[nodiscard]] static auto read(LinuxThreadId thread_id,
                                   const TaskStatOperations& operations) noexcept
        -> std::expected<LinuxTaskPageFaultCounters, core::Error>;
    [[nodiscard]] static auto parse(std::string_view record, LinuxThreadId expected_thread_id)
        -> std::expected<LinuxTaskPageFaultCounters, core::Error>;
    [[nodiscard]] static auto read_bounded(
        LinuxThreadId, std::span<char>, const BoundedTaskStatOperations&) noexcept
        -> std::expected<LinuxTaskPageFaultCounters, BoundedTaskStatReadFailure>;
};

[[nodiscard]] auto production_bounded_task_stat_operations() noexcept
    -> BoundedTaskStatOperations;

} // namespace epycbench::platform_linux::detail
