#pragma once

#include "epycbench/kernel/gather_u64_task.hpp"

namespace epycbench::kernel::detail::gather_inspection {
struct GatherL1Geometry {
    std::string_view cache_id, worker_id;
    std::uint64_t capacity_bytes, line_bytes, ways;
};

[[nodiscard]] auto project_gather_pages(const GatherU64Config&, std::uint64_t base_page_bytes,
                                       std::string_view workload_id)
    -> std::expected<nlohmann::ordered_json, core::Error>;

[[nodiscard]] auto project_gather_cache_sets(const GatherU64Config&, std::uint64_t base_page_bytes,
                                            std::string_view workload_id, const GatherL1Geometry&)
    -> std::expected<nlohmann::ordered_json, core::Error>;
} // namespace epycbench::kernel::detail::gather_inspection
