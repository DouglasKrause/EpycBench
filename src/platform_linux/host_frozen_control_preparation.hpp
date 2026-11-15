#pragma once

#include "platform_linux/host_work_admission.hpp"

#include <optional>
#include <pthread.h>
#include <sched.h>

namespace epycbench::platform_linux::host_work {
enum class ControlOperation : unsigned char {
    state, capacity, reservation, allocation, identity, affinity_capture,
    affinity_check, affinity_changed, affinity_set, current_cpu, page_size,
    mapping, protection, attr_init, attr_joinable, attr_guard, attr_stack,
    stack_observation, attr_destroy, unmap, clock
};
struct ControlFailure final {
    ControlOperation operation{};
    int native_error{};
};
[[nodiscard]] auto frozen_control_raw_now() noexcept -> std::expected<std::uint64_t, ControlFailure>;
// Stable enclosing State owns this object. All registrations after acquisition
// are inline. Native attributes and stack backing outlive the actual thread.
// This is a Linux preparation leaf, not an authority to activate a source Run.
class FrozenControlPreparation final {
  public:
    FrozenControlPreparation() noexcept = default;
    FrozenControlPreparation(const FrozenControlPreparation&) = delete;
    auto operator=(const FrozenControlPreparation&) -> FrozenControlPreparation& = delete;
    ~FrozenControlPreparation() noexcept;
    [[nodiscard]] auto capture_original_main(ResourceLedger&, std::size_t capacity) noexcept
        -> std::expected<void, ControlFailure>;
    [[nodiscard]] auto check_original_main() noexcept -> std::expected<void, ControlFailure>;
    [[nodiscard]] auto prepare_control(ResourceLedger&, core::LinuxCpuId) noexcept
        -> std::expected<void, ControlFailure>;
    [[nodiscard]] auto pin_and_observe_control() noexcept -> std::expected<void, ControlFailure>;
    [[nodiscard]] auto check_control() noexcept -> std::expected<void, ControlFailure>;
    [[nodiscard]] auto attributes() const noexcept -> const pthread_attr_t*;
    [[nodiscard]] auto original_main_id() const noexcept -> std::uint64_t { return main_id_; }
    [[nodiscard]] auto on_original_main() const noexcept -> bool;
    [[nodiscard]] auto on_control_thread() const noexcept -> bool;
    [[nodiscard]] auto control_failure() const noexcept -> const std::optional<ControlFailure>& { return control_failure_; }
    [[nodiscard]] auto control_id() const noexcept -> std::uint64_t { return control_id_; }
    // Caller must first confirm join AND destroy the actual native thread owner.
    // Failure retains remaining owners and the whole applicable lease.
    [[nodiscard]] auto retire() noexcept -> bool;
    [[nodiscard]] auto failure() const noexcept -> const std::optional<ControlFailure>& { return failure_; }
    [[nodiscard]] auto cleanup_failures() const noexcept -> std::span<const ControlFailure> {
        return {cleanup_.data(), cleanup_count_};
    }
    [[nodiscard]] auto retired() const noexcept -> bool;
  private:
    [[nodiscard]] auto fail(ControlOperation, int = 0) noexcept -> std::unexpected<ControlFailure>;
    [[nodiscard]] auto control_fail(ControlOperation, int = 0) noexcept -> std::unexpected<ControlFailure>;
    void cleanup_failure(ControlOperation, int) noexcept;
    std::array<cpu_set_t*, 3> masks_{};
    std::size_t mask_bytes_{}, capacity_{};
    std::uint64_t main_id_{}, control_id_{};
    core::LinuxCpuId control_cpu_{0};
    void* mapping_{};
    std::size_t mapping_bytes_{}, page_bytes_{};
    pthread_attr_t attributes_{}, observed_attributes_{};
    bool attributes_owned_{}, observed_attributes_owned_{}, captured_{}, mapping_owned_{};
    std::optional<ResourceLease> masks_lease_, control_lease_;
    std::optional<ControlFailure> failure_, control_failure_;
    std::array<ControlFailure, 8> cleanup_{};
    std::size_t cleanup_count_{};
    std::size_t retirement_attempts_{};
};
} // namespace epycbench::platform_linux::host_work
