#pragma once

#include "epycbench/core/error.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace epycbench::platform_linux {

namespace detail { class HwmonTemperatureAccess; }

class HwmonTemperatureSource {
  public:
    [[nodiscard]] static auto discover()
        -> std::expected<std::optional<HwmonTemperatureSource>, core::Error>;
    [[nodiscard]] auto read() const noexcept -> std::expected<std::int64_t, core::Error>;

    HwmonTemperatureSource(HwmonTemperatureSource&&) noexcept;
    auto operator=(HwmonTemperatureSource&&) noexcept -> HwmonTemperatureSource&;
    HwmonTemperatureSource(const HwmonTemperatureSource&) = delete;
    auto operator=(const HwmonTemperatureSource&) -> HwmonTemperatureSource& = delete;
    ~HwmonTemperatureSource() noexcept;

  private:
    using ReadOperation = std::expected<std::string, core::Error> (*)(void*, const std::string&);
    HwmonTemperatureSource(std::string input, void* context, ReadOperation read_operation) noexcept;
    std::string input_;
    void* context_{};
    ReadOperation read_{};
    friend class detail::HwmonTemperatureAccess;
};

} // namespace epycbench::platform_linux
