# Qualify kernels from your own build

`epycbench run` needs reviewed object-code evidence for both canonical kernels.
Building the CLI or passing an automated object audit does not supply semantic
approval. This guide creates evidence from your own GCC and Clang release builds;
it does not depend on the original project's private qualification files.

The default manifest hashes name historical independent-agent qualifications.
Keep those defaults unchanged. Supply your own reviewed hashes and the matching
review route through the explicit CMake options below.

## 1. Generate four pending report pairs

Use the dependencies and build instructions in the top-level README. You also
need Python 3, Git, and GNU Binutils (`readelf`, `objdump`, `c++filt`). The audits
inspect built ELF objects; they perform no benchmark or hardware qualification.
Use an x86-64 Linux build host, the canonical presets, and your actual compilers.
Other compiler versions may produce code that fails the prescribed audit.

Work in a clean Git checkout. An exported directory must first be initialized
and committed as a repository using your normal Git identity. Generated `build/`
and `.local/` files must stay ignored. Record `git rev-parse HEAD`; all four JSON
reports must identify that same commit with `source.dirty` equal to `false`.
Do not edit tracked source or change compiler/optimization options during review.

From the source root:

```sh
set -eu
cmake --preset release -DBUILD_TESTING=OFF
cmake --build --preset release
cmake --preset release-clang -DBUILD_TESTING=OFF
cmake --build --preset release-clang
for PRESET in release release-clang; do
  case "$PRESET" in
    release) COMPILER_FAMILY=GCC ;;
    release-clang) COMPILER_FAMILY=Clang ;;
  esac
  python3 tools/run-bandwidth-object-audit.py \
    --source-root "$PWD" --build-directory "$PWD/build/$PRESET" \
    --compile-commands "$PWD/build/$PRESET/compile_commands.json" \
    --timed-source src/kernel/sequential_read_bandwidth_execute.cpp \
    --implementation-id scalar-u64-sequential/v1 \
    --semantic-symbol 'epycbench::kernel::execute_sequential_read_bandwidth(epycbench::kernel::PreparedSequentialReadBandwidth const&, epycbench::kernel::SequentialReadBandwidthPassCount)' \
    --compiler-family "$COMPILER_FAMILY" --qualification-preset "$PRESET" \
    --family-policy tools/kernel-object-audit/policies/bandwidth-v1.json \
    --machine-report "build/$PRESET/qualification/EB-KRN-BW-006/object-code-audit.json" \
    --human-report "build/$PRESET/qualification/EB-KRN-BW-006/object-code-audit.txt"
  python3 tools/run-latency-object-audit.py \
    --source-root "$PWD" --build-directory "$PWD/build/$PRESET" \
    --compile-commands "$PWD/build/$PRESET/compile_commands.json" \
    --timed-source src/kernel/dependent_load_latency_execute.cpp \
    --implementation-id scalar-u64-random-cycle/v1 \
    --semantic-symbol 'epycbench::kernel::execute_dependent_load_latency(epycbench::kernel::PreparedDependentLoadLatency const&, epycbench::kernel::DependentLoadLatencySchedule)' \
    --compiler-family "$COMPILER_FAMILY" --qualification-preset "$PRESET" \
    --family-policy tools/kernel-object-audit/policies/latency-v1.json \
    --machine-report "build/$PRESET/qualification/EB-KRN-LAT-007/object-code-audit.json" \
    --human-report "build/$PRESET/qualification/EB-KRN-LAT-007/object-code-audit.txt"
done
```

This generates one machine/readable report pair per kernel per compiler, without
the development test suite. Their paths are
`build/<preset>/qualification/<issue>/object-code-audit.json` and
`object-code-audit.txt`, where issues are `EB-KRN-BW-006` and `EB-KRN-LAT-007`.
Each JSON report must say `automated_disposition: "pass"`,
`qualification_status: "pending_human_review"`, and human review `status: "pending"`.
The two release audit adapters above return **0** for that pending state. The
underlying `tools/audit-kernel-object-code.py` returns **3** for the same state;
exits 1 and 2 are tooling and policy failures. Neither successful adapter exit
nor driver exit 3 is approval.

