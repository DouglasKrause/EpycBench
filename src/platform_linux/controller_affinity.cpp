#include "platform_linux/controller_affinity.hpp"

namespace epycbench::platform_linux {
namespace {
auto mismatch(std::string_view message) -> core::Error {
    return {.category = core::ErrorCategory::platform_linux,
            .code = core::StableErrorCode{"linux.affinity.controller"},
            .message = std::string(message), .operation = "controller_affinity",
            .os_error = {}, .context = nlohmann::json::object()};
}
}

ControllerAffinity::~ControllerAffinity() {
    if (original_) static_cast<void>(operations_.set(*original_));
}

auto ControllerAffinity::pin(core::LinuxCpuId cpu) -> std::expected<core::LinuxCpuId, core::Error> {
    if (original_) return std::unexpected{mismatch("Controller affinity is already active")};
    auto previous = operations_.get();
    if (!previous) return std::unexpected{previous.error()};
    if (!previous->contains(cpu))
        return std::unexpected{mismatch("Selected controller CPU is outside the current effective affinity")};
    auto applied = operations_.set(core::CpuSet{cpu});
    if (!applied) return std::unexpected{applied.error()};
    original_.emplace(std::move(*previous));
    return observe(cpu);
}

auto ControllerAffinity::observe(core::LinuxCpuId expected) const -> std::expected<core::LinuxCpuId, core::Error> {
    const auto mask = operations_.get();
    if (!mask) return std::unexpected{mask.error()};
    const auto cpu = operations_.current();
    if (!cpu) return std::unexpected{cpu.error()};
    if (*mask != core::CpuSet{expected} || *cpu != expected)
        return std::unexpected{mismatch("Controller affinity or observed CPU differs from its frozen CPU")};
    return *cpu;
}

auto ControllerAffinity::restore() -> std::expected<void, core::Error> {
    if (!original_) return {};
    auto restored = operations_.set(*original_);
    if (!restored) return restored;
    const auto readback = operations_.get();
    if (!readback) return std::unexpected{readback.error()};
    if (*readback != *original_) return std::unexpected{mismatch("Original controller affinity could not be restored")};
    original_.reset();
    return {};
}
} // namespace epycbench::platform_linux
