#include "epycbench/kernel/stable_key_sort_u64_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <array>
#include <functional>

namespace epycbench::kernel {
namespace {
using Input = std::span<const std::byte>;
using Output = std::span<std::byte>;
auto error(std::string_view reason) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{"kernel.task.stable_key_sort_u64." + std::string{reason}},
            .message = "Stable key sort validation failed",
            .operation = "kernel.task.stable_key_sort_u64",
            .os_error = {},
            .context = {{"classification", reason}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}
auto read(Input bytes, std::size_t index) noexcept -> std::uint64_t {
    std::uint64_t value{};
    for (unsigned i{}; i < 8U; ++i)
        value |= std::uint64_t{std::to_integer<unsigned char>(bytes[8U * index + i])} << (8U * i);
    return value;
}
void write(Output bytes, std::size_t index, std::uint64_t value) noexcept {
    for (unsigned i{}; i < 8U; ++i)
        bytes[8U * index + i] = static_cast<std::byte>((value >> (8U * i)) & 255U);
}
constexpr auto ordinal_mask = (UINT64_C(1) << 60U) - 1U;
auto record(const StableKeySortU64Config& c, std::uint64_t ordinal) noexcept -> std::uint64_t {
    return (detail::mix64_v1(c.input_seed() + ordinal) & ~ordinal_mask) | ordinal;
}
template <class Accept> void canonical_input(const StableKeySortU64Config& c, Accept accept) {
    accept(0U, stable_key_sort_u64_input_magic);
    accept(1U, c.record_count());
    for (std::uint64_t i = 0; i < c.record_count(); ++i)
        accept(stable_key_sort_u64_header_words + i, record(c, i));
}
// The positions of earlier records within a key determine the stable output.
auto canonical_output(const StableKeySortU64Config& c, Output output) -> std::uint64_t {
    std::array<std::size_t, stable_key_sort_u64_partitions> next{};
    for (std::size_t i = 0; i < c.record_count(); ++i)
        ++next[record(c, i) >> 60U];
    std::size_t start{};
    for (auto& count : next) {
        const auto size = count;
        count = start;
        start += size;
    }
    std::uint64_t folded{};
    for (std::size_t i = 0; i < c.record_count(); ++i) {
        const auto word = record(c, i);
        const auto position = next[word >> 60U]++;
        write(output, position, word);
        folded += (position + 1U) * word;
    }
    return folded;
}
} // namespace

auto StableKeySortU64Config::create(std::uint64_t records, std::uint64_t seed)
    -> std::expected<StableKeySortU64Config, core::Error> {
    if (!records || records > stable_key_sort_u64_maximum_records)
        return std::unexpected{error("invalid_record_count")};
    return StableKeySortU64Config{records, seed};
}
auto StableKeySortU64Config::work(TaskRepetitionCount r) const
    -> std::expected<TaskWork, core::Error> {
    if (r.value() > maximum_repetitions())
        return std::unexpected{error("work_overflow")};
    return TaskWork::create(element_count(), r);
}
auto initialize_stable_key_sort_u64_input(const StableKeySortU64Config& c, Output buf)
    -> std::expected<void, core::Error> {
    if (buf.size() < c.logical_bytes().value())
        return std::unexpected{error("storage_too_small")};
    canonical_input(c, [&](auto i, auto value) { write(buf, i, value); });
    return {};
}
auto prepare_stable_key_sort_u64_reference(const StableKeySortU64Config& c, Input input,
                                           Output output)
    -> std::expected<StableKeySortU64Reference, core::Error> {
    if (input.size() < c.logical_bytes().value() || output.size() < c.output_bytes().value())
        return std::unexpected{error("storage_too_small")};
    const std::less<const std::byte*> less;
    if (less(input.data(), output.data() + output.size()) &&
        less(output.data(), input.data() + input.size()))
        return std::unexpected{error("overlapping_storage")};
    bool matches = true;
    canonical_input(c, [&](auto i, auto value) { matches &= read(input, i) == value; });
    if (!matches)
        return std::unexpected{error("input_content_mismatch")};
    const auto folded = canonical_output(c, output);
    std::ranges::fill(output.subspan(c.output_bytes().value()),
                      stable_key_sort_u64_padding_sentinel);
    return StableKeySortU64Reference{c, core::sha256(input.first(c.logical_bytes().value())),
                                     core::sha256(output.first(c.output_bytes().value())), folded};
}
auto reset_stable_key_sort_u64_output(const StableKeySortU64Reference& ref, Output buf)
    -> std::expected<void, core::Error> {
    const auto& c = ref.configuration();
    if (buf.size() < c.output_bytes().value())
        return std::unexpected{error("storage_too_small")};
    std::ranges::fill(buf.first(c.output_bytes().value()), std::byte{0xff});
    std::ranges::fill(buf.subspan(c.output_bytes().value()), stable_key_sort_u64_padding_sentinel);
    return {};
}
auto verify_stable_key_sort_u64_output(const StableKeySortU64Reference& ref, Input buf)
    -> std::expected<XorU64Verification, core::Error> {
    const auto& c = ref.configuration();
    if (buf.size() < c.output_bytes().value())
        return std::unexpected{error("storage_too_small")};
    bool matches = true;
    std::uint64_t previous{};
    for (std::size_t i = 0; i < c.record_count(); ++i) {
        const auto word = read(buf, i);
        const auto ordinal = word & ordinal_mask;
        // The ordering of the tagged words is key first and ordinal second.
        if (ordinal >= c.record_count() || word != record(c, ordinal) || (i && word <= previous)) {
            matches = false;
            break;
        }
        previous = word;
    }
    const bool padding = std::ranges::all_of(buf.subspan(c.output_bytes().value()), [](auto value) {
        return value == stable_key_sort_u64_padding_sentinel;
    });
    return XorU64Verification{c.element_count(), ref.expected_output_digest(),
                              core::sha256(buf.first(c.output_bytes().value())), matches, padding};
}
} // namespace epycbench::kernel
