#pragma once
#include "epycbench/core/digest.hpp"
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace epycbench::results::kernel_report {
// This identity binds original report bytes, never approval or object contents.
enum class Family { latency, bandwidth };
enum class Disposition { pass, fail };
struct Limits {
    std::size_t input_bytes, string_bytes, integer_digits, nodes, entries, depth, retained_bytes;
};
struct Usage {
    std::size_t input_bytes{}, string_bytes{}, integer_digits{}, nodes{}, entries{}, depth{},
        retained_bytes{};
    auto operator==(const Usage&) const -> bool = default;
};
// Closed validated values retain Python's integer evidence without binary64 rounding.
class Value {
  public:
    enum class Kind { null, boolean, integer, string, array, object };
    [[nodiscard]] auto kind() const noexcept -> Kind { return kind_; }
    [[nodiscard]] auto text() const noexcept -> std::string_view { return text_; }
    [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }
    [[nodiscard]] auto children() const noexcept -> std::span<const Value> { return children_; }
    [[nodiscard]] auto member(std::string_view) const noexcept -> const Value*;

  private:
    Kind kind_{Kind::null};
    std::string name_, text_;
    std::vector<Value> children_;
    friend struct Parser;
};
struct ReportView {
    Family family;
    Disposition disposition;
    const Value& content;
};
class Owner {
  public:
    Owner(const Owner&);
    auto operator=(const Owner&) -> Owner&;
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    ~Owner();
    [[nodiscard]] auto report() const noexcept -> std::optional<ReportView>;
    [[nodiscard]] auto bytes() const noexcept -> std::string_view;
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest*;
    [[nodiscard]] auto usage() const noexcept -> Usage;

  private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
    explicit Owner(std::unique_ptr<Storage>);
    friend auto retain(std::string_view, Limits) -> std::expected<Owner, core::Error>;
};
[[nodiscard]] auto retain(std::string_view, Limits) -> std::expected<Owner, core::Error>;
} // namespace epycbench::results::kernel_report
