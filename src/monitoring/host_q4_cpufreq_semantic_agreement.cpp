#include "monitoring/host_q4_cpufreq_semantic_agreement.hpp"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <limits>
#include <ranges>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Failure = MonitoringQ4CpuFreqSemanticFailure;
using Kind = MonitoringQ4CpuFreqSemanticFailureKind;
using Field = MonitoringQ4CpuFreqSemanticField;
using View = platform_linux::detail::CpuFrequencyPolicyView;

using Reason = MonitoringQ4CapacityReason;
auto fail(Kind kind, Field field = Field::none, std::uint64_t ordinal = 0,
          MonitoringQ4CpuFreqProductionFailure production =
              MonitoringQ4CpuFreqProductionFailure::none, Reason reason = Reason::none) {
    return Failure{kind, field, ordinal, production, reason};
}
auto trim(std::string_view value) noexcept {
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (!value.empty() && space(value.front()))
        value.remove_prefix(1);
    while (!value.empty() && space(value.back()))
        value.remove_suffix(1);
    return value;
}
auto valid_utf8(std::string_view text) noexcept -> bool {
    std::size_t i{};
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i++]);
        if (c < 0x80)
            continue;
        unsigned need{};
        std::uint32_t value{};
        std::uint32_t minimum{};
        if ((c & 0xe0U) == 0xc0U) {
            need = 1;
            value = c & 0x1fU;
            minimum = 0x80;
        } else if ((c & 0xf0U) == 0xe0U) {
            need = 2;
            value = c & 0x0fU;
            minimum = 0x800;
        } else if ((c & 0xf8U) == 0xf0U) {
            need = 3;
            value = c & 0x07U;
            minimum = 0x10000;
        } else
            return false;
        if (i + need > text.size())
            return false;
        for (unsigned n{}; n < need; ++n) {
            const auto d = static_cast<unsigned char>(text[i++]);
            if ((d & 0xc0U) != 0x80U)
                return false;
            value = (value << 6U) | (d & 0x3fU);
        }
        if (value < minimum || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU))
            return false;
    }
    return true;
}
auto oracle_integer(std::string_view input) -> std::optional<std::uint64_t> {
    const auto value = trim(input);
    if (value.empty())
        return std::nullopt;
    std::uint64_t result{};
    for (char c : value) {
        if (c < '0' || c > '9')
            return std::nullopt;
        const auto d = static_cast<unsigned>(c - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - d) / 10U)
            return std::nullopt;
        result = result * 10U + d;
    }
    return result;
}
auto oracle_khz(std::string_view value) -> std::optional<std::uint64_t> {
    auto parsed = oracle_integer(value);
    if (!parsed || *parsed == 0 || *parsed > std::numeric_limits<std::uint64_t>::max() / 1000U)
        return std::nullopt;
    return *parsed * 1000U;
}
auto oracle_token(std::string_view input) noexcept -> std::optional<std::string_view> {
    auto value = input;
    if (value.ends_with("\r\n"))
        value.remove_suffix(2);
    else if (value.ends_with('\n'))
        value.remove_suffix(1);
    if (value.empty())
        return std::nullopt;
    for (char c : value) {
        const auto b = static_cast<unsigned char>(c);
        if (b < 0x20U || b == 0x7fU)
            return std::nullopt;
    }
    return value;
}
auto oracle_cpus(std::string_view input, std::uint32_t maximum, std::uint64_t count_max,
                 std::vector<core::LinuxCpuId>& result, bool& exhausted) noexcept -> bool {
    exhausted = false;
    result.clear();
    auto value = trim(input);
    if (value.empty())
        return false;
    std::size_t start{};
    while (start < value.size()) {
        auto comma = value.find(',', start);
        auto end = comma == std::string_view::npos ? value.size() : comma;
        if (start == end)
            return false;
        auto dash = value.find('-', start);
        std::uint64_t first{}, last{};
        auto parse = [&](std::size_t a, std::size_t b, std::uint64_t& out) {
            if (a == b)
                return false;
            auto r = std::from_chars(value.data() + a, value.data() + b, out);
            return r.ec == std::errc{} && r.ptr == value.data() + b;
        };
        if (dash == std::string_view::npos || dash >= end) {
            if (!parse(start, end, first))
                return false;
            last = first;
        } else {
            if (value.find('-', dash + 1) < end || !parse(start, dash, first) ||
                !parse(dash + 1, end, last) || first > last)
                return false;
        }
        if (last > maximum)
            return false;
        for (std::uint64_t cpu = first;; ++cpu) {
            const auto cpu_id = core::LinuxCpuId{static_cast<std::uint32_t>(cpu)};
            const auto position = std::lower_bound(result.begin(), result.end(), cpu_id);
            if (position == result.end() || *position != cpu_id) {
                if (result.size() == result.capacity()) {
                    exhausted = true;
                    return false;
                }
                result.insert(position, cpu_id);
            }
            if (cpu == last)
                break;
        }
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
        if (start == value.size())
            return false;
    }
    return !result.empty() && result.size() <= count_max;
}
auto less_domain(const MonitoringQ4CpuFreqDomain& a, const MonitoringQ4CpuFreqDomain& b) {
    return std::lexicographical_compare(a.cpus.begin(), a.cpus.end(), b.cpus.begin(), b.cpus.end());
}
auto same_semantic(const MonitoringQ4CpuFreqDomain& a, const MonitoringQ4CpuFreqDomain& b) {
    return std::ranges::equal(a.cpus, b.cpus) && a.source_kind == b.source_kind &&
           a.driver == b.driver &&
           a.governor == b.governor && a.minimum_hz == b.minimum_hz && a.maximum_hz == b.maximum_hz;
}
} // namespace

