"""EB-KRN-BW-006 family policy; semantic approval remains human-owned."""

from __future__ import annotations

import re
from dataclasses import asdict, dataclass

from .framework import FlagRequirements, Instruction, PolicyContext, PolicyRequirements, register_width

POLICY_IDENTIFIER = "epycbench.kernel-object-audit.bandwidth/v1"
IMPLEMENTATION_ID = "scalar-u64-sequential/v1"
SEMANTIC_SYMBOL = (
    "epycbench::kernel::execute_sequential_read_bandwidth("
    "epycbench::kernel::PreparedSequentialReadBandwidth const&, "
    "epycbench::kernel::SequentialReadBandwidthPassCount)"
)
EXPECTED_OFFSETS = (0, 8, 16, 24, 32, 40, 48, 56)

REQUIRED_FLAGS = FlagRequirements(
    optimization="-O3", lto=False, loop_vectorization=False,
    slp_vectorization=False, unrolling=False, native_arch=False,
    native_tune=False, sanitizers=False, coverage=False, profiling=False,
)

HUMAN_REVIEW = (
    ("bandwidth.v1.review.traversal", "Selected control flow is the timed bandwidth traversal."),
    ("bandwidth.v1.review.eight_loads", "Exactly eight benchmark loads occur per logical 64-byte block."),
    ("bandwidth.v1.review.load_form", "Every benchmark load is one 64-bit scalar memory-to-GPR operation."),
    ("bandwidth.v1.review.offsets", "Logical offsets are exactly 0,8,16,24,32,40,48,56."),
    ("bandwidth.v1.review.load_order", "Benchmark-load program order is ascending logical word order."),
    ("bandwidth.v1.review.lane_mapping", "Each logical word maps to its corresponding accumulator lane."),
    ("bandwidth.v1.review.eight_chains", "Exactly eight independent persistent accumulator chains exist."),
    ("bandwidth.v1.review.persistence", "Accumulator chains persist across every block and pass."),
    ("bandwidth.v1.review.block_stride", "Block stride is 64 bytes with complete ascending coverage."),
    ("bandwidth.v1.review.pass_major", "All blocks complete before the next pass begins."),
    ("bandwidth.v1.review.pass_restart", "Each pass restarts at region start without resetting lanes."),
    ("bandwidth.v1.review.no_extra_access", "No hidden extra benchmark access exists."),
    ("bandwidth.v1.review.no_stores", "No benchmark-region write occurs."),
    ("bandwidth.v1.review.scalar_only", "Traversal has no vector, gather, prefetch, call, or helper logic."),
    ("bandwidth.v1.review.reduction", "Final scalar reduction includes every lane exactly once."),
    ("bandwidth.v1.review.terminal", "The reduced terminal reaches the ABI scalar return."),
    ("bandwidth.v1.review.setup", "Setup, metadata, and stack accesses are not benchmark accesses."),
)


def _finding(identifier: str, status: str, evidence: object) -> dict[str, object]:
    return {"id": identifier, "status": status, "evidence": evidence}


def _loop_body(context: PolicyContext, start: int, stop: int) -> list[Instruction]:
    return [item for item in context.instructions if start <= item.address <= stop]


def _reads(instructions: list[Instruction]) -> list[Instruction]:
    return [item for item in instructions if item.mnemonic != "lea" and any(
        operand.access_role in ("read", "read_write") and not operand.stack_derived
        for operand in item.memory_operands
    )]


def _writes(instructions: list[Instruction]) -> list[Instruction]:
    return [item for item in instructions if any(
        operand.access_role in ("write", "read_write") and not operand.stack_derived
        for operand in item.memory_operands
    )]


def _destination(item: Instruction) -> str | None:
    return item.register_def_candidates[0] if item.register_def_candidates else None


def _load_evidence(item: Instruction) -> dict[str, object]:
    return {
        "address": item.address, "mnemonic": item.mnemonic,
        "destination": _destination(item), "operands": item.operands,
        "memory": [asdict(value) for value in item.memory_operands],
        "definitions": list(item.register_def_candidates),
        "uses": list(item.register_use_candidates),
    }


