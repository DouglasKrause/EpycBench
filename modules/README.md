# Kernel modules

The normal EpycBench build creates `modules/bandwidth.so` and `modules/latency.so`
beside the executable. These are compiled from the included source. Put additional
compatible `.so` files in that directory and restart EpycBench to discover them.
Use the module's kernel ID in the existing Scenario `kernel` field.
`--module-dir PATH` selects another module directory. Saved-result validation and
export do not load modules and work without that directory.

The initial interface supports the existing sequential read-bandwidth and
dependent-load-latency measurement contracts. A module supplies a unique kernel
ID, an implementation ID, version/build information, and stateless preparation,
execution, verification and cleanup functions through
[`plugin.h`](../include/epycbench/kernel/plugin.h). No internal EpycBench C++ class
crosses this boundary.

Kernel IDs are `memory.bandwidth.read/<name>/v1` or
`memory.latency.dependent-load/<name>/v1`. The two standard modules retain their
original IDs, `memory.bandwidth.read/v1` and `memory.latency.dependent-load/v1`.
Choose a contributor/project-specific name for additional modules.
Implementation IDs must also be unique within their measurement family, so the
recorded implementation always identifies one installed module.
Both IDs are at most 127 bytes, using ASCII letters, digits, `.`, `-`, `_` and `/`.
Implementation tokens `0`, `1`, `scalar_u64_sequential_v1` and
`scalar_u64_random_cycle_v1` are reserved for older saved records.
Version text is nonempty and at most 128 bytes; the combined compiler/build
description is nonempty and at most 4096 bytes.

EpycBench owns memory allocation, NUMA/CPU placement, deterministic initialization,
latency start scheduling, clocks, repetitions, monitoring, statistics and output.
Module loading, preparation and answer verification occur outside timed execution.
V1 modules use host-owned read-only buffers and no per-worker module state. Their
entry descriptor and strings must remain immutable, and callbacks must support
concurrent workers and must not throw exceptions. The module is held open until
all workers finish; its cleanup hook then runs before unloading.

For bandwidth, `work_units` counts complete passes through the region. Return the
unsigned 64-bit sum of all 64-bit words across those passes, with ordinary modulo
2^64 addition. For latency, each 64-byte node starts with the next node's unsigned
64-bit index. Starting at `start_index`, perform `work_units` dependent reads and
return the final index. The host retains the existing accounting and expected
answer rules. New measurement families and parameter models require a later
interface extension.

## Build an independent example

The example needs only the public interface header and a C++23 compiler:

```sh
cmake -S modules/example -B build/example-plugin -DCMAKE_BUILD_TYPE=Release
cmake --build build/example-plugin
cp build/example-plugin/example-bandwidth.so build/release/modules/
```

This does not rebuild `build/release/epycbench`. Restart EpycBench and select
`memory.bandwidth.read/example/v1`, using the same bandwidth settings as before.
For a separate source checkout, pass `-DEPYCBENCH_INCLUDE_DIR=/path/to/EpycBench/include`.
The module must target the same Linux/CPU environment as the running program.

The module descriptor names the actual timed entry symbol for automatic basic
inspection. The standard build and the example build run that check automatically
when the checker is available. For a module built separately with only the public
header, the same check can be added to its build command:

```sh
python3 tools/check-kernel-code.py --module /path/to/module.so --family bandwidth \
  --symbol epycbench_kernel_execute_v1 --output /path/to/module.kernel-check.json
```

Use `--family latency` for dependent-load modules. Unknown inspection shapes or
unavailable inspection tools remain nonblocking warnings; no human approval is
required. EpycBench records the module's file hash and build information so
saved results identify the code that ran. A shared-library file hash is identity
information, not an approval signature.
