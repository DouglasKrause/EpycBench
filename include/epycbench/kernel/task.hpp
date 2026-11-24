#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/core/error.hpp"
#include "epycbench/core/quantity.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace epycbench::kernel {

inline constexpr std::string_view sum_u64_task_id = "task.sum.u64/v1";
inline constexpr std::string_view sum_u64_single_implementation_id = "scalar-u64-sum-single/v1";
inline constexpr std::string_view sum_u64_four_implementation_id = "scalar-u64-sum-four/v1";
inline constexpr std::string_view sum_u64_avx2_implementation_id = "avx2-u64-sum/v1";
inline constexpr std::string_view sum_u64_generator_profile = "epycbench.generator.mix64/v1";
inline constexpr std::string_view sum_u64_correctness_profile =
    "epycbench.task.sum-u64.modulo-reference/v1";
inline constexpr std::string_view xor_u64_task_id = "task.map.xor.u64/v1";
inline constexpr std::string_view xor_u64_single_implementation_id = "scalar-u64-xor-single/v1";
inline constexpr std::string_view xor_u64_four_implementation_id = "scalar-u64-xor-four/v1";
inline constexpr std::string_view xor_u64_generator_profile = sum_u64_generator_profile;
inline constexpr std::string_view xor_u64_correctness_profile =
    "epycbench.task.map-xor-u64.full-output-reference/v1";
inline constexpr std::uint64_t xor_u64_default_mask = 11400714819323198485ULL;
inline constexpr std::byte xor_u64_padding_sentinel{0xa5};

class ElementCount {
  public:
    [[nodiscard]] static auto create(std::uint64_t value)
        -> std::expected<ElementCount, core::Error>;
    [[nodiscard]] constexpr auto value() const noexcept -> std::uint64_t { return value_; }
    auto operator<=>(const ElementCount&) const = default;

  private:
    explicit constexpr ElementCount(std::uint64_t value) noexcept : value_{value} {}
    std::uint64_t value_;
};

class TaskRepetitionCount {
  public:
    [[nodiscard]] static auto create(std::uint64_t value)
        -> std::expected<TaskRepetitionCount, core::Error>;
    [[nodiscard]] constexpr auto value() const noexcept -> std::uint64_t { return value_; }
    auto operator<=>(const TaskRepetitionCount&) const = default;

  private:
    explicit constexpr TaskRepetitionCount(std::uint64_t value) noexcept : value_{value} {}
    std::uint64_t value_;
};

class SumU64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t element_count, std::uint64_t input_seed = 0)
        -> std::expected<SumU64Config, core::Error>;
    [[nodiscard]] constexpr auto element_count() const noexcept -> ElementCount {
        return element_count_;
    }
    [[nodiscard]] constexpr auto input_seed() const noexcept -> std::uint64_t {
        return input_seed_;
    }
    [[nodiscard]] constexpr auto logical_bytes() const noexcept -> core::ByteCount {
        return logical_bytes_;
    }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t;
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t;
    auto operator<=>(const SumU64Config&) const = default;

  private:
    SumU64Config(ElementCount count, std::uint64_t seed, core::ByteCount bytes) noexcept
        : element_count_{count}, input_seed_{seed}, logical_bytes_{bytes} {}
    ElementCount element_count_;
    std::uint64_t input_seed_;
    core::ByteCount logical_bytes_;
};

class SumU64Work {
  public:
    [[nodiscard]] static auto create(ElementCount elements, TaskRepetitionCount repetitions)
        -> std::expected<SumU64Work, core::Error>;
    [[nodiscard]] static auto create(const SumU64Config& configuration,
                                     TaskRepetitionCount repetitions)
        -> std::expected<SumU64Work, core::Error>;
    [[nodiscard]] constexpr auto completed_tasks() const noexcept -> TaskRepetitionCount {
        return repetitions_;
    }
    [[nodiscard]] constexpr auto processed_elements() const noexcept -> ElementCount {
        return processed_elements_;
    }
    auto operator<=>(const SumU64Work&) const = default;

  private:
    SumU64Work(TaskRepetitionCount repetitions, ElementCount processed_elements) noexcept
        : repetitions_{repetitions}, processed_elements_{processed_elements} {}
    TaskRepetitionCount repetitions_;
    ElementCount processed_elements_;
};
// Work counts carry quantities, while each task's configuration retains semantics.
using TaskWork = SumU64Work;

