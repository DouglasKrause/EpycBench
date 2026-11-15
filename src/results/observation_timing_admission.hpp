#pragma once
#include "results/expected_pair_evidence_context.hpp"
#include "results/private_error_owner.hpp"

namespace epycbench::results::block_evidence {
enum class Domain : std::uint8_t;
}
namespace epycbench::results::observation_timing {
enum class Boundary : std::uint8_t { completed_enclosing_start, failed_attempt_start };
enum class Clock : std::uint8_t { monotonic_raw };
enum class Disposition : std::uint8_t { available, not_recorded, acquisition_failed };
enum class Family : std::uint8_t { bandwidth, latency };
struct IdentityView final {
    const journal::RunId* run{};
    block_evidence::Domain domain{};
    core::ExperimentId experiment{0U};
    std::string_view scenario_profile, scenario_digest, resolved_plan_digest;
    std::string_view workload, first_variant, second_variant;
    Family family{};
    kernel::DependentLoadLatencyBlockId block{0U};
    std::uint64_t slot{};
    measurement::ComparisonVariantSlot variant{};
    core::ObservationId observation{0U};
};
class Input final {
  public:
    Input(IdentityView, Boundary, Disposition) noexcept;
    Input(Input&&) noexcept;
    auto operator=(Input&&) noexcept -> Input&;
    ~Input() = default;
    Input(const Input&) = delete;
    auto operator=(const Input&) -> Input& = delete;
    [[nodiscard]] auto available() const noexcept -> bool { return live_; }
    IdentityView identity;
    Boundary boundary;
    Disposition disposition;
    std::optional<Clock> clock;
    std::optional<platform_linux::RawMonotonicTimestamp> raw_start;
    const journal::Timestamp* utc{};
    std::optional<private_error::Owner> error;
  private:
    bool live_{true};
    friend class Owner;
};
struct Limits final {
    std::size_t records{}, utc_bytes{}, error_nodes{}, error_depth{}, error_string_bytes{};
    std::size_t error_binary_bytes{}, error_retained_bytes{}, retained_bytes{}, simultaneous_bytes{};
};
struct Usage final {
    std::size_t records{}, utc_bytes{}, error_nodes{}, error_depth{}, error_string_bytes{};
    std::size_t error_binary_bytes{}, error_retained_bytes{}, retained_bytes{}, simultaneous_bytes{};
    auto operator==(const Usage&) const -> bool = default;
};
struct Coverage final {
    bool requested{}, completed_starts_available{};
    std::size_t completed{}, failed{};
    std::array<std::size_t, 3> completed_dispositions{}, failed_dispositions{};
};
class Owner final {
  public:
    struct State;
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    ~Owner();
    Owner(const Owner&) = delete;
    auto operator=(const Owner&) -> Owner& = delete;
    [[nodiscard]] auto identity() const noexcept -> std::optional<IdentityView>;
    [[nodiscard]] auto boundary() const noexcept -> std::optional<Boundary>;
    [[nodiscard]] auto disposition() const noexcept -> std::optional<Disposition>;
    [[nodiscard]] auto clock() const noexcept -> std::optional<Clock>;
    [[nodiscard]] auto raw_start() const noexcept -> std::optional<platform_linux::RawMonotonicTimestamp>;
    [[nodiscard]] auto utc() const noexcept -> const journal::Timestamp*;
    [[nodiscard]] auto error() const noexcept -> const private_error::Owner*;
    [[nodiscard]] auto calibration_sequence() const noexcept -> std::optional<std::uint64_t>;
    [[nodiscard]] auto usage() const noexcept -> const Usage*;
    [[nodiscard]] auto shared_context_bytes() const noexcept -> std::size_t;
    [[nodiscard]] static auto state_bytes() noexcept -> std::size_t;
    [[nodiscard]] static auto staging_bytes() noexcept -> std::size_t;
  private:
    explicit Owner(std::unique_ptr<State>) noexcept;
    static auto prepare(block_evidence::PairFactsLease&&, const Input&, const Usage&,
        std::uint64_t) -> std::expected<Owner, block_evidence::Error>;
    void commit(Input&) noexcept;
    std::unique_ptr<State> state_;
    friend class block_evidence::Accumulator;
};
namespace detail {
[[nodiscard]] auto measure(const Input&, const block_evidence::PairFactsLease&,
    block_evidence::Domain) noexcept -> std::expected<Usage, block_evidence::Error>;
[[nodiscard]] auto matches(const Input&, const runtime::FactualObservationEvidence*,
    const runtime::FailedObservationEvidence*) noexcept -> bool;
[[nodiscard]] auto accumulate(const Usage&, const Usage&) noexcept -> std::optional<Usage>;
[[nodiscard]] auto fits(const Usage&, const Limits&) noexcept -> bool;
[[nodiscard]] constexpr auto add(std::size_t& total, std::size_t amount) noexcept -> bool {
    if (amount > SIZE_MAX - total) return false;
    total += amount; return true;
}
} // namespace detail
} // namespace epycbench::results::observation_timing
