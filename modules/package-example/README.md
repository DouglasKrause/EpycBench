# Standalone kernel package example

This example implements a deterministic byte transform with scalar and blocked
variants. Both use the same task oracle. The blocked variant declares its
`block_size` tuning in YAML. From the EpycBench source root, run

```sh
cmake -S modules/package-example -B example-build \
    -DEPYCBENCH_INCLUDE_DIR="$PWD/include"
cmake --build example-build
epycbench kernel list --module-dir example-build/package
epycbench kernel validate --module-dir example-build/package
```

If you're using the installed example, copy the directory
`share/epycbench/examples/kernel-package` to your workspace. From that copy,
run `cmake -S . -B example-build -DEPYCBENCH_INCLUDE_DIR=/path/to/prefix/include`,
substituting the EpycBench installation prefix. The remaining commands are
the same.

Keep the YAML files and shared objects together. Use `scenario build` with
the same `--module-dir` and a topology file to select the workload and its
implementations. The public header `epycbench/kernel/package.h`
defines how the host passes named parameters, tuning and borrowed buffer views.
