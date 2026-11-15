#pragma once

#include "results/filesystem_journal.hpp"
#include "results/process_subject_admission.hpp"
#include "results/private_execution_plan.hpp"

namespace epycbench::results::preparation_observation {
enum class Error { state, identity, capacity, source, serialization, journal, clock };
struct TimestampSource {
    void* context{};
    // Fixed canonical UTC bytes. Exceptions are contained by the adapter.
    bool (*read)(void*,std::array<char,30>&){};
};
struct CallFailures {
    const core::Error* primary{};
    std::span<const core::Error> cleanup;
};
struct CheckCallResult {
    CallFailures failures;
    const nlohmann::json* snapshot{};
    const validation_event::Owner* assessment{};
};
using CheckCall=CheckCallResult (*)(void*,const process_subject::Session&);
using KernelCall=CallFailures (*)(void*,const process_subject::Session&);
// Borrows remain valid until retirement; moving either parent invalidates use.
// Successful return values are the existing producer owners, never new receipts.
class Adapter final {
  public:
    Adapter(const Adapter&)=delete;
    Adapter(Adapter&&) noexcept;
    auto operator=(const Adapter&) -> Adapter& =delete;
    auto operator=(Adapter&&) -> Adapter& =delete;
    [[nodiscard]] static auto prepare(process_subject::Session&,filesystem_journal::Writer&,
        event_profile::Limits,bool native_platform,bool acquired_utc,TimestampSource={}) -> std::expected<Adapter,Error>;
    // Delivers the immutable creation failures in their original ordinal order.
    [[nodiscard]] auto creation_failures(const journal::Timestamp&) -> std::expected<void,Error>;
    [[nodiscard]] auto report(const journal::Timestamp&,std::string_view,kernel_report::Limits)
        -> std::expected<kernel_report::Owner,core::Error>;
    [[nodiscard]] auto approval(const journal::Timestamp&,std::string_view,
        kernel_report::Owner&&,kernel_report::Owner&&,std::array<std::string_view,2>,kernel_approval::Limits)
        -> std::expected<kernel_approval::Owner,core::Error>;
    // The actual acquisition and all partial inputs remain owned by context;
    // returned references stay valid through the complete observer call.
    [[nodiscard]] auto kernel_evidence(const journal::Timestamp&,void*,KernelCall)
        -> std::expected<void,Error>;
    [[nodiscard]] auto controller_checks(const journal::Timestamp&,void*,CheckCall)
        -> std::expected<void,Error>;
    [[nodiscard]] auto selection(const journal::Timestamp&,normalization_serialization::Owner&&,
        implementation_selection::Limits) -> std::expected<implementation_selection::Owner,implementation_selection::Error>;
    [[nodiscard]] auto association(const journal::Timestamp&,implementation_selection::Owner&&,
        base_build_serialization::Owner&&,build_object_association::ApprovalInputs&&,
        build_object_association::Limits) -> std::expected<build_object_association::Owner,build_object_association::Error>;
    [[nodiscard]] auto execution_plan(const journal::Timestamp&,build_object_association::Owner&&,
        results::execution_plan::Limits) -> std::expected<results::execution_plan::Owner,results::execution_plan::Error>;
    [[nodiscard]] auto preflight(std::uint64_t,const journal::Timestamp&,results::preflight::Input&,const nlohmann::json* snapshot=nullptr)
        -> std::expected<void,core::Error>;
    [[nodiscard]] auto planning(const journal::Timestamp&,const results::execution_plan::Owner&)
        -> std::expected<filesystem_journal::Receipt,Error>;
    // Existing closed facts are admitted against the actual Writer prefix by C2.
    [[nodiscard]] auto close(const journal::Timestamp&,const nlohmann::json&)
        -> std::expected<filesystem_journal::Receipt,Error>;
    [[nodiscard]] auto failure() const noexcept -> std::optional<Error> {return failure_;}
    [[nodiscard]] auto bound_to(const filesystem_journal::Writer& writer) const noexcept -> bool {
        return session_&&writer_==&writer&&!failure_;
    }
    [[nodiscard]] auto bound_to(const process_subject::Session& session,const filesystem_journal::Writer& writer) const noexcept -> bool {
        return session_==&session&&bound_to(writer);
    }
  private:
    Adapter(process_subject::Session&,filesystem_journal::Writer&,event_profile::Limits,bool,bool,TimestampSource) noexcept;
    [[nodiscard]] auto observe(const journal::Timestamp&) -> std::optional<journal::Timestamp>;
    [[nodiscard]] auto valid(bool before_start=true) const -> bool;
    [[nodiscard]] auto append(const journal::Timestamp&,event_profile::Kind,process_subject::Producer,
        const nlohmann::json&) -> std::expected<filesystem_journal::Receipt,Error>;
    void failed(const journal::Timestamp&,std::string_view,const core::Error&);
    void typed_failure(const journal::Timestamp&,std::string_view code,unsigned);
    process_subject::Session* session_{};
    filesystem_journal::Writer* writer_{};
    event_profile::Limits limits_;
    std::uint64_t ordinal_{};
    bool native_platform_{},acquired_utc_{};
    TimestampSource clock_;
    std::optional<Error> failure_;
    const nlohmann::json* pending_checks_{};
    std::optional<core::Sha256Digest> pending_digest_,pending_assessment_;
    bool checks_called_{};
};
}
