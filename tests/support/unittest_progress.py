"""Shared progress indicator and ETA estimation helpers for Python suites."""

from __future__ import annotations

import math
import time
import unittest

_HOOK_INSTALLED = False


def format_duration(seconds: float) -> str:
    """Format a non-negative duration in seconds into a concise string."""
    if not math.isfinite(seconds) or seconds < 0.0:
        return "n/a"
    if seconds < 10.0:
        return f"{seconds:.1f}s"
    rounded = int(round(seconds))
    if rounded < 60:
        return f"{rounded}s"
    if rounded < 3600:
        return f"{rounded // 60}m{rounded % 60:02d}s"
    if rounded < 86400:
        return f"{rounded // 3600}h{(rounded % 3600) // 60:02d}m"
    days = rounded // 86400
    hours = (rounded % 86400) // 3600
    minutes = (rounded % 3600) // 60
    return f"{days}d{hours:02d}h{minutes:02d}m"


def estimate_linear_eta(
    completed: int | float, total: int | float, elapsed_seconds: float
) -> tuple[float | None, str]:
    """Return ``(remaining_seconds, eta_text)`` from completed count and elapsed time."""
    if total <= 0:
        return None, "n/a"
    if completed >= total:
        return 0.0, "0.0s"
    if completed <= 0 or elapsed_seconds <= 0.0 or not math.isfinite(elapsed_seconds):
        return None, "calculating"
    remaining = elapsed_seconds * ((total - completed) / completed)
    return remaining, f"~{format_duration(remaining)}"


def format_progress_eta(
    completed: int | float, total: int | float, elapsed_seconds: float
) -> str:
    """Format ``elapsed <t> | eta <t>`` for loop progress lines."""
    clamped = max(0.0, elapsed_seconds)
    _, eta_text = estimate_linear_eta(completed, total, clamped)
    return f"elapsed {format_duration(clamped)} | eta {eta_text}"


def install_unittest_progress() -> None:
    """Install a progress + ETA prefix on unittest TextTestRunner/Result."""
    global _HOOK_INSTALLED
    if _HOOK_INSTALLED:
        return
    _HOOK_INSTALLED = True

    orig_run = unittest.TextTestRunner.run
    orig_start_test = unittest.TextTestResult.startTest

    def _run_with_total(self: unittest.TextTestRunner, test: unittest.TestSuite | unittest.TestCase):
        self._koi_total_tests = test.countTestCases()
        self._koi_started_at = time.monotonic()
        return orig_run(self, test)

    def _make_result_with_total(self: unittest.TextTestRunner) -> unittest.TextTestResult:
        res = self.resultclass(self.stream, self.descriptions, self.verbosity)
        res._koi_total_tests = getattr(self, "_koi_total_tests", 0)
        res._koi_current_test = 0
        res._koi_started_at = getattr(self, "_koi_started_at", time.monotonic())
        return res

    def _start_test_with_progress(self: unittest.TextTestResult, test: unittest.TestCase) -> None:
        current = getattr(self, "_koi_current_test", 0) + 1
        self._koi_current_test = current
        total = getattr(self, "_koi_total_tests", 0)
        if getattr(self, "showAll", False) and hasattr(self, "stream"):
            if total > 0:
                completed = current - 1
                started_at = getattr(self, "_koi_started_at", None)
                if completed >= 1 and total > 1 and started_at is not None:
                    _, eta_text = estimate_linear_eta(completed, total, time.monotonic() - started_at)
                    self.stream.write(f"[{current}/{total} | eta {eta_text}] ")
                else:
                    self.stream.write(f"[{current}/{total}] ")
            else:
                self.stream.write(f"[{current}] ")
            self.stream.flush()
        orig_start_test(self, test)

    unittest.TextTestRunner.run = _run_with_total  # type: ignore[method-assign]
    unittest.TextTestRunner._makeResult = _make_result_with_total  # type: ignore[method-assign]
    unittest.TextTestResult.startTest = _start_test_with_progress  # type: ignore[method-assign]


install_unittest_progress()
