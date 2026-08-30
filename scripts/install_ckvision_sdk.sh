#!/usr/bin/env bash
set -euo pipefail

# Download and verify an immutable ckVision SDK archive for CI.  The archive
# must contain an installed CMake package, never a source checkout.
if [[ $# -ne 1 ]]; then
  echo "usage: $0 <destination>" >&2
  exit 64
fi

sdk_url="${CKVISION_SDK_URL:?CKVISION_SDK_URL is required}"
sdk_sha256="${CKVISION_SDK_SHA256:?CKVISION_SDK_SHA256 is required}"
destination="$1"

if [[ ! "$sdk_url" =~ ^https:// ]]; then
  echo "CKVISION_SDK_URL must use HTTPS" >&2
  exit 64
fi
if [[ ! "$sdk_sha256" =~ ^[[:xdigit:]]{64}$ ]]; then
  echo "CKVISION_SDK_SHA256 must be a SHA-256 digest" >&2
  exit 64
fi

archive="${destination}.tar.gz"
rm -rf "$destination"
mkdir -p "$destination"
trap 'rm -f "$archive"' EXIT

curl --fail --location --retry 3 --retry-all-errors --silent --show-error \
  --output "$archive" "$sdk_url"

actual_sha256="$(shasum -a 256 "$archive" | awk '{print $1}')"
if [[ "${actual_sha256,,}" != "${sdk_sha256,,}" ]]; then
  echo "ckVision SDK checksum mismatch" >&2
  exit 65
fi

tar -xzf "$archive" -C "$destination"
config_path="$(find "$destination" -type f -path '*/lib/cmake/ckvision/ckvisionConfig.cmake' -print -quit)"
if [[ -z "$config_path" ]]; then
  echo "ckVision SDK does not contain lib/cmake/ckvision/ckvisionConfig.cmake" >&2
  exit 65
fi

sdk_prefix="$(dirname "$config_path")"
sdk_prefix="$(dirname "$sdk_prefix")"
sdk_prefix="$(dirname "$sdk_prefix")"
sdk_prefix="$(dirname "$sdk_prefix")"
printf '%s\n' "$sdk_prefix"