Prepare editable manifest templates and byte-for-byte copies of the reports:

```sh
umask 077
mkdir -p .local
python3 tools/prepare-kernel-evidence.py \
  --source-root "$PWD" \
  --gcc-build "$PWD/build/release" \
  --clang-build "$PWD/build/release-clang" \
  --output "$PWD/.local/kernel-review"
```

The destination must be new. Each family directory contains four original
reports and `approval.template.json`. Its `REVIEW_REQUIRED` fields are
deliberately invalid for production admission. The helper stages inputs only;
it never writes an approval, edits a report, or decides that a checklist item is
supported.

## 2. Obtain and record semantic review

Use the human-approval route for both families. Keep the original
machine/readable reports unchanged, including their pending status. Review the actual instructions,
control flow and register/data dependencies against every emitted checklist
item. Automated PASS is supporting evidence, not a substitute for this review.

### Human approval

A qualified human reviewer must support all **17 bandwidth items and 12 latency
items for each compiler**: 58 compiler/item assessments in total. The two templates
already contain the exact ordered item IDs, compiler labels, policy/source IDs,
object/function hashes, report paths and report hashes. Check those bindings
against the actual files and compiler output.

For each family, the reviewer makes a separate `approval.json` based on
`approval.template.json` only after completing the review:

- `approval.actor`: the actual human reviewer identity.
- `approval.recorded_at_utc`: actual whole-second UTC time, `YYYY-MM-DDTHH:MM:SSZ`.
- `approval.statement`: a substantive statement approving the exact two compiler
  artifacts after reviewing every listed requirement; retain detailed review
  notes with the reports.
- `approval.disposition`: `approved` only after that approval.
- Each ordered `approval.semantic_items[].disposition`: `supported` only when
  that item is supported for **both** compiler artifacts.

The manifest schema is
`schemas/kernel-object-qualification-approval-v1.schema.json`. Keep its closed
shape; do not add fields to the admitted manifest for your notes. Do not
mechanically replace placeholders without doing the review. If an item is
unsupported or unresolved, retain that finding and correct/rebuild/review the
affected artifacts before seeking approval. A pending or partial review cannot
be used for a Run. Use `EPYCBENCH_KERNEL_EVIDENCE_REVIEW_ROUTE=human_approval`.

### Existing independent-agent evidence

The compiled program also supports the existing `independent_agents` route via
`retain_agent`. That route requires two actual independent assessments, exact
frozen-input bindings and the versioned agent manifest; it is an advanced
alternative for evidence already qualified through that protocol. The portable
workflow here uses `human_approval`. Its template helper never creates agent
qualification or converts an agent identity into a human approval.

## 3. Retain exactly five files per family

After genuine approval/qualification, each family must have this content:

```text
approval.json
gcc/object-code-audit.json
gcc/object-code-audit.txt
clang/object-code-audit.json
clang/object-code-audit.txt
```

The manifest is still named `approval.json` for the agent route. Hash its exact
bytes without a `sha256:` prefix. Retain each group under that hash, using a new
absolute evidence root with no symlink components. Directories from that root
down must be owned by the invoking user with mode `0700`; files must be regular,
owned by that user, mode `0600`, and have one link. Use copies, not hard links.

For the human route after creating both reviewed `approval.json` files:

```sh
umask 077
mkdir -m 700 .local/kernel-evidence
BW_SHA=$(sha256sum .local/kernel-review/EB-KRN-BW-006/approval.json | cut -d ' ' -f 1)
LAT_SHA=$(sha256sum .local/kernel-review/EB-KRN-LAT-007/approval.json | cut -d ' ' -f 1)
for ISSUE in EB-KRN-BW-006 EB-KRN-LAT-007; do
  REVIEW_DIR="$PWD/.local/kernel-review/$ISSUE"
  MANIFEST_SHA=$(sha256sum "$REVIEW_DIR/approval.json" | cut -d ' ' -f 1)
  DEST="$PWD/.local/kernel-evidence/$MANIFEST_SHA"
  mkdir -m 700 "$DEST" "$DEST/gcc" "$DEST/clang"
  cp "$REVIEW_DIR/approval.json" "$DEST/approval.json"
  for COMPILER in gcc clang; do
    cp "$REVIEW_DIR/$COMPILER/object-code-audit.json" "$DEST/$COMPILER/"
    cp "$REVIEW_DIR/$COMPILER/object-code-audit.txt" "$DEST/$COMPILER/"
  done
done
```

