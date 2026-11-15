#pragma once
#include "epycbench/core/error.hpp"
#include <memory>
#include <expected>
namespace epycbench::results::private_error {
struct Limits { std::size_t nodes, depth, string_bytes, binary_bytes, retained_bytes; };
struct Usage {
    std::size_t nodes{}, depth{}, string_bytes{}, binary_bytes{}, retained_bytes{};
    auto operator==(const Usage&) const -> bool = default;
};
class Owner {
  public:
    Owner(const Owner&)=delete;
    auto operator=(const Owner&) -> Owner& =delete;
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    ~Owner();
    [[nodiscard]] auto source() const noexcept -> const core::Error*;
    [[nodiscard]] auto usage() const noexcept -> Usage;
    [[nodiscard]] static auto fixed_bytes() noexcept -> std::size_t;
  private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
    explicit Owner(std::unique_ptr<Storage>) noexcept;
    friend auto retain(const core::Error&,Limits) -> std::expected<Owner,core::Error>;
};
[[nodiscard]] auto retain(const core::Error&,Limits) -> std::expected<Owner,core::Error>;
namespace detail {
[[nodiscard]] auto checked_add(std::size_t&,std::size_t) noexcept -> bool;
}
} // namespace epycbench::results::private_error