def _direct_access(item: Instruction):
    values = [operand for operand in item.memory_operands
              if operand.access_role in ("read", "read_write") and not operand.stack_derived]
    return values[0] if len(values) == 1 else None


def _integer(text: str) -> int | None:
    try:
        return int(text.strip(), 0)
    except ValueError:
        return None


def _logical_offsets(body: list[Instruction], loads: list[Instruction]) -> dict[str, object]:
    """Track simple cursor+constant forms relative to the block-entry cursor."""
    accesses = [_direct_access(item) for item in loads]
    first = accesses[0] if accesses else None
    if first is None or first.base is None or first.index is not None:
        return {"resolved": False, "offsets": [], "loads": [], "updates": [],
                "final_deltas": {}, "cursor_registers": []}

    deltas = {first.base: 0}
    cursor_registers = {first.base}
    load_addresses = {item.address: access for item, access in zip(loads, accesses)}
    offsets: list[int] = []
    evidence: list[dict[str, object]] = []
    updates: list[dict[str, object]] = []
    resolved = True

    for item in body:
        if item.address in load_addresses:
            access = load_addresses[item.address]
            logical = None
            if (access is not None and access.base in deltas and access.index is None
                    and access.displacement_resolved):
                logical = deltas[access.base] + (access.displacement_value or 0)
                offsets.append(logical)
            else:
                resolved = False
            evidence.append({"address": item.address,
                             "base": None if access is None else access.base,
                             "cursor_delta": None if access is None else deltas.get(access.base),
                             "displacement": None if access is None else access.displacement_value,
                             "logical_offset": logical})

        operands = [value.strip().lower() for value in item.operands.split(",")]
        destination = _destination(item)
        recognized = False
        if (item.mnemonic in ("add", "sub") and len(operands) == 2
                and destination in deltas and operands[0] == destination):
            immediate = _integer(operands[1])
            if immediate is not None:
                before = deltas[destination]
                deltas[destination] += immediate if item.mnemonic == "add" else -immediate
                updates.append({"address": item.address, "operation": item.mnemonic,
                                "destination": destination, "before": before,
                                "after": deltas[destination]})
                recognized = True
        elif item.mnemonic == "mov" and len(operands) == 2 and destination == operands[0]:
            source = operands[1]
            if source in deltas:
                deltas[destination] = deltas[source]
                cursor_registers.add(destination)
                updates.append({"address": item.address, "operation": "copy",
                                "source": source, "destination": destination,
                                "after": deltas[destination]})
                recognized = True
        elif item.mnemonic == "lea" and destination is not None and len(item.memory_operands) == 1:
            address = item.memory_operands[0]
            if (address.base in deltas and address.index is None
                    and address.displacement_resolved):
                deltas[destination] = deltas[address.base] + (address.displacement_value or 0)
                cursor_registers.add(destination)
                updates.append({"address": item.address, "operation": "lea",
                                "source": address.base, "destination": destination,
                                "after": deltas[destination]})
                recognized = True

        if destination in deltas and item.register_def_candidates and not recognized:
            del deltas[destination]

    return {"resolved": resolved and len(offsets) == len(loads), "offsets": offsets,
            "loads": evidence, "updates": updates, "final_deltas": dict(sorted(deltas.items())),
            "cursor_registers": sorted(cursor_registers)}


def _accumulator_for_load(body: list[Instruction], index: int) -> str | None:
    item = body[index]
    destination = _destination(item)
    if destination is None:
        return None
    if item.mnemonic.startswith(("add", "sub")):
        return destination
    for candidate in body[index + 1:]:
        definitions = set(candidate.register_def_candidates)
        uses = set(candidate.register_use_candidates)
        if destination in uses and candidate.mnemonic.startswith("add"):
            return _destination(candidate)
        if destination in definitions:
            return None
    return None


