"""Controlled repository policy registry; no external module loading is permitted."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Protocol, Sequence

from .bandwidth_policy import BandwidthV1Policy
from .latency_policy import LatencyV1Policy


class RegisteredPolicy(Protocol):
    identifier: str
    version: str


PolicyFactory = Callable[[], RegisteredPolicy]


def build_policy_registry(factories: Sequence[PolicyFactory]) -> dict[str, PolicyFactory]:
    registry: dict[str, PolicyFactory] = {}
    for factory in factories:
        policy = factory()
        if not policy.identifier or policy.identifier in registry:
            raise ValueError("policy identifiers must be nonempty and unique")
        registry[policy.identifier] = factory
    return registry


@dataclass(frozen=True)
class SyntheticLeafPolicy:
    """Test-only policy proving controlled executable-policy dispatch."""

    identifier: str = "epycbench.kernel-object-audit.synthetic-leaf/v1"
    version: str = "epycbench.kernel-object-audit.synthetic-leaf/v1"

    def manifest(self):
        return {"identifier": self.identifier, "version": self.version,
                "synthetic": True, "human_review": ["synthetic.semantic-review"]}

    def requirements(self):
        from .framework import FlagRequirements, PolicyRequirements
        return PolicyRequirements(
            implementation_id="synthetic-leaf/v1",
            semantic_symbol="synthetic_leaf",
            compiler_presets=(("Clang", "synthetic-clang"),),
            compiler_flags=(("Clang", FlagRequirements(
                optimization="-O3", lto=False, loop_vectorization=False,
                slp_vectorization=False, unrolling=False, native_arch=False,
                native_tune=False, sanitizers=False, coverage=False, profiling=False,
            )),),
        )

    def evaluate(self, context):
        del context
        return (
            [{"id": "synthetic.leaf", "status": "pass", "evidence": ["synthetic-only"]}],
            [{"id": "synthetic.semantic-review", "question": "Review synthetic semantics.",
              "evidence": ["synthetic-only"]}],
        )


@dataclass(frozen=True)
class SyntheticCallPolicy:
    """Test-only policy used to exercise generic call rejection."""

    identifier: str = "epycbench.kernel-object-audit.synthetic-call/v1"
    version: str = "epycbench.kernel-object-audit.synthetic-call/v1"

    def manifest(self):
        return {"identifier": self.identifier, "version": self.version,
                "synthetic": True, "human_review": ["synthetic.call-review"]}

    def requirements(self):
        from .framework import FlagRequirements, PolicyRequirements
        return PolicyRequirements(
            implementation_id="synthetic-call/v1",
            semantic_symbol="synthetic_call",
            compiler_presets=(("Clang", "synthetic-clang"),),
            compiler_flags=(("Clang", FlagRequirements(
                optimization="-O3", lto=False, loop_vectorization=False,
                slp_vectorization=False, unrolling=False, native_arch=False,
                native_tune=False, sanitizers=False, coverage=False, profiling=False,
            )),),
        )

    def evaluate(self, context):
        del context
        return [], [{"id": "synthetic.call-review", "question": "Review synthetic call.",
                     "evidence": ["synthetic-only"]}]


POLICY_REGISTRY = build_policy_registry(
    (SyntheticLeafPolicy, SyntheticCallPolicy, LatencyV1Policy, BandwidthV1Policy)
)
