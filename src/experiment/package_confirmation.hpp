#pragma once

#include "epycbench/experiment/coordinator.hpp"
#include "scenario/package_scenario.hpp"

#include <algorithm>
#include <array>

namespace epycbench::experiment {
inline constexpr std::size_t package_confirmation_blocks = 20U;

inline void apply_package_confirmation_policy(nlohmann::json& authored) {
    auto& execution = authored.at("execution");
    execution["blocks"] = package_confirmation_blocks;
    execution.erase("precision");
}

struct ProspectivePackageConfirmation {
    Schedule schedule;
    nlohmann::json authored;
};

[[nodiscard]] inline auto prospective_package_confirmation(const Schedule& discovery,
    std::size_t ordinal,std::uint64_t seed,std::optional<std::uint64_t> managed_seed,
    const nlohmann::json& source) -> std::expected<ProspectivePackageConfirmation,core::Error> {
    const auto reject=[](std::string_view kind,std::string_view message) {
        return std::unexpected{core::Error{.category=core::ErrorCategory::runtime,
            .code=core::StableErrorCode{"experiment.confirmation."+std::string{kind}},
            .message=std::string{message},.operation="experiment",.os_error={},.context={}}};
    };
    if (ordinal>=discovery.trials().size())
        return reject("discovery","Confirmation requires a scheduled exploration trial");
    if (seed==discovery.seed() || seed==managed_seed)
        return reject("seed","Use a distinct confirmation seed");
    const std::array selected{discovery.trials()[ordinal].configuration};
    auto schedule=Schedule::create(discovery.objective(),seed,1U,selected);
    if (!schedule) return std::unexpected{schedule.error()};
    const auto order=schedule->trials().front().comparison_order_seed;
    if (std::ranges::any_of(discovery.trials(),[order](const auto& trial) {
            return trial.comparison_order_seed==order;
        })) return reject("seed","The confirmation order seed collides with discovery");
    if (source.at("schema")!=scenario::package_scenario_schema || source.at("mode")!="comparison" ||
        source.at("roles").size()!=2U) return reject("comparison","Confirmation requires a two-role package comparison");
    auto authored=source;
    apply_package_confirmation_policy(authored);
    authored.at("execution")["order_seed"]=order;
    return ProspectivePackageConfirmation{std::move(*schedule),std::move(authored)};
}
}