def _flow_after(instructions: list[Instruction], source_index: int, source: str) -> set[str]:
    live = {source}
    for item in instructions[source_index + 1:]:
        definitions = set(item.register_def_candidates)
        uses = set(item.register_use_candidates)
        if definitions:
            propagated = bool(live & uses)
            live -= definitions
            if propagated:
                live |= definitions
    return live


def _full_gpr(register: str) -> str | None:
    """Return the architectural 64-bit GPR named by a 32/64-bit spelling."""
    value = register.lower()
    legacy = {
        "eax": "rax", "ebx": "rbx", "ecx": "rcx", "edx": "rdx",
        "esi": "rsi", "edi": "rdi", "esp": "rsp", "ebp": "rbp",
        "eip": "rip",
    }
    if value in legacy:
        return legacy[value]
    if re.fullmatch(r"r(?:ax|bx|cx|dx|si|di|sp|bp|ip|8|9|1[0-5])", value):
        return value
    match = re.fullmatch(r"(r(?:8|9|1[0-5]))d", value)
    return match[1] if match else None


def _zeroed_gpr(item: Instruction) -> str | None:
    """Recognize only x86 idioms that prove a complete GPR becomes zero."""
    operands = [value.strip().lower() for value in item.operands.split(",")]
    if len(operands) != 2 or register_width(operands[0]) not in (32, 64):
        return None
    destination = _full_gpr(operands[0])
    if destination is None:
        return None
    if item.mnemonic in ("xor", "sub") and _full_gpr(operands[1]) == destination:
        return destination
    if item.mnemonic == "mov" and _integer(operands[1]) == 0:
        return destination
    return None


def _canonical_definitions(item: Instruction) -> set[str]:
    return {value for register in item.register_def_candidates
            if (value := _full_gpr(register)) is not None}


def _pass_restart_evidence(
    pass_loop: tuple[int, int, list[Instruction]], block_start: int,
    qword: list[Instruction], block_registers: set[str], distinct_lanes: set[str],
) -> tuple[str, dict[str, object], list[int]]:
    """Prove pointer reload or stable-base/scaled-index restart at a pass header."""
    pass_start, pass_stop, pass_body = pass_loop
    first_load = min((item.address for item in qword), default=block_start)
    header = [item for item in pass_body if pass_start <= item.address < first_load]
    tail = [item for item in pass_body if item.address > block_start and item.address <= pass_stop]
    lane_registers = {_full_gpr(lane) for lane in distinct_lanes}
    lane_registers.discard(None)
    accumulator_resets = [item.address for item in (*tail, *header)
                          if _zeroed_gpr(item) in lane_registers]

    pointer_reloads = []
    for item in header:
        operands = [value.strip().lower() for value in item.operands.split(",")]
        destination = _full_gpr(operands[0]) if operands else None
        source = _full_gpr(operands[1]) if len(operands) == 2 else None
        source_changes = [] if source is None else [
            candidate.address for candidate in pass_body
            if candidate.address != item.address and source in _canonical_definitions(candidate)
        ]
        if (item.mnemonic == "mov" and destination in block_registers
                and source is not None and source != destination and not source_changes):
            pointer_reloads.append({"address": item.address, "cursor": destination,
                                    "stable_base": source})

    accesses = [_direct_access(item) for item in qword]
    indexed = bool(accesses) and all(
        access is not None and access.base is not None and access.index is not None
        and access.scale == 8 and access.base == accesses[0].base
        and access.index == accesses[0].index for access in accesses
    )
    index_evidence: dict[str, object] | None = None
    index_status = "review"
    if indexed:
        base = _full_gpr(accesses[0].base)
        index = _full_gpr(accesses[0].index)
        header_definitions = [item for item in header if index in _canonical_definitions(item)]
        zeroes = [item for item in header_definitions if _zeroed_gpr(item) == index]
        base_changes = [item.address for item in pass_body
                        if base in _canonical_definitions(item)]
        index_updates = [item for item in pass_loop[2]
                         if index in _canonical_definitions(item)
                         and item.mnemonic in ("add", "sub")]
        late_zeroes = [item.address for item in pass_loop[2]
                       if item.address >= first_load and _zeroed_gpr(item) == index]
        stride_words = None
        for item in index_updates:
            operands = [value.strip().lower() for value in item.operands.split(",")]
            if len(operands) == 2 and _full_gpr(operands[0]) == index:
                immediate = _integer(operands[1])
                if immediate is not None:
                    stride_words = immediate if item.mnemonic == "add" else -immediate
        index_evidence = {
            "model": "scaled_index_reset", "stable_base": base, "index": index,
            "scale": 8, "zeroing": [item.address for item in zeroes],
            "header_definitions": [item.address for item in header_definitions],
            "base_changes": base_changes, "stride_words": stride_words,
            "stride_bytes": None if stride_words is None else stride_words * 8,
            "first_load": first_load, "late_zeroing": late_zeroes,
        }
        if base is not None and index is not None and not base_changes and zeroes:
            index_status = "pass"
        elif (base is not None and index is not None and not base_changes
              and (header_definitions or index_updates or late_zeroes)):
            index_status = "fail"

    if accumulator_resets:
        status = "fail"
        model = "accumulator_reset"
    elif pointer_reloads:
        status = "pass"
        model = "pointer_reload"
    elif index_status in ("pass", "fail"):
        status = index_status
        model = "scaled_index_reset"
    elif block_registers and not indexed:
        status = "fail"
        model = "pointer_restart_missing"
    else:
        status = "review"
        model = "unresolved"
    evidence = {
        "model": model, "outer_back_edge": pass_stop, "outer_target": pass_start,
        "first_load": first_load, "pointer_reloads": pointer_reloads,
        "index_restart": index_evidence, "accumulator_resets": accumulator_resets,
    }
    return status, evidence, accumulator_resets


