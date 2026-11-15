#pragma once
#include "platform_linux/journal_file_operations.hpp"
#include <array>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
namespace epycbench::platform_linux::journal_file {
enum class Phase {
    argument,
    root,
    open,
    stat,
    read,
    write,
    file_sync,
    directory_sync,
    snapshot,
    close,
    rename
};
enum class Reason {
    state,
    limits,
    overflow,
    capacity,
    type,
    owner,
    mode,
    link,
    collision,
    interrupted_budget,
    no_progress,
    over_report,
    io,
    premature_eof,
    mutation
};
struct Issue {
    Phase phase{};
    Reason reason{};
    int native_error{};
    auto operator==(const Issue&) const -> bool = default;
};
struct CloseReport {
    std::array<Issue, 2> issues{};
    std::size_t count{}, attempts{};
};
struct Failure {
    Issue primary{};
    CloseReport cleanup{};
    bool created{};
};
struct Limits {
    std::size_t root_bytes{}, root_components{}, component_bytes{}, files{}, descriptors{},
        file_bytes{}, transfer_bytes{}, operations{};
};
struct Progress {
    std::size_t operations{}, bytes_read{}, bytes_written{}, initial_size{};
    bool created{}, live{}, writable{};
};
class File {
  public:
    struct State;
    ~File();
    File(File&&) noexcept;
    auto operator=(File&&) noexcept -> File&;
    File(const File&) = delete;
    auto operator=(const File&) -> File& = delete;
    [[nodiscard]] static auto create(std::string_view, std::string_view, Limits)
        -> std::expected<File, Failure>;
    [[nodiscard]] static auto open_read_only(std::string_view, std::string_view, Limits)
        -> std::expected<File, Failure>;
    // Same directory admission as create/read, without opening or creating a leaf.
    // A later file operation revalidates the root; this is not a durable capability.
    [[nodiscard]] static auto preflight_root(std::string_view, Limits)
        -> std::expected<void, Failure>;
    [[nodiscard]] auto write_some(std::span<const std::byte>)
        -> std::expected<std::size_t, Failure>;
    [[nodiscard]] auto read_some(std::span<std::byte>) -> std::expected<std::size_t, Failure>;
    [[nodiscard]] auto rename_no_replace(std::string_view) -> std::expected<void, Failure>;
    [[nodiscard]] auto sync_file() -> std::expected<void, Failure>;
    [[nodiscard]] auto sync_directory() -> std::expected<void, Failure>;
    [[nodiscard]] auto verify_snapshot() -> std::expected<void, Failure>;
    [[nodiscard]] auto close() noexcept -> CloseReport;
    [[nodiscard]] auto progress() const noexcept -> Progress;
    [[nodiscard]] static auto storage(Limits, std::size_t leaf_bytes)
        -> std::expected<std::size_t, Failure>;

  private:
    explicit File(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
    friend class detail::Access;
};
namespace detail {
class Access {
  public:
    [[nodiscard]] static auto open(std::string_view, std::string_view, Limits, bool, Operations)
        -> std::expected<File, Failure>;
    [[nodiscard]] static auto preflight_root(std::string_view, Limits, Operations)
        -> std::expected<void, Failure>;
  private:
    [[nodiscard]] static auto open_impl(std::string_view, std::string_view, Limits, bool, Operations, bool)
        -> std::expected<File, Failure>;
};
} // namespace detail
} // namespace epycbench::platform_linux::journal_file
