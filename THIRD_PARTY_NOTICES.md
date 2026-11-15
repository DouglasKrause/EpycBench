# Third-party dependencies and algorithm attribution

EpycBench's project license does not replace third-party licenses. Preserve the
applicable upstream copyright notices, license texts and disclaimers when
redistributing third-party material.

## Configured dependencies

`cmake/EpycBenchDependencies.cmake` imports these dependencies from the build
environment. The versions below identify the existing release build's dependency
records; they are not new minimum-version requirements. Links point to the
corresponding upstream version's license or licensing statement.

| Dependency | Recorded version | Upstream license |
| --- | --- | --- |
| hwloc | 2.14.0 | [BSD-3-Clause](https://github.com/open-mpi/hwloc/blob/hwloc-2.14.0/COPYING) |
| libnuma | 2.0.19 | [GNU LGPL v2.1](https://github.com/numactl/numactl/blob/v2.0.19/README.md#license-copyrights-acknowledgements) |
| libfyaml | 0.9.6 | [MIT](https://github.com/pantoniou/libfyaml/blob/v0.9.6/LICENSE) |
| CLI11 | 2.7.2 | [BSD-3-Clause](https://github.com/CLIUtils/CLI11/blob/v2.7.2/LICENSE) |
| nlohmann/json | 3.12.0 | [MIT](https://github.com/nlohmann/json/blob/v3.12.0/LICENSE.MIT) |
| fmt | 12.2.0 | [MIT](https://github.com/fmtlib/fmt/blob/12.2.0/LICENSE) |
| Catch2 | 3.16.0 | [BSL-1.0](https://github.com/catchorg/Catch2/blob/v3.16.0/LICENSE.txt) |

libnuma is the linked library; upstream's separate `numactl` executable and demo
programs have GPL v2 terms. Do not substitute those executable licenses for the
library's license.

This inventory accompanies the project source. It does not constitute the full
notice/source-compliance material for a binary distribution. Any binary or
bundled-dependency release must retain the notices and satisfy the terms for the
actual included dependency versions and linking arrangement, including libnuma.

## SplitMix algorithm attribution

The fixed-increment SplitMix64 word sequence used by Statistics and its
independent oracle corresponds to Sebastiano Vigna's 2015
[reference implementation](https://prng.di.unimi.it/splitmix64.c). That reference
dedicates rights to the public domain to the extent possible under law, and
includes permission to use, copy, modify and distribute with a warranty disclaimer.
The attribution does not change EpycBench's fixed sequence or qualification
requirements. No new third-party implementation is vendored by this notice.
