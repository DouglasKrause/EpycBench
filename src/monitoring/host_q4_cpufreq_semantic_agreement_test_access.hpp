#pragma once
#include "monitoring/host_q4_cpufreq_semantic_agreement.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4CpuFreqSemanticAgreementTestAccess final {
  public:
    static void exhaust_membership_stock(MonitoringQ4CpuFreqSemanticWorkspace& workspace,
                                         unsigned stock) {
        std::vector<core::LinuxCpuId> empty;
        switch(stock) {
        case 0: workspace.oracle_parse_scratch_.swap(empty); break;
        case 1: workspace.oracle_memberships_.swap(empty); break;
        case 2: workspace.production_scratch_.parsed.swap(empty); break;
        case 3: workspace.production_memberships_.swap(empty); break;
        default: break;
        }
    }

    enum class Fault : unsigned char {
        none,
        reject_production,
        reject_oracle,
        disagree_source,
        throw_allocation,
        throw_unknown
    };
    static auto workspace(std::uint32_t maximum, std::vector<core::LinuxCpuId> admitted,
                          std::shared_ptr<const void> binding) {
        return *MonitoringQ4CpuFreqSemanticWorkspace::prepare_with(
            std::move(binding), maximum, static_cast<std::uint64_t>(maximum) + 1U, 128,
            admitted, {});
    }
    static auto workspace_limits(std::uint32_t maximum, std::uint64_t slots,
                                 std::uint64_t bytes,
                                 std::vector<core::LinuxCpuId> admitted,
                                 std::shared_ptr<const void> binding) {
        return *MonitoringQ4CpuFreqSemanticWorkspace::prepare_with(
            std::move(binding), maximum, slots, bytes, admitted, {});
    }
    static auto prepare_fault(std::uint32_t maximum, std::vector<core::LinuxCpuId> admitted,
                              std::shared_ptr<const void> binding, unsigned fail_at,
                              bool unknown = false) {
        struct Context {
            unsigned fail_at;
            bool unknown;
        } context{fail_at, unknown};
        return MonitoringQ4CpuFreqSemanticWorkspace::prepare_with(
            std::move(binding), maximum, static_cast<std::uint64_t>(maximum) + 1U, 128,
            admitted,
            {&context, [](void* raw, unsigned stage) {
                 if (stage == static_cast<Context*>(raw)->fail_at) {
                     if (static_cast<Context*>(raw)->unknown)
                         throw 1;
                     else
                         throw std::bad_alloc{};
                 }
             }});
    }
    struct Policy final {
        std::uint32_t number{};
        std::array<std::optional<std::string>, 7> bytes{};
        bool hardware_unreadable{};
    };
    static auto inventory(std::vector<Policy> input, std::shared_ptr<const void> binding) {
        std::vector<MonitoringQ4CpuFreqPolicyInventory> policies;
        std::vector<char> contents;
        for (auto& source : input) {
            MonitoringQ4CpuFreqPolicyInventory policy{};
            policy.policy_number = source.number;
            for (std::size_t index{}; index < source.bytes.size(); ++index) {
                if (!source.bytes[index])
                    continue;
                const auto offset = contents.size();
                contents.insert(contents.end(), source.bytes[index]->begin(),
                                source.bytes[index]->end());
                policy.attributes[index] = {true, false, offset, source.bytes[index]->size(), {}};
            }
            if (source.hardware_unreadable) {
                auto& hardware = policy.attributes[5];
                hardware = {};
                hardware.present = true;
                hardware.hardware_unreadable = true;
            }
            policies.push_back(std::move(policy));
        }
        return MonitoringQ4CpuFreqInventory{
            {}, std::move(policies), std::move(contents), std::move(binding)};
    }
    static auto evaluate_with_fault(MonitoringQ4CpuFreqSemanticWorkspace&& workspace,
                                    const MonitoringQ4CpuFreqInventory& inventory, Fault fault) {
        return MonitoringQ4CpuFreqSemanticAgreement::evaluate_with(
            std::move(workspace), inventory,
            static_cast<MonitoringQ4CpuFreqSemanticAgreement::Fault>(fault), nullptr, nullptr);
    }
    static auto evaluate_observing_views(MonitoringQ4CpuFreqSemanticWorkspace&& workspace,
                                         const MonitoringQ4CpuFreqInventory& inventory,
                                         bool& exact) {
        struct Context {
            const MonitoringQ4CpuFreqInventory* inventory;
            bool* exact;
        } context{&inventory, &exact};
        exact = true;
        return MonitoringQ4CpuFreqSemanticAgreement::evaluate_with(
            std::move(workspace), inventory, MonitoringQ4CpuFreqSemanticAgreement::Fault::none,
            &context,
            [](void* raw, std::size_t policy, std::size_t field, const char* data,
               std::size_t size) {
                const auto& observed = *static_cast<Context*>(raw);
                const auto bytes = observed.inventory->bytes(
                    observed.inventory->policies_[policy].attributes[field]);
                *observed.exact =
                    *observed.exact && data == bytes.data() && size == bytes.size();
            });
    }
};
} // namespace epycbench::monitoring::qualification::detail
