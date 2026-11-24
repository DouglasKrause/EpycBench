#include "epycbench/kernel/csr_u64_task.hpp"
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
            .code = core::StableErrorCode{"kernel.task.csr_u64." + std::string{reason}},
            .message = "Sparse matrix-vector validation failed",
            .operation = "kernel.task.csr_u64",
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
auto header(const CsrU64Config& c) noexcept {
    return std::array{csr_u64_input_magic, c.rows(), c.columns(), c.nonzeros()};
}
auto degree(const CsrU64Config& c, std::uint64_t row) noexcept {
    return std::min(c.columns(), row % 5U);
}
auto column(const CsrU64Config& c, std::uint64_t row, std::uint64_t j,
            std::uint64_t count) noexcept {
    const auto lo = j * c.columns() / count, hi = (j + 1U) * c.columns() / count;
    return lo + detail::mix64_v1((c.input_seed() ^ UINT64_C(0xd1b54a32d192ed03)) + 4U * row + j) %
                    (hi - lo);
}
auto coefficient(const CsrU64Config& c, std::uint64_t row, std::uint64_t j) noexcept {
    return detail::mix64_v1((c.input_seed() ^ UINT64_C(0x8cb92baa3f3d8dd7)) + 4U * row + j) |
           UINT64_C(1);
}
auto vector_value(const CsrU64Config& c, std::uint64_t col) noexcept {
    return detail::mix64_v1(c.input_seed() + col);
}

template <typename Accept> void canonical_input(const CsrU64Config& c, Accept accept) {
    const auto hdr = header(c);
    for (std::size_t i{}; i < hdr.size(); ++i)
        accept(i, hdr[i]);
    const auto indices = hdr.size() + c.rows() + 1U;
    const auto coefficients = indices + c.nonzeros(), vector = coefficients + c.nonzeros();
    std::uint64_t offset{};
    for (std::uint64_t row{}; row < c.rows(); ++row) {
        accept(hdr.size() + row, offset);
        const auto n = degree(c, row);
        for (std::uint64_t j{}; j < n; ++j, ++offset) {
            accept(indices + offset, column(c, row, j, n));
            accept(coefficients + offset, coefficient(c, row, j));
        }
    }
    accept(hdr.size() + c.rows(), offset);
    for (std::uint64_t col{}; col < c.columns(); ++col)
        accept(vector + col, vector_value(c, col));
}
template <typename Accept>
auto canonical_output(const CsrU64Config& c, Accept accept) -> std::uint64_t {
    const auto hdr = header(c);
    for (std::size_t i{}; i < hdr.size(); ++i)
        accept(i, hdr[i]);
    std::uint64_t folded{};
    for (std::uint64_t row{}; row < c.rows(); ++row) {
        std::uint64_t sum{};
        const auto n = degree(c, row);
        for (std::uint64_t j{}; j < n; ++j)
            sum += coefficient(c, row, j) * vector_value(c, column(c, row, j, n));
        accept(hdr.size() + row, sum);
        folded += sum;
    }
    return folded;
}
} // namespace

auto CsrU64Config::create(std::uint64_t rows, std::uint64_t cols, std::uint64_t seed)
    -> std::expected<CsrU64Config, core::Error> {
    if (!rows || rows > csr_u64_maximum_rows || !cols || cols > csr_u64_maximum_columns)
        return std::unexpected{error("invalid_dimensions")};
    std::uint64_t nonzeros{};
    for (std::uint64_t phase{}; phase < 5U; ++phase)
        nonzeros += (rows / 5U + (phase < rows % 5U)) * std::min(cols, phase);
    return CsrU64Config{rows, cols, seed, nonzeros};
}
auto CsrU64Config::work(TaskRepetitionCount r) const -> std::expected<TaskWork, core::Error> {
    if (r.value() > maximum_repetitions())
        return std::unexpected{error("work_overflow")};
    return TaskWork::create(element_count(), r);
}
auto initialize_csr_u64_input(const CsrU64Config& c, Output buf)
    -> std::expected<void, core::Error> {
    if (buf.size() < c.logical_bytes().value())
        return std::unexpected{error("storage_too_small")};
    canonical_input(c, [&](auto i, auto value) { write(buf, i, value); });
    return {};
}
auto prepare_csr_u64_reference(const CsrU64Config& c, Input input, Output output)
    -> std::expected<CsrU64Reference, core::Error> {
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
    const auto folded = canonical_output(c, [&](auto i, auto value) { write(output, i, value); });
    std::ranges::fill(output.subspan(c.output_bytes().value()), csr_u64_padding_sentinel);
    return CsrU64Reference{c, core::sha256(input.first(c.logical_bytes().value())),
                           core::sha256(output.first(c.output_bytes().value())), folded};
}
auto reset_csr_u64_output(const CsrU64Reference& ref, Output buf)
    -> std::expected<void, core::Error> {
    const auto& c = ref.configuration();
    if (buf.size() < c.output_bytes().value())
        return std::unexpected{error("storage_too_small")};
    canonical_output(c, [&](auto i, auto value) { write(buf, i, ~value); });
    std::ranges::fill(buf.subspan(c.output_bytes().value()), csr_u64_padding_sentinel);
    return {};
}
auto verify_csr_u64_output(const CsrU64Reference& ref, Input buf)
    -> std::expected<XorU64Verification, core::Error> {
    const auto& c = ref.configuration();
    if (buf.size() < c.output_bytes().value())
        return std::unexpected{error("storage_too_small")};
    bool matches = true;
    canonical_output(c, [&](auto i, auto value) { matches &= read(buf, i) == value; });
    const bool padding = std::ranges::all_of(buf.subspan(c.output_bytes().value()), [](auto value) {
        return value == csr_u64_padding_sentinel;
    });
    return XorU64Verification{c.element_count(), ref.expected_output_digest(),
                              core::sha256(buf.first(c.output_bytes().value())), matches, padding};
}
} // namespace epycbench::kernel
