#pragma once

#include "epycbench/runtime/runner_control_reader.hpp"

namespace epycbench::runtime::detail {

struct RunnerControlStopOperation {
    void* context{};
    std::expected<void, core::Error> (*request_stop)(void*){};
};

struct RunnerControlReaderAccess {
    [[nodiscard]] static auto create(ipc::ByteReader* reader, RunnerControlStopOperation stop)
        -> std::expected<RunnerControlReadCapability, core::Error>;
};

} // namespace epycbench::runtime::detail
