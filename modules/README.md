# Kernel modules

Current Runs discover kernel packages
in `kernel-packages/` beside the executable. Use `--module-dir PATH` to select an
independent installation. Each task has a manifest and an independent oracle.
Each implementation has its own manifest and timed callback. Use the
[package Scenarios](../examples/scenarios/packages) for bundled workloads.

Build and inspect the independent example with the public package ABI:

```sh
cmake -S modules/package-example -B build/example-package
cmake --build build/example-package
build/release/epycbench --module-dir build/example-package/package kernel validate
```

For a separate source checkout, set `EPYCBENCH_INCLUDE_DIR` to the directory
containing the public headers. See the
[standalone package example](package-example/README.md) for contributor guidance.

The task package owns input generation and independent reference verification.
The Runner owns placement, measurement and evidence capture. The installed public
interface is [package.h](../include/epycbench/kernel/package.h).
