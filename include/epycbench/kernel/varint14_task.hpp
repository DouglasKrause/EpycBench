#pragma once
#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {
inline constexpr std::string_view varint14_task_id = "task.parse.varint14.u64/v1";
inline constexpr std::string_view varint14_branch_implementation_id = "scalar-u14-varint-branch/v1";
inline constexpr std::string_view varint14_masked_implementation_id =
    "scalar-u14-varint-masked-lookahead/v1";
inline constexpr std::string_view varint14_generator_profile =
    "epycbench.generator.balanced-varint14/v1";
inline constexpr std::string_view varint14_correctness_profile =
    "epycbench.task.varint14.canonical-values/v1";
inline constexpr std::uint64_t varint14_maximum_records = UINT64_C(1) << 20U;
inline constexpr std::uint64_t varint14_shuffle_seed_xor = UINT64_C(0xd1b54a32d192ed03);

enum class Varint14Distribution { grouped, shuffled };
[[nodiscard]] auto varint14_distribution_token(Varint14Distribution) noexcept -> std::string_view;
struct Varint14Work {
    TaskRepetitionCount completed_tasks;
    core::WorkCount decoded_records;
    core::WorkCount encoded_bytes;
    auto operator==(const Varint14Work&) const -> bool = default;
};
class Varint14Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t record_count, Varint14Distribution,
                                     std::uint64_t input_seed = 0U)
        -> std::expected<Varint14Config, core::Error>;
    [[nodiscard]] auto record_count() const noexcept -> ElementCount { return records_; }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto distribution() const noexcept -> Varint14Distribution {
        return distribution_;
    }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(records_.value() + records_.value() / 2U);
    }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t {
        return static_cast<std::size_t>(logical_bytes().value());
    }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t {
        return UINT64_MAX / logical_bytes().value();
    }
    [[nodiscard]] auto work(TaskRepetitionCount) const -> std::expected<Varint14Work, core::Error>;
    auto operator<=>(const Varint14Config&) const = default;

  private:
    Varint14Config(ElementCount records, Varint14Distribution distribution, std::uint64_t seed)
        : records_{records}, distribution_{distribution}, seed_{seed} {}
    ElementCount records_;
    Varint14Distribution distribution_;
    std::uint64_t seed_;
};
class Varint14Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const Varint14Config& {
        return configuration_;
    }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& {
        return digest_;
    }
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return answer_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount count) const noexcept
        -> std::uint64_t {
        return answer_ * count.value();
    }

  private:
    Varint14Reference(Varint14Config config, core::Sha256Digest digest, std::uint64_t answer)
        : configuration_{config}, digest_{digest}, answer_{answer} {}
    Varint14Config configuration_;
    core::Sha256Digest digest_;
    std::uint64_t answer_;
    friend auto prepare_varint14_reference(const Varint14Config&, std::span<const std::byte>)
        -> std::expected<Varint14Reference, core::Error>;
};
// N is even, 2..2^20. Item i is mix64/v1(seed+i)&127 for i<N/2, and
// 128+mix64/v1(seed+i)%16256 otherwise. Encode minimally using one/two base-128
// bytes (low 7 bits first, bit7 continuation, second byte <128). Grouped keeps
// item order. Shuffled uses canonical_latency_permutation(N,mix64(seed^xor)).
// Both orders contain identical values and exactly 3N/2 bytes. A distribution
// is fixed input identity for a Run; it never changes between timed trials.
// Grouped has two long width-outcome runs. Shuffled does not imply measured
// hardware unpredictability, branch misses or isolated branch cost.
[[nodiscard]] auto initialize_varint14_input(const Varint14Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
// Untimed full canonical-prefix verification, digest and an integer-derived
// sum of generated values, independent of the timed decoder. No spans retained.
// Allocation padding is unchanged, unhashed and not parsed work.
[[nodiscard]] auto prepare_varint14_reference(const Varint14Config&, std::span<const std::byte>)
    -> std::expected<Varint14Reference, core::Error>;
} // namespace epycbench::kernel
