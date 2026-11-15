#pragma once

namespace epycbench::runtime::detail {

struct PairwiseRunnerExecutionBackendTestAccess final {
    static void set_pre_storage_hook(void (*)()) noexcept;
    static void set_post_run_hook(void (*)()) noexcept;
    static void set_post_finalize_hook(void (*)()) noexcept;
};

} // namespace epycbench::runtime::detail
