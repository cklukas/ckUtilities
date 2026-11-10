#!/usr/bin/env python3
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Remove only validated RPM build-ID metadata from the layout listing.

The caller still checks every remaining path against the runtime payload gate.
No installation, extraction, shell evaluation or host filesystem resolution.
"""
import argparse
import posixpath
import re
import shlex
import stat
import subprocess
import sys


BUILD_IDS = "/usr/lib/.build-id"
# RPM accepts build IDs of 16..64 bytes and uses .N for duplicate IDs.
BUILD_LINK = re.compile(r"^/usr/lib/\.build-id/([0-9a-f]{2})/([0-9a-f]+)(?:\.([1-9][0-9]*))?$")
BUILD_DIRECTORY = re.compile(r"^/usr/lib/\.build-id/[0-9a-f]{2}$")
QUERY = "[%{FILENAMES:shescape} %{FILEMODES} %{FILELINKTOS:shescape}\\n]"


def safe_text(value):
    # Newlines/semicolons cannot be represented in the caller's CMake list.
    return not any(c in value for c in "\0\r\n\t;")


def link_destination(path, target):
    if not target or not safe_text(target) or "//" in target:
        raise ValueError(f"Invalid RPM symlink target: {path} -> {target!r}")
    parts = [] if target.startswith("/") else posixpath.dirname(path).split("/")[1:]
    for part in target.split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            if not parts:
                raise ValueError(f"RPM symlink escapes package root: {path}")
            parts.pop()
        else:
            parts.append(part)
    return "/" + "/".join(parts)


def verified_listing(package, tools):
    query = subprocess.run(
        ["rpm", "-qp", "--queryformat", QUERY, "--", package],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True,
        encoding="utf-8", errors="strict",
    )
    # RPM's shescape format quotes strings; shlex only parses that quoting.
    # It never invokes a shell or interpolates package-controlled content.
    fields = shlex.split(query.stdout, comments=False, posix=True)
    if not fields or len(fields) % 3:
        raise ValueError("Malformed RPM file/mode/link query")
    records = {}
    for offset in range(0, len(fields), 3):
        path, mode, target = fields[offset:offset + 3]
        if (not safe_text(path) or not path.startswith("/") or "//" in path
                or posixpath.normpath(path) != path or path in records
                or not re.fullmatch(r"[0-9]{1,5}", mode)):
            raise ValueError(f"Invalid/duplicate RPM payload record: {path!r}")
        mode = int(mode)
        if mode > 0o177777 or stat.S_IFMT(mode) not in (stat.S_IFREG, stat.S_IFDIR, stat.S_IFLNK):
            raise ValueError(f"Unexpected RPM file type: {path}")
        if not stat.S_ISLNK(mode) and target:
            raise ValueError(f"Unexpected link metadata on RPM non-symlink: {path}")
        records[path] = mode, target

    for path in records:
        parent = posixpath.dirname(path)
        while parent != "/":
            if parent in records and not stat.S_ISDIR(records[parent][0]):
                raise ValueError(f"RPM payload ancestor is not a directory: {parent}")
            parent = posixpath.dirname(parent)
    for path in ("/", "/usr", "/usr/bin", "/usr/share"):
        if path in records and not stat.S_ISDIR(records[path][0]):
            raise ValueError(f"RPM runtime root is not a directory: {path}")

    owned = {"/usr/bin/" + tool for tool in tools}
    for tool in owned:
        value = records.get(tool)
        if not value or not stat.S_ISREG(value[0]) or not value[0] & 0o111:
            raise ValueError(f"RPM tool is missing or not a regular executable: {tool}")

    hidden, directories, links = set(), set(), set()
    for path, (mode, target) in records.items():
        if path in ("/usr/lib", BUILD_IDS) or BUILD_DIRECTORY.fullmatch(path):
            if not stat.S_ISDIR(mode):
                raise ValueError(f"RPM build-ID ancestor is not a directory: {path}")
            directories.add(path)
        elif path.startswith(BUILD_IDS + "/"):
            match = BUILD_LINK.fullmatch(path)
            length = len(match[1]) + len(match[2]) if match else 0
            if not match or not 32 <= length <= 128 or length % 2 or not stat.S_ISLNK(mode):
                raise ValueError(f"Nonstandard RPM build-ID symlink: {path}")
            if link_destination(path, target) not in owned:
                raise ValueError(f"RPM build-ID link does not target an owned tool: {path} -> {target}")
            links.add(path)
        elif stat.S_ISLNK(mode):
            raise ValueError(f"Unexpected RPM runtime symlink outside build-ID metadata: {path}")
    for directory in directories:
        if not any(path.startswith(directory + "/") for path in links):
            raise ValueError(f"Unrelated/empty RPM build-ID directory: {directory}")
    hidden.update(directories)
    hidden.update(links)
    return [path for path in records if path not in hidden]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package")
    parser.add_argument("tools", nargs="+")
    args = parser.parse_args()
    if any(not re.fullmatch(r"[a-zA-Z0-9._+-]+", tool) for tool in args.tools):
        parser.error("Invalid owned tool name")
    try:
        listing = verified_listing(args.package, args.tools)
    except subprocess.CalledProcessError as error:
        print(f"RPM metadata query failed: {error.stderr.strip()}", file=sys.stderr)
        return 1
    except (ValueError, UnicodeError, OSError) as error:
        print(f"RPM layout metadata validation failed: {error}", file=sys.stderr)
        return 1
    print("\n".join(listing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
