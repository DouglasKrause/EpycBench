#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/id.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace epycbench::platform_linux {

namespace detail { class CpuFrequencyAccess; }

enum class CpuFrequencyCurrentKind { hardware_current, scaling_current };

class CpuFrequencyPolicySource {
  public:
    [[nodiscard]] static auto discover(core::LinuxCpuId cpu)
        -> std::expected<std::optional<CpuFrequencyPolicySource>, core::Error>;
    [[nodiscard]] auto read() const noexcept -> std::expected<std::uint64_t, core::Error>;

    [[nodiscard]] auto related_cpus() const noexcept -> std::span<const core::LinuxCpuId>;
    [[nodiscard]] auto kind() const noexcept -> CpuFrequencyCurrentKind;
    [[nodiscard]] auto scaling_driver() const noexcept -> const std::string&;
    [[nodiscard]] auto scaling_governor() const noexcept -> const std::string&;
    [[nodiscard]] auto scaling_minimum_hz() const noexcept -> std::uint64_t;
    [[nodiscard]] auto scaling_maximum_hz() const noexcept -> std::uint64_t;

    CpuFrequencyPolicySource(CpuFrequencyPolicySource&&) noexcept;
    auto operator=(CpuFrequencyPolicySource&&) noexcept -> CpuFrequencyPolicySource&;
    CpuFrequencyPolicySource(const CpuFrequencyPolicySource&) = delete;
    auto operator=(const CpuFrequencyPolicySource&) -> CpuFrequencyPolicySource& = delete;
    ~CpuFrequencyPolicySource() noexcept;

  private:
    using ReadOperation = std::expected<std::string, core::Error> (*)(void*, const std::string&);
    CpuFrequencyPolicySource(std::vector<core::LinuxCpuId> related_cpus,
                             CpuFrequencyCurrentKind kind, std::string driver,
                             std::string governor, std::uint64_t minimum_hz,
                             std::uint64_t maximum_hz, std::string current_path,
                             void* context, ReadOperation read) noexcept;

    std::vector<core::LinuxCpuId> related_cpus_;
    CpuFrequencyCurrentKind kind_;
    std::string driver_;
    std::string governor_;
    std::uint64_t minimum_hz_;
    std::uint64_t maximum_hz_;
    std::string current_path_;
    void* context_{};
    ReadOperation read_{};

    friend class detail::CpuFrequencyAccess;
};

} // namespace epycbench::platform_linux
