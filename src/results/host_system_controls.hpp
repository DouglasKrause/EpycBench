#pragma once
#include "epycbench/results/journal_records.hpp"
#include "results/topology_evidence_owner.hpp"
#include <array>
#include <span>
#include <variant>

namespace epycbench::results::host_controls {
inline constexpr std::string_view profile = "epycbench.results.host-system-controls/v1";
enum class Producer { controller, runner };
enum class State { not_attempted, available, unavailable, failed };
enum class Reason { not_supported, source_missing, access_denied, read_failed, malformed_source, unsupported_value };
enum class SmtMode { on, off, forceoff, notsupported, notimplemented, threads };
enum class ThpEnabled { always, madvise, never };
enum class ThpDefrag { always, defer, defer_madvise, madvise, never };
struct SmtControl {
    SmtMode mode;
    std::optional<std::uint32_t> threads_per_core;
    auto operator==(const SmtControl&) const -> bool = default;
};
using Value = std::variant<std::monostate, SmtControl, bool, std::uint32_t, ThpEnabled, ThpDefrag>;
struct Slot {
    State state{State::not_attempted};
    Value value;
    const journal::Timestamp* observed_at{};
    std::optional<Reason> reason;
    std::optional<std::uint64_t> failure_event;
};
struct Input {
    const journal::RunId& run;
    std::uint64_t ordinal;
    Producer producer;
    std::array<Slot, 5> observations;
    const topology_evidence::Owner* topology{};
};
struct Limits { std::size_t observations, retained_bytes, canonical_bytes; };
struct Usage {
    std::size_t observations{}, retained_bytes{}, canonical_bytes{};
    auto operator==(const Usage&) const -> bool = default;
};
struct Observation {
    std::string id;
    State state{State::not_attempted};
    Value value;
    std::optional<std::string> observed_at;
    std::optional<Reason> reason;
    std::optional<std::string> failure_event;
};
struct TopologyBinding {
    core::Sha256Digest canonical_sha256;
    topology::TopologyFingerprints fingerprints;
    auto operator==(const TopologyBinding&) const -> bool = default;
};
class Owner {
  public:
    Owner(const Owner&) = delete;
    auto operator=(const Owner&) -> Owner& = delete;
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    [[nodiscard]] auto observations() const noexcept -> std::span<const Observation>;
    [[nodiscard]] auto topology() const noexcept -> const TopologyBinding*;
    [[nodiscard]] auto canonical() const noexcept -> std::string_view { return canonical_; }
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest*;
    [[nodiscard]] auto usage() const noexcept -> Usage { return usage_; }
    [[nodiscard]] auto run_id() const noexcept -> std::string_view { return live_ ? std::string_view{run_} : std::string_view{}; }
    [[nodiscard]] auto producer() const noexcept -> std::optional<Producer> { return live_ ? std::optional{producer_} : std::nullopt; }
    [[nodiscard]] auto ordinal() const noexcept -> std::optional<std::uint64_t> { return live_ ? std::optional{ordinal_} : std::nullopt; }
  private:
    Owner() = default;
    bool live_{true};
    std::string run_;
    Producer producer_{};
    std::uint64_t ordinal_{};
    std::array<Observation, 5> observations_{};
    std::optional<TopologyBinding> topology_;
    std::string canonical_;
    core::Sha256Digest digest_{core::sha256(std::string_view{})};
    Usage usage_{};
    friend auto retain(const Input&, Limits) -> std::expected<Owner, core::Error>;
};
[[nodiscard]] auto retain(const Input&, Limits) -> std::expected<Owner, core::Error>;
} // namespace epycbench::results::host_controls
