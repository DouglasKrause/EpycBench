#pragma once
#include "epycbench/scenario/resolution.hpp"
#include "results/private_error_owner.hpp"
#include <memory>
#include <span>

namespace epycbench::results::authored_source {
struct Limits {
    std::size_t source_bytes;
    std::size_t name_bytes;
    std::size_t override_count;
    std::size_t override_bytes;
    std::size_t diagnostics;
    std::size_t canonical_bytes;
    std::size_t nodes;
    std::size_t total_bytes;
};
enum class Availability { not_attempted, not_obtained, available };
struct FailureReference {
    std::uint64_t value;
    auto operator==(const FailureReference&) const -> bool = default;
};
struct Input {
    Availability availability;
    std::string_view bytes;
    std::string_view name;
    scenario::ScenarioFormat format;
    std::span<const std::string_view> rejected_overrides;
    std::optional<FailureReference> acquisition_failure;
};
class SourceBundle {
  public:
    SourceBundle(const SourceBundle&);
    auto operator=(const SourceBundle&) -> SourceBundle&;
    SourceBundle(SourceBundle&&) noexcept;
    auto operator=(SourceBundle&&) noexcept -> SourceBundle&;
    ~SourceBundle();
    [[nodiscard]] auto input() const noexcept -> Input;
    [[nodiscard]] auto logical_bytes() const noexcept -> std::size_t;

  private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
    explicit SourceBundle(std::unique_ptr<Storage>);
    friend auto retain(Input, Limits) -> std::expected<SourceBundle, core::Error>;
};
enum class State {
    not_attempted,
    source_unavailable,
    override_rejected,
    scenario_rejected,
    resolution_failed,
    resolved,
    expected_plan_mismatch
};
struct SafeDiagnostic {
    std::string_view code;
};
struct Usage {
    std::size_t bytes;
    std::size_t nodes;
    std::size_t canonical_bytes;
};
class ResolutionRecord {
  public:
    ResolutionRecord(ResolutionRecord&&) noexcept;
    auto operator=(ResolutionRecord&&) noexcept -> ResolutionRecord&;
    ~ResolutionRecord();
    ResolutionRecord(const ResolutionRecord&) = delete;
    auto operator=(const ResolutionRecord&) -> ResolutionRecord& = delete;
    [[nodiscard]] auto source() const noexcept -> const SourceBundle*;
    [[nodiscard]] auto state() const noexcept -> State;
    [[nodiscard]] auto plan() const noexcept -> const scenario::ScenarioDigestedPlan*;
    [[nodiscard]] auto diagnostics() const noexcept -> std::span<const SafeDiagnostic>;
    [[nodiscard]] auto usage() const noexcept -> Usage;
    [[nodiscard]] auto upstream_error() const noexcept -> const private_error::Owner*;

  private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
    explicit ResolutionRecord(std::unique_ptr<Storage>);
    friend auto resolve(SourceBundle, const scenario::ResourceResolutionContext&, Limits,
                        const scenario::ScenarioDigestedPlan*)
        -> std::expected<ResolutionRecord, core::Error>;
    friend auto resolve(SourceBundle, const scenario::ResourceResolutionContext&, Limits,
                        std::optional<scenario::ScenarioResolutionLimits>,
                        const scenario::ScenarioDigestedPlan*)
        -> std::expected<ResolutionRecord, core::Error>;
};
[[nodiscard]] auto retain(Input, Limits) -> std::expected<SourceBundle, core::Error>;
[[nodiscard]] auto resolve(SourceBundle, const scenario::ResourceResolutionContext&, Limits,
                           const scenario::ScenarioDigestedPlan* expected = nullptr)
    -> std::expected<ResolutionRecord, core::Error>;
[[nodiscard]] auto resolve(SourceBundle, const scenario::ResourceResolutionContext&, Limits,
                           std::optional<scenario::ScenarioResolutionLimits>,
                           const scenario::ScenarioDigestedPlan* expected = nullptr)
    -> std::expected<ResolutionRecord, core::Error>;

} // namespace epycbench::results::authored_source
