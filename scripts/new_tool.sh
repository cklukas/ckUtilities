#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <tool-name> <description>" >&2
  exit 1
fi

tool_name="$1"
description="$2"

cat <<MSG
Scaffolding for ${tool_name} is not automated yet.
Please create src/tools/${tool_name}/ and add a docs/tools/${tool_name}.md entry.
MSG
