"""Fail-closed Linux ELF x86-64 Kernel object audit primitives."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shlex
import subprocess
import sys
from dataclasses import asdict, dataclass, replace
from pathlib import Path
from types import MappingProxyType
from typing import Mapping, Protocol, Sequence

from . import FRAMEWORK_VERSION, REPORT_SCHEMA_VERSION


class AuditError(RuntimeError):
    """A required audit fact could not be established."""


@dataclass(frozen=True)
class CommandResult:
    returncode: int
    stdout: str
    stderr: str


class CommandRunner(Protocol):
    def run(self, arguments: Sequence[str], cwd: Path | None = None) -> CommandResult: ...


class SubprocessRunner:
    def run(self, arguments: Sequence[str], cwd: Path | None = None) -> CommandResult:
        completed = subprocess.run(
            list(arguments), cwd=cwd, check=False, capture_output=True, text=True
        )
        return CommandResult(completed.returncode, completed.stdout, completed.stderr)


def checked_run(runner: CommandRunner, arguments: Sequence[str], cwd: Path | None = None) -> str:
    result = runner.run(arguments, cwd)
    if result.returncode != 0:
        raise AuditError(f"tool failed: {Path(arguments[0]).name} (exit {result.returncode})")
    return result.stdout


def inside(root: Path, candidate: Path) -> Path:
    resolved_root = root.resolve(strict=False)
    resolved = candidate.resolve(strict=False)
    try:
        resolved.relative_to(resolved_root)
    except ValueError as error:
        raise AuditError("resolved path escapes declared root") from error
    return resolved


def normalize_path(path: Path, source_root: Path, build_root: Path) -> str:
    resolved = path.resolve(strict=False)
    for root, marker in ((source_root, "<SOURCE_ROOT>"), (build_root, "<BUILD_ROOT>")):
        try:
            relative = resolved.relative_to(root.resolve(strict=False))
            return marker if str(relative) == "." else f"{marker}/{relative.as_posix()}"
        except ValueError:
            pass
    return Path(path).name


def normalize_evidence_text(value: str, source_root: Path, build_root: Path) -> str:
    for root, marker in ((source_root, "<SOURCE_ROOT>"), (build_root, "<BUILD_ROOT>")):
        value = value.replace(str(root.resolve(strict=False)), marker)
    prefix_map = re.match(r"(-f(?:debug|file)-prefix-map=)([^=]+)(=.*)", value)
    if prefix_map and prefix_map[2].startswith("/"):
        return f"{prefix_map[1]}<ABSOLUTE_PATH>{prefix_map[3]}"
    for prefix in ("-I", "-L", "--sysroot="):
        if value.startswith(prefix) and value[len(prefix):].startswith("/"):
            return f"{prefix}<ABSOLUTE_PATH>"
    if value.startswith("/"):
        return "<ABSOLUTE_PATH>"
    return re.sub(r"(?:^|(?<=\s)|(?<==))/(?:[^\s,:=]+/)*[^\s,:=]+", "<ABSOLUTE_PATH>", value)


def normalize_arguments(arguments: Sequence[str], source_root: Path, build_root: Path) -> list[str]:
    normalized: list[str] = []
    for argument in arguments:
        normalized.append(normalize_evidence_text(argument, source_root, build_root))
    return normalized


@dataclass(frozen=True)
class CompileRecord:
    source: Path
    directory: Path
    arguments: tuple[str, ...]
    output: Path
    normalized_arguments: tuple[str, ...]


def load_compile_record(
    database_path: Path, source_root: Path, build_root: Path, timed_source: Path
) -> CompileRecord:
    try:
        raw = json.loads(database_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise AuditError("compile database is unavailable or malformed") from error
    if not isinstance(raw, list):
        raise AuditError("compile database root must be an array")
    expected = inside(source_root, source_root / timed_source)
    matches: list[tuple[dict[str, object], Path]] = []
    for entry in raw:
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            raise AuditError("compile database entry is malformed")
        directory = Path(str(entry.get("directory", build_root)))
        if not directory.is_absolute():
            directory = build_root / directory
        candidate = Path(entry["file"])
        if not candidate.is_absolute():
            candidate = directory / candidate
        if candidate.resolve(strict=False) == expected:
            matches.append((entry, directory.resolve(strict=False)))
    if len(matches) != 1:
        raise AuditError(f"expected one compile record, found {len(matches)}")
    entry, directory = matches[0]
    if isinstance(entry.get("arguments"), list) and all(
        isinstance(value, str) for value in entry["arguments"]
    ):
        arguments = list(entry["arguments"])
    elif isinstance(entry.get("command"), str):
        try:
            arguments = shlex.split(entry["command"], posix=True)
        except ValueError as error:
            raise AuditError("compile command cannot be tokenized") from error
    else:
        raise AuditError("compile record has neither arguments nor command")
    outputs: list[str] = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "-o":
            if index + 1 >= len(arguments):
                raise AuditError("compile output option is incomplete")
            outputs.append(arguments[index + 1])
            index += 2
            continue
        if argument.startswith("-o") and len(argument) > 2:
            outputs.append(argument[2:])
        index += 1
    if len(outputs) != 1:
        raise AuditError(f"expected one compile output, found {len(outputs)}")
    output = Path(outputs[0])
    if not output.is_absolute():
        output = directory / output
    output = inside(build_root, output)
    if output.suffix != ".o":
        raise AuditError("compile output is not an object-style artifact")
    return CompileRecord(
        expected,
        directory,
        tuple(arguments),
        output,
        tuple(normalize_arguments(arguments, source_root, build_root)),
    )


@dataclass(frozen=True)
class EffectiveFlags:
    optimization: str | None = None
    lto: bool | None = None
    loop_vectorization: bool | None = None
    slp_vectorization: bool | None = None
    unrolling: bool | None = None
    native_arch: bool = False
    native_tune: bool = False
    sanitizers: bool = False
    coverage: bool = False
    profiling: bool = False
    ambiguous: tuple[str, ...] = ()


@dataclass(frozen=True)
class FlagRequirements:
    optimization: str | None = None
    lto: bool | None = None
    loop_vectorization: bool | None = None
    slp_vectorization: bool | None = None
    unrolling: bool | None = None
    native_arch: bool | None = None
    native_tune: bool | None = None
    sanitizers: bool | None = None
    coverage: bool | None = None
    profiling: bool | None = None


def interpret_flags(arguments: Sequence[str]) -> EffectiveFlags:
    state: dict[str, object] = {
        "optimization": None, "lto": None, "loop_vectorization": None,
        "slp_vectorization": None, "unrolling": None, "native_arch": False,
        "native_tune": False, "sanitizers": False, "coverage": False,
        "profiling": False,
    }
    ambiguous: list[str] = []
    for arg in arguments:
        if re.fullmatch(r"-O(?:0|1|2|3|s|z|g|fast)", arg): state["optimization"] = arg
        elif arg == "-fno-lto": state["lto"] = False
        elif arg == "-flto" or arg.startswith("-flto="): state["lto"] = True
        elif arg in ("-fno-vectorize", "-fno-tree-loop-vectorize"): state["loop_vectorization"] = False
        elif arg in ("-fvectorize", "-ftree-loop-vectorize"): state["loop_vectorization"] = True
        elif arg in ("-fno-slp-vectorize", "-fno-tree-slp-vectorize"): state["slp_vectorization"] = False
        elif arg in ("-fslp-vectorize", "-ftree-slp-vectorize"): state["slp_vectorization"] = True
        elif arg == "-fno-unroll-loops": state["unrolling"] = False
        elif arg == "-funroll-loops": state["unrolling"] = True
        elif arg == "-march=native": state["native_arch"] = True
        elif arg == "-mtune=native": state["native_tune"] = True
        elif arg.startswith("-fsanitize="): state["sanitizers"] = True
        elif arg in ("--coverage", "-fprofile-arcs", "-ftest-coverage"): state["coverage"] = True
        elif arg.startswith(("-fprofile-", "-fprofile=")) or arg in ("-pg", "-finstrument-functions"):
            state["profiling"] = True
        elif arg.startswith(("-fno-profile-", "-fno-sanitize=")):
            ambiguous.append(arg)
    return EffectiveFlags(**state, ambiguous=tuple(ambiguous))


def evaluate_flag_requirements(
    flags: EffectiveFlags, requirements: FlagRequirements
) -> list[dict[str, object]]:
    findings: list[dict[str, object]] = []
    ambiguous = bool(flags.ambiguous)
    for name, expected in asdict(requirements).items():
        if expected is None:
            continue
        actual = getattr(flags, name)
        unresolved = actual is None or ambiguous
        findings.append({
            "id": f"qualification.flag.{name}",
            "status": "fail" if unresolved or actual != expected else "pass",
            "expected": expected,
            "actual": actual,
            "reason": "ambiguous" if ambiguous else ("unresolved" if actual is None else "evaluated"),
        })
    return findings


@dataclass(frozen=True)
class Section:
    index: int
    name: str
    address: int
    offset: int
    size: int


@dataclass(frozen=True)
class Symbol:
    mangled: str
    demangled: str
    binding: str
    symbol_type: str
    section_index: int
    value: int
    size: int


SECTION_RE = re.compile(
    r"^\s*\[\s*(\d+)\]\s+(\S+)\s+\S+\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\b"
)
SYMBOL_RE = re.compile(
    r"^\s*\d+:\s*([0-9a-fA-F]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+\S+\s+(\S+)\s+(.+?)\s*$"
)


def parse_sections(text: str) -> dict[int, Section]:
    sections: dict[int, Section] = {}
    for line in text.splitlines():
        match = SECTION_RE.match(line)
        if match:
            section = Section(int(match[1]), match[2], int(match[3], 16), int(match[4], 16), int(match[5], 16))
            sections[section.index] = section
    if not sections:
        raise AuditError("no ELF sections parsed")
    return sections


def parse_symbols(text: str, demangled: dict[str, str]) -> list[Symbol]:
    symbols: list[Symbol] = []
    for line in text.splitlines():
        match = SYMBOL_RE.match(line)
        if not match or not match[5].isdigit():
            continue
        name = match[6].split("@", 1)[0]
        symbols.append(Symbol(name, demangled.get(name, name), match[4], match[3], int(match[5]), int(match[1], 16), int(match[2])))
    return symbols


def select_symbol(symbols: Sequence[Symbol], semantic_signature: str, sections: dict[int, Section]) -> tuple[Symbol, Section]:
    candidates = [symbol for symbol in symbols if symbol.demangled == semantic_signature]
    if len(candidates) != 1:
        raise AuditError(f"expected one semantic symbol, found {len(candidates)}")
    symbol = candidates[0]
    if symbol.symbol_type != "FUNC" or symbol.binding not in ("GLOBAL", "WEAK"):
        raise AuditError("semantic symbol is not an externally defined STT_FUNC")
    if symbol.size <= 0:
        raise AuditError("semantic symbol has zero size")
    section = sections.get(symbol.section_index)
    if section is None:
        raise AuditError("semantic symbol section is unresolvable")
    start = symbol.value - section.address
    if start < 0 or start + symbol.size > section.size:
        raise AuditError("semantic symbol range is outside its section")
    for other in symbols:
        if other is symbol or other.symbol_type != "FUNC" or other.section_index != symbol.section_index or other.size <= 0:
            continue
        if max(symbol.value, other.value) < min(symbol.value + symbol.size, other.value + other.size):
            raise AuditError("semantic symbol range overlaps another symbol")
    return symbol, section


def extract_function_bytes(object_path: Path, symbol: Symbol, section: Section) -> bytes:
    start = section.offset + (symbol.value - section.address)
    try:
        with object_path.open("rb") as stream:
            stream.seek(start)
            data = stream.read(symbol.size)
    except OSError as error:
        raise AuditError("object bytes cannot be read") from error
    if len(data) != symbol.size:
        raise AuditError("function byte range is incomplete")
    return data


@dataclass(frozen=True)
class MemoryOperand:
    text: str
    base: str | None
    index: str | None
    scale: int | None
    displacement: str | None
    displacement_value: int | None
    displacement_resolved: bool
    width_bits: int | None
    width_bytes: int | None
    stack_derived: bool
    access_role: str


@dataclass(frozen=True)
class Instruction:
    address: int
    raw_bytes: str
    mnemonic: str
    operands: str
    memory_operands: tuple[MemoryOperand, ...] = ()
    branch_target: int | None = None
    conditional_branch: bool = False
    back_edge: bool = False
    register_def_candidates: tuple[str, ...] = ()
    register_use_candidates: tuple[str, ...] = ()


INSTRUCTION_RE = re.compile(r"^\s*([0-9a-fA-F]+):\s+((?:[0-9a-fA-F]{2}\s+)+)\s*([A-Za-z][A-Za-z0-9.]*)\s*(.*?)\s*$")
CONTINUATION_RE = re.compile(r"^\s*([0-9a-fA-F]+):\s+((?:[0-9a-fA-F]{2}(?:\s+|$))+)$")
ADDRESS_LINE_RE = re.compile(r"^\s*([0-9a-fA-F]+):")
MEMORY_RE = re.compile(r"\[([^\]]+)\]")
REGISTER_RE = re.compile(
    r"\b(?:r(?:ax|bx|cx|dx|si|di|sp|bp|ip|8|9|1[0-5])|"
    r"r(?:8|9|1[0-5])d|e(?:ax|bx|cx|dx|si|di|sp|bp|ip)|"
    r"(?:ax|bx|cx|dx|si|di|sp|bp)|[abcd][lh]|"
    r"(?:xmm|ymm|zmm)\d+|mm\d+)\b", re.I
)
WIDTH_QUALIFIERS = {
    "BYTE": 8, "WORD": 16, "DWORD": 32, "QWORD": 64,
    "XMMWORD": 128, "YMMWORD": 256, "ZMMWORD": 512,
}


def register_width(register: str) -> int | None:
    value = register.lower()
    if value.startswith("zmm"): return 512
    if value.startswith("ymm"): return 256
    if value.startswith("xmm"): return 128
    if value.startswith("mm"): return 64
    if value.startswith("r") and (value[1:] in ("ax", "bx", "cx", "dx", "si", "di", "sp", "bp", "ip") or value[1:].isdigit()): return 64
    if value.startswith("e") or value.endswith("d"): return 32
    if value in ("ax", "bx", "cx", "dx", "si", "di", "sp", "bp"): return 16
    if value in ("al", "ah", "bl", "bh", "cl", "ch", "dl", "dh"): return 8
    return None


def parse_displacement(expression: str) -> tuple[str | None, int | None, bool]:
    terms = re.findall(r"(?<!\*)([+-](?:0x[0-9a-fA-F]+|\d+))", expression)
    displacement = terms[-1] if terms else None
    value = int(displacement, 0) if displacement else 0
    remainder = REGISTER_RE.sub("", expression)
    remainder = re.sub(r"\*(?:1|2|4|8)", "", remainder)
    remainder = re.sub(r"[+-](?:0x[0-9a-fA-F]+|\d+)", "", remainder)
    remainder = remainder.replace("+", "").replace("-", "")
    return displacement, value if not remainder else None, not remainder


def parse_memory(text: str, mnemonic: str = "") -> tuple[MemoryOperand, ...]:
    result: list[MemoryOperand] = []
    for match in MEMORY_RE.finditer(text):
        expression = match[1].replace(" ", "")
        registers = REGISTER_RE.findall(expression)
        base = registers[0].lower() if registers else None
        index = registers[1].lower() if len(registers) > 1 else None
        scale_match = re.search(r"\*(1|2|4|8)", expression)
        displacement, displacement_value, displacement_resolved = parse_displacement(expression)
        prefix = text[:match.start()]
        qualifier = re.search(r"\b(BYTE|WORD|DWORD|QWORD|XMMWORD|YMMWORD|ZMMWORD)\s+PTR\s*$", prefix, re.I)
        width_bits = WIDTH_QUALIFIERS[qualifier[1].upper()] if qualifier else None
        if width_bits is None:
            outside_memory = MEMORY_RE.sub("", text)
            outside_registers = REGISTER_RE.findall(outside_memory)
            widths = {register_width(value) for value in outside_registers}
            widths.discard(None)
            if len(widths) == 1:
                width_bits = widths.pop()
        operand_index = text[:match.start()].count(",")
        if mnemonic.startswith(("cmp", "test", "prefetch")):
            role = "read"
        elif operand_index == 0 and mnemonic.startswith(("mov", "stos", "xchg")):
            role = "read_write" if mnemonic.startswith("xchg") else "write"
        elif operand_index == 0 and mnemonic.startswith(("add", "sub", "and", "or", "xor", "inc", "dec")):
            role = "read_write"
        elif operand_index > 0 or mnemonic.startswith(("lods", "mov")):
            role = "read"
        else:
            role = "unknown"
        result.append(MemoryOperand(match[0], base, index, int(scale_match[1]) if scale_match else None,
                                    displacement, displacement_value, displacement_resolved,
                                    width_bits, width_bits // 8 if width_bits is not None else None,
                                    base in ("rsp", "rbp", "esp", "ebp"), role))
    return tuple(result)


def parse_disassembly(text: str, start: int, stop: int) -> list[Instruction]:
    instructions: list[Instruction] = []
    for line in text.splitlines():
        match = INSTRUCTION_RE.match(line)
        if not match:
            continuation = CONTINUATION_RE.match(line)
            if continuation:
                address = int(continuation[1], 16)
                raw_bytes = " ".join(continuation[2].split())
                byte_count = len(raw_bytes.split())
                if not instructions:
                    raise AuditError("orphan instruction-byte continuation")
                previous = instructions[-1]
                expected = previous.address + len(previous.raw_bytes.split())
                if address != expected:
                    raise AuditError("instruction-byte continuation is not contiguous")
                if not start <= address < stop or address + byte_count > stop:
                    raise AuditError("instruction-byte continuation is outside the function range")
                instructions[-1] = replace(
                    previous, raw_bytes=f"{previous.raw_bytes} {raw_bytes}"
                )
                continue
            if ADDRESS_LINE_RE.match(line):
                raise AuditError("malformed instruction or continuation line")
            continue
        address = int(match[1], 16)
        if not start <= address < stop:
            raise AuditError("decoded instruction is outside the function range")
        mnemonic = match[3].lower()
        operands = match[4].split("#", 1)[0].strip()
        target = None
        if mnemonic.startswith("j") or mnemonic in ("call", "callq"):
            target_match = re.match(r"(?:0x)?([0-9a-fA-F]+)\b", operands)
            if target_match:
                target = int(target_match[1], 16)
        registers = tuple(value.lower() for value in REGISTER_RE.findall(operands))
        defs = registers[:1] if registers and mnemonic not in ("cmp", "test", "push") else ()
        read_modify = mnemonic.startswith(("add", "sub", "and", "or", "xor", "inc", "dec", "shl", "shr", "sal", "sar", "imul"))
        uses = registers if defs and read_modify else (registers[1:] if defs else registers)
        conditional = mnemonic.startswith("j") and mnemonic not in ("jmp", "jmpq")
        instructions.append(Instruction(address, " ".join(match[2].split()), mnemonic, operands,
                                        parse_memory(operands, mnemonic), target, conditional,
                                        target is not None and target < address, defs, uses))
    if not instructions:
        raise AuditError("no instructions parsed in function range")
    validate_complete_decode(instructions, start, stop)
    return instructions


def validate_complete_decode(instructions: Sequence[Instruction], start: int, stop: int) -> None:
    expected = start
    for instruction in sorted(instructions, key=lambda item: item.address):
        byte_count = len(instruction.raw_bytes.split())
        if byte_count <= 0:
            raise AuditError("decoded instruction has no bytes")
        if instruction.address < expected:
            raise AuditError("decoded instruction ranges overlap")
        if instruction.address > expected:
            raise AuditError("function disassembly contains an undecoded gap")
        end = instruction.address + byte_count
        if end > stop:
            raise AuditError("decoded instruction extends outside the function range")
        expected = end
    if expected != stop:
        raise AuditError("function disassembly is truncated")


@dataclass(frozen=True)
class Relocation:
    offset: int
    relocation_type: str
    target: str


def parse_relocations(text: str, start: int, stop: int) -> list[Relocation]:
    relocations: list[Relocation] = []
    pattern = re.compile(r"^\s*([0-9a-fA-F]+)\s+(R_\S+)\s+(.+?)\s*$")
    for line in text.splitlines():
        match = pattern.match(line)
        if match:
            offset = int(match[1], 16)
            if start <= offset < stop:
                relocations.append(Relocation(offset, match[2], match[3]))
    return relocations


CALLS = {"call", "callq"}
RETURNS = {"ret", "retq"}
FENCES = {"lfence", "mfence", "sfence", "cpuid", "serialize"}
CLOCKS = {"rdtsc", "rdtscp"}
SYSCALLS = {"syscall", "sysenter", "int", "int3"}
GATHERS = {"vgatherdps", "vgatherdpd", "vgatherqps", "vgatherqpd", "vpgatherdd", "vpgatherdq", "vpgatherqd", "vpgatherqq"}
SIMD_PREFIXES = ("xmm", "ymm", "zmm", "mm")


def instruction_inventory(
    instructions: Sequence[Instruction], function_start: int | None = None,
    function_stop: int | None = None,
) -> dict[str, list[int]]:
    inventory = {key: [] for key in ("calls", "returns", "prefetch", "vector_candidates", "memory", "fences", "clocks", "syscalls", "atomics", "conditional_branches", "unconditional_branches", "back_edges", "external_tail_transfers", "copy_helper_relocations")}
    if function_start is None:
        function_start = min(item.address for item in instructions)
    if function_stop is None:
        function_stop = max(item.address + len(item.raw_bytes.split()) for item in instructions)
    for item in instructions:
        mnemonic = item.mnemonic
        if mnemonic in CALLS: inventory["calls"].append(item.address)
        if mnemonic in RETURNS: inventory["returns"].append(item.address)
        if mnemonic.startswith("prefetch"): inventory["prefetch"].append(item.address)
        if mnemonic in GATHERS or any(prefix in item.operands.lower() for prefix in SIMD_PREFIXES): inventory["vector_candidates"].append(item.address)
        if item.memory_operands: inventory["memory"].append(item.address)
        if mnemonic in FENCES: inventory["fences"].append(item.address)
        if mnemonic in CLOCKS: inventory["clocks"].append(item.address)
        if mnemonic in SYSCALLS: inventory["syscalls"].append(item.address)
        if (mnemonic == "lock" or mnemonic.startswith("lock") or
                item.operands.lower().startswith("lock ") or
                (mnemonic in ("xchg", "xchgq", "xchgl", "xchgw", "xchgb") and item.memory_operands)):
            inventory["atomics"].append(item.address)
        if mnemonic.startswith("j"):
            inventory["conditional_branches" if item.conditional_branch else "unconditional_branches"].append(item.address)
        if mnemonic in ("jmp", "jmpq") and (
            item.branch_target is None or not function_start <= item.branch_target < function_stop
        ):
            inventory["external_tail_transfers"].append(item.address)
        if item.back_edge: inventory["back_edges"].append(item.address)
    return inventory


def is_copy_helper_target(target: str) -> bool:
    normalized = target.split("+", 1)[0].split("-", 1)[0].split("@", 1)[0]
    normalized = normalized.lstrip("_")
    return bool(re.match(r"^(?:memcpy|memmove)(?:$|[_.])", normalized)) or normalized.startswith(
        ("compiler_memcpy", "compiler_memmove", "builtin_memcpy", "builtin_memmove")
    )


def generic_findings(inventory: dict[str, list[int]]) -> list[dict[str, object]]:
    checks = (("calls", "generic.no_calls"), ("prefetch", "generic.no_prefetch"),
              ("fences", "generic.no_fences"), ("clocks", "generic.no_clocks"),
              ("syscalls", "generic.no_syscalls"), ("atomics", "generic.no_atomics"),
              ("external_tail_transfers", "generic.no_external_tail_transfers"),
              ("copy_helper_relocations", "generic.no_copy_helpers"))
    return [{"id": finding_id, "status": "fail" if inventory[key] else "pass", "addresses": inventory[key]} for key, finding_id in checks]


class FamilyPolicy(Protocol):
    identifier: str
    version: str
    def manifest(self) -> dict[str, object]: ...
    def requirements(self) -> "PolicyRequirements": ...
    def evaluate(self, context: "PolicyContext") -> tuple[list[dict[str, object]], list[dict[str, object]]]: ...


@dataclass(frozen=True)
class PolicyRequirements:
    implementation_id: str
    semantic_symbol: str
    compiler_presets: tuple[tuple[str, str], ...]
    compiler_flags: tuple[tuple[str, FlagRequirements], ...]


@dataclass(frozen=True)
class PolicyContext:
    implementation_id: str
    semantic_symbol: str
    compiler_family: str
    qualification_preset: str
    compiler_target: str
    effective_flags: EffectiveFlags
    symbol: Symbol
    function_start: int
    function_stop: int
    instructions: tuple[Instruction, ...]
    relocations: tuple[Relocation, ...]
    inventory: Mapping[str, tuple[int, ...]]
    generic_findings: tuple[Mapping[str, object], ...]


def load_policy(path: Path) -> FamilyPolicy:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        identifier = value["identifier"]
        version = value["version"]
    except (OSError, json.JSONDecodeError, KeyError, TypeError) as error:
        raise AuditError("family policy is unavailable or malformed") from error
    if not isinstance(identifier, str) or not isinstance(version, str):
        raise AuditError("family policy fields are invalid")
    from .policies import POLICY_REGISTRY
    factory = POLICY_REGISTRY.get(identifier)
    if factory is None:
        raise AuditError("family policy identifier is unknown")
    policy = factory()
    if policy.identifier != identifier or policy.version != version or value != policy.manifest():
        raise AuditError("family policy identifier/version mismatch")
    return policy


def policy_binding_findings(
    policy: FamilyPolicy, implementation_id: str, semantic_symbol: str,
    compiler_family: str, qualification_preset: str, flags: EffectiveFlags,
) -> list[dict[str, object]]:
    requirements = policy.requirements()
    presets = dict(requirements.compiler_presets)
    flag_sets = dict(requirements.compiler_flags)
    findings = [
        {"id": "qualification.binding.implementation_id",
         "status": "pass" if implementation_id == requirements.implementation_id else "fail",
         "expected": requirements.implementation_id, "actual": implementation_id},
        {"id": "qualification.binding.semantic_symbol",
         "status": "pass" if semantic_symbol == requirements.semantic_symbol else "fail",
         "expected": requirements.semantic_symbol, "actual": semantic_symbol},
        {"id": "qualification.binding.compiler_family",
         "status": "pass" if compiler_family in presets and compiler_family in flag_sets else "fail",
         "expected": sorted(set(presets) & set(flag_sets)), "actual": compiler_family},
        {"id": "qualification.binding.preset",
         "status": "pass" if presets.get(compiler_family) == qualification_preset else "fail",
         "expected": presets.get(compiler_family), "actual": qualification_preset},
    ]
    required_flags = flag_sets.get(compiler_family)
    if required_flags is not None:
        findings.extend(evaluate_flag_requirements(flags, required_flags))
    return findings


def validate_policy_results(
    findings: object, review_items: object
) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    if not isinstance(findings, list) or not all(
        isinstance(item, dict) and isinstance(item.get("id"), str) and
        item.get("status") in ("pass", "fail", "review") for item in findings
    ):
        raise AuditError("family policy findings are malformed")
    if not isinstance(review_items, list) or not all(
        isinstance(item, dict) and isinstance(item.get("id"), str) and
        isinstance(item.get("question"), str) and isinstance(item.get("evidence"), list)
        for item in review_items
    ):
        raise AuditError("family policy review requirements are malformed")
    return findings, review_items


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def validate_report(report: dict[str, object]) -> None:
    required = {
        "schema", "framework_version", "family_policy_version", "source", "implementation_id",
        "qualification", "compiler", "compile_command", "effective_flags", "object", "symbol",
        "function", "tools", "instructions", "relocations", "inventory", "generic_findings",
        "family_findings", "automated_disposition", "human_review", "qualification_status",
    }
    missing = sorted(required - report.keys())
    if missing:
        raise AuditError(f"report is missing required fields: {', '.join(missing)}")
    if report["schema"] != REPORT_SCHEMA_VERSION:
        raise AuditError("report schema version is invalid")


def normalize_report_privacy(value: object, source_root: Path, build_root: Path) -> object:
    if isinstance(value, str):
        return normalize_evidence_text(value, source_root, build_root)
    if isinstance(value, list):
        return [normalize_report_privacy(item, source_root, build_root) for item in value]
    if isinstance(value, tuple):
        return [normalize_report_privacy(item, source_root, build_root) for item in value]
    if isinstance(value, dict):
        return {key: normalize_report_privacy(item, source_root, build_root) for key, item in value.items()}
    return value


def human_report(report: dict[str, object]) -> str:
    generic = [f"{item['id']}: {item['status']}" for item in report["generic_findings"]]
    family = [f"{item['id']}: {item['status']} — {json.dumps(item.get('evidence', []), sort_keys=True)}"
              for item in report["family_findings"]]
    reviews = [f"{item['id']}: {item['question']} — {json.dumps(item.get('evidence', []), sort_keys=True)}"
               for item in report["human_review"]["items"]]
    qualification = report["qualification"]
    return "\n".join((
        f"Kernel object audit: {report['qualification_status']}",
        f"Policy: {qualification['policy_identifier']} ({report['family_policy_version']})",
        f"Implementation binding: {report['implementation_id']}",
        f"Artifact: {report['object']['identity']}",
        f"Symbol: {report['symbol']['demangled']}",
        f"Toolchain: {report['compiler']['family']} {report['compiler']['version']} ({report['compiler']['target']})",
        f"Preset: {qualification['preset']} ({qualification['configuration']})",
        f"Controls: {json.dumps(report['effective_flags'], sort_keys=True)}",
        f"Object SHA-256: {report['object']['sha256']}",
        f"Function SHA-256: {report['function']['sha256']}",
        "Generic findings:", *(f"  {item}" for item in generic),
        "Family and binding findings:", *(f"  {item}" for item in family),
        f"Human review: {report['human_review']['status']}", *(f"  {item}" for item in reviews),
        f"Automated disposition: {report['automated_disposition']}",
        f"Qualification status: {report['qualification_status']}",
        "",
    ))


def tool_version(runner: CommandRunner, executable: str) -> str:
    output = checked_run(runner, [executable, "--version"])
    first = output.splitlines()
    if not first:
        raise AuditError("tool version output is empty")
    return first[0]


def validate_compiler_family(requested: str, executable: str, version: str) -> dict[str, object]:
    evidence = f"{Path(executable).name} {version}".lower()
    has_clang = "clang" in evidence
    has_gcc = any(token in evidence for token in ("g++", "gcc", "gnu compiler collection")) and not has_clang
    actual = "Clang" if has_clang else ("GCC" if has_gcc else "unknown")
    if actual == "unknown" or actual != requested:
        raise AuditError("compiler family evidence is incompatible or ambiguous")
    return {"id": "qualification.compiler_family", "status": "pass",
            "expected": requested, "actual": actual}


def validate_target(target: str) -> dict[str, object]:
    value = target.strip().lower()
    architecture_ok = "x86_64" in value or "amd64" in value
    linux_ok = "linux" in value
    incompatible = any(token in value for token in ("aarch64", "arm", "i386", "i486", "i586", "i686", "mingw", "windows", "darwin", "apple"))
    if not value or not architecture_ok or not linux_ok or incompatible:
        raise AuditError("compiler target is incompatible with Linux ELF x86-64/SysV")
    return {"id": "qualification.target", "status": "pass",
            "expected": "Linux ELF x86-64 SysV", "actual": target}


def demangle_symbols(runner: CommandRunner, names: Sequence[str], cxxfilt: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for name in sorted(set(names)):
        output = checked_run(runner, [cxxfilt, name]).strip()
        if not output:
            raise AuditError("demangler returned an empty name")
        result[name] = output
    return result


def git_evidence(runner: CommandRunner, source_root: Path) -> dict[str, object]:
    top_level = checked_run(runner, ["git", "rev-parse", "--show-toplevel"], source_root).strip()
    if (not Path(top_level).is_absolute() or
            Path(top_level).resolve() != source_root.resolve()):
        raise AuditError("Git top-level does not match declared source root")
    commit = checked_run(runner, ["git", "rev-parse", "HEAD"], source_root).strip()
    status = checked_run(runner, ["git", "status", "--porcelain", "--untracked-files=normal"], source_root)
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise AuditError("Git revision is unavailable")
    return {"commit": commit, "dirty": bool(status.strip())}


def audit(arguments: argparse.Namespace, runner: CommandRunner | None = None) -> tuple[dict[str, object], str, int]:
    runner = runner or SubprocessRunner()
    source_root = Path(arguments.source_root).resolve()
    build_root = Path(arguments.build_directory).resolve()
    database = Path(arguments.compile_commands) if arguments.compile_commands else build_root / "compile_commands.json"
    database = inside(build_root, database if database.is_absolute() else build_root / database)
    record = load_compile_record(database, source_root, build_root, Path(arguments.timed_source))
    if not record.output.is_file():
        raise AuditError("resolved object artifact is absent")
    flags = interpret_flags(record.arguments)
    policy = load_policy(Path(arguments.family_policy))
    compiler_version = tool_version(runner, record.arguments[0])
    compiler_family_finding = validate_compiler_family(
        arguments.compiler_family, record.arguments[0], compiler_version
    )
    compiler_target_output = checked_run(runner, [record.arguments[0], "-dumpmachine"]).strip()
    target_finding = validate_target(compiler_target_output)
    binding_findings = policy_binding_findings(
        policy, arguments.implementation_id, arguments.semantic_symbol,
        arguments.compiler_family, arguments.qualification_preset, flags,
    )
    readelf_sections = checked_run(runner, [arguments.readelf, "--sections", "--wide", str(record.output)])
    readelf_symbols = checked_run(runner, [arguments.readelf, "--symbols", "--wide", str(record.output)])
    raw_symbols = [match[6].split("@", 1)[0] for line in readelf_symbols.splitlines() if (match := SYMBOL_RE.match(line)) and match[5].isdigit()]
    symbols = parse_symbols(readelf_symbols, demangle_symbols(runner, raw_symbols, arguments.cxxfilt))
    sections = parse_sections(readelf_sections)
    symbol, section = select_symbol(symbols, arguments.semantic_symbol, sections)
    function_bytes = extract_function_bytes(record.output, symbol, section)
    stop = symbol.value + symbol.size
    disassembly = checked_run(runner, [arguments.objdump, "-d", "-Mintel", f"--section={section.name}", f"--start-address={symbol.value}", f"--stop-address={stop}", str(record.output)])
    reloc_text = checked_run(runner, [arguments.objdump, "-r", f"--section={section.name}", str(record.output)])
    instructions = parse_disassembly(disassembly, symbol.value, stop)
    relocations = parse_relocations(reloc_text, symbol.value, stop)
    inventory = instruction_inventory(instructions, symbol.value, stop)
    inventory["copy_helper_relocations"] = [
        item.offset
        for item in relocations
        if is_copy_helper_target(item.target)
    ]
    generic = generic_findings(inventory)
    context = PolicyContext(
        arguments.implementation_id, arguments.semantic_symbol, arguments.compiler_family,
        arguments.qualification_preset, compiler_target_output, flags, symbol,
        symbol.value, stop, tuple(instructions), tuple(relocations),
        MappingProxyType({key: tuple(value) for key, value in inventory.items()}),
        tuple(MappingProxyType(dict(item)) for item in generic),
    )
    family, review_items = validate_policy_results(*policy.evaluate(context))
    family = [compiler_family_finding, target_finding, *binding_findings, *family]
    automated = "fail" if any(item["status"] == "fail" for item in generic + family) else "pass"
    status = "automated_failure" if automated == "fail" else "pending_human_review"
    compiler_family = arguments.compiler_family
    object_bytes = record.output.read_bytes()
    report: dict[str, object] = {
        "schema": REPORT_SCHEMA_VERSION,
        "framework_version": FRAMEWORK_VERSION,
        "family_policy_version": policy.version,
        "source": git_evidence(runner, source_root),
        "implementation_id": arguments.implementation_id,
        "qualification": {"preset": arguments.qualification_preset, "configuration": "Release",
                          "policy_identifier": policy.identifier},
        "compiler": {"family": compiler_family, "version": compiler_version, "target": compiler_target_output, "identity": Path(record.arguments[0]).name},
        "compile_command": list(record.normalized_arguments),
        "effective_flags": asdict(flags),
        "object": {"identity": normalize_path(record.output, source_root, build_root), "sha256": sha256_bytes(object_bytes)},
        "symbol": {"mangled": symbol.mangled, "demangled": symbol.demangled, "binding": symbol.binding, "type": symbol.symbol_type, "section": section.name, "value": symbol.value, "size": symbol.size},
        "function": {"sha256": sha256_bytes(function_bytes), "start": symbol.value, "stop": stop},
        "tools": {"readelf": tool_version(runner, arguments.readelf), "objdump": tool_version(runner, arguments.objdump), "digest": "Python hashlib.sha256"},
        "instructions": [asdict(item) for item in instructions],
        "relocations": [asdict(item) for item in relocations],
        "inventory": inventory,
        "generic_findings": generic,
        "family_findings": family,
        "automated_disposition": automated,
        "human_review": {"required": True, "status": "pending", "items": review_items},
        "qualification_status": status,
    }
    report = normalize_report_privacy(report, source_root, build_root)
    validate_report(report)
    return report, human_report(report), 2 if automated == "fail" else 3


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser(description="Audit one explicitly selected Kernel timed-TU object")
    value.add_argument("--source-root", required=True)
    value.add_argument("--build-directory", required=True)
    value.add_argument("--compile-commands")
    value.add_argument("--timed-source", required=True)
    value.add_argument("--implementation-id", required=True)
    value.add_argument("--semantic-symbol", required=True)
    value.add_argument("--compiler-family", choices=("GCC", "Clang"), required=True)
    value.add_argument("--qualification-preset", required=True)
    value.add_argument("--family-policy", required=True)
    value.add_argument("--machine-report", required=True)
    value.add_argument("--human-report", required=True)
    value.add_argument("--readelf", default="readelf")
    value.add_argument("--objdump", default="objdump")
    value.add_argument("--cxxfilt", default="c++filt")
    return value


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parser().parse_args(argv)
    report_path = Path(arguments.machine_report)
    human_path = Path(arguments.human_report)
    try:
        report, text, exit_code = audit(arguments)
        report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        human_path.write_text(text, encoding="utf-8")
        return exit_code
    except AuditError as error:
        failure = {"schema": REPORT_SCHEMA_VERSION, "framework_version": FRAMEWORK_VERSION,
                   "qualification_status": "tooling_failure", "error": str(error),
                   "human_review": {"required": True, "status": "blocked", "items": []}}
        try:
            report_path.write_text(json.dumps(failure, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            human_path.write_text(f"Kernel object audit: tooling_failure\nError: {error}\n", encoding="utf-8")
        except OSError:
            pass
        print(f"audit-kernel-object-code: {error}", file=sys.stderr)
        return 1
