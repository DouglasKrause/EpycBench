#include "epycbench/kernel/two_rank_histogram_u64.hpp"
#include "kernel/counter_trial_generation.hpp"
#include "kernel/histogram_u64_leaf.hpp"
#include "kernel/mix64.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <utility>

namespace epycbench::kernel {

namespace {
using namespace detail::histogram_leaf;
auto error(std::string_view reason) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{"kernel.task.histogram_u64." + std::string{reason}},
            .message = "Weighted histogram validation failed",
            .operation = "kernel.task.histogram_u64",
            .os_error = {},
            .context = {{"classification", reason}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}
auto record(const HistogramU64Config& c, std::uint64_t i) noexcept -> std::uint64_t {
    const auto word = detail::mix64_v1(c.input_seed() + i);
    const auto key = (word >> 56U) & (i % 4U < 3U ? UINT64_C(3) : UINT64_C(255));
    return (key << 56U) | (word & weight_mask);
}
template <class Accept> void canonical_input(const HistogramU64Config& c, Accept accept) {
    accept(0U, histogram_u64_input_magic);
    accept(1U, c.record_count());
    for (std::uint64_t i = 0; i < c.record_count(); ++i)
        accept(histogram_u64_header_words + i, record(c, i));
}
auto overlaps(Input first, Input second) -> bool {
    const std::less<const std::byte*> less;
    return less(first.data(), second.data() + second.size()) &&
           less(second.data(), first.data() + first.size());
}
template <class Accept>
auto canonical_output(const HistogramU64Config& c, Accept accept) -> std::uint64_t {
    std::array<HistogramU64RankBins::Bin, histogram_u64_bins> bins{};
    for (std::uint64_t i = 0; i < c.record_count(); ++i) {
        const auto word = record(c, i);
        auto& bin = bins[word >> 56U];
        ++bin.count;
        bin.sum += word & weight_mask;
    }
    std::uint64_t folded{};
    for (std::size_t k = 0; k < bins.size(); ++k) {
        accept(2U * k, bins[k].count);
        accept(2U * k + 1U, bins[k].sum);
        folded += (k + 1U) * (bins[k].count + bins[k].sum);
    }
    return folded;
}
} // namespace

auto HistogramU64Config::create(std::uint64_t records, std::uint64_t seed)
    -> std::expected<HistogramU64Config, core::Error> {
    if (!records || records > histogram_u64_maximum_records)
        return std::unexpected{error("invalid_record_count")};
    return HistogramU64Config{records, seed};
}
auto HistogramU64Config::work(TaskRepetitionCount r) const -> std::expected<TaskWork, core::Error> {
    if (r.value() > maximum_repetitions())
        return std::unexpected{error("work_overflow")};
    return TaskWork::create(element_count(), r);
}
auto initialize_histogram_u64_input(const HistogramU64Config& c, Output buf)
    -> std::expected<void, core::Error> {
    if (buf.size() < c.logical_bytes().value())
        return std::unexpected{error("storage_too_small")};
    canonical_input(c, [&](auto i, auto value) { write(buf, i, value); });
    return {};
}
auto prepare_histogram_u64_reference(const HistogramU64Config& c, Input input, Output output)
    -> std::expected<HistogramU64Reference, core::Error> {
    if (input.size() < c.logical_bytes().value() || output.size() < c.output_bytes().value())
        return std::unexpected{error("storage_too_small")};
    if (overlaps(input, output))
        return std::unexpected{error("overlapping_storage")};
    bool matches = true;
    canonical_input(c, [&](auto i, auto value) { matches &= read(input, i) == value; });
    if (!matches)
        return std::unexpected{error("input_content_mismatch")};
    const auto folded = canonical_output(c, [&](auto i, auto value) { write(output, i, value); });
    std::ranges::fill(output.subspan(c.output_bytes().value()), histogram_u64_padding_sentinel);
    return HistogramU64Reference{c, core::sha256(input.first(c.logical_bytes().value())),
                                 core::sha256(output.first(c.output_bytes().value())), folded};
}
auto reset_histogram_u64_output(const HistogramU64Reference& ref, Output buf)
    -> std::expected<void, core::Error> {
    const auto& c = ref.configuration();
    if (buf.size() < c.output_bytes().value())
        return std::unexpected{error("storage_too_small")};
    canonical_output(c, [&](auto i, auto value) { write(buf, i, ~value); });
    std::ranges::fill(buf.subspan(c.output_bytes().value()), histogram_u64_padding_sentinel);
    return {};
}
auto verify_histogram_u64_output(const HistogramU64Reference& ref, Input buf)
    -> std::expected<XorU64Verification, core::Error> {
    const auto& c = ref.configuration();
    if (buf.size() < c.output_bytes().value())
        return std::unexpected{error("storage_too_small")};
    bool matches = true;
    canonical_output(c, [&](auto i, auto value) { matches &= read(buf, i) == value; });
    const bool padding = std::ranges::all_of(buf.subspan(c.output_bytes().value()), [](auto value) {
        return value == histogram_u64_padding_sentinel;
    });
    return XorU64Verification{*ElementCount::create(histogram_u64_bins),
                              ref.expected_output_digest(),
                              core::sha256(buf.first(c.output_bytes().value())), matches, padding};
}

HistogramU64RankOperation::HistogramU64RankOperation(HistogramU64RankOperation&& other) noexcept
    : bins_{std::exchange(other.bins_, nullptr)}, input_{other.input_}, pattern_{other.pattern_},
      generation_{other.generation_}, rank_{other.rank_}, range_{other.range_} {}

auto HistogramU64RankOperation::execute() && noexcept -> std::optional<HistogramU64RankCompletion> {
    auto* bins = std::exchange(bins_, nullptr);
    if (!bins)
        return std::nullopt;
    if (pattern_ == HistogramU64Pattern::shared_atomic)
        accumulate<true>(input_, *bins, range_.record_count);
    else
        accumulate<false>(input_, *bins, range_.record_count);
    return HistogramU64RankCompletion{generation_, rank_, range_};
}

auto HistogramU64TwoRankTrial::prepare(const HistogramU64Config& config, HistogramU64Pattern pattern,
                                       Input input, Output scratch, Output output)
    -> std::expected<HistogramU64TwoRankTrial, core::Error> {
    if (pattern != HistogramU64Pattern::shared_atomic && pattern != HistogramU64Pattern::private_partials)
        return std::unexpected{error("invalid_cooperative_pattern")};
    if (scratch.size() < histogram_u64_scratch_bytes || input.size() < config.native_bytes() ||
        output.size() < config.output_bytes().value())
        return std::unexpected{error("storage_too_small")};
    if (reinterpret_cast<std::uintptr_t>(scratch.data()) % histogram_u64_scratch_alignment)
        return std::unexpected{error("unaligned_scratch")};
    if (overlaps(input, scratch) || overlaps(output, scratch))
        return std::unexpected{error("overlapping_storage")};
    const auto generation = detail::next_counter_trial_generation();
    if (!generation)
        return std::unexpected{error("cooperative_generations_exhausted")};
    auto reference = prepare_histogram_u64_reference(config, input, output);
    if (!reference)
        return std::unexpected{std::move(reference.error())};
    if (auto reset = reset_histogram_u64_output(*reference, output); !reset)
        return std::unexpected{std::move(reset.error())};
    auto padding = scratch.subspan(histogram_u64_scratch_bytes);
    std::ranges::fill(padding, histogram_u64_padding_sentinel);
    return HistogramU64TwoRankTrial{*reference, pattern, input, output, padding,
                                  std::construct_at(reinterpret_cast<HistogramU64Scratch*>(scratch.data())),
                                  *generation};
}

HistogramU64TwoRankTrial::HistogramU64TwoRankTrial(HistogramU64TwoRankTrial&& other) noexcept
    : reference_{other.reference_}, pattern_{other.pattern_}, input_{other.input_},
      output_{other.output_}, scratch_padding_{other.scratch_padding_},
      scratch_{std::exchange(other.scratch_, nullptr)}, generation_{other.generation_},
      issued_{other.issued_}, checksum_{other.checksum_} {}

HistogramU64TwoRankTrial::~HistogramU64TwoRankTrial() noexcept {
    if (scratch_)
        std::destroy_at(scratch_);
}

auto histogram_u64_rank_ranges(const HistogramU64Config& config) noexcept
    -> std::array<HistogramU64RankRange, histogram_u64_rank_count> {
    const auto records = config.record_count();
    const auto split = (records + 1U) / 2U;
    return {HistogramU64RankRange{0U, split}, HistogramU64RankRange{split, records - split}};
}

auto HistogramU64TwoRankTrial::take_rank(std::size_t rank)
    -> std::expected<HistogramU64RankOperation, core::Error> {
    if (!scratch_ || rank >= issued_.size() || issued_[rank])
        return std::unexpected{error("cooperative_rank_unavailable")};
    issued_[rank] = true;
    const auto range = histogram_u64_rank_ranges(reference_.configuration())[rank];
    const auto input = input_.subspan(8U * (histogram_u64_header_words + range.first_record),
                                      8U * range.record_count);
    const auto slice = pattern_ == HistogramU64Pattern::shared_atomic ? 0U : rank;
    return HistogramU64RankOperation{scratch_->ranks[slice], input, pattern_, generation_, rank, range};
}

auto HistogramU64TwoRankTrial::finalize(const HistogramU64RankCompletions& completions)
    -> std::expected<HistogramU64TwoRankWork, core::Error> {
    if (!scratch_ || checksum_)
        return std::unexpected{error("invalid_cooperative_trial")};
    const auto& config = reference_.configuration();
    const auto ranges = histogram_u64_rank_ranges(config);
    for (std::size_t rank{}; rank < completions.size(); ++rank) {
        const auto& done = completions[rank];
        if (!done || done->generation_ != generation_ || done->rank() != rank ||
            done->range() != ranges[rank])
            return std::unexpected{error("invalid_cooperative_completion")};
    }
    const bool shared = pattern_ == HistogramU64Pattern::shared_atomic;
    const auto updates = 2U * config.record_count();
    checksum_ = shared ? materialize<false>(*scratch_, output_) : materialize<true>(*scratch_, output_);
    return HistogramU64TwoRankWork{*config.work(*TaskRepetitionCount::create(1U)), histogram_u64_bins,
                                 shared ? updates : 0U, shared ? 0U : updates,
                                 shared ? 0U : histogram_u64_bins, *checksum_};
}

auto HistogramU64TwoRankTrial::verify() const -> std::expected<HistogramU64TwoRankVerification, core::Error> {
    if (!scratch_ || !checksum_)
        return std::unexpected{error("invalid_cooperative_trial")};
    const auto output = verify_histogram_u64_output(reference_, output_);
    if (!output)
        return std::unexpected{output.error()};
    const bool padding = std::ranges::all_of(scratch_padding_, [](auto byte) {
        return byte == histogram_u64_padding_sentinel;
    });
    return HistogramU64TwoRankVerification{*output, padding, *checksum_ == reference_.single_task_answer()};
}
} // namespace epycbench::kernel
