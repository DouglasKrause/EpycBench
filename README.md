# EpycBench

EpycBench is a Linux C++23 command-line framework for AMD EPYC topology discovery,
explicit CPU and NUMA memory placement, memory bandwidth and dependent-load
latency measurements, scalar Sum/XOR task comparisons, and raw statistical results.

This source distribution contains the application, kernel module sources and
interface header, build files, an automatic kernel checker, editable Scenarios
and user documentation. The development test
suite, internal project history and private qualification data are excluded.
Building this distribution creates a new source/compiler configuration; it does
not inherit another machine's qualification or performance conclusions.

The packaged workflow covers topology inspection, Scenario authoring,
sweeps, experiments, benchmark execution and offline Result analysis.

## Build

Provide a C++23 compiler with the library features checked by CMake, CMake 3.25 or
newer, Ninja, Git, Python 3.8 or newer, and development packages for hwloc,
libnuma, libfyaml 0.9.6 or newer, CLI11, nlohmann-json and fmt. CMake uses an
installed FTXUI 6.1.9 or fetches the pinned source archive. For an offline
build, supply the corresponding source directory through
`-DFETCHCONTENT_SOURCE_DIR_FTXUI=PATH`.

```sh
cmake --preset release
cmake --build --preset release --target epycbench --parallel
./build/release/epycbench version --json
```

Use `release-clang` for the Clang build. Canonical release presets avoid
`-march=native`. This distribution has no development CTests and refuses
`BUILD_TESTING=ON`. An archive without Git metadata records an unknown source
revision and a dirty source state; a Git checkout records its actual revision
and status. Preserve the generated version information with your results.

## Kernel packages

Keep `build/release/kernel-packages/` beside the executable. Each package has
a YAML task definition, an independent oracle, and one YAML manifest and shared
object per implementation. EpycBench discovers packages in that directory unless
`--module-dir PATH` selects another installation. Saved Result readers use the
retained manifests and binary digests.

The [editable Scenarios](examples/scenarios/README.md) cover
[scalar Sum](examples/scenarios/scalar-sum-u64.yaml),
[XOR](examples/scenarios/scalar-xor-u64.yaml),
[fixed-input multicore Sum](examples/scenarios/partitioned-sum-u64.yaml), and
[flat and hierarchical aggregation](examples/scenarios/packages/hierarchical-sum-u64.yaml).
Additional examples include [shared-state coordination](examples/scenarios/lock-counter.yaml),
[mixed workloads](examples/scenarios/packages/mixed-sum-gather.yaml), and
[a streaming Copy-to-XOR pipeline](examples/scenarios/packages/copy-xor-stream.yaml).
For NUMA memory policy comparisons, use [bind and interleave](examples/scenarios/bandwidth-bind-interleave.yaml),
[remote and local replicas](examples/scenarios/bandwidth-read-only-replica.yaml),
or [per-worker and per-node replicas](examples/scenarios/packages/bandwidth-replica-scope.yaml).
Select CPUs and NUMA nodes from the target topology. Each role's placement,
implementation, work and verification stay associated with its measured interval.

See the [standalone contributor example](modules/package-example/README.md) for
a package you can compile and install independently of the executable.

Run `epycbench result replay RESULT` to inspect saved observations and
`epycbench result export --profile summary RESULT` to read the comparison.
[Worker counter settings](examples/scenarios/packages/worker-hardware-counters.yaml)
request CPU cycles and retired instructions and preserve the original counts
or an explicit reason they were unavailable. `--package-energy 0` requests
raw energy readings for Linux CPU package 0, and `epycbench result energy RESULT`
reads the linked measurements offline. Package energy brackets the controller's
execution and includes concurrent package activity.

## Terminal workbench

Run `./build/release/epycbench` in an interactive terminal or add the
`tui` subcommand. Choose **New Scenario** and select the local machine
or an exported topology. Assign workloads and place worker CPUs and NUMA
memory using each role's **Placement** map. Select **Validate Scenario**,
then save to a new filename. **Resume Work** reopens drafts and saved
work. `epycbench tui PATH` opens a saved Result or study.

Use arrow keys and Enter or click to open rows. Drag items to highlighted
targets, or press Space to pick up an item and Space or Enter to drop it.
The same actions are available on narrow terminals. Press Escape to go back
and Ctrl+U to clear a text field. Worker placement shows the exact ordered
Linux CPU list before applying changes.

