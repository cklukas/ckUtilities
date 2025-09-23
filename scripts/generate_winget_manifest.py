#!/usr/bin/env python3
"""Generate Windows Package Manager (winget) manifests for ck-utilities.

The script emits the three-file manifest set (version, defaultLocale, installer)
into a directory structure compatible with a git-hosted winget source. The
output directory will contain `<publisher>/<package>/<version>/...` files so it
can be published as-is.
"""

from __future__ import annotations

import argparse
import pathlib
import textwrap

MANIFEST_VERSION = "1.5.0"

def manifest_paths(base: pathlib.Path, package_identifier: str, version: str, locale: str) -> dict[str, pathlib.Path]:
    publisher, package = package_identifier.split(".", 1)
    manifest_root = base / publisher / package / version
    manifest_root.mkdir(parents=True, exist_ok=True)
    stem = package_identifier
    return {
        "version": manifest_root / f"{stem}.yaml",
        "locale": manifest_root / f"{stem}.locale.{locale}.yaml",
        "installer": manifest_root / f"{stem}.installer.yaml",
    }

def write_manifest(path: pathlib.Path, contents: str) -> None:
    path.write_text(textwrap.dedent(contents).lstrip(), encoding="utf-8")

def build_version_manifest(args) -> str:
    return f"""
    PackageIdentifier: {args.package_identifier}
    PackageVersion: {args.version}
    DefaultLocale: {args.locale}
    ManifestType: version
    ManifestVersion: {MANIFEST_VERSION}
    """

def build_locale_manifest(args) -> str:
    license_line = f"License: {args.license}" if args.license else ""
    license_url_line = f"LicenseUrl: {args.license_url}" if args.license_url else ""
    short_description = args.short_description or "ck-utilities command-line toolkit"
    publisher_url_line = f"PublisherUrl: {args.publisher_url}" if args.publisher_url else ""
    return "\n".join(
        line for line in (
            f"PackageIdentifier: {args.package_identifier}",
            f"PackageVersion: {args.version}",
            f"PackageName: {args.package_name}",
            f"Publisher: {args.publisher}",
            publisher_url_line,
            license_line,
            license_url_line,
            f"ShortDescription: {short_description}",
            "ManifestType: defaultLocale",
            f"ManifestVersion: {MANIFEST_VERSION}",
        ) if line
    ) + "\n"

def build_installer_manifest(args) -> str:
    switch_entries: list[tuple[str, str]] = []
    if args.silent_switch:
        switch_entries.append(("Silent", args.silent_switch))
        switch_entries.append(("SilentWithProgress", args.silent_switch))
    if args.custom_switch:
        switch_entries.append(("Custom", args.custom_switch))

    installer_lines = [
        "Installers:",
        "  - Architecture: x64",
        f"    InstallerUrl: {args.installer_url}",
        f"    InstallerSha256: {args.installer_sha256}",
    ]

    if switch_entries:
        installer_lines.append("    InstallerSwitches:")
        installer_lines.extend(f"      {name}: {value}" for name, value in switch_entries)

    installer_lines.extend([
        "    NestedInstallerFiles:",
        "      - RelativeFilePath: cku-win-installer.exe",
    ])

    install_modes = "\n".join(["InstallModes:", "  - interactive", "  - silent"])

    installer_block = "\n".join(installer_lines)

    return "\n".join(line for line in (
        f"PackageIdentifier: {args.package_identifier}",
        f"PackageVersion: {args.version}",
        f"InstallerLocale: {args.locale}",
        "Platform:",
        "  - Windows.Desktop",
        "MinimumOSVersion: 10.0.0.0",
        "InstallerType: zip",
        "NestedInstallerType: exe",
        "Scope: machine",
        install_modes,
        installer_block,
        "ManifestType: installer",
        f"ManifestVersion: {MANIFEST_VERSION}",
    ) if line) + "\n"

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate winget manifests")
    parser.add_argument("--output", required=True, type=pathlib.Path, help="Directory to write manifests into")
    parser.add_argument("--package-identifier", required=True)
    parser.add_argument("--package-name", required=True)
    parser.add_argument("--publisher", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--installer-url", required=True)
    parser.add_argument("--installer-sha256", required=True)
    parser.add_argument("--locale", default="en-US")
    parser.add_argument("--license", default="")
    parser.add_argument("--license-url", default="")
    parser.add_argument("--publisher-url", default="")
    parser.add_argument("--short-description", default="")
    parser.add_argument("--silent-switch", default="")
    parser.add_argument("--custom-switch", default="")
    return parser.parse_args()

def main() -> None:
    args = parse_args()
    paths = manifest_paths(args.output, args.package_identifier, args.version, args.locale)
    write_manifest(paths["version"], build_version_manifest(args))
    write_manifest(paths["locale"], build_locale_manifest(args))
    write_manifest(paths["installer"], build_installer_manifest(args))

def cli() -> None:
    main()

if __name__ == "__main__":
    cli()
