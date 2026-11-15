#pragma once
#include "results/filesystem_journal.hpp"
#include "runtime/pairwise_v2_native_lifecycle.hpp"

namespace epycbench::results::producer_progress {
namespace progress = runtime::detail::progress;
struct TimestampSource {
    void* context{};
    // Exactly YYYY-MM-DDTHH:MM:SS.nnnnnnnnnZ; no inferred monotonic/UTC conversion.
    bool (*read)(void*, std::array<char,30>&) noexcept{};
    progress::Utc origin{progress::Utc::supplied};
};
struct Limits {
    event_profile::Limits parse;
    std::size_t scratch_bytes{}, work{};
};
namespace detail {
// Internal phase accounting shared by reconciliation and its finite boundary tests.
// The controller owns canonical throughout; compact output never builds its DOM.
void reserve_reconciliation(std::string_view canonical, event_profile::Version,
    const Limits&, std::size_t events, core::JcsLimits,
    event_profile::detail::Work&);
}
enum class Error { state, identity, capacity, schema, timestamp, journal, unsupported };
struct ProgressFailure final {
    progress::Kind kind{};
    std::uint64_t ordinal{};
    std::optional<Error> error;
    std::optional<event_profile::Error> semantic;
    std::optional<block_evidence::Error> block;
};
class Adapter final {
  public:
    ~Adapter();
    Adapter(Adapter&&) noexcept;
    Adapter(const Adapter&) = delete;
    auto operator=(const Adapter&) -> Adapter& = delete;
    [[nodiscard]] static auto prepare(filesystem_journal::Writer&, const progress::Offer&,
        event_profile::Scope, TimestampSource, Limits) -> std::expected<Adapter,Error>;
    // Writer and timestamp context outlive this owner and the native controller.
    // The returned callback is valid only until this Adapter moves or retires.
    [[nodiscard]] auto consumer() noexcept -> runtime::detail::NativeProgressConsumer;
    [[nodiscard]] auto failure() const noexcept -> std::optional<Error>;
    [[nodiscard]] auto semantic_failure() const noexcept -> std::optional<event_profile::Error>;
    [[nodiscard]] auto failure_snapshot() const noexcept -> std::optional<ProgressFailure>;
    [[nodiscard]] auto received() const noexcept -> std::uint64_t;
    [[nodiscard]] auto persisted() const noexcept -> std::uint64_t;
    [[nodiscard]] auto reconcile(runtime::detail::PairwiseV2NativeController&,
        const PairwiseRunResult&, core::JcsLimits) -> std::expected<filesystem_journal::Receipt,Error>;
  private:
    struct Data;
    explicit Adapter(std::unique_ptr<Data>) noexcept;
    [[nodiscard]] auto consume(const runtime::detail::ValidatedProgressView&) noexcept
        -> runtime::detail::ProgressConsumeResult;
    std::unique_ptr<Data> data_;
};
}