MonitoringQ4CpuFreqSemanticWorkspace::MonitoringQ4CpuFreqSemanticWorkspace(
    MonitoringQ4CpuFreqSemanticWorkspace&& other) noexcept
    : binding_{std::move(other.binding_)}, maximum_cpu_{other.maximum_cpu_},
      maximum_count_{other.maximum_count_}, admitted_{std::move(other.admitted_)},
      views_{std::move(other.views_)}, production_scratch_{std::move(other.production_scratch_)},
      oracle_parse_scratch_{std::move(other.oracle_parse_scratch_)},
      oracle_memberships_{std::move(other.oracle_memberships_)},
      oracle_policies_{std::move(other.oracle_policies_)},
      production_memberships_{std::move(other.production_memberships_)},
      production_domains_{std::move(other.production_domains_)},
      oracle_domains_{std::move(other.oracle_domains_)},
      production_cpus_{std::move(other.production_cpus_)},
      oracle_cpus_{std::move(other.oracle_cpus_)},
      consumed_{std::exchange(other.consumed_, true)} {}

auto MonitoringQ4CpuFreqSemanticWorkspace::prepare(const MonitoringQ4SourceReady& ready) noexcept
    -> std::expected<MonitoringQ4CpuFreqSemanticWorkspace, MonitoringQ4CpuFreqSemanticFailure> {
    const auto& structural = ready.inputs().structural();
    return prepare_with(ready.authority().kernel_maximum_binding_,
                        static_cast<std::uint32_t>(structural.maximum_cpu_id().value),
                        structural.maximum_policy_directories().value,
                        structural.per_file_content_bytes().value, ready.inputs().admitted_cpus(),
                        {});
}

