#pragma once
#include "epycbench/results/journal_records.hpp"
#include "epycbench/topology/stable_id.hpp"
#include "results/topology_evidence_owner.hpp"
#include <span>
#include <variant>

namespace epycbench::results::pu_cpu_identity {
inline constexpr std::string_view profile = "epycbench.results.amd-pu-cpu-identity/v1";
enum class Producer { controller, runner };
enum class State { not_attempted, available, unavailable, failed };
enum class Reason { not_supported, source_missing, access_denied, read_failed, malformed_source, unsupported_value };
struct Signature { std::uint32_t eax; auto operator==(const Signature&) const -> bool = default; };
struct Microcode { std::uint32_t revision; auto operator==(const Microcode&) const -> bool = default; };
using Value = std::variant<std::monostate, Signature, Microcode>;
struct Slot {
    State state{State::not_attempted};
    Value value;
    const journal::Timestamp* observed_at{};
    std::optional<Reason> reason;
    std::optional<std::uint64_t> failure_event;
};
struct Row {
    const topology::StableProcessingUnitId& stable_pu_id;
    core::LinuxCpuId linux_cpu;
    Slot signature;
    Slot microcode;
};
struct Input {
    const journal::RunId& run;
    std::uint64_t ordinal;
    Producer producer;
    const topology_evidence::Owner& topology;
    std::span<const Row> rows;
};
struct Limits { std::size_t processing_units, observations, canonical_bytes, retained_bytes; };
struct Usage {
    std::size_t processing_units{}, observations{}, canonical_bytes{}, retained_bytes{};
    auto operator==(const Usage&) const -> bool = default;
};
struct DecodedSignature {
    std::uint32_t eax, family, model, stepping;
    auto operator==(const DecodedSignature&) const -> bool = default;
};
struct Observation {
    std::string id;
    State state{State::not_attempted};
    std::variant<std::monostate, DecodedSignature, Microcode> value;
    std::optional<std::string> observed_at;
    std::optional<Reason> reason;
    std::optional<std::string> failure_event;
};
struct OwnedRow {
    std::string stable_pu_id;
    core::LinuxCpuId linux_cpu;
    bool topology_online;
    Observation signature, microcode;
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
    [[nodiscard]] auto rows() const noexcept -> std::span<const OwnedRow>;
    [[nodiscard]] auto topology() const noexcept -> const TopologyBinding*;
    [[nodiscard]] auto unrepresented_present_cpus() const noexcept -> std::string_view { return unrepresented_; }
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
    std::vector<OwnedRow> rows_;
    std::optional<TopologyBinding> topology_;
    std::string unrepresented_, canonical_;
    core::Sha256Digest digest_{core::sha256(std::string_view{})};
    Usage usage_{};
    friend auto retain(const Input&, Limits) -> std::expected<Owner, core::Error>;
};
[[nodiscard]] auto retain(const Input&, Limits) -> std::expected<Owner, core::Error>;
#ifdef EPYCBENCH_PU_CPU_IDENTITY_TESTING
namespace testing { inline std::size_t reload_calls{}, output_calls{}; }
#endif
} // namespace epycbench::results::pu_cpu_identity
