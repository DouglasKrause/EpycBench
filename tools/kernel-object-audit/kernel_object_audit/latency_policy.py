"""EB-KRN-LAT-007 family policy; semantic approval remains human-owned."""

from __future__ import annotations

from dataclasses import asdict, dataclass

from .framework import (
    FlagRequirements,
    Instruction,
    PolicyContext,
    PolicyRequirements,
    register_width,
)


POLICY_IDENTIFIER = "epycbench.kernel-object-audit.latency/v1"
IMPLEMENTATION_ID = "scalar-u64-random-cycle/v1"
SEMANTIC_SYMBOL = (
    "epycbench::kernel::execute_dependent_load_latency("
    "epycbench::kernel::PreparedDependentLoadLatency const&, "
    "epycbench::kernel::DependentLoadLatencySchedule)"
)

GCC_FLAGS = FlagRequirements(
    optimization="-O3", lto=False, loop_vectorization=False,
    slp_vectorization=False, unrolling=False, native_arch=False,
    native_tune=False, sanitizers=False, coverage=False, profiling=False,
)
CLANG_FLAGS = GCC_FLAGS

HUMAN_REVIEW = (
    ("latency.v1.review.candidate_loop", "Candidate loop is the timed dependent-load loop."),
    ("latency.v1.review.one_load", "Exactly one benchmark successor load occurs per logical step."),
    ("latency.v1.review.load_form", "The successor is one 64-bit scalar memory-to-GPR load."),
    ("latency.v1.review.successor_only", "The load touches only successor bytes 0 through 7."),
    ("latency.v1.review.dependency", "The loaded successor is the sole next-iteration node index."),
    ("latency.v1.review.single_chain", "No second, lookahead, or interleaved chain exists."),
    ("latency.v1.review.no_bounds", "No successor bounds validation is present."),
    ("latency.v1.review.one_step", "One back-edge iteration equals one logical step."),
    ("latency.v1.review.no_stores", "No benchmark-region write is present."),
    ("latency.v1.review.scalar_traversal", "Traversal uses no SIMD, gather, or prefetch."),
    ("latency.v1.review.terminal", "The final loaded/current node reaches the ABI scalar return."),
    ("latency.v1.review.setup", "Setup metadata loads are outside successor-load semantics."),
)


def _finding(identifier: str, status: str, evidence: object) -> dict[str, object]:
    return {"id": identifier, "status": status, "evidence": evidence}


def _loop_instructions(context: PolicyContext, start: int, stop: int) -> list[Instruction]:
    return [item for item in context.instructions if start <= item.address <= stop]


def _memory_reads(instructions: list[Instruction]) -> list[Instruction]:
    return [item for item in instructions if item.mnemonic != "lea" and any(
        operand.access_role in ("read", "read_write") and not operand.stack_derived
        for operand in item.memory_operands
    )]


def _destination(item: Instruction) -> str | None:
    return item.register_def_candidates[0] if item.register_def_candidates else None


def _is_scalar_gpr(register: str | None) -> bool:
    width = register_width(register or "")
    return width is not None and width <= 64


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


def _address_cycle_proven(
    instructions: list[Instruction], source_index: int, source: str,
    address_registers: set[str],
) -> bool:
    live = _flow_after(instructions, source_index, source)
    for item in instructions[:source_index]:
        definitions = set(item.register_def_candidates)
        uses = set(item.register_use_candidates)
        if definitions and live & uses:
            live |= definitions
    return bool(live & address_registers)


def _load_evidence(item: Instruction) -> dict[str, object]:
    return {
        "address": item.address,
        "mnemonic": item.mnemonic,
        "destination": _destination(item),
        "memory": [asdict(operand) for operand in item.memory_operands],
        "definitions": list(item.register_def_candidates),
        "uses": list(item.register_use_candidates),
    }


