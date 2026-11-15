#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/id.hpp"
#include "epycbench/core/quantity.hpp"
#include "epycbench/topology/stable_id.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

namespace epycbench::memory {

namespace detail {
struct PairwiseMemoryAccess;
}

enum class PlacementPolicy { bind };
enum class PageMode { base };

class ExactRatio {
  public:
    [[nodiscard]] static auto create(std::uint64_t numerator, std::uint64_t denominator) noexcept
        -> std::expected<ExactRatio, core::Error>;

    [[nodiscard]] auto numerator() const noexcept -> std::uint64_t { return numerator_; }
    [[nodiscard]] auto denominator() const noexcept -> std::uint64_t { return denominator_; }

    auto operator==(const ExactRatio&) const noexcept -> bool = default;

  private:
    friend struct detail::PairwiseMemoryAccess;
    ExactRatio(std::uint64_t numerator, std::uint64_t denominator) noexcept
        : numerator_{numerator}, denominator_{denominator} {}

    std::uint64_t numerator_;
    std::uint64_t denominator_;
};

struct RegionNumaTarget {
    topology::StableNumaNodeId stable_node;
    core::LinuxNumaNodeId linux_node;
    core::NumaNodeId topology_node;

    auto operator==(const RegionNumaTarget&) const noexcept -> bool = default;
};

struct RegionRequestInput {
    core::WorkerId worker_id;
    core::ByteCount requested_bytes;
    core::ByteCount page_size;
    PlacementPolicy placement_policy;
    PageMode page_mode;
    std::vector<RegionNumaTarget> numa_targets;
    ExactRatio placement_threshold;
    bool prefault;
    bool verify_before;
    bool verify_after;
};

class RegionRequest {
  public:
    [[nodiscard]] static auto create(RegionRequestInput input) noexcept
        -> std::expected<RegionRequest, core::Error>;

    [[nodiscard]] auto worker_id() const noexcept -> core::WorkerId { return worker_id_; }
    [[nodiscard]] auto requested_bytes() const noexcept -> core::ByteCount {
        return requested_bytes_;
    }
    [[nodiscard]] auto page_size() const noexcept -> core::ByteCount { return page_size_; }
    [[nodiscard]] auto page_mode() const noexcept -> PageMode { return page_mode_; }
    [[nodiscard]] auto placement_policy() const noexcept -> PlacementPolicy {
        return placement_policy_;
    }
    [[nodiscard]] auto numa_targets() const noexcept -> std::span<const RegionNumaTarget> {
        return numa_targets_;
    }
    [[nodiscard]] auto placement_threshold() const noexcept -> const ExactRatio& {
        return placement_threshold_;
    }
    [[nodiscard]] auto prefault() const noexcept -> bool { return prefault_; }
    [[nodiscard]] auto verify_before() const noexcept -> bool { return verify_before_; }
    [[nodiscard]] auto verify_after() const noexcept -> bool { return verify_after_; }
    [[nodiscard]] auto requires_read_only() const noexcept -> bool { return true; }

  private:
    friend struct detail::PairwiseMemoryAccess;
    explicit RegionRequest(RegionRequestInput input) noexcept
        : worker_id_{input.worker_id}, requested_bytes_{input.requested_bytes},
          page_size_{input.page_size}, placement_policy_{input.placement_policy},
          page_mode_{input.page_mode}, numa_targets_{std::move(input.numa_targets)},
          placement_threshold_{input.placement_threshold}, prefault_{input.prefault},
          verify_before_{input.verify_before}, verify_after_{input.verify_after} {}

    core::WorkerId worker_id_;
    core::ByteCount requested_bytes_;
    core::ByteCount page_size_;
    PlacementPolicy placement_policy_;
    PageMode page_mode_;
    std::vector<RegionNumaTarget> numa_targets_;
    ExactRatio placement_threshold_;
    bool prefault_;
    bool verify_before_;
    bool verify_after_;
};

} // namespace epycbench::memory
