#pragma once

#include "results/producer_progress.hpp"

namespace epycbench::results::native_lifecycle {
// Prepared from the same retained planning/preflight/request owners. The request
// body is admitted by C2 only after the controller actually validates the ACK.
struct Predecessors {
    std::uint64_t planning{}, preflight{}, controller{}, pair_ordinal{};
    std::string session_id;
    std::string request_facts;
};
enum class Error { state, capacity, timestamp, journal, schema, progress };
class Adapter final {
  public:
    ~Adapter();
    Adapter(Adapter&&) noexcept;
    Adapter(const Adapter&) = delete;
    auto operator=(Adapter&&) -> Adapter& = delete;
    [[nodiscard]] static auto prepare(filesystem_journal::Writer&, Predecessors,
        event_profile::Scope, producer_progress::TimestampSource,
        producer_progress::Limits, runtime::detail::progress::Configuration,
        bool native_platform) -> std::expected<Adapter,Error>;
    // All callback borrows expire on move/destruction. Writer, timestamp context
    // and this adapter outlive the controller, including its cleanup.
    [[nodiscard]] auto observer() noexcept -> runtime::detail::NativeLifecycleObserver;
    [[nodiscard]] auto consumer() noexcept -> runtime::detail::NativeProgressConsumer;
    [[nodiscard]] auto drain_retired() noexcept -> bool;
    [[nodiscard]] auto failure() const noexcept -> std::optional<Error>;
    [[nodiscard]] auto progress_failure() const noexcept -> std::optional<producer_progress::ProgressFailure>;
    [[nodiscard]] auto reconcile(runtime::detail::PairwiseV2NativeController&,
        const PairwiseRunResult&, core::JcsLimits)
        -> std::expected<filesystem_journal::Receipt,Error>;
  private:
    struct Data;
    [[nodiscard]] static auto admission(event_profile::Availability value) noexcept
        -> event_profile::ProducerAdmission { return event_profile::ProducerAdmission{value}; }
    explicit Adapter(std::unique_ptr<Data>) noexcept;
    std::unique_ptr<Data> data_;
};
}
