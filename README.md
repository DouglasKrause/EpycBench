# EpycBench

EpycBench is a Linux C++23 command-line framework for AMD EPYC topology discovery,
explicit CPU and NUMA memory placement, memory bandwidth and dependent-load
latency measurements, and raw statistical results.

This source distribution contains the application, build files, an automatic
kernel checker, one editable Scenario and user documentation. The development test
suite, internal project history and private qualification data are excluded.
Building this distribution creates a new source/compiler configuration; it does
not inherit another machine's qualification or performance conclusions.

The packaged workflows are topology inspection, Scenario resolution, benchmark
execution and Results validation/export. Controlled host campaign entry points
require separate operational tools, configuration, history and qualification
inputs; those workflows are outside this distribution.

## Build

Provide a C++23 compiler with the library features checked by CMake, CMake 3.25 or
newer, Ninja, Git, Python 3.8 or newer, and development packages for hwloc,
libnuma, libfyaml 0.9.6 or newer, CLI11, nlohmann-json and fmt. Dependencies are
located from the configured system; the build does not download them.

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

## Start menu and demo

Run `./build/release/epycbench` without arguments to choose **1) Help**, **2) Demo test run**, or
**3) Exit**. Help and the demo return to the menu; end-of-input also exits.

The built-in demo uses fixed settings: worker CPU 0, service CPU 15 on a separate
core, and memory nodes 0 and 1. It runs a 64 MiB bandwidth comparison and an 8 MiB
latency comparison, each with four blocks and 100 ms target observations. These
resources must be available; the program does not choose substitutes. Setup,
calibration and warmup add to the measured work, so this is not a wall-time limit.

Results are saved in the private `epycbench-demo-results` folder in the current
working directory, with journals in its `epycbench` subfolder. The demo creates
these directories as needed and needs no Scenario file or XDG directory setup.
Normal placement, monitoring and result checks still apply. The small preset is
for demonstration, not a hardware qualification.

## Inspect topology and resolve a Scenario

```sh
./build/release/epycbench topology export --live --output topology.json
./build/release/epycbench topology validate --input topology.json
./build/release/epycbench scenario resolve --topology topology.json examples/local-remote-read.yaml
```

Topology export refuses an existing destination unless `--force` is supplied.
Topology and resolved Scenario output describe your machine; review them before
sharing. The optional development reference fixture is excluded.

The example chooses one effective worker core and a separate service core through
selectors. Inspect the resolved Linux CPU and NUMA identities, then edit the
candidate memory node and selections for your machine. The candidate node in the
example is an editable value, not an assertion about your topology. A local/remote
comparison requires a suitable second memory node; no fixed remote penalty is
assumed. Resolution performs no benchmark and does not establish host readiness.

## Run a benchmark

The build automatically checks the compiled measurement routines for obvious
failures to perform their intended memory work. No assembly review, approval files
or second compiler are required. Ordinary compiler variations are accepted;
uncertain inspection results are reported without blocking the build. See
[automatic kernel checks](docs/QUALIFICATION.md) for the report and its limits.

Run the edited Scenario:

```sh
./build/release/epycbench run examples/local-remote-read.yaml
```

The command checks actual CPU/page placement and required monitoring. Missing
permissions, unsupported sources or failed admission remain failures. There is no
automatic host tuning or permission repair. A build or Kernel audit does not
establish repeatability, Monitoring overhead or a placement improvement on a new
machine; preserve raw evidence and state the conditions of any performance claim.

## Validate and export results

```sh
./build/release/epycbench result validate RESULT.epycbench.json
./build/release/epycbench result export --profile complete RESULT.epycbench.json
./build/release/epycbench result export --profile shareable RESULT.epycbench.json
./build/release/epycbench result export --profile summary RESULT.epycbench.json
./build/release/epycbench result export --profile observation-table RESULT.epycbench.json
```

Validation checks canonical structure, integrity, associations and statistical
recalculation. Export validates its source first. Reduced profiles remain linkable
and are not anonymous. Treat native Results and journals as private machine data;
review any exported artifact before sharing.

`SOURCE-MANIFEST.json` records the distributed relative filenames, sizes and
SHA-256 hashes. It is an inventory, not a software or hardware qualification.

EpycBench is licensed under the [MIT License](LICENSE), copyright (c) 2026 Douglas
Krause. Preserve the [third-party notices](THIRD_PARTY_NOTICES.md); dependencies
retain their own terms.
