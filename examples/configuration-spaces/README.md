# Configuration space examples

[Sum](sum.yaml) varies candidate worker count, implementation, and global
input size. [XOR](xor.yaml) varies the common input size and mask. [Copy](copy.yaml)
holds the reference placement and task inputs fixed while varying the candidate
CPU. [Genetic Sum](genetic-sum.yaml) varies the candidate worker and implementation
choices at one fixed workload.
[Generated Sum](generated-sum.yaml) compares each compiled accumulator recipe
against a common scalar control. [Generated Sum compiler options](generated-sum-compiler-options.yaml)
keeps the recipe fixed while varying the compiler's loop-alignment flag. Both
recipes use the corresponding opt-in generated package bundle.
[Latency working sets](latency-working-set.yaml) varies the common dependent-load
input between 8 KiB and 128 MiB at fixed worker and memory placement.

```sh
epycbench scenario preview --topology topology.json --limit 8 sum.yaml
```

Package recipes use `epycbench.configuration-space/v3`. Each axis names a setting
in the embedded package Scenario using a JSON Pointer. Edit Linux CPU and NUMA
node selections to the intended topology. Calibration, warmup, monitoring,
precision stopping and the authored order seed remain fixed. Only a base
Scenario with streaming enabled permits a pipeline chunk-count axis. The
[matched-page Gather recipe](packages/gather-matched-pages.yaml) is a package
example. Use the recorded source revision and your own study paths when opening
frozen studies whose recipes use earlier schemas.
