#pragma once
#include "results/process_subject_facts.hpp"
#include "results/authored_source.hpp"
#include "results/host_evidence_assembly.hpp"
#include <memory>

namespace epycbench::results::process_subject { class Session; }
namespace epycbench::results::run_creation {
using process_subject::Target;
using process_subject::FailureFact;
struct Binding {
    Target target;
    std::uint64_t failure_ordinal;
    auto operator==(const Binding&) const -> bool = default;
};
struct Input {
    const journal::RunId& run;
    process_subject::Domain domain;
    process_subject::Producer producer;
    authored_source::SourceBundle source;
    process_subject::BaseBuild build;
    host_assembly::Owner host;
    std::vector<FailureFact> failures;
    std::vector<Binding> bindings;
};
struct Quantities {
    std::size_t records{}, artifact_slots{}, source_bytes{}, source_name_bytes{},
        override_count{}, override_bytes{}, failure_facts{}, targets{}, bindings{},
        error_context_nodes{}, error_string_bytes{}, error_binary_bytes{},
        canonical_child_bytes{}, retained_bytes{}, consumer_simultaneous_bytes{};
    auto operator==(const Quantities&) const -> bool = default;
};
using Limits = Quantities;
using Usage = Quantities;
enum class Error { state, identity, reference, invalid_input, capacity };
struct TargetPosition { std::size_t failure, target; };
namespace detail {
struct State {
    std::string run;
    process_subject::Domain domain;
    std::uint64_t sequence{1};
    std::optional<authored_source::SourceBundle> source;
    std::optional<process_subject::BaseBuild> build;
    std::optional<host_assembly::Owner> host;
    std::vector<FailureFact> failures;
    std::vector<Binding> bindings;
    std::vector<TargetPosition> targets;
    std::vector<std::size_t> binding_index;
    Usage usage;
};
[[nodiscard]] auto fits(const Usage&, const Limits&) noexcept -> bool;
[[nodiscard]] auto add(std::size_t&, std::size_t) noexcept -> bool;
[[nodiscard]] auto elements(std::size_t&, std::size_t, std::size_t) noexcept -> bool;
[[nodiscard]] auto same_source(const authored_source::SourceBundle&,
                               const authored_source::SourceBundle&) noexcept -> bool;
[[nodiscard]] auto fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto bound_failure(std::span<const FailureFact>, std::span<const Binding>,
    std::span<const std::size_t>, const Target&) noexcept -> const FailureFact*;
}
class Owner {
  public:
    ~Owner();
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    Owner(const Owner&) = delete;
    auto operator=(const Owner&) -> Owner& = delete;
    [[nodiscard]] auto facts() const noexcept -> const detail::State* { return state_.get(); }
    [[nodiscard]] auto usage() const noexcept -> Usage;
    // A target resolves only through an explicit immutable binding.
    [[nodiscard]] auto failure_for(const Target&) const noexcept -> const FailureFact*;
    [[nodiscard]] auto failure_at(std::uint64_t) const noexcept -> const FailureFact*;
  private:
    explicit Owner(std::unique_ptr<detail::State>) noexcept;
    static auto prepare(const Input&, const Limits&, std::size_t prior,
                        std::size_t session_stock) -> std::expected<Owner, Error>;
    void commit(Input&) noexcept;
    std::unique_ptr<detail::State> state_;
    friend class process_subject::Session;
};
} // namespace epycbench::results::run_creation
