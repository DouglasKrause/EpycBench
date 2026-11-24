#pragma once

#include "epycbench/platform_linux/thread_affinity.hpp"
#include "epycbench/platform_linux/thread_identity.hpp"

#include <optional>

namespace epycbench::platform_linux {

// Lives on the calling Controller thread from admission through worker retirement.
class ControllerAffinity {
  public:
    struct Operations {
        decltype(&get_current_thread_affinity) get = get_current_thread_affinity;
        decltype(&set_current_thread_affinity) set = set_current_thread_affinity;
        decltype(&current_cpu) current = current_cpu;
    };
    ControllerAffinity() = default;
    explicit ControllerAffinity(Operations operations) : operations_(operations) {}
    ControllerAffinity(const ControllerAffinity&) = delete;
    ControllerAffinity& operator=(const ControllerAffinity&) = delete;
    ~ControllerAffinity();

    [[nodiscard]] auto pin(core::LinuxCpuId) -> std::expected<core::LinuxCpuId, core::Error>;
    [[nodiscard]] auto observe(core::LinuxCpuId) const -> std::expected<core::LinuxCpuId, core::Error>;
    [[nodiscard]] auto restore() -> std::expected<void, core::Error>;

  private:
    Operations operations_;
    std::optional<core::CpuSet> original_;
};
} // namespace epycbench::platform_linux
