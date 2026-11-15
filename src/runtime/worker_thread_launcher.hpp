#pragma once

#include "epycbench/core/error.hpp"

#include <cstddef>
#include <expected>
#include <thread>

namespace epycbench::runtime::detail {

using WorkerThreadEntry = void (*)(void*, std::size_t) noexcept;

struct WorkerThreadLauncher {
    void* context;
    std::expected<std::jthread, core::Error> (*launch)(void*, WorkerThreadEntry, void*,
                                                       std::size_t);
};

[[nodiscard]] auto production_worker_thread_launcher() noexcept -> WorkerThreadLauncher;

} // namespace epycbench::runtime::detail
