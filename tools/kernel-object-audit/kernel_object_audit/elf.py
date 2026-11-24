"""Read Linux ELF symbols and disassembly from binutils output."""

from __future__ import annotations

import re
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Protocol, Sequence


class AuditError(RuntimeError):
    """A required audit fact could not be established."""


@dataclass(frozen=True)
class CommandResult:
    returncode: int
    stdout: str
    stderr: str


class CommandRunner(Protocol):
    def run(self, arguments: Sequence[str], cwd: Path | None = None) -> CommandResult: ...


def checked_run(runner: CommandRunner, arguments: Sequence[str], cwd: Path | None = None) -> str:
    result = runner.run(arguments, cwd)
    if result.returncode != 0:
        raise AuditError(f"tool failed: {Path(arguments[0]).name} (exit {result.returncode})")
    return result.stdout


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


def demangle_symbols(runner: CommandRunner, names: Sequence[str], cxxfilt: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for name in sorted(set(names)):
        output = checked_run(runner, [cxxfilt, name]).strip()
        if not output:
            raise AuditError("demangler returned an empty name")
        result[name] = output
    return result
