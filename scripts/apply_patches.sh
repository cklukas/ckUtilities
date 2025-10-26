#!/usr/bin/env bash
set -euo pipefail

log() {
  printf '[patches] %s\n' "$1"
}

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

declare -a SEARCH_ROOTS=()
if [[ $# -eq 0 ]]; then
  SEARCH_ROOTS+=("$REPO_ROOT/build")
else
  for arg in "$@"; do
    if [[ -d "$arg" ]]; then
      SEARCH_ROOTS+=("$(cd "$arg" && pwd)")
    elif [[ -d "$REPO_ROOT/$arg" ]]; then
      SEARCH_ROOTS+=("$(cd "$REPO_ROOT/$arg" && pwd)")
    else
      log_path="$arg"
      [[ "$arg" != /* ]] && log_path="$REPO_ROOT/$arg"
      log "Warning: patch root not found, skipping: ${log_path}"
    fi
  done
fi

apply_tvision_ncurses_patch() {
  local patched_any=0
  local found_sources=0
  for root in "${SEARCH_ROOTS[@]}"; do
    [[ -d "$root" ]] || continue
    while IFS= read -r file; do
      [[ -z "$file" ]] && continue
      found_sources=1
      if grep -q 'set(_ncurses_include_base "${NCURSESW_INCLUDE}")' "$file"; then
        log "Turbo Vision ncurses include patch already applied: ${file#"$REPO_ROOT/"}"
        continue
      fi

      log "Applying Turbo Vision ncurses include patch: ${file#"$REPO_ROOT/"}"

      python3 - "$file" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()

if 'set(_ncurses_include_base "${NCURSESW_INCLUDE}")' in text:
    sys.exit(0)

old_block = """    find_path(NCURSESW_INCLUDE "ncursesw/ncurses.h")
    if (NCURSESW_INCLUDE)
        target_include_directories(${PROJECT_NAME} PRIVATE "${NCURSESW_INCLUDE}/ncursesw")
    else()
        find_path(NCURSESW_INCLUDE "ncurses.h")
        if (NCURSESW_INCLUDE)
            target_include_directories(${PROJECT_NAME} PRIVATE "${NCURSESW_INCLUDE}")
        else()
            tv_message(FATAL_ERROR "'ncursesw' development headers not found")
        endif()
    endif()
"""

new_block = """    find_path(NCURSESW_INCLUDE "ncursesw/ncurses.h")
    if (NCURSESW_INCLUDE)
        set(_ncurses_include_base "${NCURSESW_INCLUDE}")
        if (_ncurses_include_base MATCHES "/ncursesw$")
            get_filename_component(_ncurses_include_base "${_ncurses_include_base}" DIRECTORY)
        endif()
        target_include_directories(${PROJECT_NAME} PRIVATE
            "${_ncurses_include_base}"
            "${_ncurses_include_base}/ncursesw")
    else()
        find_path(NCURSESW_INCLUDE "ncurses.h")
        if (NCURSESW_INCLUDE)
            target_include_directories(${PROJECT_NAME} PRIVATE "${NCURSESW_INCLUDE}")
        else()
            tv_message(FATAL_ERROR "'ncursesw' development headers not found")
        endif()
    endif()
"""

if old_block not in text:
    sys.stderr.write(f"Expected Turbo Vision block not found in {path}\n")
    sys.exit(1)

text = text.replace(old_block, new_block, 1)

if 'unset(_ncurses_include_base)' not in text:
    marker = "    endif()\n\n    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_NCURSES)\n"
    if marker not in text:
        sys.stderr.write(f"Unable to locate insertion point for unset() in {path}\n")
        sys.exit(1)
    text = text.replace(
        "    endif()\n\n    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_NCURSES)\n",
        "    endif()\n\n    unset(_ncurses_include_base)\n\n    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_NCURSES)\n",
        1,
    )

path.write_text(text)
PY
      patched_any=1
    done < <(find "$root" -path "*/_deps/tvision-src/source/CMakeLists.txt" 2>/dev/null || true)
  done

  if [[ $found_sources -eq 0 ]]; then
    log "Turbo Vision sources not present yet; patch will be applied after configure."
  elif [[ $patched_any -eq 0 ]]; then
    log "Turbo Vision patch already up to date in scanned build directories."
  fi
}

apply_tvision_ncurses_patch

apply_tvision_shift_mouse_patch() {
  local patched_any=0
  local found_sources=0
  for root in "${SEARCH_ROOTS[@]}"; do
    [[ -d "$root" ]] || continue
    while IFS= read -r file; do
      [[ -z "$file" ]] && continue
      found_sources=1
      log "Applying Turbo Vision mouse modifier patch: ${file#"$REPO_ROOT/"}"

      python3 - "$file" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()

if "mmShift = 0x04" in text and "mmShift | mmAlt | mmCtrl" in text and "kbShift" in text:
    sys.exit(0)

old_const = "const ushort\n    mmAlt = 0x08,\n    mmCtrl = 0x10;\n"
new_const = "const ushort\n    mmShift = 0x04,\n    mmAlt = 0x08,\n    mmCtrl = 0x10;\n"

if old_const in text:
    text = text.replace(old_const, new_const, 1)
elif "mmShift = 0x04" not in text:
    sys.stderr.write(f"Unable to locate mouse modifier constants in {path}\n")
    sys.exit(1)

mod_old = "uint mod = butm & (mmAlt | mmCtrl);"
if mod_old in text:
    text = text.replace(mod_old, "uint mod = butm & (mmShift | mmAlt | mmCtrl);")

if "(mmAlt | mmCtrl)) - 32;" in text:
    text = text.replace("(mmAlt | mmCtrl)) - 32;", "(mmShift | mmAlt | mmCtrl)) - 32;", 1)

if "uint but = butm & ~(mmAlt | mmCtrl);" in text:
    text = text.replace("uint but = butm & ~(mmAlt | mmCtrl);", "uint but = butm & ~(mmShift | mmAlt | mmCtrl);", 1)

state_old = "ev.mouse.controlKeyState = (-!!(mod & mmAlt) & kbLeftAlt) | (-!!(mod & mmCtrl) & kbLeftCtrl);"
state_new = "ev.mouse.controlKeyState = (-!!(mod & mmShift) & kbShift) | (-!!(mod & mmAlt) & kbLeftAlt) | (-!!(mod & mmCtrl) & kbLeftCtrl);"
if state_old in text:
    text = text.replace(state_old, state_new)

if "mmShift = 0x04" not in text or "mmShift | mmAlt | mmCtrl" not in text or "kbShift" not in text:
    sys.stderr.write(f"Turbo Vision mouse patch failed for {path}\n")
    sys.exit(1)

path.write_text(text)
PY
      patched_any=1
    done < <(find "$root" -path "*/_deps/tvision-src/source/platform/termio.cpp" 2>/dev/null || true)
  done

  if [[ $found_sources -eq 0 ]]; then
    log "Turbo Vision sources not present yet; mouse modifier patch will be applied after configure."
  elif [[ $patched_any -eq 0 ]]; then
    log "Turbo Vision mouse modifier patch already up to date in scanned build directories."
  fi
}

apply_tvision_shift_mouse_patch
