# Automatic kernel checks

Build with either release preset, then run your Scenario. No human or agent review,
second compiler, approval files, evidence directory or clean Git checkout is
required. The entire program and its measurement routines are compiled from source.

The build runs `tools/check-kernel-code.py` against each compiled kernel module's
declared timed function. It uses
GNU Binutils (`readelf`, `objdump`, `c++filt`) to inspect the instructions without
running a benchmark. It looks for basic repeated memory-reading work and the
read-to-read dependency needed by the latency routine.

Only a recognized, fundamental failure blocks the build, such as clearly missing
repeated memory work or a simple latency loop whose reads are independent.
Different register choices, loop arrangements and small inefficiencies are allowed.
Unrecognized instruction shapes or unavailable inspection tools produce a warning
and allow the build to continue. There is no review prompt or approval procedure.

The reports are `build/<preset>/modules/bandwidth.kernel-check.json` and
`latency.kernel-check.json`. They record the module's file hash, the routine's
check result and, when available, its compiled-function hash. This is a limited sanity
check, not proof of identical performance across compilers. The report stays in the
build directory; Run does not ask you to import it or manufacture an approval.

During measurement, the existing checks still validate returned answers and work
accounting. Results retain module identity, file hash, compiler and build information.
Preserve those details
when comparing measurements: passing these basic checks does not establish
repeatability or make different builds scientifically interchangeable.

The [standalone contributor example](../modules/README.md) uses the same build-time
check when the checker is available. There is no startup Python or report-import
requirement. Experts can inspect the instructions themselves with ordinary tools
such as `objdump -d -C build/release/modules/bandwidth.so`; EpycBench needs no
additional review flag.