auto MonitoringQ4CpuFreqSemanticWorkspace::prepare_with(
    std::shared_ptr<const void> binding, std::uint32_t maximum_cpu, std::uint64_t maximum_count,
    std::uint64_t content_bytes, std::span<const core::LinuxCpuId> admitted,
    PrepareOperations operations) noexcept
    -> std::expected<MonitoringQ4CpuFreqSemanticWorkspace,
                     MonitoringQ4CpuFreqSemanticFailure> {
    if (maximum_count > std::numeric_limits<std::size_t>::max())
        return std::unexpected{fail(Kind::capacity, Field::none, 0,
            MonitoringQ4CpuFreqProductionFailure::none, Reason::semantic_count_size)};
    if (content_bytes > std::numeric_limits<std::size_t>::max())
        return std::unexpected{fail(Kind::capacity, Field::none, 0,
            MonitoringQ4CpuFreqProductionFailure::none, Reason::semantic_content_size)};
    auto allocation_reason = Reason::semantic_admitted_allocation;
    try {
        MonitoringQ4CpuFreqSemanticWorkspace value;
        unsigned stage{};
        const auto checkpoint = [&](Reason reason) {
            allocation_reason = reason;
            if (operations.fault)
                operations.fault(operations.context, stage);
            ++stage;
        };
        value.binding_ = std::move(binding);
        value.maximum_cpu_ = maximum_cpu;
        value.maximum_count_ = maximum_count;
        const auto n = static_cast<std::size_t>(maximum_count);
        const auto b = static_cast<std::size_t>(content_bytes);
        checkpoint(Reason::semantic_admitted_allocation);
        value.admitted_.reserve(admitted.size());
        value.admitted_.insert(value.admitted_.end(), admitted.begin(), admitted.end());
        checkpoint(Reason::semantic_views_allocation);
        value.views_.reserve(n);
        checkpoint(Reason::semantic_parsed_allocation);
        value.production_scratch_.parsed.reserve(n);
        checkpoint(Reason::semantic_selected_allocation);
        value.production_scratch_.selected.reserve(n);
        checkpoint(Reason::semantic_oracle_parse_allocation);
        value.oracle_parse_scratch_.reserve(n);
        checkpoint(Reason::semantic_oracle_members_allocation);
        value.oracle_memberships_.reserve(n);
        checkpoint(Reason::semantic_oracle_policies_allocation);
        value.oracle_policies_.reserve(n);
        checkpoint(Reason::semantic_production_members_allocation);
        value.production_memberships_.reserve(n);
        checkpoint(Reason::semantic_production_domains_allocation);
        value.production_domains_.resize(value.admitted_.size());
        checkpoint(Reason::semantic_oracle_domains_allocation);
        value.oracle_domains_.resize(value.admitted_.size());
        for (auto* domains : {&value.production_domains_, &value.oracle_domains_})
            for (auto& domain : *domains) {
                checkpoint(Reason::semantic_driver_allocation);
                domain.driver.reserve(b);
                checkpoint(Reason::semantic_governor_allocation);
                domain.governor.reserve(b);
            }
        checkpoint(Reason::semantic_production_cpus_allocation);
        value.production_cpus_.resize(value.admitted_.size());
        checkpoint(Reason::semantic_oracle_cpus_allocation);
        value.oracle_cpus_.resize(value.admitted_.size());
        return value;
    } catch (const std::bad_alloc&) {
        return std::unexpected{fail(Kind::capacity, Field::none, 0,
            MonitoringQ4CpuFreqProductionFailure::none, allocation_reason)};
    } catch (...) {
        return std::unexpected{fail(Kind::contained)};
    }
}

auto MonitoringQ4CpuFreqSemanticAgreement::evaluate(
    MonitoringQ4CpuFreqSemanticWorkspace&& workspace,
    const MonitoringQ4CpuFreqInventory& inventory) noexcept
    -> std::expected<MonitoringQ4CpuFreqSemanticSnapshot, MonitoringQ4CpuFreqSemanticFailure> {
    return evaluate_with(std::move(workspace), inventory, Fault::none, nullptr, nullptr);
}

