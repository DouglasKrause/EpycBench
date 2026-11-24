# Automatic kernel checks

Build with either release preset, then run your Scenario. No human or agent review,
second compiler, approval files, evidence directory or clean Git checkout is
required. The entire program and its measurement routines are compiled from source.

The build runs `tools/check-kernel-code.py` against each compiled kernel module's
declared timed function. It uses
GNU Binutils (`readelf`, `objdump`, `c++filt`) to inspect the instructions without
running a benchmark. It looks for repeated reads in bandwidth/Sum, repeated
reads and writes in XOR, and the read-to-read dependency needed by latency.

Only a recognized, fundamental failure blocks the build, such as clearly missing
repeated memory work or a simple latency loop whose reads are independent.
Different register choices, loop arrangements and small inefficiencies are allowed.
Unrecognized instruction shapes or unavailable inspection tools produce a warning
and allow the build to continue. There is no review prompt or approval procedure.

Reports under `build/<preset>/kernel-packages/` include the timed bandwidth,
latency, Sum and XOR implementation checks. They record the module's file hash, the routine's
check result and, when available, its compiled-function hash. This is a limited sanity
check, not proof of identical performance across compilers. The report stays in the
build directory; Run does not ask you to import it or manufacture an approval.

Untimed checks validate returned answers and work accounting. XOR additionally
checks every output word, its digest and padding after kernel timing and
observation monitoring close. Results retain module identity, file hash, compiler and build information.
Preserve those details
when comparing measurements: passing these basic checks does not establish
repeatability or make different builds scientifically interchangeable.

Fixed-problem multicore Sum uses the collective Sum task package. Its host execution profile
includes synchronization and partial-sum combination on every round; module
checks alone do not qualify that orchestration. Results retain the collective
timing scope, balanced input partitions, independent per-rank/global checksum
checks, completed rounds and scratch-padding verification. Whole-task throughput
uses the collective interval, while local worker intervals remain diagnostics.
These checks do not establish core/SMT/NUMA scaling or physical unit utilization
on untested hardware.

Build the [standalone contributor example](../modules/package-example/README.md)
and run `epycbench kernel validate --module-dir example-build/package` to
check its manifests and module interfaces. A Run verifies the work against
the independent task oracle. To inspect a compiled routine, use a tool such
as `objdump -d -C build/release/kernel-packages/bandwidth-read.so`.
