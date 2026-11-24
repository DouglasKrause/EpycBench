#include "epycbench/kernel/gemm_u64_task.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <functional>
#include <limits>

namespace epycbench::kernel {
namespace {
constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
auto error(std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{"kernel.task.gemm_u64." + std::string{classification}},
            .message = "Exact unsigned matrix task validation failed",
            .operation = "kernel.task.gemm_u64",
            .os_error = {},
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}
void store(std::span<std::byte> bytes, std::size_t index, std::uint64_t value) noexcept {
    for (unsigned i{}; i < 8U; ++i)
        bytes[index * 8U + i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
}
auto load(std::span<const std::byte> bytes, std::size_t index) noexcept -> std::uint64_t {
    std::uint64_t value{};
    for (unsigned i{}; i < 8U; ++i)
        value |= std::uint64_t{std::to_integer<unsigned char>(bytes[index * 8U + i])} << (8U * i);
    return value;
}
auto generated(const GemmU64Config& c, std::uint64_t ordinal) noexcept -> std::uint64_t {
    return detail::mix64_v1(c.input_seed() + ordinal);
}
// Independent of the timed kernels' full-width product and loop organization.
auto limb_product(std::uint64_t a, std::uint64_t b) noexcept -> std::uint64_t {
    const auto a0 = a & UINT64_C(0xffffffff), a1 = a >> 32U;
    const auto b0 = b & UINT64_C(0xffffffff), b1 = b >> 32U;
    return a0 * b0 + ((a0 * b1 + a1 * b0) << 32U);
}
auto expected_word(const GemmU64Config& c, std::uint64_t index) noexcept -> std::uint64_t {
    const auto row = index / c.columns(), column = index % c.columns();
    std::uint64_t answer{};
    for (std::uint64_t k{}; k < c.inner(); ++k)
        answer += limb_product(generated(c, row * c.inner() + k),
                               generated(c, c.rows() * c.inner() + k * c.columns() + column));
    return answer;
}
auto matches_rows(const GemmU64Config& c, std::span<const std::byte> bytes,
                  std::uint64_t first_a, std::uint64_t first_b) noexcept -> bool {
    if (load(bytes, 0U) != gemm_u64_input_magic || load(bytes, 1U) != c.rows() ||
        load(bytes, 2U) != c.inner() || load(bytes, 3U) != c.columns())
        return false;
    const auto a_words = c.rows() * c.inner();
    for (std::size_t i{}; i < a_words; ++i)
        if (load(bytes, i + 4U) != generated(c, first_a + i))
            return false;
    for (std::size_t i{}; i < c.inner() * c.columns(); ++i)
        if (load(bytes, i + a_words + 4U) != generated(c, first_b + i))
            return false;
    return true;
}

auto matches_input(const GemmU64Config& c, std::span<const std::byte> bytes) noexcept -> bool {
    return matches_rows(c, bytes, 0U, c.rows() * c.inner());
}
} // namespace

auto GemmU64Config::create(std::uint64_t rows, std::uint64_t inner, std::uint64_t columns,
                           std::uint64_t seed) -> std::expected<GemmU64Config, core::Error> {
    for (const auto dimension : {rows, inner, columns})
        if (!dimension || dimension > gemm_u64_maximum_dimension)
            return std::unexpected{error("dimension_out_of_range")};
    // The fixed initial range bounds both reference work and all size products.
    const GemmU64Config c{rows, inner, columns, seed};
    if (c.input_bytes().value() >
        std::numeric_limits<std::size_t>::max() - c.output_bytes().value())
        return std::unexpected{error("storage_not_representable")};
    return c;
}
auto GemmU64Config::input_bytes() const noexcept -> core::ByteCount {
    return core::ByteCount::from_base_units(gemm_u64_header_bytes +
                                            (rows_ * inner_ + inner_ * columns_) * 8U);
}
auto GemmU64Config::output_bytes() const noexcept -> core::ByteCount {
    return core::ByteCount::from_base_units(rows_ * columns_ * 8U);
}
auto GemmU64Config::native_input_bytes() const noexcept -> std::size_t {
    return static_cast<std::size_t>(input_bytes().value());
}
auto GemmU64Config::native_output_bytes() const noexcept -> std::size_t {
    return static_cast<std::size_t>(output_bytes().value());
}
auto GemmU64Config::output_elements() const noexcept -> ElementCount {
    return *ElementCount::create(rows_ * columns_);
}
auto GemmU64Config::maximum_repetitions() const noexcept -> std::uint64_t {
    return maximum / (rows_ * inner_ * columns_);
}
auto GemmU64Config::work(TaskRepetitionCount count) const
    -> std::expected<GemmU64Work, core::Error> {
    if (count.value() > maximum_repetitions())
        return std::unexpected{error("work_overflow")};
    return GemmU64Work{count,
                       core::WorkCount::from_base_units(rows_ * inner_ * columns_ * count.value()),
                       core::WorkCount::from_base_units(rows_ * columns_ * count.value())};
}
namespace {
auto initialize_rows(const GemmU64Config& c, std::uint64_t first_a, std::uint64_t first_b,
                     std::span<std::byte> input)
    -> std::expected<void, core::Error> {
    if (input.size() < c.native_input_bytes())
        return std::unexpected{error("storage_too_small")};
    store(input, 0U, gemm_u64_input_magic);
    store(input, 1U, c.rows());
    store(input, 2U, c.inner());
    store(input, 3U, c.columns());
    const auto a_words = c.rows() * c.inner();
    for (std::size_t i{}; i < a_words; ++i)
        store(input, i + 4U, generated(c, first_a + i));
    for (std::size_t i{}; i < c.inner() * c.columns(); ++i)
        store(input, i + a_words + 4U, generated(c, first_b + i));
    return {};
}
} // namespace
auto initialize_gemm_u64_input(const GemmU64Config& c, std::span<std::byte> input)
    -> std::expected<void, core::Error> {
    return initialize_rows(c, 0U, c.rows() * c.inner(), input);
}
auto GemmU64RowPartition::for_rank(const GemmU64Config& whole, std::size_t rank)
    -> std::expected<GemmU64RowPartition, core::Error> {
    if (rank > 1U || whole.rows() < 2U)
        return std::unexpected{error("two_nonempty_row_partitions_required")};
    const auto split = (whole.rows() + 1U) / 2U;
    const auto rows = rank == 0U ? split : whole.rows() - split;
    const auto local = GemmU64Config::create(rows, whole.inner(), whole.columns(), whole.input_seed());
    if (!local) return std::unexpected{local.error()};
    return GemmU64RowPartition{whole, *local, rank == 0U ? 0U : split};
}
auto GemmU64RowPartition::initialize_input(std::span<std::byte> input) const
    -> std::expected<void, core::Error> {
    return initialize_rows(local_, first_ * whole_.inner(), whole_.rows() * whole_.inner(), input);
}
auto GemmU64RowPartition::prepare_reference(std::span<const std::byte> input, std::span<std::byte> output) const
    -> std::expected<std::uint64_t, core::Error> {
    if (input.size() < local_.native_input_bytes() || output.size() < local_.native_output_bytes())
        return std::unexpected{error("storage_too_small")};
    const std::less<const std::byte*> less;
    if (less(input.data(), output.data() + output.size()) && less(output.data(), input.data() + input.size()))
        return std::unexpected{error("overlapping_storage")};
    if (!matches_rows(local_, input, first_ * whole_.inner(), whole_.rows() * whole_.inner()))
        return std::unexpected{error("input_content_mismatch")};
    const auto start = first_ * whole_.columns();
    std::uint64_t answer{};
    for (std::size_t i{}; i < local_.output_elements().value(); ++i) {
        const auto value = expected_word(whole_, start + i);
        store(output, i, value);
        answer += value;
    }
    std::ranges::fill(output.subspan(local_.native_output_bytes()), gemm_u64_padding_sentinel);
    return answer;
}
auto GemmU64RowPartition::single_task_checksum() const noexcept -> std::uint64_t {
    const auto start = first_ * whole_.columns();
    std::uint64_t answer{};
    for (std::uint64_t i{}; i < local_.output_elements().value(); ++i)
        answer += expected_word(whole_, start + i);
    return answer;
}
auto prepare_gemm_u64_reference(const GemmU64Config& c, std::span<const std::byte> input,
                                std::span<std::byte> output)
    -> std::expected<GemmU64Reference, core::Error> {
    if (input.size() < c.native_input_bytes() || output.size() < c.native_output_bytes())
        return std::unexpected{error("storage_too_small")};
    const std::less<const std::byte*> less;
    if (less(input.data(), output.data() + output.size()) &&
        less(output.data(), input.data() + input.size()))
        return std::unexpected{error("overlapping_storage")};
    if (!matches_input(c, input))
        return std::unexpected{error("input_content_mismatch")};
    std::uint64_t answer{};
    for (std::size_t i{}; i < c.output_elements().value(); ++i) {
        const auto value = expected_word(c, i);
        store(output, i, value);
        answer += value;
    }
    std::ranges::fill(output.subspan(c.native_output_bytes()), gemm_u64_padding_sentinel);
    return GemmU64Reference{c, core::sha256(input.first(c.native_input_bytes())),
                            core::sha256(output.first(c.native_output_bytes())), answer};
}
auto reconstruct_gemm_u64_reference(const GemmU64Config& c, std::span<std::byte> scratch)
    -> std::expected<GemmU64Reference, core::Error> {
    const auto input_bytes = c.native_input_bytes();
    const auto output_bytes = c.native_output_bytes();
    if (scratch.size() < input_bytes + output_bytes)
        return std::unexpected{error("storage_too_small")};
    const auto input = scratch.first(input_bytes);
    const auto output = scratch.subspan(input_bytes, output_bytes);
    const auto initialized = initialize_gemm_u64_input(c, input);
    if (!initialized) return std::unexpected{initialized.error()};
    return prepare_gemm_u64_reference(c, input, output);
}

auto reset_gemm_u64_output(const GemmU64Reference& reference, std::span<std::byte> output)
    -> std::expected<void, core::Error> {
    const auto& c = reference.configuration();
    if (output.size() < c.native_output_bytes())
        return std::unexpected{error("storage_too_small")};
    for (std::size_t i{}; i < c.output_elements().value(); ++i)
        store(output, i, ~expected_word(c, i));
    std::ranges::fill(output.subspan(c.native_output_bytes()), gemm_u64_padding_sentinel);
    return {};
}
auto verify_gemm_u64_output(const GemmU64Reference& reference, std::span<const std::byte> output)
    -> std::expected<GemmU64Verification, core::Error> {
    const auto& c = reference.configuration();
    if (output.size() < c.native_output_bytes())
        return std::unexpected{error("storage_too_small")};
    bool matches = true;
    for (std::size_t i{}; i < c.output_elements().value(); ++i)
        matches &= load(output, i) == expected_word(c, i);
    const bool padding = std::ranges::all_of(output.subspan(c.native_output_bytes()),
                                             [](auto b) { return b == gemm_u64_padding_sentinel; });
    return GemmU64Verification{c.output_elements(), reference.expected_output_digest(),
                               core::sha256(output.first(c.native_output_bytes())), matches,
                               padding};
}
} // namespace epycbench::kernel
