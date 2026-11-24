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

Package discovery and binary loading optionally use the system OpenSSL 3
libcrypto for SHA-256. Builds without it use EpycBench's portable SHA-256.
[OpenSSL license](https://github.com/openssl/openssl/blob/openssl-3.6.5/LICENSE.txt)

libnuma is the linked library; upstream's separate `numactl` executable and demo
programs have GPL v2 terms. Do not substitute those executable licenses for the
library's license.

This inventory accompanies the project source. It does not constitute the full
notice/source-compliance material for a binary distribution. Any binary or
bundled-dependency release must retain the notices and satisfy the terms for the
actual included dependency versions and linking arrangement, including libnuma.

## FTXUI

The interactive workbench links FTXUI 6.1.9. CMake uses an installed copy of
that version or fetches the exact source archive by SHA-256. Offline builders
can set `FETCHCONTENT_SOURCE_DIR_FTXUI` to the extracted source directory.
[FTXUI source and license](https://github.com/ArthurSonzogni/FTXUI/tree/5cfed50702f52d51c1b189b5f97f8beaf5eaa2a6)

```text
The MIT License

Copyright (c) 2019 Arthur Sonzogni.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## SplitMix algorithm attribution

The fixed-increment SplitMix64 word sequence used by Statistics and its
independent oracle corresponds to Sebastiano Vigna's 2015
[reference implementation](https://prng.di.unimi.it/splitmix64.c). That reference
dedicates rights to the public domain to the extent possible under law, and
includes permission to use, copy, modify and distribute with a warranty disclaimer.
The attribution does not change EpycBench's fixed sequence or qualification
requirements. No new third-party implementation is vendored by this notice.