Create a sweep from the Scenario, preview candidates and use **Plan
experiment** to select a strategy and budget. Freeze the plan before
starting execution. The fullscreen interface suspends during each attempt
and returns at the attempt boundary. Press **P** to pause after the current
attempt or **Ctrl+C** to interrupt execution. **Open Analysis** shows the
saved trial evidence and Results.

## Inspect topology and resolve a Scenario

```sh
./build/release/epycbench topology export --live --output topology.json
./build/release/epycbench topology validate --input topology.json
./build/release/epycbench scenario resolve --topology topology.json examples/scenarios/scalar-sum-u64.yaml
```

Use `scenario preview` with the same topology option to inspect combinations of
task settings without running them. The [Sum space](examples/configuration-spaces/sum.yaml)
varies core count, implementation and input size. The
[XOR space](examples/configuration-spaces/xor.yaml) varies input size and mask.
The output includes each authored Scenario and either a resolved plan with
identities or diagnostics. Use `--limit` and `--offset` to page through a space.
Use the following commands to build a comparison, author a sweep and plan an experiment.

```sh
./build/release/epycbench scenario build --topology topology.json > sum.json
./build/release/epycbench scenario sweep --topology topology.json --scenario sum.json > recipe.json
mkdir -m 700 sum-study
./build/release/epycbench experiment plan --topology topology.json --directory sum-study \
  --strategy enumeration --seed 42 --budget 3 recipe.json
```

In the sweep step, choose the objective and the values to vary. The recipe
embeds the saved JSON or YAML Scenario and can be previewed after the source
moves. Use `--edit FILE` to revise a saved recipe. In the workbench,
**Save Scenario** saves bare filenames in
`$XDG_DATA_HOME/epycbench/scenarios` (default `~/.local/share/epycbench/scenarios`).
After saving, **Create a sweep from this Scenario** reuses the file and
topology. Use **Resume Work** to reopen a saved Scenario or recipe later.
Use the guided bandwidth matrix or cache-relative latency setup to save a
group of named package comparisons from the selected topology. The matrix
pairs local memory with each eligible remote NUMA node for a reader on each
eligible worker node. The latency guide uses the selected reader's cache
capacities for paired local and remote working sets.

The builder shows the dedicated controller core and its Linux CPU. Accept the
automatic choice or select a physical core by its topology ID. Workers and
dedicated services must fit on the remaining cores across all cases and
worker choices in a sweep. The experiment plan freezes the controller CPU
and checks it again on resume.

Calibration, warmup, monitoring, precision stopping and the authored order seed
stay fixed across a sweep. Only an authored streaming pipeline offers a
pipeline chunk-count axis.

Topology export refuses an existing destination unless `--force` is supplied.
Topology and resolved Scenario output describe your machine; review them before
sharing. The optional development reference fixture is excluded.

The [local and remote read example](examples/local-remote-read.yaml) selects
CPU 0 and memory nodes 0 and 1. Edit those IDs for your machine and check the
resolved Linux CPU, NUMA and memory geometry. A local and remote comparison
requires a suitable second memory node. Resolution performs no benchmark and
does not establish host readiness.

The opt-in generated candidates use the ordinary Sum task and oracle. Build
`epycbench_generated_sum_candidates` with the release preset to stage them
under `build/release/generated-sum/kernel-packages`. Pass that directory with
`--module-dir` when previewing the [generated Sum space](examples/configuration-spaces/generated-sum.yaml).
The implementation manifests retain the exact binary and compiler receipt digests.

## Run a benchmark

A settings filename is relative to the current directory: `demo.yaml` means
`demo.yaml` beside where you launched the command; use `settings/demo.yaml` only
when you have placed it in that subdirectory. Absolute paths also work.

The build automatically checks the compiled measurement routines for obvious
failures to perform their intended memory work. No assembly review, approval files
or second compiler are required. Ordinary compiler variations are accepted;
uncertain inspection results are reported without blocking the build. See
[automatic kernel checks](docs/QUALIFICATION.md) for the report and its limits.

Set the Scenario’s `output.directory` to an absolute path for this account.
The example uses `/tmp/epycbench-local-remote-results`. Check the authored
settings against saved topology, then create the owner-only Result directory.

