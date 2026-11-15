#pragma once

#include "epycbench/core/error.hpp"

#include <cstdint>
#include <expected>
#include <string>

namespace epycbench::monitoring::qualification::detail {
enum class MonitoringQ3ParserAgreementStage:unsigned char{oracle,agreement};
struct MonitoringQ3ParserAgreementFailure final{MonitoringQ3ParserAgreementStage stage{};core::Error primary;};

class MonitoringQ3TemperatureParserAgreement final {
  public:
    MonitoringQ3TemperatureParserAgreement(const MonitoringQ3TemperatureParserAgreement&) = delete;
    MonitoringQ3TemperatureParserAgreement(MonitoringQ3TemperatureParserAgreement&&) noexcept = default;

    [[nodiscard]] static auto authenticate(std::string owned_bytes) noexcept
        -> std::expected<MonitoringQ3TemperatureParserAgreement, core::Error>;
    [[nodiscard]] auto value() const noexcept -> std::int64_t { return value_; }

  private:
    struct Operations final {
        void* context{};
        std::expected<std::int64_t, core::Error> (*production)(void*, std::string_view){};
        std::expected<std::int64_t, core::Error> (*oracle)(void*, std::string_view){};
    };
    [[nodiscard]] static auto authenticate_with(std::string, Operations) noexcept
        -> std::expected<MonitoringQ3TemperatureParserAgreement, core::Error>;
    [[nodiscard]] static auto authenticate_staged(std::string) noexcept
        ->std::expected<MonitoringQ3TemperatureParserAgreement,MonitoringQ3ParserAgreementFailure>;
    [[nodiscard]] static auto authenticate_staged_with(std::string,Operations) noexcept
        ->std::expected<MonitoringQ3TemperatureParserAgreement,MonitoringQ3ParserAgreementFailure>;
    explicit MonitoringQ3TemperatureParserAgreement(std::int64_t value) noexcept : value_{value} {}
    std::int64_t value_{};
    friend class MonitoringQ3TemperatureParserAgreementTestAccess;
    friend class MonitoringQ3TemperatureCycleAuthority;
};

class MonitoringQ3TemperatureParserAgreementTestAccess final {
  public:
    using Operations=MonitoringQ3TemperatureParserAgreement::Operations;
    [[nodiscard]] static auto authenticate(std::string bytes, Operations operations) noexcept {
        return MonitoringQ3TemperatureParserAgreement::authenticate_with(std::move(bytes),operations);
    }
    [[nodiscard]] static auto authenticate_staged(std::string bytes,Operations operations)noexcept{
        return MonitoringQ3TemperatureParserAgreement::authenticate_staged_with(std::move(bytes),operations);
    }
};

} // namespace epycbench::monitoring::qualification::detail