Keep notes, inventories and rejected assessments separately. Do not overwrite
earlier reviewed groups or change an approved report/manifest in place. A change
creates a new identity and needs the affected review again.

## 4. Configure the reviewed evidence and rebuild

For the human route, using the shell values calculated above:

```sh
cmake --preset release \
  -DEPYCBENCH_KERNEL_EVIDENCE_ROOT="$PWD/.local/kernel-evidence" \
  -DEPYCBENCH_KERNEL_EVIDENCE_BANDWIDTH_SHA256="$BW_SHA" \
  -DEPYCBENCH_KERNEL_EVIDENCE_LATENCY_SHA256="$LAT_SHA" \
  -DEPYCBENCH_KERNEL_EVIDENCE_REVIEW_ROUTE=human_approval
cmake --build --preset release
```

Use the same four options with `release-clang` to configure that consumer, with
the same `human_approval` route. Both hashes must be exactly 64 lowercase
hexadecimal digits.
The program selects the configured route explicitly; it does not inspect bytes
to guess a route or fall back after rejected admission.

Preserve the source, compiler configuration, object/function and report bindings
when configuring the final consumer. Configuration changes the evidence locator
and expected digests; it does not grant qualification or prove arbitrary changed
kernel bytes equivalent. The normal Run path must still admit the exact reports,
manifest and current Build/object associations. Source/compiler/timed-code changes
require refreshed affected evidence. Never substitute an older manifest merely
because it was accepted on another machine.

## 5. Verify the actual linked consumer

Before benchmarking, verify that the final executable still contains the exact
two qualified functions. Run this after the rebuild above, using the original
reports from the same retained, reviewed groups:

```sh
python3 tools/verify-kernel-retention.py \
  --executable "$PWD/build/release/epycbench" --compiler-family GCC \
  --bandwidth-report "$PWD/.local/kernel-evidence/$BW_SHA/gcc/object-code-audit.json" \
  --latency-report "$PWD/.local/kernel-evidence/$LAT_SHA/gcc/object-code-audit.json" \
  --output "$PWD/.local/kernel-retention-gcc.json"
```

If you also configure/rebuild and use the Clang consumer, verify it separately:

```sh
python3 tools/verify-kernel-retention.py \
  --executable "$PWD/build/release-clang/epycbench" --compiler-family Clang \
  --bandwidth-report "$PWD/.local/kernel-evidence/$BW_SHA/clang/object-code-audit.json" \
  --latency-report "$PWD/.local/kernel-evidence/$LAT_SHA/clang/object-code-audit.json" \
  --output "$PWD/.local/kernel-retention-clang.json"
```

The helper runs only the consumer's `version --json` metadata command, checks
its compiler family, canonical preset and clean source identity against both reports,
and uses GNU `readelf`/`c++filt` plus the existing audit framework to resolve each
semantic function and extract its actual ELF bytes. Both function hashes must
match the compiler-matched reports exactly. Missing, ambiguous, altered or
unavailable function evidence refuses verification; stripped-away symbol
information is insufficient. A receipt binds the complete executable hash,
version metadata and exact report hashes. Consumer/report drift refuses, and an
existing receipt is preserved. Retain this private receipt with your evidence.

Successful comparison establishes linked retention only. It does not issue
semantic approval or hardware qualification. If a function changed, correct the
build or refresh the affected qualification; do not replace a reviewed digest
with an observed value merely to pass comparison.

You can now use the README's `epycbench run` workflow when your host satisfies
the requested CPU/NUMA/monitoring and resource requirements. Kernel qualification
alone does not establish topology correctness, Monitoring overhead, reproducible
measurements, hardware qualification, or release acceptance. A refused or failed
Run remains a refused or failed Run with its retained evidence.
