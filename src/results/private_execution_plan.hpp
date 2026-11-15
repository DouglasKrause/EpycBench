#pragma once
#include "results/private_build_object_association.hpp"

namespace epycbench::results::execution_plan {
inline constexpr std::string_view profile="epycbench.private-execution-plan/v1";
inline constexpr std::string_view artifact_profile="epycbench.private-execution-plan-artifact/v1";
enum class PairKind { comparison,matrix };
enum class Error { state,origin,build,object_evidence,reference,incompatible_placement,policy,capacity,encoding };
struct Pair {
    std::size_t ordinal{},experiment_index{},child_experiment_index{},workload_index{},baseline_index{},candidate_index{};
    PairKind kind{};
    scenario::MatrixComparisonOrder order{};
    std::uint64_t order_seed{};
    auto operator==(const Pair&) const -> bool = default;
};
struct MeasuredCeiling {
    std::uint64_t low{};
    bool two_to_64{};
    auto operator==(const MeasuredCeiling&) const -> bool = default;
};
struct Counts {
    std::uint64_t minimum_blocks{},maximum_blocks{},minimum_valid_per_variant{},maximum_per_variant{},root_seed{};
    MeasuredCeiling maximum_observations;
    bool stopping_enabled{},monitoring_present{};
    auto operator==(const Counts&) const -> bool = default;
};
struct Quantities {
    std::size_t experiments{},variants{},pairs{},child_retained_bytes{},child_canonical_bytes{},index_bytes{},pair_bytes{},
        semantic_bytes{},canonical_bytes{},planning_work{},retained_bytes{},consumer_simultaneous_bytes{};
    auto operator==(const Quantities&) const -> bool = default;
};
using Limits=Quantities;
using Usage=Quantities;
class Owner;
class ExecutionPlanDigest {
  public:
    [[nodiscard]] auto value() const noexcept -> const core::Sha256Digest& { return value_; }
    auto operator==(const ExecutionPlanDigest&) const -> bool = default;
  private:
    explicit ExecutionPlanDigest(core::Sha256Digest value) noexcept:value_{value}{}
    core::Sha256Digest value_;
    friend auto produce(build_object_association::Owner&&,Limits) -> std::expected<Owner,Error>;
};
namespace detail {
struct NameIndex { std::size_t experiment,variant;std::string_view name; };
struct State {
    std::optional<build_object_association::Owner> child;
    std::vector<Pair> pairs;
    Counts counts;
    std::string semantic,canonical;
    ExecutionPlanDigest execution_digest;
    core::Sha256Digest digest;
    Usage usage;
};
[[nodiscard]] auto checked_add(std::size_t,std::size_t) noexcept -> std::optional<std::size_t>;
[[nodiscard]] auto checked_multiply(std::size_t,std::size_t) noexcept -> std::optional<std::size_t>;
[[nodiscard]] auto measured_ceiling(std::uint64_t) noexcept -> std::optional<MeasuredCeiling>;
[[nodiscard]] auto valid_ceiling(MeasuredCeiling) noexcept -> bool;
[[nodiscard]] auto order_token(scenario::MatrixComparisonOrder) noexcept -> std::optional<std::string_view>;
[[nodiscard]] auto fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto find_name(std::span<const NameIndex>,std::size_t,std::string_view,std::size_t&,std::size_t)
    -> std::expected<std::size_t,Error>;
}
class Owner {
  public:
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    ~Owner();
    Owner(const Owner&)=delete;
    auto operator=(const Owner&) -> Owner& =delete;
    [[nodiscard]] auto evidence() const noexcept -> const build_object_association::Owner*;
    [[nodiscard]] auto pairs() const noexcept -> std::span<const Pair>;
    [[nodiscard]] auto counts() const noexcept -> const Counts*;
    [[nodiscard]] auto measurement() const noexcept -> const nlohmann::ordered_json*;
    [[nodiscard]] auto monitoring() const noexcept -> const nlohmann::ordered_json*;
    [[nodiscard]] auto semantic() const noexcept -> std::string_view;
    [[nodiscard]] auto canonical() const noexcept -> std::string_view;
    [[nodiscard]] auto execution_digest() const noexcept -> const ExecutionPlanDigest*;
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest*;
    [[nodiscard]] auto usage() const noexcept -> Usage;
  private:
    explicit Owner(std::unique_ptr<detail::State>) noexcept;
    std::unique_ptr<detail::State> state_;
    friend auto produce(build_object_association::Owner&&,Limits) -> std::expected<Owner,Error>;
};
[[nodiscard]] auto produce(build_object_association::Owner&&,Limits) -> std::expected<Owner,Error>;
} // namespace epycbench::results::execution_plan
