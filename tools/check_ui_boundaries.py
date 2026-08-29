#!/usr/bin/env python3
"""Reject UI-framework headers in ckUtilities domain libraries.

This check is intentionally narrow during the incremental migration. The
listed files and directories are the framework-independent seams that already
exist today. New domain libraries must be added here when introduced. UI and
composition targets remain free to include exactly one UI framework until
their migration slice has completed.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DOMAIN_PATHS = (
    "lib/ckai_core",
    "src/common/app-info",
    "src/common/options",
    "src/tools/ck-du/include/disk_usage_core.hpp",
    "src/tools/ck-du/include/disk_usage_options.hpp",
    "src/tools/ck-du/src/disk_usage_core.cpp",
    "src/tools/ck-du/src/disk_usage_options.cpp",
    "src/tools/ck-edit/include/ck/edit/markdown_parser.hpp",
    "src/tools/ck-edit/src/markdown_parser.cpp",
    "include/ck/find/search_backend.hpp",
    "include/ck/find/search_model.hpp",
    "src/tools/ck-find/src/search_backend.cpp",
    "src/tools/ck-find/src/search_model.cpp",
    "src/tools/json-view/include/json_view_core.hpp",
    "src/tools/json-view/src/json_view_core.cpp",
)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".mm"}
FORBIDDEN_INCLUDE = re.compile(r"^\s*#\s*include\s*[<\"](?:tvision|cvision)/", re.MULTILINE)


def source_files(path: Path) -> list[Path]:
    if path.is_file():
        return [path] if path.suffix in SOURCE_SUFFIXES else []
    return sorted(candidate for candidate in path.rglob("*") if candidate.is_file() and candidate.suffix in SOURCE_SUFFIXES)


def main() -> int:
    files: list[Path] = []
    missing: list[Path] = []
    for relative_path in DOMAIN_PATHS:
        candidate = ROOT / relative_path
        if not candidate.exists():
            missing.append(candidate)
            continue
        files.extend(source_files(candidate))

    if missing:
        print("UI-boundary checker configuration refers to missing paths:", file=sys.stderr)
        for path in missing:
            print(f"  {path.relative_to(ROOT)}", file=sys.stderr)
        return 2

    violations: list[tuple[Path, int, str]] = []
    for path in files:
        text = path.read_text(encoding="utf-8")
        for match in FORBIDDEN_INCLUDE.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            violations.append((path, line, match.group(0).strip()))

    if violations:
        print("Domain libraries must not include Turbo Vision or ckVision headers:", file=sys.stderr)
        for path, line, include in violations:
            print(f"  {path.relative_to(ROOT)}:{line}: {include}", file=sys.stderr)
        return 1

    print(f"UI-boundary check passed: {len(files)} domain source files scanned.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