@dataclass(frozen=True)
class BandwidthV1Policy:
    identifier: str = POLICY_IDENTIFIER
    version: str = POLICY_IDENTIFIER

    def requirements(self) -> PolicyRequirements:
        return PolicyRequirements(
            implementation_id=IMPLEMENTATION_ID,
            semantic_symbol=SEMANTIC_SYMBOL,
            compiler_presets=(("GCC", "release"), ("Clang", "release-clang")),
            compiler_flags=(("GCC", REQUIRED_FLAGS), ("Clang", REQUIRED_FLAGS)),
        )

    def manifest(self) -> dict[str, object]:
        return {
            "identifier": self.identifier,
            "version": self.version,
            "implementation_id": IMPLEMENTATION_ID,
            "semantic_symbol": SEMANTIC_SYMBOL,
            "compiler_presets": {"GCC": "release", "Clang": "release-clang"},
            "compiler_flags": {"GCC": asdict(REQUIRED_FLAGS), "Clang": asdict(REQUIRED_FLAGS)},
            "human_review": [identifier for identifier, _ in HUMAN_REVIEW],
        }

    def evaluate(self, context: PolicyContext):
        findings: list[dict[str, object]] = []
        edges = [item for item in context.instructions if item.back_edge and item.branch_target is not None]
        loops = [(item.branch_target, item.address, _loop_body(context, item.branch_target, item.address))
                 for item in edges]
        viable = [loop for loop in loops if _reads(loop[2])]
        if not viable:
            findings.append(_finding("bandwidth.v1.traversal.missing", "fail",
                                     {"back_edges": [item.address for item in edges]}))
            return findings, self._review_items(context, None, None, [], [])

        minimum_span = min(stop - start for start, stop, _ in viable)
        candidates = [loop for loop in viable if loop[1] - loop[0] == minimum_span]
        if len(candidates) != 1:
            findings.append(_finding("bandwidth.v1.traversal.ambiguous", "fail", [
                {"start": item[0], "stop": item[1]} for item in candidates
            ]))
            return findings, self._review_items(context, None, None, [], [])
        block_loop = candidates[0]
        block_start, block_stop, body = block_loop
        containing = [loop for loop in loops
                      if loop != block_loop and loop[0] <= block_start and loop[1] >= block_stop]
        pass_loop = min(containing, key=lambda item: item[1] - item[0]) if containing else None
        findings.append(_finding("bandwidth.v1.traversal", "pass",
                                 {"block": [block_start, block_stop],
                                  "pass": [pass_loop[0], pass_loop[1]] if pass_loop else None}))
        findings.append(_finding("bandwidth.v1.block_loop", "pass",
                                 {"start": block_start, "stop": block_stop}))
        findings.append(_finding("bandwidth.v1.pass_structure", "pass" if pass_loop else "review",
                                 None if pass_loop is None else {"start": pass_loop[0], "stop": pass_loop[1]}))

        reads = _reads(body)
        scalar = [item for item in reads if register_width(_destination(item) or "") == 64]
        qword = [item for item in scalar if any(
            operand.access_role in ("read", "read_write") and operand.width_bits == 64
            and not operand.stack_derived for operand in item.memory_operands
        )]
        vector = [item for item in reads if any((operand.width_bits or 0) > 64
                                                for operand in item.memory_operands)
                  or item.address in context.inventory.get("vector_candidates", ())
                  or "gather" in item.mnemonic]
        findings.append(_finding("bandwidth.v1.vector_gather", "fail" if vector else "pass",
                                 [_load_evidence(item) for item in vector]))
        invalid_width = [item for item in reads if item not in qword]
        findings.append(_finding("bandwidth.v1.load_width", "fail" if invalid_width else "pass",
                                 [_load_evidence(item) for item in invalid_width]))
        if len(qword) < 8:
            findings.append(_finding("bandwidth.v1.benchmark_load.missing", "fail",
                                     {"expected": 8, "actual": len(qword)}))
        elif len(qword) > 8:
            findings.append(_finding("bandwidth.v1.benchmark_load.extra", "fail",
                                     {"expected": 8, "actual": len(qword)}))
        else:
            findings.append(_finding("bandwidth.v1.benchmark_load.count", "pass",
                                     [_load_evidence(item) for item in qword]))

        accesses = [_direct_access(item) for item in qword]
        logical = _logical_offsets(body, qword)
        offsets = tuple(logical["offsets"])
        if len(qword) == 8 and logical["resolved"]:
            duplicates = len(set(offsets)) != len(offsets)
            findings.append(_finding("bandwidth.v1.word_offset.duplicate",
                                     "fail" if duplicates else "pass", logical))
            findings.append(_finding("bandwidth.v1.word_offset.set",
                                     "pass" if tuple(sorted(offsets)) == EXPECTED_OFFSETS else "fail",
                                     {"expected": list(EXPECTED_OFFSETS), "actual": list(offsets),
                                      "affine": logical}))
            findings.append(_finding("bandwidth.v1.word_offset.order",
                                     "pass" if offsets == EXPECTED_OFFSETS else "fail", logical))
        else:
            findings.append(_finding("bandwidth.v1.unresolved_logical_offset", "review", logical))

        lanes = [_accumulator_for_load(body, body.index(item)) for item in qword]
        distinct_lanes = {lane for lane in lanes if lane is not None}
        if len(qword) == 8 and len(distinct_lanes) < 8:
            findings.extend((
                _finding("bandwidth.v1.lane_association", "fail", lanes),
                _finding("bandwidth.v1.accumulator_count", "fail",
                         {"expected": 8, "actual": len(distinct_lanes)}),
                _finding("bandwidth.v1.accumulator_collapse", "fail", lanes),
            ))
        elif len(qword) == 8 and len(distinct_lanes) == 8:
            findings.extend((
                _finding("bandwidth.v1.lane_association", "review", lanes),
                _finding("bandwidth.v1.accumulator_count", "pass", sorted(distinct_lanes)),
                _finding("bandwidth.v1.accumulator_collapse", "pass", sorted(distinct_lanes)),
            ))
        else:
            findings.append(_finding("bandwidth.v1.unresolved_lane_proof", "review", lanes))

        block_registers = set(logical["cursor_registers"])
        address_updates = [item for item in body if set(item.register_def_candidates) & block_registers
                           and re.search(r"(?:0x[0-9a-f]+|\b\d+\b)", item.operands, re.I)]
        stride = [register for register, delta in logical["final_deltas"].items() if delta == 64]
        stride_status = "pass" if stride else ("fail" if logical["updates"] else "review")
        findings.append(_finding("bandwidth.v1.block_stride", stride_status,
                                 {"accepted_registers": stride, "affine": logical,
                                  "updates": [item.address for item in address_updates]}))
        findings.append(_finding("bandwidth.v1.multi_block_unroll", "fail" if len(qword) > 8 else "pass",
                                 {"loads": len(qword)}))

        if pass_loop:
            restart_status, restart_evidence, accumulator_resets = _pass_restart_evidence(
                pass_loop, block_start, qword, block_registers, distinct_lanes
            )
            findings.append(_finding("bandwidth.v1.pass_order", "pass",
                                     {"block_back_edge": block_stop, "pass_back_edge": pass_loop[1]}))
            findings.append(_finding("bandwidth.v1.pass_restart", restart_status,
                                     restart_evidence))
            findings.append(_finding("bandwidth.v1.accumulator_persistence",
                                     "fail" if accumulator_resets else "review",
                                     accumulator_resets))
        else:
            findings.extend((
                _finding("bandwidth.v1.unresolved_pass_proof", "review", None),
                _finding("bandwidth.v1.pass_restart", "review", None),
                _finding("bandwidth.v1.accumulator_persistence", "review", sorted(distinct_lanes)),
            ))

        stores = _writes(body)
        proven_stores = []
        ambiguous_stores = []
        for item in stores:
            registers = {value for operand in item.memory_operands
                         for value in (operand.base, operand.index) if value}
            (proven_stores if registers & block_registers else ambiguous_stores).append(item)
        findings.append(_finding("bandwidth.v1.benchmark_store", "fail" if proven_stores else "pass",
                                 [item.address for item in proven_stores]))
        if ambiguous_stores:
            findings.append(_finding("bandwidth.v1.unresolved_store_provenance", "review",
                                     [item.address for item in ambiguous_stores]))

        all_instructions = list(context.instructions)
        reaching_return = []
        for lane in distinct_lanes:
            first = next((index for index, item in enumerate(all_instructions)
                          if item.address >= block_start and _destination(item) == lane), None)
            if first is not None and "rax" in _flow_after(all_instructions, first, lane):
                reaching_return.append(lane)
        reduction_status = ("pass" if len(reaching_return) == 8 else
                            ("fail" if reaching_return else "review"))
        findings.append(_finding("bandwidth.v1.terminal_reduction", reduction_status,
                                 {"lanes": sorted(distinct_lanes),
                                  "reaching_rax": sorted(reaching_return)}))
        findings.append(_finding("bandwidth.v1.terminal_return", reduction_status,
                                 {"abi_return": "rax", "reaching_rax": sorted(reaching_return)}))
        if reduction_status == "review":
            findings.append(_finding("bandwidth.v1.unresolved_reduction_proof", "review",
                                     {"lanes": sorted(distinct_lanes),
                                      "reaching_rax": sorted(reaching_return)}))
        return findings, self._review_items(context, block_loop, pass_loop, reads, qword)

    def _review_items(self, context, block_loop, pass_loop, reads, qword):
        evidence = {
            "block_loop": None if block_loop is None else [block_loop[0], block_loop[1]],
            "pass_loop": None if pass_loop is None else [pass_loop[0], pass_loop[1]],
            "back_edges": list(context.inventory.get("back_edges", ())),
            "memory_reads": [_load_evidence(item) for item in reads],
            "benchmark_candidates": [_load_evidence(item) for item in qword],
            "vector_candidates": list(context.inventory.get("vector_candidates", ())),
            "generic_findings": [dict(item) for item in context.generic_findings],
            "abi_return_register": "rax",
        }
        return [{"id": identifier, "question": question, "evidence": [evidence]}
                for identifier, question in HUMAN_REVIEW]
