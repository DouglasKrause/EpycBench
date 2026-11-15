#pragma once
#include "platform_linux/journal_file.hpp"
#include "results/incremental_event_transaction.hpp"
namespace epycbench::results::filesystem_journal {
namespace file = platform_linux::journal_file;
enum class Durability { process, persistent_block };
enum class State { open, sealed, failed, closed, moved_from };
enum class Error {
    state,
    limits,
    overflow,
    capacity,
    allocation,
    semantic,
    io,
    premature_eof,
    mutation
};
struct Failure {
    Error kind{};
    std::optional<event_profile::Error> semantic;
    std::optional<file::Failure> io;
};
struct CloseReport {
    std::optional<Failure> primary;
    std::array<file::Issue, 2> secondary{};
    std::size_t count{}, attempts{};
};
struct Position {
    std::uint64_t records{};
    std::optional<core::Sha256Digest> digest;
    std::size_t bytes{};
};
struct Receipt {
    State state{State::moved_from};
    Position semantic, complete;
    std::optional<Position> synchronized;
    std::size_t written_bytes{}, partial_frame_bytes{};
    bool directory_synchronized{}, file_eof{}, persistence_uncertain{};
    std::optional<event_profile::Summary> recovery;
    std::optional<Failure> failure;
    std::optional<CloseReport> cleanup;
};
struct Limits {
    file::Limits file;
    std::size_t working_bytes{};
};
namespace detail {
class Access;
}
class Writer {
  public:
    struct Data;
    ~Writer();
    Writer(Writer&&) noexcept;
    auto operator=(Writer&&) noexcept -> Writer&;
    Writer(const Writer&) = delete;
    auto operator=(const Writer&) -> Writer& = delete;
    [[nodiscard]] static auto create(std::string_view, const journal::RunId&, event_profile::Limits,
                                     Limits, Durability) -> std::expected<Writer, Failure>;
    [[nodiscard]] static auto storage(event_profile::Limits, Limits, std::size_t)
        -> std::expected<std::size_t, Failure>;
    [[nodiscard]] auto append(const event_profile::Input&) -> std::expected<Receipt, Failure>;
    [[nodiscard]] auto append(const event_profile::SuppliedEvent&)
        -> std::expected<Receipt, Failure>;
    [[nodiscard]] auto seal() -> std::expected<Receipt, Failure>;
    [[nodiscard]] auto close() noexcept -> CloseReport;
    [[nodiscard]] auto receipt() const noexcept -> Receipt;
    [[nodiscard]] auto records() const noexcept -> std::span<const event_profile::Record>;

  private:
    explicit Writer(std::unique_ptr<Data>) noexcept;
    std::unique_ptr<Data> data_;
    friend class detail::Access;
};
class Recovery {
  public:
    struct Data;
    ~Recovery();
    Recovery(Recovery&&) noexcept;
    auto operator=(Recovery&&) noexcept -> Recovery&;
    Recovery(const Recovery&) = delete;
    auto operator=(const Recovery&) -> Recovery& = delete;
    [[nodiscard]] static auto read(std::string_view, const journal::RunId&, event_profile::Limits,
                                   Limits) -> std::expected<Recovery, Failure>;
    [[nodiscard]] static auto storage(event_profile::Limits, Limits, std::size_t)
        -> std::expected<std::size_t, Failure>;
    [[nodiscard]] auto close() noexcept -> CloseReport;
    [[nodiscard]] auto receipt() const noexcept -> Receipt;
    [[nodiscard]] auto records() const noexcept -> std::span<const event_profile::Record>;

  private:
    explicit Recovery(std::unique_ptr<Data>) noexcept;
    std::unique_ptr<Data> data_;
    friend class detail::Access;
};
namespace detail {
class Access {
  public:
    [[nodiscard]] static auto transaction(const Writer&) noexcept -> const event_profile::Transaction*;
    [[nodiscard]] static auto transaction(const Recovery&) noexcept -> const event_profile::Transaction*;
    [[nodiscard]] static auto create(std::string_view, const journal::RunId&, event_profile::Limits,
                                     Limits, Durability, file::detail::Operations)
        -> std::expected<Writer, Failure>;
    [[nodiscard]] static auto read(std::string_view, const journal::RunId&, event_profile::Limits,
                                   Limits, file::detail::Operations)
        -> std::expected<Recovery, Failure>;
};
} // namespace detail
} // namespace epycbench::results::filesystem_journal
