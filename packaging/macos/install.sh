#!/usr/bin/env bash
set -euo pipefail

PREFIX="${1:-/usr/local}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BIN_SOURCE="${ROOT_DIR}/bin/ckjsonview"
LICENSE_SOURCE="${ROOT_DIR}/share/licenses/ck-utilities/LICENSE"
DOC_DIR="${ROOT_DIR}/share/doc/ck-utilities"

if [[ ! -x "${BIN_SOURCE}" ]]; then
  echo "ckjsonview binary not found at ${BIN_SOURCE}" >&2
  exit 1
fi

install -d "${PREFIX}/bin"
install -m 0755 "${BIN_SOURCE}" "${PREFIX}/bin/ckjsonview"

if [[ -f "${LICENSE_SOURCE}" ]]; then
  install -d "${PREFIX}/share/licenses/ck-utilities"
  install -m 0644 "${LICENSE_SOURCE}" "${PREFIX}/share/licenses/ck-utilities/LICENSE"
fi

if [[ -d "${DOC_DIR}" ]]; then
  install -d "${PREFIX}/share/doc/ck-utilities"
  find "${DOC_DIR}" -maxdepth 1 -type f -print0 | while IFS= read -r -d '' doc; do
    install -m 0644 "${doc}" "${PREFIX}/share/doc/ck-utilities/$(basename "${doc}")"
  done
fi

echo "ckjsonview installed to ${PREFIX}/bin."
