#!/usr/bin/env python3
"""Reject legacy UI implementation markers from the active product tree."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
CHECKED_PATHS = (
    "CMakeLists.txt",
    "cmake",
    "include",
    "lib",
    "packaging",
    "scripts",
    "src",
    "tests",
    "tools",
    ".github",
)
EXCLUDED_PREFIXES = (Path("src/tools"),)
EXCLUDED_FILES = {
    Path("cmake/VerifyCkVisionCutover.cmake"),
    Path("tools/check_ckvision_only.py"),
    Path("tools/check_ui_boundaries.py"),
}
SOURCE_SUFFIXES = {".cmake", ".cpp", ".cxx", ".h", ".hpp", ".mm", ".py", ".sh", ".yml", ".yaml"}
FORBIDDEN_PATTERNS = (
    re.compile(r"(?:tvision|turbo[ -]?vision|\b(?:n?curses))", re.IGNORECASE),
    re.compile(r"\bUses_T[A-Za-z_]*"),
)


def is_checked(path: Path) -> bool:
    relative = path.relative_to(ROOT)
    if relative in EXCLUDED_FILES or any(relative.is_relative_to(prefix) for prefix in EXCLUDED_PREFIXES):
        return False
    return path.name == "CMakeLists.txt" or path.suffix in SOURCE_SUFFIXES


def files_to_scan() -> list[Path]:
    files: list[Path] = []
    for relative in CHECKED_PATHS:
        path = ROOT / relative
        if path.is_file():
            if is_checked(path):
                files.append(path)
        elif path.is_dir():
            files.extend(candidate for candidate in path.rglob("*") if candidate.is_file() and is_checked(candidate))
    return sorted(set(files))


def main() -> int:
    violations: list[tuple[Path, int, str]] = []
    for path in files_to_scan():
        text = path.read_text(encoding="utf-8")
        for forbidden in FORBIDDEN_PATTERNS:
            for match in forbidden.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                violations.append((path, line, match.group(0)))

    if violations:
        print("Active product files must remain ckVision-only:", file=sys.stderr)
        for path, line, marker in violations:
            print(f"  {path.relative_to(ROOT)}:{line}: {marker}", file=sys.stderr)
        return 1

    print(f"ckVision-only check passed: {len(files_to_scan())} active files scanned.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
