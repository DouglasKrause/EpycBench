#pragma once
#include "kernel/package_manifest.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
namespace epycbench::results::package_observation {
using Json = nlohmann::json;
using U64 = std::uint64_t;
using RankKey = std::pair<std::size_t, std::size_t>;
struct Interval {
    U64 begin{std::numeric_limits<U64>::max()};
    U64 end{};
    void include(U64 start, U64 finish) {
        begin = std::min(begin, start);
        end = std::max(end, finish);
    }
};
struct MeasuredRole {
    Interval interval;
    U64 units;
};
struct BoundRanks {
    std::map<std::string, std::size_t, std::less<>> by_role;
    std::map<RankKey, const Json*> geometry;
    std::vector<const kernel::packages::TaskManifest*> tasks;
    std::size_t rank_count{};
    std::set<std::string> cooperative;
};
[[nodiscard]] auto invalid(std::string reason) -> core::Error;
[[nodiscard]] auto hex(const Json& value) -> bool;
[[nodiscard]] bool unsigned_value(const Json& value);
[[nodiscard]] auto verified(const Json& trial, U64 repetitions, bool empty_partition) -> bool;
[[nodiscard]] auto verified_mapping(const Json& admitted, const Json& cpu, const Json& rank) -> bool;
[[nodiscard]] bool placed(const Json& placement, const Json& role, std::size_t rank,
                          const Json& regions, const Json& shared_node, bool shared_cpu_local,
                          bool supplied);
[[nodiscard]] auto block_input_identity(const Json& block, const Json& roles) -> std::string;
[[nodiscard]] auto bind_ranks(const Json& plan, const kernel::packages::Catalog& catalog)
    -> std::expected<BoundRanks, core::Error>;
[[nodiscard]] auto inspect_bound_block(const Json& plan, const BoundRanks& bound, const Json& block,
                                       U64 measured_repetitions, std::size_t ordinal,
                                       U64 previous_end, bool warm, std::string_view scope,
                                       bool retain_nonoverlap = false)
    -> std::expected<std::vector<MeasuredRole>, core::Error>;
} // namespace epycbench::results::package_observation
