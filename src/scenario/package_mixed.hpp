#pragma once

#include "kernel/package_manifest.hpp"

#include <array>
#include <cstddef>
#include <vector>

#include <nlohmann/json.hpp>

namespace epycbench::scenario {

inline constexpr std::string_view package_mixed_protocol = "epycbench.package-mixed-alone/v1";

// The together condition has ordinal equal to the role count. For two roles,
// all six permutations occur once and every directed inter-block transition
// occurs once per cycle. Larger groups rotate the positions of all conditions.
inline auto mixed_condition_order(std::size_t roles, std::size_t block) -> std::vector<std::size_t> {
    if (roles == 2U) {
        constexpr std::array<std::array<std::size_t, 3>, 6> cycle{{
            {0U, 1U, 2U}, {0U, 2U, 1U}, {2U, 1U, 0U},
            {1U, 0U, 2U}, {1U, 2U, 0U}, {2U, 0U, 1U}}};
        const auto& order = cycle[block % cycle.size()];
        return {order.begin(), order.end()};
    }
    const auto conditions = roles + 1U;
    std::vector<std::size_t> order;
    order.reserve(conditions);
    for (std::size_t i = 0; i < conditions; ++i)
        order.push_back((i + block) % conditions);
    return order;
}

inline auto mixed_role_alone_plan(const nlohmann::json& mixed, std::size_t index,
                                  const kernel::packages::Catalog& catalog) -> nlohmann::json {
    using Json = nlohmann::json;
    const auto& role = mixed.at("scenario").at("roles").at(index);
    const auto& id = role.at("id").get_ref<const std::string&>();
    const auto& workload = role.at("workload").get_ref<const std::string&>();
    auto plan = mixed;
    plan.erase("mixed_protocol");
    auto& source = plan.at("scenario");
    source["mode"] = "standalone";
    source["roles"] = Json::array({role});
    source["workloads"] = {{workload, mixed.at("scenario").at("workloads").at(workload)}};
    plan["placement"] = Json::array({mixed.at("placement").at(index)});
    plan["bindings"] = {{id, mixed.at("bindings").at(id)}};
    plan["geometry"] = Json::array();
    for (const auto& row : mixed.at("geometry"))
        if (row.at("role") == id) plan["geometry"].push_back(row);
    const auto& task_id = source.at("workloads").at(workload).at("task").get_ref<const std::string&>();
    const auto& implementation = role.at("implementation").get_ref<const std::string&>();
    const auto& task = *catalog.find(task_id, implementation)->task;
    plan["scope"] = task.collective
        ? (task.collective_finalizer ? "collective-release-to-completion"
                                    : "collective-worker-wall-time")
        : "role-enclosing-wall-time";
    return plan;
}

} // namespace epycbench::scenario
