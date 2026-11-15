#pragma once

#include "epycbench/monitoring/page_faults.hpp"

namespace epycbench::monitoring::detail {

class PageFaultSourceAccess {
  public:
    using ReadOperation =
        std::expected<platform_linux::LinuxTaskPageFaultCounters, core::Error> (*)(
            void*, platform_linux::LinuxThreadId) noexcept;

    [[nodiscard]] static auto create(core::WorkerId worker_id,
                                     platform_linux::LinuxThreadId thread_id, void* reader_context,
                                     ReadOperation read_operation) noexcept -> PageFaultSource {
        return PageFaultSource{worker_id, thread_id, reader_context, read_operation};
    }
};

} // namespace epycbench::monitoring::detail
