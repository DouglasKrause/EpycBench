#pragma once

#include "epycbench/kernel/histogram_u64_task.hpp"

#include <array>
#include <optional>

namespace epycbench::kernel {

inline constexpr std::size_t histogram_u64_rank_count = 2U;
inline constexpr std::size_t histogram_u64_scratch_alignment = 64U;
inline constexpr std::size_t histogram_u64_scratch_bytes = 2U * 16U * histogram_u64_bins;

enum class HistogramU64Pattern { shared_atomic, private_partials };

struct HistogramU64RankRange {
    std::uint64_t first_record;
    std::uint64_t record_count;
    auto operator<=>(const HistogramU64RankRange&) const = default;
};

[[nodiscard]] auto histogram_u64_rank_ranges(const HistogramU64Config&) noexcept
    -> std::array<HistogramU64RankRange, histogram_u64_rank_count>;

class HistogramU64RankOperation;
class HistogramU64TwoRankTrial;

class HistogramU64RankCompletion {
  public:
    [[nodiscard]] auto rank() const noexcept -> std::size_t { return rank_; }
    [[nodiscard]] auto range() const noexcept -> HistogramU64RankRange { return range_; }

  private:
    friend class HistogramU64RankOperation;
    friend class HistogramU64TwoRankTrial;
    HistogramU64RankCompletion(std::uint64_t generation, std::size_t rank,
                               HistogramU64RankRange range) noexcept
        : generation_{generation}, rank_{rank}, range_{range} {}

    std::uint64_t generation_;
    std::size_t rank_;
    HistogramU64RankRange range_;
};

using HistogramU64RankCompletions =
    std::array<std::optional<HistogramU64RankCompletion>, histogram_u64_rank_count>;

struct HistogramU64RankBins;
struct HistogramU64Scratch;

class HistogramU64RankOperation {
  public:
    HistogramU64RankOperation(HistogramU64RankOperation&&) noexcept;
    HistogramU64RankOperation(const HistogramU64RankOperation&) = delete;
    auto operator=(const HistogramU64RankOperation&) -> HistogramU64RankOperation& = delete;
    auto operator=(HistogramU64RankOperation&&) -> HistogramU64RankOperation& = delete;

    // Timed. Consumes this rank's operand and scans only its assigned records.
    [[nodiscard]] auto execute() && noexcept -> std::optional<HistogramU64RankCompletion>;

  private:
    friend class HistogramU64TwoRankTrial;
    HistogramU64RankOperation(HistogramU64RankBins& bins, std::span<const std::byte> input,
                              HistogramU64Pattern pattern, std::uint64_t generation,
                              std::size_t rank, HistogramU64RankRange range) noexcept
        : bins_{&bins}, input_{input}, pattern_{pattern}, generation_{generation}, rank_{rank},
          range_{range} {}

    HistogramU64RankBins* bins_;
    std::span<const std::byte> input_;
    HistogramU64Pattern pattern_;
    std::uint64_t generation_;
    std::size_t rank_;
    HistogramU64RankRange range_;
};

struct HistogramU64TwoRankWork {
    TaskWork grouping;
    std::uint64_t bins_written;
    std::uint64_t atomic_updates;
    std::uint64_t plain_updates;
    std::uint64_t partial_bins_combined;
    std::uint64_t checksum;
};

struct HistogramU64TwoRankVerification {
    XorU64Verification output;
    bool scratch_padding_intact;
    bool checksum_matches;
    [[nodiscard]] auto matched() const noexcept -> bool {
        return output.matched() && scratch_padding_intact && checksum_matches;
    }
};

// Owns the scratch objects for one grouping in three disjoint caller-owned buffers.
// Keep the buffers live and stationary. Use worker completion to join/synchronize
// both rank operations before finalizing or destroying the trial.
class HistogramU64TwoRankTrial {
  public:
    [[nodiscard]] static auto prepare(const HistogramU64Config&, HistogramU64Pattern,
                                      std::span<const std::byte> input,
                                      std::span<std::byte> scratch, std::span<std::byte> output)
        -> std::expected<HistogramU64TwoRankTrial, core::Error>;
    HistogramU64TwoRankTrial(HistogramU64TwoRankTrial&&) noexcept;
    HistogramU64TwoRankTrial(const HistogramU64TwoRankTrial&) = delete;
    auto operator=(const HistogramU64TwoRankTrial&) -> HistogramU64TwoRankTrial& = delete;
    auto operator=(HistogramU64TwoRankTrial&&) -> HistogramU64TwoRankTrial& = delete;
    ~HistogramU64TwoRankTrial() noexcept;

    [[nodiscard]] auto take_rank(std::size_t rank)
        -> std::expected<HistogramU64RankOperation, core::Error>;

    // Timed after both rank operations complete. Materializes all ordered bins
    // and includes the private partial merge in the candidate's work.
    [[nodiscard]] auto finalize(const HistogramU64RankCompletions&)
        -> std::expected<HistogramU64TwoRankWork, core::Error>;

    // Untimed. Checks the full final output, padding and one-grouping checksum.
    [[nodiscard]] auto verify() const -> std::expected<HistogramU64TwoRankVerification, core::Error>;

  private:
    HistogramU64TwoRankTrial(HistogramU64Reference reference, HistogramU64Pattern pattern,
                             std::span<const std::byte> input, std::span<std::byte> output,
                             std::span<const std::byte> scratch_padding, HistogramU64Scratch* scratch,
                             std::uint64_t generation) noexcept
        : reference_{reference}, pattern_{pattern}, input_{input}, output_{output},
          scratch_padding_{scratch_padding}, scratch_{scratch}, generation_{generation} {}

    HistogramU64Reference reference_;
    HistogramU64Pattern pattern_;
    std::span<const std::byte> input_;
    std::span<std::byte> output_;
    std::span<const std::byte> scratch_padding_;
    HistogramU64Scratch* scratch_;
    std::uint64_t generation_;
    std::array<bool, histogram_u64_rank_count> issued_{};
    std::optional<std::uint64_t> checksum_;
};

} // namespace epycbench::kernel
