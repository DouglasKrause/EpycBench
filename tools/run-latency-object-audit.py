#!/usr/bin/env python3
"""Run LAT-007 automation while preserving external human approval."""

from __future__ import annotations

import json
import sys
from pathlib import Path

TOOL_ROOT = Path(__file__).resolve().parent / "kernel-object-audit"
sys.path.insert(0, str(TOOL_ROOT))

from kernel_object_audit.framework import main
from kernel_object_audit.latency_integration import ctest_exit_code


def _option_path(arguments: list[str], option: str) -> Path:
    try:
        return Path(arguments[arguments.index(option) + 1])
    except (ValueError, IndexError) as error:
        raise ValueError(f"missing required integration option: {option}") from error


def run(arguments: list[str]) -> int:
    try:
        machine_report = _option_path(arguments, "--machine-report")
        human_report = _option_path(arguments, "--human-report")
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    machine_report.parent.mkdir(parents=True, exist_ok=True)
    human_report.parent.mkdir(parents=True, exist_ok=True)
    driver_exit = main(arguments)
    report = None
    if driver_exit == 3:
        try:
            report = json.loads(machine_report.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return 1
    return ctest_exit_code(driver_exit, report)


if __name__ == "__main__":
    raise SystemExit(run(sys.argv[1:]))
