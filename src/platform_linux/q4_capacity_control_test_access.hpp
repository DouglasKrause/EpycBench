#pragma once

#include "epycbench/platform_linux/q4_capacity_control.hpp"

#include <chrono>
#include <cstddef>
#include <sys/socket.h>
#include <poll.h>

namespace epycbench::platform_linux::detail {

// Private syscall boundary used only by the capacity-control unit proof.  The
// production entry point always supplies the native operations.
struct Q4CapacityControlOperations final {
    int (*poll)(pollfd*, nfds_t, int){};
    ssize_t (*sendmsg)(int, const msghdr*, int){};
    ssize_t (*read)(int, void*, std::size_t){};
    int (*shutdown)(int, int){};
    int (*close)(int){};
    std::chrono::steady_clock::time_point (*now)(){};
};

[[nodiscard]] auto q4_capacity_control_exchange_with_operations(
    int descriptor, std::uint64_t evidence_bytes, std::uint64_t stdout_bytes,
    std::uint64_t timeout_milliseconds,
    const Q4CapacityControlOperations& operations) noexcept
    -> Q4CapacityControlResult;

} // namespace epycbench::platform_linux::detail
