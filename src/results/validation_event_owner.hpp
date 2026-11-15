#pragma once
#include "epycbench/core/digest.hpp"
#include "epycbench/monitoring/acquisition_driver.hpp"
#include "epycbench/results/journal_records.hpp"
#include <array>
#include <span>

namespace epycbench::results::validation_event {
inline constexpr std::string_view profile = "epycbench.results.monitoring-capability-assessment/v1";
enum class Producer { controller, runner };
enum class Evaluation { not_requested, not_evaluated, evaluated };
enum class Outcome { pass, fail, warning, unavailable, not_applicable };
enum class Effect { none, undetermined, reject, warn, disable };
enum class Status { accepted, accepted_with_warnings, incomplete, rejected };
struct Slot {
    Evaluation evaluation;
    std::optional<monitoring::MonitoringSourceSupport> support;
    const journal::Timestamp* checked_at{};
};
struct Input {
    const journal::RunId& run;
    std::uint64_t ordinal;
    Producer producer;
    const monitoring::MonitoringExecutionPolicy& policy;
    std::array<Slot, 4> slots;
};
struct Limits {
    std::size_t checks, retained_bytes, canonical_bytes;
};
struct Usage {
    std::size_t checks{}, retained_bytes{}, canonical_bytes{};
    auto operator==(const Usage&) const -> bool = default;
};
struct Check {
    std::string id;
    monitoring::MonitoringChannelDisposition requested;
    Evaluation evaluation;
    std::optional<monitoring::MonitoringSourceSupport> support;
    std::optional<std::string> checked_at;
    std::optional<Outcome> outcome;
    Effect effect;
    std::optional<monitoring::MonitoringEffectiveChannelDisposition> effective;
};
class Owner {
  public:
    Owner(const Owner&) = delete;
    auto operator=(const Owner&) -> Owner& = delete;
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    [[nodiscard]] auto run_id() const noexcept -> std::string_view { return live_ ? std::string_view{run_} : std::string_view{}; }
    [[nodiscard]] auto producer() const noexcept -> std::optional<Producer> { return live_ ? std::optional{producer_} : std::nullopt; }
    [[nodiscard]] auto ordinal() const noexcept -> std::optional<std::uint64_t> { return live_ ? std::optional{ordinal_} : std::nullopt; }
    [[nodiscard]] auto checks() const noexcept -> std::span<const Check>;
    [[nodiscard]] auto warning_indices() const noexcept -> std::span<const std::size_t>;
    [[nodiscard]] auto policy() const noexcept -> const monitoring::MonitoringExecutionPolicy*;
    [[nodiscard]] auto status() const noexcept -> std::optional<Status>;
    [[nodiscard]] auto canonical() const noexcept -> std::string_view;
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest*;
    [[nodiscard]] auto usage() const noexcept -> Usage;

  private:
    explicit Owner(const monitoring::MonitoringExecutionPolicy&);
    bool live_{true};
    std::string run_;
    Producer producer_{};
    std::uint64_t ordinal_{};
    monitoring::MonitoringExecutionPolicy policy_;
    std::array<Check, 4> checks_{};
    std::array<std::size_t, 4> warnings_{};
    std::size_t warning_count_{};
    Status status_{};
    std::string canonical_;
    core::Sha256Digest digest_{core::sha256(std::string_view{})};
    Usage usage_{};
    friend auto retain(const Input&, Limits) -> std::expected<Owner, core::Error>;
};
[[nodiscard]] auto retain(const Input&, Limits) -> std::expected<Owner, core::Error>;
} // namespace epycbench::results::validation_event
