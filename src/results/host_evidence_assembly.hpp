#pragma once
#include "results/topology_evidence_owner.hpp"
#include "results/host_system_controls.hpp"
#include "results/private_kernel_identity.hpp"
#include "results/pu_cpu_identity.hpp"
#include "results/frequency_policy_owner.hpp"
#include "results/anonymous_thp_policy_owner.hpp"
#include "results/process_thp_restriction.hpp"
#include <variant>
namespace epycbench::results::host_assembly {
struct NotAttempted {};
enum class Reason { not_supported, source_missing, access_denied };
struct Unavailable { Reason reason; };
// A declaration resolved by later journal composition, not an accepted event.
struct Failed { std::uint64_t failure_event_sequence; };
template<class T> using Slot = std::variant<NotAttempted, Unavailable, Failed, T>;
struct Categories {
    Slot<topology_evidence::Owner> topology;
    Slot<host_controls::Owner> system_controls;
    Slot<private_kernel_identity::Owner> kernel_identity;
    Slot<pu_cpu_identity::Owner> pu_identity;
    Slot<frequency_policy_owner::Owner> frequency;
    Slot<anonymous_thp_policy_owner::Owner> anonymous_thp;
    Slot<process_thp_restriction::Owner> process_thp;
};
struct Input { const journal::RunId& run; Categories categories; };
struct Limits { std::size_t slots, canonical_bytes, retained_bytes; };
struct Usage { std::size_t slots{}, canonical_bytes{}, retained_bytes{}; auto operator==(const Usage&) const -> bool = default; };
class Owner {
  public:
    Owner(const Owner&) = delete;
    auto operator=(const Owner&) -> Owner& = delete;
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    [[nodiscard]] auto run_id() const noexcept -> std::string_view { return live_ ? std::string_view{run_} : std::string_view{}; }
    [[nodiscard]] auto categories() const noexcept -> const Categories* { return live_ ? &categories_ : nullptr; }
    [[nodiscard]] auto usage() const noexcept -> Usage { return usage_; }
  private:
    Owner() = default;
    bool live_{true};
    std::string run_;
    Categories categories_;
    Usage usage_;
    friend auto assemble(Input&, Limits) -> std::expected<Owner, core::Error>;
};
[[nodiscard]] auto assemble(Input&, Limits) -> std::expected<Owner, core::Error>;
namespace detail {
// Pure checked accounting shared with boundary tests; no owner mutation seam.
[[nodiscard]] auto add_stock(Usage&, std::size_t canonical, std::size_t retained,
                             std::size_t embedded) noexcept -> bool;
}
} // namespace epycbench::results::host_assembly
