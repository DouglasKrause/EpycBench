# Scenario examples

Start with [scalar Sum](scalar-sum-u64.yaml), [XOR](scalar-xor-u64.yaml), or
[fixed-input multicore Sum](partitioned-sum-u64.yaml). The examples select
Linux CPU and NUMA node IDs. Adapt those IDs to the effective topology, then
resolve the file against your topology artifact.

```sh
epycbench scenario resolve --topology topology.json scalar-sum-u64.yaml
```

Authored package Scenarios use `epycbench.scenario/packages-v1`. Workloads hold
task IDs and inputs. Roles choose implementations, tuning settings, and
placement. The [configuration space examples](../configuration-spaces/) show
how to sweep those selections.

[Hierarchical Sum](hierarchical-sum-u64.yaml) compares flat and topology-tree
aggregation. [SMT Sum](partitioned-sum-smt-numa.yaml) compares one worker with
two siblings. The companion [SMT placement comparison](packages/partitioned-sum-smt-remote.yaml)
holds those workers fixed and varies memory placement.

[Bind and interleave](bandwidth-bind-interleave.yaml) applies both policies to the
same two memory nodes. The [read-only replica example](bandwidth-read-only-replica.yaml)
compares remote and local copies of the same input. [Replica scope](packages/bandwidth-replica-scope.yaml)
uses two workers to compare one copy per worker with one per node.

The [packages directory](packages/) also includes a [mixed workload](packages/mixed-sum-gather.yaml),
a [sequential Copy-to-XOR pipeline](packages/copy-xor-pipeline.yaml) and a
[streaming Copy-to-XOR pipeline](packages/copy-xor-stream.yaml).
[First-touch](first-touch.yaml) and [allocation latency](allocation-latency.yaml)
use standalone mapping task packages. In the first-touch example, set the
`page_mode` task parameter to `base` or `hugetlb-2m`; HugeTLB mode requires a
pre-provisioned 2 MiB page pool.

For topology-specific bandwidth and latency comparisons, choose
**NUMA node-pair read bandwidth matrix** or **Dependent-load latency around
cache capacities** in the Scenario builder. Both save named comparison cases
as editable package Scenarios and produce one Result per case.

Set an absolute owner-only `output.directory` before running an example and
create the directory. For a recorded study, use the original settings cited
by its protocol.