```sh
./build/release/epycbench scenario admit --check-output --topology topology.json examples/local-remote-read.yaml
mkdir -p -m 700 /tmp/epycbench-local-remote-results
./build/release/epycbench run examples/local-remote-read.yaml
```

The command checks actual CPU/page placement and required monitoring. Missing
permissions, unsupported sources or failed admission remain failures. There is no
automatic host tuning or permission repair. A build or Kernel audit does not
establish repeatability, Monitoring overhead or a placement improvement on a new
machine; preserve raw evidence and state the conditions of any performance claim.

To run several resolved configurations in sequence, freeze their preview
ordinals in a private directory:

```sh
mkdir -m 700 sum-experiment
./build/release/epycbench experiment plan --topology topology.json --directory sum-experiment \
  --seed 42 --budget 3 --candidate 0 --candidate 4 --candidate 0 examples/configuration-spaces/sum.yaml
./build/release/epycbench experiment status sum-experiment
./build/release/epycbench experiment run sum-experiment
```

Each trial is charged before it starts, and each Run checks its resolved plan
before measurement. Resume with the same directory after an interruption.
It retains the attempt history and private Run evidence.

Use `--strategy enumeration` or `--strategy seeded-random` in place of the
`--candidate` options to plan an exploration. The plan records the source
space, proposal order and any rejected configurations.

After an exploration finishes, `experiment confirm --topology FILE --trial ORDINAL
--seed N EXPLORATION` can freeze a fresh comparison for one completed valid trial.
Run and inspect it with the same commands on the `confirmation/` subdirectory.
Keep that directory with its parent so the retained discovery evidence remains
available for replay.

Use `experiment analyze DIRECTORY` to inspect retained per-Run summaries,
worker counts, comparison intervals and descriptive distributions. Use
`--offset` and `--limit` to page through the trials.

## Validate and export results

```sh
./build/release/epycbench result validate RESULT.epycbench.json
./build/release/epycbench result replay RESULT.epycbench.json
./build/release/epycbench result index DIRECTORY
./build/release/epycbench result compare BEFORE AFTER
./build/release/epycbench result fingerprint RESULT.epycbench.json
./build/release/epycbench result export --profile complete RESULT.epycbench.json
./build/release/epycbench result export --profile shareable RESULT.epycbench.json
./build/release/epycbench result export --profile summary RESULT.epycbench.json
./build/release/epycbench result export --profile observation-table RESULT.epycbench.json
```

Validation checks canonical structure, integrity, associations and statistical
recalculation. Export validates its source first. In a terminal, all four
profiles save JSON in an `exports/` folder beside the original result and print
the full saved path. Repeated exports use numbered filenames. Summary export also
shows a compact performance overview; the other profiles show a save confirmation.
JSON is the default. Add `--format csv` to any profile to convert its JSON export
to CSV before saving:

```sh
./build/release/epycbench result export --profile summary --format csv RESULT.epycbench.json
```

CSV uses `path,type,value` columns, with one row per value (or empty container).
Paths identify nested fields and list indexes; no results are recalculated.
For a chosen destination, redirect as before, for example `> summary.csv`;
redirected exports contain only the selected format.

After a benchmark publishes its result, it automatically saves a summary export
and shows the performance summary after the completion/error information. The
view includes performance values, comparison differences, uncertainty, conclusions
and sample counts. Guided memory cases publish individual Results and journals.
Mixed workload replay includes each role's matched solo and concurrent
observations, overlap and eligibility for slowdown. Cross-build Result
comparisons describe the observed difference and identify both Builds.
For streaming pipelines, replay exposes the reference and observed SHA-256 of
the ordered bytes passed between stages. Chunk counts can be compared when
the external inputs, handoff, verified work and logical output counts match.
Reduced profiles remain linkable and are not anonymous.
Treat native Results and journals as private machine data;
review any exported artifact before sharing.

`SOURCE-MANIFEST.json` records the distributed relative filenames, sizes and
SHA-256 hashes. It is an inventory, not a software or hardware qualification.

EpycBench is licensed under the [MIT License](LICENSE), copyright (c) 2026 Douglas
Krause. Preserve the [third-party notices](THIRD_PARTY_NOTICES.md); dependencies
retain their own terms.
