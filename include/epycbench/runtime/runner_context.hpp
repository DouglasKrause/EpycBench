#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/topology/fingerprint.hpp"
#include "epycbench/topology/model.hpp"
#include "epycbench/topology/stable_id.hpp"

#include <expected>
#include <utility>

namespace epycbench::runtime {

namespace detail {
struct RunnerContextFactory;
}

class RunnerContext {
  public:
    [[nodiscard]] static auto create(topology::TopologySnapshot topology) noexcept
        -> std::expected<RunnerContext, core::Error>;

    [[nodiscard]] auto topology() const noexcept -> const topology::TopologySnapshot& {
        return topology_;
    }
    [[nodiscard]] auto stable_ids() const noexcept -> const topology::StableTopologyIds& {
        return stable_ids_;
    }
    [[nodiscard]] auto fingerprints() const noexcept -> const topology::TopologyFingerprints& {
        return fingerprints_;
    }

  private:
    RunnerContext(topology::TopologySnapshot topology, topology::StableTopologyIds stable_ids,
                  topology::TopologyFingerprints fingerprints)
        : topology_{std::move(topology)}, stable_ids_{std::move(stable_ids)},
          fingerprints_{std::move(fingerprints)} {}

    topology::TopologySnapshot topology_;
    topology::StableTopologyIds stable_ids_;
    topology::TopologyFingerprints fingerprints_;

    friend struct detail::RunnerContextFactory;
};

struct RunnerContextProvider {
    void* context;
    std::expected<RunnerContext, core::Error> (*acquire)(void*) noexcept;
};

[[nodiscard]] auto production_runner_context_provider() noexcept -> RunnerContextProvider;

} // namespace epycbench::runtime