@dataclass(frozen=True)
class LatencyV1Policy:
    identifier: str = POLICY_IDENTIFIER
    version: str = POLICY_IDENTIFIER

    def requirements(self) -> PolicyRequirements:
        return PolicyRequirements(
            implementation_id=IMPLEMENTATION_ID,
            semantic_symbol=SEMANTIC_SYMBOL,
            compiler_presets=(("GCC", "release"), ("Clang", "release-clang")),
            compiler_flags=(("GCC", GCC_FLAGS), ("Clang", CLANG_FLAGS)),
        )

    def manifest(self) -> dict[str, object]:
        return {
            "identifier": self.identifier,
            "version": self.version,
            "implementation_id": IMPLEMENTATION_ID,
            "semantic_symbol": SEMANTIC_SYMBOL,
            "compiler_presets": {"GCC": "release", "Clang": "release-clang"},
            "compiler_flags": {"GCC": asdict(GCC_FLAGS), "Clang": asdict(CLANG_FLAGS)},
            "human_review": [identifier for identifier, _ in HUMAN_REVIEW],
        }

    def evaluate(self, context: PolicyContext):
        findings: list[dict[str, object]] = []
        back_edges = [item for item in context.instructions if item.back_edge and item.branch_target is not None]
        loops = [(item.branch_target, item.address, _loop_instructions(context, item.branch_target, item.address))
                 for item in back_edges]
        viable = [loop for loop in loops if _memory_reads(loop[2])]
        if not viable:
            findings.append(_finding("latency.v1.candidate_loop.missing", "fail",
                                     {"back_edges": [item.address for item in back_edges]}))
            return findings, self._review_items(context, None, [], [])

        proven_by_loop: list[tuple[tuple[int, int, list[Instruction]], list[Instruction]]] = []
        for loop in viable:
            proven = []
            for index, item in enumerate(loop[2]):
                if item.mnemonic == "lea":
                    continue
                destination = _destination(item)
                if not destination or not _is_scalar_gpr(destination):
                    continue
                readable = [operand for operand in item.memory_operands
                            if operand.access_role == "read" and not operand.stack_derived]
                if not readable:
                    continue
                address_registers = {value for operand in readable for value in (operand.base, operand.index) if value}
                if _address_cycle_proven(loop[2], index, destination, address_registers):
                    proven.append(item)
            proven_by_loop.append((loop, proven))

        independently_qualifying = [entry for entry in proven_by_loop if entry[1]]
        if len(independently_qualifying) > 1:
            findings.append(_finding("latency.v1.candidate_loop.ambiguous", "fail", [
                {"start": loop[0], "stop": loop[1], "loads": [item.address for item in loads]}
                for loop, loads in independently_qualifying
            ]))
            return findings, self._review_items(context, None, [], [])
        if len(viable) > 1 and len(independently_qualifying) != 1:
            findings.append(_finding("latency.v1.candidate_loop.ambiguous", "review", [
                {"start": loop[0], "stop": loop[1]} for loop in viable
            ]))
            return findings, self._review_items(context, None, [], [])

        selected, proven = independently_qualifying[0] if independently_qualifying else proven_by_loop[0]
        loop_start, loop_stop, body = selected
        findings.append(_finding("latency.v1.candidate_loop", "pass",
                                 {"start": loop_start, "stop": loop_stop,
                                  "back_edge": loop_stop, "conditional": body[-1].conditional_branch}))

        reads = _memory_reads(body)
        scalar_reads = [item for item in reads if _is_scalar_gpr(_destination(item))]
        qword_reads = [item for item in scalar_reads if any(
            operand.access_role == "read" and operand.width_bits == 64 and not operand.stack_derived
            for operand in item.memory_operands
        )]
        vector_reads = [item for item in reads if any(
            (operand.width_bits or 0) > 64 for operand in item.memory_operands
        ) or item.address in context.inventory.get("vector_candidates", ())]
        gather_reads = [item for item in reads if "gather" in item.mnemonic]
        findings.append(_finding("latency.v1.vector_gather", "fail" if vector_reads or gather_reads else "pass",
                                 [_load_evidence(item) for item in vector_reads + gather_reads]))

        if not qword_reads:
            findings.append(_finding("latency.v1.successor_load.missing", "fail",
                                     [_load_evidence(item) for item in scalar_reads]))
        invalid_width = [item for item in scalar_reads if any(
            operand.width_bits != 64 for operand in item.memory_operands
            if operand.access_role == "read" and not operand.stack_derived
        )]
        findings.append(_finding("latency.v1.successor_width", "fail" if invalid_width else "pass",
                                 [_load_evidence(item) for item in invalid_width]))

        if len(proven) > 1:
            findings.extend((
                _finding("latency.v1.successor_load.multiple", "fail", [_load_evidence(item) for item in proven]),
                _finding("latency.v1.second_chain", "fail", [_load_evidence(item) for item in proven]),
                _finding("latency.v1.multi_step_unroll", "fail", [item.address for item in proven]),
            ))
        elif len(proven) == 1:
            findings.extend((
                _finding("latency.v1.successor_load", "pass", _load_evidence(proven[0])),
                _finding("latency.v1.second_chain", "pass", [proven[0].address]),
                _finding("latency.v1.multi_step_unroll", "pass", [proven[0].address]),
            ))
        else:
            findings.append(_finding("latency.v1.benchmark_provenance", "review",
                                     [_load_evidence(item) for item in qword_reads]))

        unresolved_reads = [item for item in reads if item not in proven]
        if unresolved_reads:
            findings.append(_finding("latency.v1.unresolved_memory_provenance", "review",
                                     [_load_evidence(item) for item in unresolved_reads]))

        successor = proven[0] if len(proven) == 1 else None
        payload = []
        payload_review = []
        if successor:
            for operand in successor.memory_operands:
                widened = operand.access_role == "read" and (operand.width_bytes or 0) > 8
                direct_payload_offset = (
                    operand.access_role == "read" and operand.displacement_resolved and
                    operand.displacement_value is not None and 8 <= operand.displacement_value <= 63
                )
                defining_node_address = any(
                    item.address < successor.address and item.mnemonic.startswith("lea") and
                    _destination(item) == operand.base and any(
                        memory.displacement_resolved and memory.displacement_value == 0
                        for memory in item.memory_operands
                    ) for item in body
                )
                if widened or (direct_payload_offset and defining_node_address):
                    payload.append(asdict(operand))
                elif direct_payload_offset:
                    payload_review.append(asdict(operand))
        findings.append(_finding("latency.v1.payload_access", "fail" if payload else "pass", payload))
        if payload_review:
            findings.append(_finding("latency.v1.payload_provenance", "review", payload_review))

        successor_register = _destination(successor) if successor else None
        bounds = []
        if successor_register:
            for item in body:
                uses = set(item.register_use_candidates)
                if successor_register in (uses | set(item.register_def_candidates)) and (
                    item.mnemonic.startswith(("cmp", "test")) or item.mnemonic in ("and", "andq", "andl")
                ):
                    bounds.append(item.address)
        findings.append(_finding("latency.v1.bounds_validation", "fail" if bounds else "pass", bounds))

        findings.append(_finding("latency.v1.loop_count", "pass" if body[-1].conditional_branch else "review",
                                 {"branch": body[-1].address, "mnemonic": body[-1].mnemonic}))

        stores = [item for item in body if any(
            operand.access_role in ("write", "read_write") and not operand.stack_derived
            for operand in item.memory_operands
        )]
        proven_stores = []
        ambiguous_stores = []
        successor_address_registers = {
            value for operand in (successor.memory_operands if successor else ())
            for value in (operand.base, operand.index) if value
        }
        for item in stores:
            store_registers = {value for operand in item.memory_operands
                               for value in (operand.base, operand.index) if value}
            (proven_stores if store_registers & successor_address_registers else ambiguous_stores).append(item)
        findings.append(_finding("latency.v1.benchmark_store", "fail" if proven_stores else "pass",
                                 [item.address for item in proven_stores]))
        if ambiguous_stores:
            findings.append(_finding("latency.v1.unresolved_store_provenance", "review",
                                     [item.address for item in ambiguous_stores]))

        terminal_status = "review"
        terminal_evidence: object = {"successor": successor_register, "return_register": "rax"}
        if successor and successor_register:
            live = _flow_after(list(context.instructions), list(context.instructions).index(successor), successor_register)
            terminal_status = "pass" if "rax" in live else "fail"
            terminal_evidence = {"successor": successor_register, "live_at_return": sorted(live),
                                 "load": successor.address}
        findings.append(_finding("latency.v1.terminal_return", terminal_status, terminal_evidence))
        findings.append(_finding("latency.v1.dependency_chain", "review",
                                 {"successor_load": successor.address if successor else None,
                                  "address": _load_evidence(successor) if successor else None}))
        return findings, self._review_items(context, selected, reads, proven)

    def _review_items(self, context: PolicyContext, selected, reads, proven):
        loop_evidence = None if selected is None else {"start": selected[0], "stop": selected[1]}
        evidence = {
            "loop": loop_evidence,
            "back_edges": list(context.inventory.get("back_edges", ())),
            "memory_reads": [_load_evidence(item) for item in reads],
            "proven_successor_candidates": [_load_evidence(item) for item in proven],
            "vector_candidates": list(context.inventory.get("vector_candidates", ())),
            "generic_findings": [dict(item) for item in context.generic_findings],
            "return_register": "rax",
        }
        return [{"id": identifier, "question": question, "evidence": [evidence]}
                for identifier, question in HUMAN_REVIEW]
