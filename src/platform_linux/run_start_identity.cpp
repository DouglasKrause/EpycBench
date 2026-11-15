#include "platform_linux/run_start_identity.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <sys/random.h>
#include <utility>

namespace epycbench::platform_linux::detail {
namespace {
constexpr std::uint64_t last_calendar_second{253402300799ULL};
constexpr std::uint64_t last_uuid_millisecond{(std::uint64_t{1} << 48U) - 1U};

auto fail(RunStartStage stage, RunStartReason reason, int error = 0) noexcept
    -> std::unexpected<RunStartFailure> {
    return std::unexpected{RunStartFailure{stage, reason, error}};
}
auto native_entropy(void*, std::span<std::byte> bytes, unsigned int flags) noexcept
    -> RawEntropyResult {
    const auto result = ::getrandom(bytes.data(), bytes.size(), flags);
    const int error = result < 0 ? errno : 0;
    static_assert(sizeof(result) <= sizeof(std::ptrdiff_t));
    return {static_cast<std::ptrdiff_t>(result), error};
}
auto decimal(char* destination, std::size_t digits, std::uint64_t value) noexcept -> bool {
    while (digits != 0) {
        destination[--digits] = static_cast<char>('0' + value % 10U);
        value /= 10U;
    }
    return value == 0;
}
struct ClockFacts { std::uint64_t seconds, nanos, millis; };
auto read_clock(ClockOperations clock) noexcept -> std::expected<ClockFacts, RunStartFailure> {
    if (!clock.clock_gettime_raw)
        return fail(RunStartStage::inputs, RunStartReason::invalid_operations);
    const auto time = clock.clock_gettime_raw(clock.context, CLOCK_REALTIME);
    if (time.native_result == -1 && time.error_number != 0)
        return fail(RunStartStage::clock, RunStartReason::native_failure, time.error_number);
    if (time.native_result != 0 || time.error_number != 0)
        return fail(RunStartStage::clock, RunStartReason::unexpected_native_result,
                    time.error_number);
    if (!std::in_range<std::uint64_t>(time.timestamp.tv_sec) || time.timestamp.tv_nsec < 0 ||
        time.timestamp.tv_nsec > 999999999L)
        return fail(RunStartStage::clock, RunStartReason::invalid_timestamp);
    const auto seconds = static_cast<std::uint64_t>(time.timestamp.tv_sec);
    const auto nanos = static_cast<std::uint64_t>(time.timestamp.tv_nsec);
    if (seconds > last_calendar_second)
        return fail(RunStartStage::clock, RunStartReason::invalid_timestamp);
    if (seconds > std::numeric_limits<std::uint64_t>::max() / 1000U)
        return fail(RunStartStage::clock, RunStartReason::timestamp_overflow);
    const auto whole_millis = seconds * 1000U;
    const auto fraction_millis = nanos / 1000000U;
    if (fraction_millis > std::numeric_limits<std::uint64_t>::max() - whole_millis ||
        whole_millis + fraction_millis > last_uuid_millisecond)
        return fail(RunStartStage::clock, RunStartReason::timestamp_overflow);
    const auto millis = whole_millis + fraction_millis;
    if (!std::in_range<std::chrono::seconds::rep>(seconds))
        return fail(RunStartStage::clock, RunStartReason::timestamp_overflow);

    return ClockFacts{seconds, nanos, millis};
}
auto format_utc(std::uint64_t seconds, std::uint64_t nanos) noexcept
    -> std::expected<std::array<char, 31>, RunStartFailure> {
    // Seconds-based calendar arithmetic preserves the complete accepted year
    // domain. Only the within-second field uses nanoseconds; no absolute ns time.
    const std::chrono::sys_seconds point{std::chrono::seconds{
        static_cast<std::chrono::seconds::rep>(seconds)}};
    const auto day = std::chrono::floor<std::chrono::days>(point);
    const std::chrono::year_month_day calendar{day};
    const auto within_day = static_cast<std::uint64_t>((point - day).count());
    std::array<char, 31> utc{};
    utc[4] = '-'; utc[7] = '-'; utc[10] = 'T'; utc[13] = ':';
    utc[16] = ':'; utc[19] = '.'; utc[29] = 'Z';
    if (!calendar.ok() ||
        !decimal(utc.data(), 4, static_cast<std::uint64_t>(static_cast<int>(calendar.year()))) ||
        !decimal(utc.data() + 5, 2, static_cast<unsigned>(calendar.month())) ||
        !decimal(utc.data() + 8, 2, static_cast<unsigned>(calendar.day())) ||
        !decimal(utc.data() + 11, 2, within_day / 3600U) ||
        !decimal(utc.data() + 14, 2, (within_day / 60U) % 60U) ||
        !decimal(utc.data() + 17, 2, within_day % 60U) ||
        !decimal(utc.data() + 20, 9, nanos))
        return fail(RunStartStage::format, RunStartReason::formatting_failure);
    return utc;
}
} // namespace

auto acquire_event_utc(ClockOperations clock) noexcept
    -> std::expected<std::array<char, 30>, RunStartFailure> {
    const auto facts = read_clock(clock);
    if (!facts) return std::unexpected{facts.error()};
    const auto text = format_utc(facts->seconds, facts->nanos);
    if (!text) return std::unexpected{text.error()};
    std::array<char, 30> result{};
    for (std::size_t i{}; i < result.size(); ++i) result[i] = (*text)[i];
    return result;
}
auto acquire_event_utc() noexcept -> std::expected<std::array<char, 30>, RunStartFailure> {
    return acquire_event_utc(production_clock_operations());
}

auto RunStartIdentity::acquire() noexcept -> std::expected<RunStartIdentity, RunStartFailure> {
    return RunStartIdentityAccess::acquire(production_clock_operations(),
                                           {nullptr, native_entropy});
}
auto RunStartIdentityAccess::acquire(ClockOperations clock, EntropyOperations entropy) noexcept
    -> std::expected<RunStartIdentity, RunStartFailure> {
    if (clock.clock_gettime_raw == nullptr || entropy.read == nullptr)
        return fail(RunStartStage::inputs, RunStartReason::invalid_operations);
    const auto clock_facts = read_clock(clock);
    if (!clock_facts) return std::unexpected{clock_facts.error()};
    const auto [seconds, nanos, millis] = *clock_facts;

    std::array<std::byte, 10> random{};
    const auto result = entropy.read(entropy.context, random, GRND_NONBLOCK);
    if (result.count == -1 && result.error_number != 0)
        return fail(RunStartStage::entropy, RunStartReason::native_failure, result.error_number);
    if (result.count < 0 || result.error_number != 0)
        return fail(RunStartStage::entropy, RunStartReason::unexpected_native_result,
                    result.error_number);
    if (result.count < 10)
        return fail(RunStartStage::entropy, RunStartReason::short_entropy);
    if (result.count > 10)
        return fail(RunStartStage::entropy, RunStartReason::overreported_entropy);

    std::array<unsigned char, 16> bytes{};
    for (std::size_t i = 0; i < 6; ++i)
        bytes[i] = static_cast<unsigned char>((millis >> ((5U - i) * 8U)) & 0xffU);
    bytes[6] = static_cast<unsigned char>(0x70U | (std::to_integer<unsigned>(random[0]) & 0x0fU));
    bytes[7] = std::to_integer<unsigned char>(random[1]);
    bytes[8] = static_cast<unsigned char>(0x80U | (std::to_integer<unsigned>(random[2]) & 0x3fU));
    for (std::size_t i = 3; i < random.size(); ++i)
        bytes[i + 6] = std::to_integer<unsigned char>(random[i]);
    constexpr std::string_view hex = "0123456789abcdef";
    std::array<char, 37> uuid{};
    std::size_t position{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            uuid[position++] = '-';
        uuid[position++] = hex[bytes[i] >> 4U];
        uuid[position++] = hex[bytes[i] & 0x0fU];
    }

    auto utc = format_utc(seconds, nanos);
    if (!utc) return std::unexpected{utc.error()};
    return RunStartIdentity{uuid, *utc};
}
} // namespace epycbench::platform_linux::detail
