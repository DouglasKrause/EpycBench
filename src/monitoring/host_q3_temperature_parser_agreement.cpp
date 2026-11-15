#include "monitoring/host_q3_temperature_parser_agreement.hpp"

#include "platform_linux/hwmon_temperature_access.hpp"

#include <limits>
#include <string_view>

namespace epycbench::monitoring::qualification::detail {
namespace {
constexpr std::size_t byte_cap = 128U;
constexpr auto space(char value) noexcept -> bool {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' ||
           value == '\v';
}
auto error(std::string classification) -> core::Error {
    return {.category=core::ErrorCategory::monitoring,
        .code=core::StableErrorCode{"monitoring.q3.temperature.parser_agreement_failed"},
        .message="Monitoring Q3 temperature parser agreement failed",
        .operation="authenticate_monitoring_q3_temperature_parser_agreement",
        .os_error=std::nullopt,.context={{"classification",std::move(classification)}},
        .severity=core::ErrorSeverity::error,
        .continuation=core::ContinuationPolicy::abort_command};
}
auto oracle(std::string_view bytes) noexcept -> std::expected<std::int64_t,core::Error> {
    while(!bytes.empty() && space(bytes.front())) bytes.remove_prefix(1U);
    while(!bytes.empty() && space(bytes.back())) bytes.remove_suffix(1U);
    if(bytes.empty() || bytes.front()=='+') return std::unexpected{error("oracle_invalid")};
    bool negative=false;
    if(bytes.front()=='-') { negative=true; bytes.remove_prefix(1U); }
    if(bytes.empty()) return std::unexpected{error("oracle_invalid")};
    const auto limit=negative ? std::uint64_t{9223372036854775808ULL}
                              : std::uint64_t{9223372036854775807ULL};
    std::uint64_t magnitude{};
    for(const char digit:bytes) {
        if(digit<'0' || digit>'9') return std::unexpected{error("oracle_invalid")};
        const auto part=static_cast<std::uint64_t>(digit-'0');
        if(magnitude>(limit-part)/10U) return std::unexpected{error("oracle_overflow")};
        magnitude=magnitude*10U+part;
    }
    if(negative && magnitude==std::uint64_t{9223372036854775808ULL})
        return std::numeric_limits<std::int64_t>::min();
    const auto value=static_cast<std::int64_t>(magnitude);
    return negative ? -value : value;
}
}

auto MonitoringQ3TemperatureParserAgreement::authenticate(std::string owned_bytes) noexcept
    -> std::expected<MonitoringQ3TemperatureParserAgreement,core::Error> {
    auto result=authenticate_staged(std::move(owned_bytes));if(!result)return std::unexpected{std::move(result.error().primary)};return std::move(*result);
}
auto MonitoringQ3TemperatureParserAgreement::authenticate_staged(std::string owned_bytes) noexcept
 ->std::expected<MonitoringQ3TemperatureParserAgreement,MonitoringQ3ParserAgreementFailure>{return authenticate_staged_with(std::move(owned_bytes), {nullptr,
        [](void*,std::string_view bytes) {
            return platform_linux::detail::HwmonTemperatureAccess::parse(bytes);
        },
        [](void*,std::string_view bytes) { return oracle(bytes); }});
}

auto MonitoringQ3TemperatureParserAgreement::authenticate_with(
    std::string owned_bytes, Operations operations) noexcept
    -> std::expected<MonitoringQ3TemperatureParserAgreement,core::Error> {
    auto result=authenticate_staged_with(std::move(owned_bytes),operations);if(!result)return std::unexpected{std::move(result.error().primary)};return std::move(*result);
}
auto MonitoringQ3TemperatureParserAgreement::authenticate_staged_with(std::string owned_bytes,Operations operations) noexcept
 ->std::expected<MonitoringQ3TemperatureParserAgreement,MonitoringQ3ParserAgreementFailure>{
    const auto reject=[](MonitoringQ3ParserAgreementStage stage,core::Error value){return std::expected<MonitoringQ3TemperatureParserAgreement,MonitoringQ3ParserAgreementFailure>{std::unexpected{MonitoringQ3ParserAgreementFailure{stage,std::move(value)}}};};try {
        if(owned_bytes.size()>byte_cap) return reject(MonitoringQ3ParserAgreementStage::oracle,error("raw_byte_cap"));
        const std::string_view same_owned_bytes{owned_bytes};
        std::expected<std::int64_t,core::Error> production;
        try { production=operations.production(operations.context,same_owned_bytes); }
        catch (...) { return reject(MonitoringQ3ParserAgreementStage::agreement,error("contained")); }
        std::expected<std::int64_t,core::Error> independent;
        try { independent=operations.oracle(operations.context,same_owned_bytes); }
        catch (...) { return reject(MonitoringQ3ParserAgreementStage::oracle,error("contained")); }
        if(!production || !independent) {
            if(!independent) return reject(MonitoringQ3ParserAgreementStage::oracle,error(!production?"both_rejected":"acceptance_mismatch"));
            return reject(MonitoringQ3ParserAgreementStage::agreement,error("acceptance_mismatch"));
        }
        if(*production!=*independent) return reject(MonitoringQ3ParserAgreementStage::agreement,error("value_mismatch"));
        return MonitoringQ3TemperatureParserAgreement{*production};
    } catch (...) { return reject(MonitoringQ3ParserAgreementStage::agreement,error("contained")); }
}
} // namespace epycbench::monitoring::qualification::detail
