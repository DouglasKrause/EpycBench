#pragma once

#include "results/incremental_pair_evidence.hpp"
#include "runtime/pairwise_v2_lifecycle_facts.hpp"

namespace epycbench::results::process_subject { class Session; }

namespace epycbench::results::pair_lifecycle {

struct Limits final {
    std::size_t records{}, marks{}, request_bytes{}, transport_bytes{}, canonical_bytes{},
        capacity_elements{}, retained_bytes{}, simultaneous_bytes{};
    block_evidence::ContextLimits context{};
};
struct Usage final {
    std::size_t records{}, marks{}, request_bytes{}, transport_bytes{}, canonical_bytes{},
        capacity_elements{}, retained_bytes{}, simultaneous_bytes{};
    auto operator==(const Usage&) const noexcept -> bool = default;
};
struct References final {
    std::uint64_t controller_sequence{};
    std::optional<std::uint64_t> runner_sequence, pair_ordinal, normalization_sequence;
};

class Owner final {
  public:
    Owner(Owner&&) noexcept = default;
    auto operator=(Owner&&) noexcept -> Owner& = default;
    Owner(const Owner&) = delete;
    auto operator=(const Owner&) -> Owner& = delete;
    [[nodiscard]] auto facts() const noexcept -> const runtime::detail::SyntheticLifecycleFacts& { return facts_; }
    [[nodiscard]] auto transport_body() const noexcept -> std::string_view { return body_; }
    [[nodiscard]] auto prefix() const noexcept -> const block_evidence::PairPrefix* {
        return prefix_ ? &*prefix_ : nullptr;
    }
    [[nodiscard]] auto references() const noexcept -> const References& { return references_; }
    [[nodiscard]] auto usage() const noexcept -> Usage { return usage_; }
    // This source profile has no complete execution-plan membership capability.
    [[nodiscard]] static constexpr auto planning_membership_proven() noexcept -> bool { return false; }

  private:
    Owner(runtime::detail::SyntheticLifecycleFacts facts, std::string body,
          References references, Usage usage) noexcept
        : facts_{std::move(facts)}, body_{std::move(body)}, references_{references}, usage_{usage} {}
    [[nodiscard]] static auto prepare(const runtime::detail::SyntheticLifecycleView&,
        const journal::RunId&, const scenario::ScenarioDigestedPlan*,
        const block_evidence::PairPrefix*, References, Limits, std::size_t prior_stock,
        std::size_t entry_stock) -> std::expected<Owner, block_evidence::Error>;
    void commit_prefix(block_evidence::PairPrefix* prefix) noexcept {
        if (prefix) prefix_.emplace(std::move(*prefix));
    }
    runtime::detail::SyntheticLifecycleFacts facts_;
    std::string body_;
    std::optional<block_evidence::PairPrefix> prefix_;
    References references_;
    Usage usage_;
    friend class process_subject::Session;
};

struct LifecycleAdmissionStaging final {
    const runtime::detail::SyntheticLifecycleView* source{};
    const journal::RunId* run{};
    const scenario::ScenarioDigestedPlan* plan{};
    const block_evidence::PairPrefix* prefix{};
    References references;
    Limits limits;
    // Owner::prepare value parameters coexist with the copies above.
    References parameter_references;
    Limits parameter_limits;
    Usage usage;
    std::size_t prior{}, entry{}, source_backing{}, prefix_backing{}, owner_backing{}, index{};
    std::array<bool, 65U> seen{};
    std::optional<block_evidence::Error> failure;
};
struct LifecycleCopyStaging final {
    std::optional<runtime::detail::SyntheticLifecycleFacts> facts;
    std::string body;
    std::optional<Owner> owner;
    std::optional<std::expected<Owner, block_evidence::Error>> returned;
    std::optional<runtime::detail::SyntheticLifecycleFacts> constructor_facts;
    std::string constructor_body;
    References constructor_references;
    Usage constructor_usage;
    std::optional<std::expected<Owner, block_evidence::Error>> caller_return;
};
struct LifecycleContextComparisonStaging final {
    std::optional<std::expected<block_evidence::ExpectedPairContext, block_evidence::Error>> context;
    const block_evidence::ExpectedPairContext* incoming{};
    block_evidence::ContextUsage context_usage;
    block_evidence::ContextLimits context_limits;
    std::size_t construction_peak{}, comparison_peak{};
};
struct LifecycleContextHelperStaging final {
    // exactly_matches: two State/provenance/preparation/Statistics references,
    // two memory-range arguments and two memory-cell arguments.
    std::array<const void*, 12U> comparison_references;
    std::array<double, 2U> bitwise_arguments;
    std::array<std::uint64_t, 2U> bitwise_returns;
    std::array<bool, 4U> comparator_returns_and_empty_closures;
};
// Additional fixed helper cells, not an allowance for uncounted ownership.
// SHA hashes bytes directly; it never canonicalizes JSON or renders hex text.
struct LifecycleDigestStaging final {
    std::array<std::uint32_t, 8U> state, round_state;
    std::array<std::uint32_t, 64U> schedule;
    std::array<std::byte, 128U> tail;
    std::array<std::byte, 32U> result, constructor_argument;
    std::uint32_t schedule_s0, schedule_s1, sum1, choose, temporary1, sum0, majority, temporary2;
    std::size_t offset, remainder, input_index, input_offset, expansion_index,
        round_index, tail_index, output_word, output_byte;
    unsigned final_size;
    std::uint64_t bit_length;
    const std::array<std::uint32_t, 8U>* compression_state;
    const std::byte* compression_block;
    std::string_view wrapper_input;
    std::span<const char> wrapper_characters;
    std::span<const std::byte> converted_input, span_argument, copy_subspan;
    const std::byte* copied_input;
    std::byte* copied_output;
    // First comparison temporary, constructor result, span return, wrapper
    // return and second comparison temporary may coexist without elision.
    std::array<core::Sha256Digest, 5U> returned;
};
struct LifecycleConsistencyStaging final {
    const runtime::detail::SyntheticLifecycleFacts* facts;
    const runtime::detail::SyntheticLifecycleFacts* observed_capture;
    std::array<const void*, 2U> follows_captures;
    std::array<std::int64_t, 5U> durations;
    std::array<bool, 8U> secondary_sources;
    std::size_t deadline_index, close_index, secondary_index, source_index, final_index;
    runtime::detail::SyntheticLifecycleMark begin, expiry, attempt, returned;
    const runtime::detail::SyntheticCapturedDeadline* deadline;
    runtime::detail::SyntheticCancelledPhase cancelled_phase;
    std::uint8_t previous, ordinal;
    std::array<bool, 4U> helper_returns;
};
struct LifecycleIdentityStaging final {
    LifecycleDigestStaging digest;
    LifecycleConsistencyStaging consistency;
    std::array<std::string_view, 3U> root_views;
    std::span<const char> root_characters;
    std::array<std::span<const std::byte>, 2U> canonical_and_converted_bytes;
    std::array<const std::byte*, 3U> byte_comparison_iterators;
};
struct LifecycleCommitStaging final {
    std::optional<Owner> owner;
    block_evidence::PairPrefix* incoming{};
    std::size_t reserve_before{}, reserve_after{}, index{};
    std::optional<block_evidence::Error> failure;
};

} // namespace epycbench::results::pair_lifecycle
