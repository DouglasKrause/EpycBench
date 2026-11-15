#pragma once

#include "epycbench/core/digest.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace epycbench::results::journal {

class RunId {
  public:
    [[nodiscard]] static auto parse(std::string_view) -> std::expected<RunId, core::Error>;
    [[nodiscard]] auto text() const noexcept -> const std::string& { return text_; }

  private:
    explicit RunId(std::string text) : text_{std::move(text)} {}
    std::string text_;
};

class Timestamp {
  public:
    [[nodiscard]] static auto parse(std::string_view) -> std::expected<Timestamp, core::Error>;
    [[nodiscard]] auto text() const noexcept -> const std::string& { return text_; }

  private:
    explicit Timestamp(std::string text) : text_{std::move(text)} {}
    std::string text_;
};

struct Limits {
    std::size_t body_bytes;
    std::size_t depth;
    std::size_t nodes;
    std::uint64_t records;
};

struct Event {
    Timestamp timestamp;
    std::string_view type;
    std::string_view producer;
    const nlohmann::json& payload;
};

struct Record {
    nlohmann::json envelope;
    std::string canonical_body;
};

enum class Tail { clean_prefix, incomplete_tail };
struct Summary {
    Tail tail;
    std::uint64_t count;
    std::optional<core::Sha256Digest> last_digest;
    std::size_t fragment_bytes;
};

struct Feed {
    std::size_t consumed;
    std::optional<Record> record;
};

class Writer {
  public:
    [[nodiscard]] static auto create(RunId, Limits) -> std::expected<Writer, core::Error>;
    Writer(Writer&&) noexcept;
    Writer(const Writer&) = delete;
    auto operator=(Writer&&) -> Writer& = delete;
    auto operator=(const Writer&) -> Writer& = delete;
    [[nodiscard]] auto append(const Event&) -> std::expected<std::string, core::Error>;
    [[nodiscard]] auto finish() -> std::expected<Summary, core::Error>;

  private:
    Writer(RunId, Limits);
    RunId run_;
    Limits limits_;
    std::uint64_t count_{};
    std::optional<core::Sha256Digest> previous_;
    bool live_{true};
};

class Reader {
  public:
    [[nodiscard]] static auto create(RunId, Limits) -> std::expected<Reader, core::Error>;
    Reader(Reader&&) noexcept;
    Reader(const Reader&) = delete;
    auto operator=(Reader&&) -> Reader& = delete;
    auto operator=(const Reader&) -> Reader& = delete;
    [[nodiscard]] auto feed(std::string_view) -> std::expected<Feed, core::Error>;
    [[nodiscard]] auto finish() -> std::expected<Summary, core::Error>;

  private:
    Reader(RunId, Limits);
    RunId run_;
    Limits limits_;
    std::uint64_t count_{};
    std::optional<core::Sha256Digest> previous_;
    std::string body_;
    bool in_record_{};
    bool live_{true};
};

} // namespace epycbench::results::journal