class SumU64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const SumU64Config& {
        return configuration_;
    }
    [[nodiscard]] constexpr auto single_task_answer() const noexcept -> std::uint64_t {
        return single_task_answer_;
    }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& {
        return input_digest_;
    }
    // Answer arithmetic deliberately wraps. Work-count overflow is checked by SumU64Work.
    [[nodiscard]] constexpr auto expected_checksum(TaskRepetitionCount repetitions) const noexcept
        -> std::uint64_t {
        return single_task_answer_ * repetitions.value();
    }

  private:
    SumU64Reference(SumU64Config configuration, std::uint64_t answer,
                    core::Sha256Digest digest) noexcept
        : configuration_{configuration}, single_task_answer_{answer}, input_digest_{digest} {}
    SumU64Config configuration_;
    std::uint64_t single_task_answer_;
    core::Sha256Digest input_digest_;
    friend auto prepare_sum_u64_reference(const SumU64Config&, std::span<const std::byte>)
        -> std::expected<SumU64Reference, core::Error>;
    friend auto replay_sum_u64_reference(const SumU64Config&, core::ByteCount)
        -> std::expected<SumU64Reference, core::Error>;
};

// Storage may include mapping padding. Only the canonical logical prefix is written,
// checked and hashed; padding never contributes to task identity or work.
[[nodiscard]] auto initialize_sum_u64_input(const SumU64Config& configuration,
                                            std::span<std::byte> storage)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto prepare_sum_u64_reference(const SumU64Config& configuration,
                                             std::span<const std::byte> storage)
    -> std::expected<SumU64Reference, core::Error>;

// Regenerate the logical input and answer for saved replay within a caller byte
// ceiling. A replay reference carries no evidence about mapped input contents.
[[nodiscard]] auto replay_sum_u64_reference(const SumU64Config&, core::ByteCount maximum_generated_bytes)
    -> std::expected<SumU64Reference, core::Error>;

class XorU64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t element_count, std::uint64_t input_seed = 0,
                                     std::uint64_t xor_mask = xor_u64_default_mask)
        -> std::expected<XorU64Config, core::Error>;
    [[nodiscard]] auto input_configuration() const noexcept -> const SumU64Config& { return input_; }
    [[nodiscard]] auto element_count() const noexcept -> ElementCount { return input_.element_count(); }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return input_.input_seed(); }
    [[nodiscard]] auto xor_mask() const noexcept -> std::uint64_t { return mask_; }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount { return input_.logical_bytes(); }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t { return input_.native_bytes(); }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t { return input_.maximum_repetitions(); }
    auto operator<=>(const XorU64Config&) const = default;
  private:
    XorU64Config(SumU64Config input, std::uint64_t mask) noexcept : input_{input}, mask_{mask} {}
    SumU64Config input_;
    std::uint64_t mask_;
};

class XorU64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const XorU64Config& { return configuration_; }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& { return input_digest_; }
    [[nodiscard]] auto expected_output_digest() const noexcept -> const core::Sha256Digest& { return output_digest_; }
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return answer_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount repetitions) const noexcept -> std::uint64_t {
        return answer_ * repetitions.value();
    }
  private:
    XorU64Reference(XorU64Config configuration, core::Sha256Digest input,
                    core::Sha256Digest output, std::uint64_t answer) noexcept
        : configuration_{configuration}, input_digest_{input}, output_digest_{output}, answer_{answer} {}
    XorU64Config configuration_;
    core::Sha256Digest input_digest_, output_digest_;
    std::uint64_t answer_;
    friend auto prepare_xor_u64_reference(const XorU64Config&, std::span<const std::byte>,
                                          std::span<std::byte>)
        -> std::expected<XorU64Reference, core::Error>;
};

struct XorU64Verification {
    ElementCount checked_elements;
    core::Sha256Digest expected_digest;
    core::Sha256Digest actual_digest;
    bool content_matches;
    bool padding_matches;
    [[nodiscard]] auto matched() const noexcept -> bool {
        return content_matches && padding_matches && expected_digest == actual_digest;
    }
};

// Reference preparation borrows the existing output allocation as scratch for
// canonical expected bytes, hashes its logical prefix, then retains no spans.
// Reset replaces those bytes with complements and restores all output padding.
[[nodiscard]] auto prepare_xor_u64_reference(const XorU64Config&, std::span<const std::byte> input,
                                            std::span<std::byte> output)
    -> std::expected<XorU64Reference, core::Error>;
[[nodiscard]] auto reset_xor_u64_output(const XorU64Reference&, std::span<std::byte> output)
    -> std::expected<void, core::Error>;
// Content/padding mismatch is factual verification, not a discarded error.
[[nodiscard]] auto verify_xor_u64_output(const XorU64Reference&, std::span<const std::byte> output)
    -> std::expected<XorU64Verification, core::Error>;

} // namespace epycbench::kernel
