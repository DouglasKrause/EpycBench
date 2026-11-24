#!/usr/bin/env python3
"""Catch obvious loss of benchmark work in the linked routines, without review."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / "kernel-object-audit"))
from kernel_object_audit.elf import (
    AuditError, CommandResult, REGISTER_RE, SYMBOL_RE, checked_run,
    demangle_symbols, extract_function_bytes, parse_disassembly, parse_sections,
    parse_symbols, select_symbol,
)


FUNCTIONS = {
    "bandwidth": "epycbench::kernel::execute_sequential_read_bandwidth("
                 "epycbench::kernel::PreparedSequentialReadBandwidth const&, "
                 "epycbench::kernel::SequentialReadBandwidthPassCount)",
    "latency": "epycbench::kernel::execute_dependent_load_latency("
               "epycbench::kernel::PreparedDependentLoadLatency const&, "
               "epycbench::kernel::DependentLoadLatencySchedule)",
}


class Runner:
    def run(self, arguments, cwd=None):
        result = subprocess.run(arguments, cwd=cwd, capture_output=True, text=True,
                                check=False, timeout=30)
        return CommandResult(result.returncode, result.stdout, result.stderr)


def register(name):
    """Use the same name for a general register and its common partial forms."""
    aliases = {"eax": "rax", "ebx": "rbx", "ecx": "rcx", "edx": "rdx",
               "esi": "rsi", "edi": "rdi", "esp": "rsp", "ebp": "rbp"}
    if re.fullmatch(r"r(?:[89]|1[0-5])d", name):
        return name[:-1]
    return aliases.get(name, name)


def memory_reads(instructions):
    return [item for item in instructions
            if item.mnemonic not in ("lea", "nop") and not item.mnemonic.startswith("prefetch")
            and any(operand.access_role in ("read", "read_write")
                    and not operand.stack_derived for operand in item.memory_operands)]


def simple_flow(instructions):
    """Only use a negative dependency conclusion for elementary register loops."""
    known = {"mov", "movzx", "movsxd", "lea", "add", "sub", "inc", "dec",
             "shl", "shr", "sal", "sar", "imul", "and", "or", "xor",
             "cmp", "test", "nop"}
    return all((item.mnemonic in known or item.mnemonic.startswith("j"))
               and not any(operand.stack_derived or operand.access_role == "unknown"
                           for operand in item.memory_operands)
               and not any(name.startswith(("xmm", "ymm", "zmm"))
                           for name in REGISTER_RE.findall(item.operands))
               for item in instructions)


def dependent_load(loop):
    """Look for a loaded value feeding an address on the next trip round a loop."""
    for load in memory_reads(loop):
        if not load.mnemonic.startswith("mov") or not load.register_def_candidates:
            continue
        live = {register(load.register_def_candidates[0])}
        position = loop.index(load)
        for item in loop[position + 1:] + loop[:position]:
            if item.mnemonic.startswith("j") or item.mnemonic in ("cmp", "test", "nop"):
                continue
            definitions = {register(value) for value in item.register_def_candidates}
            uses = {register(value) for value in item.register_use_candidates}
            # A register cleared with XOR/SUB no longer depends on its old value.
            operands = [value.strip() for value in item.operands.split(",")]
            zeroed = item.mnemonic in ("xor", "sub") and len(operands) == 2 and operands[0] == operands[1]
            propagated = bool(live & uses) and not zeroed
            live -= definitions
            if propagated:
                live |= definitions
        addresses = {register(value) for operand in load.memory_operands
                     for value in (operand.base, operand.index) if value}
        if live & addresses:
            return True
    return False


def assess(family, instructions):
    """Fail clear defects; unfamiliar compiler shapes remain nonblocking warnings."""
    if family == "allocation-mapping":
        if any(item.mnemonic.startswith("call") or
               (item.mnemonic.startswith("jmp") and item.branch_target is None)
               for item in instructions):
            return "pass", "The adapter transfers to the supplied anonymous mapping service."
        return "fail", "The adapter contains no transfer to the mapping service."
    start = instructions[0].address
    stop = instructions[-1].address + len(instructions[-1].raw_bytes.split())
    # Helpers, indirect control flow and special memory instructions may perform
    # the work elsewhere. This small check deliberately does not prove them.
    if any(item.mnemonic.startswith(("call", "rep", "loop"))
           or (item.mnemonic.startswith("j") and
               (item.branch_target is None or not start <= item.branch_target < stop))
           for item in instructions):
        return "warning", "Work may use a helper or unfamiliar control flow; no basic failure established."
    loops = [[item for item in instructions if branch.branch_target <= item.address <= branch.address]
             for branch in instructions if branch.back_edge]
    if family == "first-touch":
        if any(operand.access_role in ("write", "read_write") and not operand.stack_derived
               for loop in loops for item in loop for operand in item.memory_operands):
            return "pass", "Repeated writes to the supplied range are present."
        return "fail", "No repeated output writes remain in the routine."
    if not loops:
        return "fail", "No repeated memory-reading work remains in the routine."
    reading_loops = [loop for loop in loops if memory_reads(loop)]
    if not reading_loops:
        # Unknown memory forms and stack spills can hide a legitimate read.
        if any(item.memory_operands or item.mnemonic.startswith(("lods", "movs", "scas"))
               for loop in loops for item in loop if item.mnemonic not in ("lea", "nop")):
            return "warning", "Repeated memory work uses an unrecognized form; no basic failure established."
        return "fail", "The repeated work contains no memory reads."
    if family in ("bandwidth", "task-sum-u64", "task-dot-f64", "task-max-u64", "task-min-u64",
                  "task-norm-l1-f64", "task-sum-f64", "task-varint14-u64", "task-gather-u64"):
        return "pass", "Repeated memory reads are present."
    if family in ("task-map-xor-u64", "task-map-affine-f64", "task-copy-bytes", "task-gemm-u64",
                  "task-compact-u64", "task-prefix-scan-u64", "task-merge-u64", "task-transpose-u64", "task-stencil-u64", "task-convolution-u64", "task-histogram-u64", "task-stable-key-sort-u64", "task-search-tree-u64", "task-scatter-u64", "task-csr-u64", "task-bfs-u64", "task-pack-u4", "task-serialize-u64", "task-shared-state"):
        if any(operand.access_role in ("write", "read_write") and not operand.stack_derived
               for loop in reading_loops for item in loop for operand in item.memory_operands):
            return "pass", "Repeated memory reads and writes are present."
        if all(simple_flow(loop) for loop in reading_loops):
            return "fail", "The memory-reading loops contain no output writes."
        return "warning", "Output writes use an unrecognized form; no basic failure established."
    if any(dependent_load(loop) for loop in reading_loops if simple_flow(loop)):
        return "pass", "Repeated memory reads retain a load-to-next-address dependency."
    if all(simple_flow(loop) for loop in reading_loops):
        return "fail", "The memory-reading loop no longer uses loaded values to choose its next address."
    return "warning", "The dependency uses an unrecognized form; no basic failure established."


def inspect(executable, runner=None, functions=None):
    runner = runner or Runner()
    functions = FUNCTIONS if functions is None else functions
    sections = parse_sections(checked_run(runner, ["readelf", "--sections", "--wide", str(executable)]))
    symbol_text = checked_run(runner, ["readelf", "--symbols", "--wide", str(executable)])
    names = [match[6].split("@", 1)[0] for line in symbol_text.splitlines()
             if (match := SYMBOL_RE.match(line)) and
             (match[6].split("@", 1)[0] in functions.values() or any(method in match[6] for method in
                 ("execute_sequential_read_bandwidth", "execute_dependent_load_latency")))]
    # A symbol may appear in both the dynamic and regular symbol tables.
    symbols = list(dict.fromkeys(parse_symbols(symbol_text, demangle_symbols(runner, names, "c++filt"))))
    results = []
    for family, signature in functions.items():
        result = {"family": family, "symbol": signature}
        try:
            symbol, section = select_symbol(symbols, signature, sections)
            raw = extract_function_bytes(executable, symbol, section)
            listing = checked_run(runner, ["objdump", "-d", "--insn-width=16", "-M", "intel",
                f"--start-address={symbol.value}", f"--stop-address={symbol.value + symbol.size}",
                str(executable)])
            instructions = parse_disassembly(listing, symbol.value, symbol.value + symbol.size)
            result["function_sha256"] = hashlib.sha256(raw).hexdigest()
            result["status"], result["detail"] = assess(family, instructions)
        except (AuditError, OSError, subprocess.TimeoutExpired) as error:
            result.update(status="warning", detail=f"Inspection unavailable: {error}")
        results.append(result)
    return results


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--executable", type=Path)
    source.add_argument("--module", type=Path)
    parser.add_argument("--family", choices=("bandwidth", "latency", "first-touch", "allocation-mapping", "task-sum-u64", "task-map-xor-u64",
                                             "task-map-affine-f64", "task-dot-f64", "task-max-u64", "task-min-u64", "task-norm-l1-f64", "task-sum-f64",
                                             "task-copy-bytes", "task-chase4-u64", "task-varint14-u64", "task-gemm-u64",
                                             "task-gather-u64", "task-compact-u64", "task-prefix-scan-u64", "task-merge-u64", "task-transpose-u64", "task-stencil-u64", "task-convolution-u64", "task-histogram-u64", "task-stable-key-sort-u64", "task-search-tree-u64", "task-scatter-u64", "task-csr-u64", "task-bfs-u64", "task-pack-u4", "task-serialize-u64", "task-shared-state"))
    parser.add_argument("--symbol", default="epycbench_kernel_execute_v1")
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args(argv)
    if arguments.module and not arguments.family:
        parser.error("--module requires --family")
    if arguments.executable and arguments.family:
        parser.error("--family applies to --module")
    target = arguments.module or arguments.executable
    try:
        results = inspect(target, functions={arguments.family: arguments.symbol} if arguments.module else None)
    except (AuditError, OSError, subprocess.TimeoutExpired) as error:
        results = [{"status": "warning", "detail": f"Inspection unavailable: {error}"}]
    status = "fail" if any(result["status"] == "fail" for result in results) else (
        "warning" if any(result["status"] == "warning" for result in results) else "pass")
    report = {"schema": "epycbench.basic-kernel-check/v1", "status": status,
              "scope": "Basic compiled-work sanity checks; not exact instruction or performance equivalence.",
              "functions": results}
    if arguments.module:
        report["artifact"] = "kernel-module"
        try:
            report["module_sha256"] = hashlib.sha256(target.read_bytes()).hexdigest()
        except OSError:
            pass  # The inspection warning already records unavailable input.
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Kernel code check: {status} ({arguments.output})")
    for result in results:
        if result["status"] != "pass":
            print(f"  {result.get('family', 'inspection')}: {result['detail']}")
    return 2 if status == "fail" else 0


if __name__ == "__main__":
    raise SystemExit(main())
