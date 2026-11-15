#pragma once

#include <string_view>

namespace epycbench::cli::demo_scenario {

// Fixed demonstration settings. Resolve their availability; never replace CPU or
// NUMA selections based on the host. The launcher supplies only the output path.
inline constexpr std::string_view source = R"yaml(schema: epycbench.scenario/v1
metadata:
  id: demo
  name: Fixed bandwidth and latency demonstration
random:
  seed: 918273645
requirements:
  operating_system: linux
  numa:
    minimum_cpu_nodes: 1
    minimum_memory_nodes: 2
resources:
  worker_sets:
    demo-worker:
      pus: [0]
  service_cpus:
    monitoring-service:
      pus: [15]
workloads:
  sequential-read:
    kernel: memory.bandwidth.read/v1
    parameters:
      working_set: 64 MiB
  dependent-read:
    kernel: memory.latency.dependent-load/v1
    parameters:
      working_set: 8 MiB
      chain_seed: 12345
experiments:
  - id: bandwidth-demo
    type: comparison
    workload: sequential-read
    workers: demo-worker
    variants:
      - id: node0
        memory:
          policy: bind
          nodes: [0]
      - id: node1
        memory:
          policy: bind
          nodes: [1]
    compare:
      baseline: node0
      candidates: [node1]
      practical_effect: 1%
      confidence: 95%
      order:
        strategy: randomized-abba
  - id: latency-demo
    type: comparison
    workload: dependent-read
    workers: demo-worker
    variants:
      - id: node0
        memory:
          policy: bind
          nodes: [0]
      - id: node1
        memory:
          policy: bind
          nodes: [1]
    compare:
      baseline: node0
      candidates: [node1]
      practical_effect: 1%
      confidence: 95%
      order:
        strategy: randomized-abba
measurement:
  clock: monotonic-raw
  calibration:
    enabled: true
    strategy: geometric-refine
  observation:
    target_duration: 100 ms
    minimum_duration: 50 ms
    maximum_duration: 200 ms
    maximum_harness_overhead: 1%
  warmup:
    minimum: 2
    maximum: 4
    stability_window: 2
    stability_threshold: 10%
  repetition:
    minimum_blocks: 4
    maximum_blocks: 4
    minimum_valid_observations_per_variant: 8
  stopping:
    enabled: false
    confidence: 95%
    ratio_interval_relative_half_width: 2%
  statistics:
    bootstrap_samples: 1000
    practical_effect: 1%
  outliers:
    flagging: median-absolute-deviation
    automatic_exclusion: false
monitoring:
  required: [page_faults]
  optional: []
  sampling:
    before_observation: true
    after_observation: true
    during_observation:
      enabled: false
  unavailable_optional_source: warn
)yaml";

} // namespace epycbench::cli::demo_scenario
