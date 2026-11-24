#pragma once

#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {
inline constexpr std::string_view search_tree_u64_task_id = "task.search.lower-bound-tree.u64/v1";
inline constexpr std::string_view search_tree_u64_binary_implementation_id =
    "binary-lower-bound-u64/v1";
inline constexpr std::string_view search_tree_u64_steps_implementation_id =
    "four-step-lower-bound-u64/v1";
inline constexpr std::string_view search_tree_u64_generator_profile =
    "epycbench.generator.balanced-threshold-queries/v1";
inline constexpr std::string_view search_tree_u64_correctness_profile =
    "epycbench.task.search-tree-u64.ordered-lower-bounds/v1";
inline constexpr std::uint64_t search_tree_u64_input_magic = UINT64_C(0x3130454552544245);
inline constexpr std::uint64_t search_tree_u64_maximum_queries = 1048576U;
inline constexpr std::size_t search_tree_u64_leaves = 16U;
inline constexpr std::size_t search_tree_u64_header_words = 17U;
inline constexpr std::byte search_tree_u64_padding_sentinel{0xa5};

enum class SearchTreeU64Order { grouped, permuted };
[[nodiscard]] auto search_tree_u64_order_token(SearchTreeU64Order) noexcept -> std::string_view;
[[nodiscard]] auto parse_search_tree_u64_order(std::string_view) noexcept -> std::optional<SearchTreeU64Order>;

class SearchTreeU64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t queries, SearchTreeU64Order order,
                                     std::uint64_t seed = 0U)
        -> std::expected<SearchTreeU64Config, core::Error>;
    [[nodiscard]] auto query_count() const noexcept -> std::uint64_t { return queries_; }
    [[nodiscard]] auto query_order() const noexcept -> SearchTreeU64Order { return order_; }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto element_count() const noexcept -> ElementCount {
        return *ElementCount::create(queries_);
    }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(8U * (search_tree_u64_header_words + queries_));
    }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t {
        return static_cast<std::size_t>(logical_bytes().value());
    }
    [[nodiscard]] auto output_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(8U * queries_);
    }
    [[nodiscard]] auto threshold_comparisons() const noexcept -> std::uint64_t {
        return 4U * queries_;
    }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t {
        return UINT64_MAX / threshold_comparisons();
    }
    [[nodiscard]] auto work(TaskRepetitionCount count) const
        -> std::expected<TaskWork, core::Error>;
    auto operator<=>(const SearchTreeU64Config&) const = default;

  private:
    SearchTreeU64Config(std::uint64_t queries, SearchTreeU64Order order,
                        std::uint64_t seed) noexcept
        : queries_{queries}, seed_{seed}, order_{order} {}
    std::uint64_t queries_, seed_;
    SearchTreeU64Order order_;
};
class SearchTreeU64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const SearchTreeU64Config& {
        return config_;
    }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& { return input_; }
    [[nodiscard]] auto expected_output_digest() const noexcept -> const core::Sha256Digest& {
        return output_;
    }
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return answer_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount count) const noexcept
        -> std::uint64_t {
        return answer_ * count.value();
    }

  private:
    SearchTreeU64Reference(SearchTreeU64Config config, core::Sha256Digest input,
                           core::Sha256Digest output, std::uint64_t answer) noexcept
        : config_{config}, input_{input}, output_{output}, answer_{answer} {}
    SearchTreeU64Config config_;
    core::Sha256Digest input_, output_;
    std::uint64_t answer_;
    friend auto prepare_search_tree_u64_reference(const SearchTreeU64Config&,
                                                  std::span<const std::byte>, std::span<std::byte>)
        -> std::expected<SearchTreeU64Reference, core::Error>;
};

[[nodiscard]] auto initialize_search_tree_u64_input(const SearchTreeU64Config&,
                                                    std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto prepare_search_tree_u64_reference(const SearchTreeU64Config&,
                                                     std::span<const std::byte>,
                                                     std::span<std::byte>)
    -> std::expected<SearchTreeU64Reference, core::Error>;
[[nodiscard]] auto reset_search_tree_u64_output(const SearchTreeU64Reference&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto verify_search_tree_u64_output(const SearchTreeU64Reference&,
                                                 std::span<const std::byte>)
    -> std::expected<XorU64Verification, core::Error>;

} // namespace epycbench::kernel
