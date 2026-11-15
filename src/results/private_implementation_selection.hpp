#pragma once
#include "results/private_normalization_serialization.hpp"
#include "epycbench/kernel/descriptor.hpp"
#include <span>

namespace epycbench::results::implementation_selection {
inline constexpr std::string_view profile="epycbench.private-implementation-selection/v1";
inline constexpr std::string_view plugin_profile="epycbench.private-implementation-selection/v2";
struct Workload {
    std::string workload_id;
    kernel::KernelId semantic;
    kernel::KernelImplementationId implementation;
    std::optional<kernel::ModuleIdentity> module{};
};
struct Binding { std::size_t experiment_index,workload_index; };
enum class Error { state,origin,source,capacity,reference,registry,encoding };
struct Quantities {
    std::size_t workloads{},bindings{},workload_text_bytes{},maximum_workload_text{},
        child_retained_bytes{},child_canonical_bytes{},canonical_bytes{},association_work{},
        retained_bytes{},consumer_simultaneous_bytes{};
    auto operator==(const Quantities&) const -> bool = default;
};
using Limits=Quantities;
using Usage=Quantities;
class Owner;
namespace detail {
struct State {
    std::optional<normalization_serialization::Owner> child;
    std::vector<Workload> workloads;
    std::vector<Binding> bindings;
    std::string canonical;
    core::Sha256Digest digest;
    Usage usage;
};
[[nodiscard]] auto fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto checked_add(std::size_t,std::size_t) noexcept -> std::optional<std::size_t>;
[[nodiscard]] auto checked_multiply(std::size_t,std::size_t) noexcept -> std::optional<std::size_t>;
// Pure bounded seams; none can construct or promote an owning child.
[[nodiscard]] auto find_workload(std::span<const Workload>,std::string_view,std::size_t& work,std::size_t limit)
    -> std::expected<std::size_t,Error>;
[[nodiscard]] auto mapping_valid(kernel::KernelId,kernel::KernelImplementationId,std::string_view) noexcept -> bool;
[[nodiscard]] auto uses_plugins(std::span<const Workload>) noexcept -> bool;
[[nodiscard]] auto valid_module(const kernel::ModuleIdentity&) noexcept -> bool;
}
class Owner {
  public:
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    ~Owner();
    Owner(const Owner&)=delete;
    auto operator=(const Owner&) -> Owner& = delete;
    [[nodiscard]] auto normalization() const noexcept -> const normalization_serialization::Owner*;
    [[nodiscard]] auto workloads() const noexcept -> std::span<const Workload>;
    [[nodiscard]] auto bindings() const noexcept -> std::span<const Binding>;
    [[nodiscard]] auto canonical() const noexcept -> std::string_view;
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest*;
    [[nodiscard]] auto usage() const noexcept -> Usage;
  private:
    explicit Owner(std::unique_ptr<detail::State>) noexcept;
    std::unique_ptr<detail::State> state_;
    friend auto retain(normalization_serialization::Owner&&,Limits) -> std::expected<Owner,Error>;
};
[[nodiscard]] auto retain(normalization_serialization::Owner&&,Limits) -> std::expected<Owner,Error>;
} // namespace epycbench::results::implementation_selection
