#pragma once
#include "results/incremental_event_profile.hpp"
#include "results/expected_pair_evidence_context.hpp"

namespace epycbench::results::event_profile {
namespace detail { struct CompositionState; }
enum class Tail { clean_prefix, incomplete_tail, corrupt };
enum class Recovery { interrupted, failed, completed };
struct Summary {
    std::uint64_t records{};
    std::optional<core::Sha256Digest> digest;
    Tail tail{};
    Recovery recovery{Recovery::interrupted};
    std::size_t fragment_bytes{};
};
struct Feed { std::size_t consumed{}; bool accepted{}; };
class Transaction {
  public:
    struct State;
    ~Transaction();
    Transaction(Transaction&&) noexcept;
    auto operator=(Transaction&&) noexcept -> Transaction&;
    Transaction(const Transaction&) = delete;
    auto operator=(const Transaction&) -> Transaction& = delete;
    [[nodiscard]] static auto create(const journal::RunId&, Limits) -> std::expected<Transaction, Error>;
    [[nodiscard]] auto append(const Input&) -> std::expected<std::string, Error>;
    [[nodiscard]] auto append(const SuppliedEvent&) -> std::expected<std::string, Error>;
    [[nodiscard]] auto feed(std::string_view) -> std::expected<Feed, Error>;
    [[nodiscard]] auto finish() -> std::expected<Summary, Error>;
    [[nodiscard]] auto records() const noexcept -> std::span<const Record>;
    [[nodiscard]] auto usage() const noexcept -> Usage;
    [[nodiscard]] auto summary() const noexcept -> Summary;
    [[nodiscard]] auto available() const noexcept -> bool;
    [[nodiscard]] auto version() const noexcept -> Version;
    [[nodiscard]] auto remaining_records() const noexcept -> std::uint64_t;
    // Fixed first inner rejection, independent of owner lifetime after copying.
    [[nodiscard]] auto pair_failure() const noexcept -> std::optional<block_evidence::Error>;
    // Borrow is invalidated by mutation or destruction; never grants admission.
    [[nodiscard]] auto composition() const noexcept -> const detail::CompositionState*;
  private:
    explicit Transaction(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
};
} // namespace epycbench::results::event_profile
