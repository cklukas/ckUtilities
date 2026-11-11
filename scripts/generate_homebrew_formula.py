#!/usr/bin/env python3
"""Generate the ckUtilities Homebrew formula for one immutable release."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import tarfile
import textwrap


LLAMA_CPP_URL = (
    "https://github.com/ggerganov/llama.cpp/archive/"
    "0124ac989f7e7bf08803788f66dbe4106bdcdd58.tar.gz"
)
LLAMA_CPP_SHA256 = "8cbda24890f30e5f80c522948b116498345a9ecaf5926d2ed2a700f6bbc3c944"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-url", required=True)
    parser.add_argument("--source-sha256", required=True)
    parser.add_argument("--homepage", required=True)
    parser.add_argument("--ci-inputs", required=True, type=pathlib.Path)
    parser.add_argument("--ckvision-archive", required=True, type=pathlib.Path)
    parser.add_argument("--backend-archive", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    return parser.parse_args()


def verified_resource(key, repository, archive_path, report, pins):
    revision = report["revisions"][key]
    if not re.fullmatch(r"[0-9a-f]{40}", revision) or revision != pins[key]:
        raise ValueError(f"{repository} checkout does not match the release source pin")
    hashes = report["source_asset_sha256"][key]
    modes = report["source_asset_git_mode"][key]
    if not hashes or set(hashes) != set(modes) or any(
        mode not in {"100644", "100755", "120000"} for mode in modes.values()
    ):
        raise ValueError(f"{repository} archive needs a complete Git file inventory")

    prefix = f"{repository}-{revision}"
    directories = {prefix}
    for name in hashes:
        directories.update(str(parent) for parent in (pathlib.PurePosixPath(prefix) / name).parents
                           if parent != pathlib.PurePosixPath("."))
    seen = set()
    verified = set()
    with archive_path.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
        expected_digest = pins.get(f"{key}_archive_sha256")
        if expected_digest is not None and digest != expected_digest:
            raise ValueError(f"{repository} archive SHA-256 differs from the release source pin")
        source.seek(0)
        with tarfile.open(fileobj=source, mode="r:gz", ignore_zeros=True) as archive:
            for member in archive:
                name = member.name
                if name in seen or name.split("/")[0] != prefix:
                    raise ValueError(f"Unexpected {repository} archive entry {name!r}")
                seen.add(name)
                if member.isdir() and name in directories:
                    continue

                relative = name.removeprefix(prefix + "/")
                if relative not in hashes:
                    raise ValueError(f"Untracked {repository} archive entry {name!r}")
                mode = modes[relative]
                if mode == "120000" and member.issym():
                    actual = hashlib.sha256(os.fsencode(member.linkname)).hexdigest()
                elif mode in {"100644", "100755"} and member.type in (tarfile.REGTYPE, tarfile.AREGTYPE):
                    executable = mode == "100755"
                    if (bool(member.mode & 0o100) != executable or member.mode & 0o7000 or
                            (not executable and member.mode & 0o011)):
                        raise ValueError(f"{repository} archive mode differs for {relative!r}")
                    with archive.extractfile(member) as data:
                        actual = hashlib.file_digest(data, "sha256").hexdigest()
                else:
                    raise ValueError(f"{repository} archive type differs for {relative!r}")
                if actual != hashes[relative]:
                    raise ValueError(f"{repository} archive bytes differ for {relative!r}")
                verified.add(relative)
        if verified != set(hashes):
            raise ValueError(f"{repository} archive is missing files from the pinned checkout")
        source.seek(0)
        if hashlib.file_digest(source, "sha256").hexdigest() != digest:
            raise ValueError(f"{repository} archive changed during verification")
    return f"https://github.com/cklukas/{repository}/archive/{revision}.tar.gz", digest


def main() -> None:
    args = parse_args()
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", args.version):
        raise SystemExit("version must be numeric semantic-version text without a leading 'v'")
    if not args.source_url.startswith("https://"):
        raise SystemExit("source URL must use HTTPS")
    if "/archive/" not in args.source_url or "/refs/heads/" in args.source_url:
        raise SystemExit("source URL must be an immutable archive tag or commit, not a branch")
    if not re.fullmatch(r"[0-9a-fA-F]{64}", args.source_sha256):
        raise SystemExit("source SHA-256 must be a 64-character hexadecimal digest")
    pins = json.loads((pathlib.Path(__file__).resolve().parent / "ci/dependency-pins.json").read_text())
    report = json.loads(args.ci_inputs.read_text())
    try:
        ckvision_url, ckvision_sha256 = verified_resource("ckvision", "ckVision", args.ckvision_archive, report, pins)
        backend_url, backend_sha256 = verified_resource("backend", "ck-core-fs-lib", args.backend_archive, report, pins)
    except (KeyError, OSError, ValueError, tarfile.TarError) as error:
        raise SystemExit(f"Homebrew resource verification failed: {error}") from error

    formula = f'''\
    class CkUtilities < Formula
      desc "ckVision-native terminal utility suite"
      homepage "{args.homepage}"
      url "{args.source_url}"
      version "{args.version}"
      sha256 "{args.source_sha256.lower()}"
      license "GPL-3.0-or-later"

      resource "ckvision" do
        url "{ckvision_url}"
        sha256 "{ckvision_sha256}"
      end

      resource "ck_core_fs" do
        url "{backend_url}"
        sha256 "{backend_sha256}"
      end

      resource "llama_cpp" do
        url "{LLAMA_CPP_URL}"
        sha256 "{LLAMA_CPP_SHA256}"
      end

      depends_on "cmake" => :build
      depends_on "ninja" => :build
      depends_on "pkg-config" => :build
      depends_on "curl"
      depends_on "nlohmann-json"

      def install
        ENV["CMAKE_BUILD_PARALLEL_LEVEL"] = "1"
        ENV["CTEST_PARALLEL_LEVEL"] = "1"
        ENV["ARCHIVE_BUILD_JOBS"] = "1"
        ENV["MAKEFLAGS"] = "-j1"
        resource("ck_core_fs").stage buildpath/"ck-core-fs-lib"
        resource("ckvision").stage buildpath/"ckvision"
        ckvision_prefix = buildpath/"ckvision-sdk"
        system "cmake", "-S", "ckvision", "-B", "ckvision-build", "-G", "Ninja",
               "-DCMAKE_BUILD_TYPE=Release",
               "-DCKVISION_BUILD_EXAMPLES=OFF",
               "-DCKVISION_BUILD_TESTING=OFF",
               "-DCKVISION_BUILD_FUZZERS=OFF",
               "-DCMAKE_INSTALL_PREFIX=#{{ckvision_prefix}}"
        system "cmake", "--build", "ckvision-build", "--parallel", "1"
        system "cmake", "--install", "ckvision-build"

        resource("llama_cpp").stage buildpath/"llama_cpp"
        llama_source = buildpath/"llama_cpp"
        unless (llama_source/"CMakeLists.txt").exist?
          llama_source = llama_source.children.find do |candidate|
            (candidate/"CMakeLists.txt").exist?
          end
        end
        odie "llama.cpp resource did not contain CMakeLists.txt" unless llama_source
        curl_prefix = Formula["curl"].opt_prefix
        json_prefix = Formula["nlohmann-json"].opt_prefix
        system "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
           "-DCMAKE_BUILD_TYPE=Release",
           "-DCK_CORE_FS_LIB_SOURCE_DIR=#{{buildpath}}/ck-core-fs-lib",
           "-DCKTOOLS_CKVISION_PREFIX=#{{ckvision_prefix}}",
           "-DBUILD_TESTING=OFF",
           "-DLLAMA_BUILD_TESTS=OFF",
           "-DLLAMA_BUILD_EXAMPLES=OFF",
           "-DCMAKE_PREFIX_PATH=#{{ckvision_prefix}};#{{curl_prefix}};#{{json_prefix}}",
           "-DCK_LLAMA_CPP_SOURCE_DIR=#{{llama_source}}",
               *std_cmake_args
        system "cmake", "--build", "build", "--parallel", "1"
        # Only the product: the executables, documentation, license and
        # sample configuration. Development headers and libraries, ours and
        # the bundled llama.cpp's, would collide with Homebrew's own
        # nlohmann-json and llama.cpp when the keg is linked.
        system "cmake", "--install", "build", "--component", "Runtime"
      end

      test do
        %w[ck-utilities ck-json-view ck-find ck-du ck-files ck-config ck-edit ck-chat ck-todo ck-sysmon].each do |tool|
          assert_match "Usage:", shell_output("#{{bin}}/#{{tool}} --help")
        end
      end
    end
    '''
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(textwrap.dedent(formula), encoding="utf-8")


if __name__ == "__main__":
    main()
