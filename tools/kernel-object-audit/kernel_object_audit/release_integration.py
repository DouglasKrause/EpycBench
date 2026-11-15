"""Release-only CTest adapter preserving external human approval."""

from __future__ import annotations


def ctest_exit_code(driver_exit: int, report: dict[str, object] | None) -> int:
    """Map only verified automated-pass/pending-human evidence to CTest success."""
    if driver_exit in (1, 2):
        return driver_exit
    if driver_exit != 3 or report is None:
        return 1
    human = report.get("human_review")
    if not isinstance(human, dict):
        return 1
    if (
        report.get("automated_disposition") == "pass"
        and report.get("qualification_status") == "pending_human_review"
        and human.get("required") is True
        and human.get("status") == "pending"
    ):
        return 0
    return 1
