#pragma once
#include "monitoring/host_q1_plan_v2_value.hpp"
#include <expected>
namespace epycbench::monitoring::qualification {
struct MonitoringQ1PlanV2ParsedCandidate {
    std::string schema, qid, profile_schema, profile_sha, profile_path, plan_digest;
    std::uint64_t worker_count;
    std::vector<std::uint64_t> workers;
    std::uint64_t n, warmup;
    std::string sequencing, stopping, retry, discard, summaries;
    std::uint64_t deadline, envelope, artifact, tree, capacity;
    std::string privacy;
    std::string algorithm, mode;
    std::uint64_t target, derived_workers, derived_capacity, derived_n, total, allowance, reserve,
        derived_deadline;
};
struct MonitoringQ1PlanV2ModelSizes final {
    std::uint64_t evidence_bytes;
    std::uint64_t backend_envelope_bytes;
};
class MonitoringQ1PlanV2ComputedLimits final {
  public:
    auto capacity() const noexcept -> std::uint64_t { return capacity_; }
    auto maximum_successor_tree_bytes() const noexcept -> std::uint64_t { return tree_; }

  private:
    MonitoringQ1PlanV2ComputedLimits(std::uint64_t capacity, std::uint64_t tree) noexcept
        : capacity_(capacity), tree_(tree) {}
    std::uint64_t capacity_;
    std::uint64_t tree_;
    friend class detail_MonitoringQ1PlanV2ValueTestAccess;
};
class detail_MonitoringQ1PlanV2ValueTestAccess final {
  public:
    static auto compute_limits(const MonitoringQ1PlanV2ParsedCandidate&, std::string_view commit)
        -> std::expected<MonitoringQ1PlanV2ComputedLimits, core::Error>;
    static auto model_fits(const MonitoringQ1PlanV2ParsedCandidate&, std::string_view commit,
                           std::uint64_t observations_per_worker)
        -> std::expected<bool, core::Error>;
    static auto model_sizes(const MonitoringQ1PlanV2ParsedCandidate&, std::string_view commit,
                            std::uint64_t observations_per_worker)
        -> std::expected<MonitoringQ1PlanV2ModelSizes, core::Error>;
    static auto validate(MonitoringQ1PlanV2ParsedCandidate, MonitoringQ1PlanV2ComputedLimits)
        -> std::expected<MonitoringQ1PlanV2Value, core::Error>;
    static auto decode(std::string_view canonical_bytes, std::string_view captured_commit)
        -> std::expected<MonitoringQ1PlanV2Value, core::Error> {
        return MonitoringQ1PlanV2Value::decode(canonical_bytes, captured_commit);
    }
};
} // namespace epycbench::monitoring::qualification