auto MonitoringQ4CpuFreqSemanticAgreement::evaluate_with(
    MonitoringQ4CpuFreqSemanticWorkspace&& workspace,
    const MonitoringQ4CpuFreqInventory& inventory, Fault fault, void* observer_context,
    ViewObserver observer) noexcept
    -> std::expected<MonitoringQ4CpuFreqSemanticSnapshot, MonitoringQ4CpuFreqSemanticFailure> {
    if (workspace.consumed_)
        return std::unexpected{fail(Kind::inactive)};
    workspace.consumed_ = true;
    if (!workspace.binding_ || workspace.binding_ != inventory.binding_)
        return std::unexpected{fail(Kind::binding)};
    try {
        workspace.views_.clear();
        for (const auto& policy : inventory.policies_) {
            const auto& a = policy.attributes;
            workspace.views_.push_back(
                {policy.policy_number, inventory.bytes(a[0]), inventory.bytes(a[1]),
                 inventory.bytes(a[2]), inventory.bytes(a[3]), inventory.bytes(a[4]),
                 a[5].present && !a[5].hardware_unreadable
                     ? std::optional{inventory.bytes(a[5])} : std::nullopt,
                 a[6].present ? std::optional{inventory.bytes(a[6])} : std::nullopt});
        }
        if (observer)
            for (std::size_t policy{}; policy < workspace.views_.size(); ++policy) {
                const auto& view = workspace.views_[policy];
                const std::array<std::optional<std::string_view>, 7> fields{
                    view.related_cpus, view.scaling_driver, view.scaling_governor,
                    view.scaling_minimum, view.scaling_maximum, view.hardware_current,
                    view.scaling_current};
                for (std::size_t field{}; field < fields.size(); ++field)
                    if (fields[field])
                        observer(observer_context, policy, field, fields[field]->data(),
                                 fields[field]->size());
            }
        if (fault == Fault::throw_allocation)
            throw std::bad_alloc{};
        if (fault == Fault::throw_unknown)
            throw 1;
        if (fault == Fault::reject_oracle)
            return std::unexpected{fail(Kind::oracle, Field::membership)};
        workspace.oracle_memberships_.clear();
        workspace.oracle_policies_.clear();
        for (std::size_t i{}; i < workspace.views_.size(); ++i) {
            const auto& v = workspace.views_[i];
            bool exhausted{};
            if (!oracle_cpus(v.related_cpus, workspace.maximum_cpu_, workspace.maximum_count_,
                             workspace.oracle_parse_scratch_, exhausted))
                return std::unexpected{fail(exhausted ? Kind::capacity : Kind::oracle,
                                            Field::membership, i + 1, MonitoringQ4CpuFreqProductionFailure::none,
                                            exhausted ? Reason::semantic_oracle_parse_slots : Reason::none)};
            auto offset = workspace.oracle_memberships_.size();
            bool equal_existing{};
            for (const auto& prior : workspace.oracle_policies_) {
                const auto prior_membership = std::span{workspace.oracle_memberships_}.subspan(
                    prior.membership_offset, prior.membership_count);
                const auto overlaps =
                    std::ranges::any_of(workspace.oracle_parse_scratch_, [&](auto cpu) {
                        return std::ranges::binary_search(prior_membership, cpu);
                    });
                if (std::ranges::equal(prior_membership, workspace.oracle_parse_scratch_)) {
                    offset = prior.membership_offset;
                    equal_existing = true;
                    break;
                }
                if (overlaps)
                    return std::unexpected{fail(Kind::overlap, Field::membership, i + 1)};
            }
            if (!equal_existing) {
                if (workspace.oracle_parse_scratch_.size() >
                    workspace.oracle_memberships_.capacity() -
                        workspace.oracle_memberships_.size())
                    return std::unexpected{fail(Kind::capacity, Field::membership, i + 1,
                        MonitoringQ4CpuFreqProductionFailure::none, Reason::semantic_oracle_member_slots)};
                workspace.oracle_memberships_.insert(workspace.oracle_memberships_.end(),
                                                      workspace.oracle_parse_scratch_.begin(),
                                                      workspace.oracle_parse_scratch_.end());
            }
            auto driver = oracle_token(v.scaling_driver),
                 governor = oracle_token(v.scaling_governor);
            auto min = oracle_khz(v.scaling_minimum), max = oracle_khz(v.scaling_maximum);
            if (!driver)
                return std::unexpected{fail(Kind::oracle, Field::driver, i + 1)};
            if (!governor)
                return std::unexpected{fail(Kind::oracle, Field::governor, i + 1)};
            if (!min)
                return std::unexpected{fail(Kind::oracle, Field::minimum, i + 1)};
            if (!max || *min > *max)
                return std::unexpected{fail(Kind::oracle, Field::maximum, i + 1)};
            if (!valid_utf8(*driver))
                return std::unexpected{
                    fail(Kind::technical_token_invalid_utf8, Field::driver, i + 1)};
            if (!valid_utf8(*governor))
                return std::unexpected{
                    fail(Kind::technical_token_invalid_utf8, Field::governor, i + 1)};
            auto hw = v.hardware_current ? oracle_khz(*v.hardware_current) : std::nullopt;
            auto sc = v.scaling_current ? oracle_khz(*v.scaling_current) : std::nullopt;
            if (inventory.policies_[i].attributes[5].hardware_unreadable && !v.scaling_current) {
                auto failure = fail(Kind::hardware_access, Field::source_kind, i + 1);
                failure.native_error = EACCES;
                return std::unexpected{failure};
            }
            if ((v.hardware_current || v.scaling_current) && !hw && !sc)
                return std::unexpected{fail(Kind::oracle, Field::source_kind, i + 1)};
            workspace.oracle_policies_.push_back(
                {offset, workspace.oracle_parse_scratch_.size(),
                 hw ? monitoring::FrequencySourceKind::hardware_current
                    : monitoring::FrequencySourceKind::scaling_current,
                 *driver, *governor, *min, *max, v.locator,
                 !v.hardware_current && !v.scaling_current});
        }
        const auto membership = [&](const auto& policy) {
            return std::span{workspace.oracle_memberships_}.subspan(policy.membership_offset,
                                                                    policy.membership_count);
        };
        for (std::size_t i{}; i < workspace.oracle_policies_.size(); ++i)
            for (std::size_t j = i + 1; j < workspace.oracle_policies_.size(); ++j) {
                const auto first_membership = membership(workspace.oracle_policies_[i]);
                const auto second_membership = membership(workspace.oracle_policies_[j]);
                const auto overlaps = std::ranges::any_of(first_membership, [&](auto cpu) {
                    return std::ranges::binary_search(second_membership, cpu);
                });
                if (overlaps && !std::ranges::equal(first_membership, second_membership))
                    return std::unexpected{fail(Kind::overlap, Field::membership, j + 1)};
                const auto& first = workspace.oracle_policies_[i];
                const auto& second = workspace.oracle_policies_[j];
                if (std::ranges::equal(first_membership, second_membership) &&
                    (first.no_current != second.no_current ||
                     (!first.no_current &&
                      (first.source_kind != second.source_kind || first.driver != second.driver ||
                       first.governor != second.governor ||
                       first.minimum_hz != second.minimum_hz ||
                       first.maximum_hz != second.maximum_hz)))) {
                    auto field = Field::support;
                    if (!first.no_current && !second.no_current) {
                        if (first.source_kind != second.source_kind)
                            field = Field::source_kind;
                        else if (first.driver != second.driver)
                            field = Field::driver;
                        else if (first.governor != second.governor)
                            field = Field::governor;
                        else if (first.minimum_hz != second.minimum_hz)
                            field = Field::minimum;
                        else if (first.maximum_hz != second.maximum_hz)
                            field = Field::maximum;
                    }
                    return std::unexpected{fail(Kind::conflict, field, j + 1)};
                }
        }
        for (std::size_t j = workspace.oracle_policies_.size(); j-- > 0;)
            for (std::size_t i{}; i < j; ++i)
                if (std::ranges::equal(membership(workspace.oracle_policies_[i]),
                                       membership(workspace.oracle_policies_[j]))) {
                    workspace.views_.erase(workspace.views_.begin() +
                                           static_cast<std::ptrdiff_t>(j));
                    workspace.oracle_policies_.erase(workspace.oracle_policies_.begin() +
                                                     static_cast<std::ptrdiff_t>(j));
                    break;
                }
        std::size_t production_domain_count{}, oracle_domain_count{}, outcome_index{};
        auto add = [&](std::vector<MonitoringQ4CpuFreqDomain>& domains,
                       std::vector<core::LinuxCpuId>& storage, std::size_t& count,
                       std::span<const core::LinuxCpuId> cpus,
                       monitoring::FrequencySourceKind source_kind, std::string_view driver,
                       std::string_view governor, std::uint64_t minimum,
                       std::uint64_t maximum, std::uint32_t locator) {
            const auto found = std::ranges::find_if(std::span{domains}.first(count),
                                                    [&](const auto& existing) {
                                                        return std::ranges::equal(existing.cpus,
                                                                                  cpus);
                                                    });
            if (found != std::span{domains}.first(count).end())
                return true;
            if (cpus.size() > storage.capacity() - storage.size())
                return false;
            const auto offset = storage.size();
            storage.insert(storage.end(), cpus.begin(), cpus.end());
            auto& domain = domains[count++];
            domain.cpus = std::span{storage}.subspan(offset, cpus.size());
            domain.source_kind = source_kind;
            domain.driver.assign(driver);
            domain.governor.assign(governor);
            domain.minimum_hz = minimum;
            domain.maximum_hz = maximum;
            domain.private_policy_locator = locator;
            return true;
        };
        for (const auto cpu : workspace.admitted_) {
            auto prod = platform_linux::detail::CpuFrequencyAccess::project_bounded(
                cpu, workspace.views_, workspace.production_scratch_);
            if (fault == Fault::reject_production)
                return std::unexpected{fail(Kind::production, Field::none, 0,
                                            MonitoringQ4CpuFreqProductionFailure::contained)};
            if (!prod)
                if (prod.error() ==
                    platform_linux::detail::CpuFrequencyBoundedFailure::capacity)
                    return std::unexpected{fail(Kind::capacity, Field::membership, 0,
                        MonitoringQ4CpuFreqProductionFailure::none, Reason::semantic_production_parse_slots)};
            if (!prod)
                return std::unexpected{fail(Kind::production, Field::none, 0,
                                            prod.error() == platform_linux::detail::
                                                                CpuFrequencyBoundedFailure::ambiguous
                                                ? MonitoringQ4CpuFreqProductionFailure::
                                                      ambiguous_policy
                                                : MonitoringQ4CpuFreqProductionFailure::
                                                      invalid_value)};
            if (prod->value) {
                auto& v = *prod->value;
                if (!valid_utf8(v.driver) || !valid_utf8(v.governor))
                    return std::unexpected{fail(Kind::technical_token_invalid_utf8)};
                if (!add(workspace.production_domains_, workspace.production_memberships_,
                         production_domain_count, v.related_cpus,
                         v.kind == platform_linux::CpuFrequencyCurrentKind::hardware_current
                             ? monitoring::FrequencySourceKind::hardware_current
                             : monitoring::FrequencySourceKind::scaling_current,
                         v.driver, v.governor, v.minimum_hz, v.maximum_hz, v.locator))
                    return std::unexpected{fail(Kind::capacity, Field::membership, 0,
                        MonitoringQ4CpuFreqProductionFailure::none, Reason::semantic_production_member_slots)};
            }
            const auto it = std::ranges::find_if(workspace.oracle_policies_, [&](const auto& p) {
                return std::ranges::binary_search(membership(p), cpu);
            });
            if (it == workspace.oracle_policies_.end())
                workspace.oracle_cpus_[outcome_index] =
                    {cpu, std::nullopt,
                     MonitoringQ4CpuFreqUnsupportedReason::no_resolvable_policy};
            else if (it->no_current)
                workspace.oracle_cpus_[outcome_index] =
                    {cpu, std::nullopt,
                     MonitoringQ4CpuFreqUnsupportedReason::no_current_attribute};
            else {
                const auto cpus = membership(*it);
                // Oracle policy spans already refer into the independently canonicalized
                // total-N store; domain spans may bind that same owned store without copying.
                auto found = std::ranges::find_if(
                    std::span{workspace.oracle_domains_}.first(oracle_domain_count),
                    [&](const auto& existing) { return std::ranges::equal(existing.cpus, cpus); });
                if (found == std::span{workspace.oracle_domains_}.first(oracle_domain_count).end()) {
                    auto& domain = workspace.oracle_domains_[oracle_domain_count++];
                    domain.cpus = cpus;
                    domain.source_kind = it->source_kind;
                    domain.driver.assign(it->driver);
                    domain.governor.assign(it->governor);
                    domain.minimum_hz = it->minimum_hz;
                    domain.maximum_hz = it->maximum_hz;
                    domain.private_policy_locator = it->locator;
                }
                workspace.oracle_cpus_[outcome_index] = {cpu, 0, std::nullopt};
            }
            if (prod->value)
                workspace.production_cpus_[outcome_index] = {cpu, 0, std::nullopt};
            else
                workspace.production_cpus_[outcome_index] =
                    {cpu, std::nullopt,
                     prod->unsupported == platform_linux::detail::CpuFrequencyUnsupportedReason::
                                              no_resolvable_policy
                         ? MonitoringQ4CpuFreqUnsupportedReason::no_resolvable_policy
                         : MonitoringQ4CpuFreqUnsupportedReason::no_current_attribute};
            ++outcome_index;
        }
        workspace.production_domains_.resize(production_domain_count);
        workspace.oracle_domains_.resize(oracle_domain_count);
        std::ranges::sort(workspace.production_domains_, less_domain);
        std::ranges::sort(workspace.oracle_domains_, less_domain);
        if (fault == Fault::disagree_source && oracle_domain_count != 0U)
            workspace.oracle_domains_.front().source_kind =
                workspace.oracle_domains_.front().source_kind ==
                        monitoring::FrequencySourceKind::hardware_current
                    ? monitoring::FrequencySourceKind::scaling_current
                    : monitoring::FrequencySourceKind::hardware_current;
        auto bind = [&](auto& outcomes, const auto& domains) {
            for (auto& o : outcomes)
                if (o.domain_ordinal) {
                    auto source = std::ranges::find_if(domains, [&](const auto& d) {
                        return std::ranges::binary_search(d.cpus, o.cpu);
                    });
                    o.domain_ordinal =
                        static_cast<std::uint64_t>(std::distance(domains.begin(), source)) + 1U;
                }
        };
        bind(workspace.production_cpus_, workspace.production_domains_);
        bind(workspace.oracle_cpus_, workspace.oracle_domains_);
        if (workspace.production_domains_.size() != workspace.oracle_domains_.size())
            return std::unexpected{fail(Kind::agreement, Field::membership)};
        for (std::size_t i{}; i < workspace.production_domains_.size(); ++i)
            if (!same_semantic(workspace.production_domains_[i], workspace.oracle_domains_[i])) {
                const auto& production = workspace.production_domains_[i];
                const auto& oracle_domain = workspace.oracle_domains_[i];
                auto field = Field::membership;
                if (std::ranges::equal(production.cpus, oracle_domain.cpus)) {
                    if (production.source_kind != oracle_domain.source_kind)
                        field = Field::source_kind;
                    else if (production.driver != oracle_domain.driver)
                        field = Field::driver;
                    else if (production.governor != oracle_domain.governor)
                        field = Field::governor;
                    else if (production.minimum_hz != oracle_domain.minimum_hz)
                        field = Field::minimum;
                    else if (production.maximum_hz != oracle_domain.maximum_hz)
                        field = Field::maximum;
                }
                return std::unexpected{fail(Kind::agreement, field, i + 1)};
            }
        if (workspace.production_cpus_ != workspace.oracle_cpus_)
            return std::unexpected{fail(Kind::agreement, Field::support)};
        return MonitoringQ4CpuFreqSemanticSnapshot{inventory.policies_.size(),
                                                   std::move(workspace.production_memberships_),
                                                   std::move(workspace.production_domains_),
                                                   std::move(workspace.production_cpus_)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{fail(Kind::contained)};
    } catch (...) {
        return std::unexpected{fail(Kind::contained)};
    }
}
} // namespace epycbench::monitoring::qualification::detail
