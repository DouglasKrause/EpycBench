"""Compatibility import for the shared release-only CTest adapter."""

from __future__ import annotations

from .release_integration import ctest_exit_code

__all__ = ["ctest_exit_code"]
