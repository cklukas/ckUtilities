#!/usr/bin/env bash
# Complete registered frontend+embedded-backend tests, including native fixtures.
set -euo pipefail
source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
work=${CKTOOLS_CI_WORK_ROOT:?bootstrap must select fresh task storage}
native=${CKTOOLS_CI_NATIVE_ROOT:?explicit ownership-enforcing fixture parent required}
vision="$source_dir/.deps/ckvision"
backend="$source_dir/.deps/ck-core-fs-lib"
export CMAKE_BUILD_PARALLEL_LEVEL=1 CTEST_PARALLEL_LEVEL=1 MAKEFLAGS=-j1 ARCHIVE_BUILD_JOBS=1
export CPACK_THREADS=1 CPACK_ARCHIVE_THREADS=1
: "${TMPDIR:?bootstrap must set task temporary storage}"
cd "$source_dir"
python3 scripts/ci/verify-inputs.py --source "$source_dir" --vision "$vision" --backend "$backend" --report "$work/ci-inputs.json"
python3 scripts/ci/check-posix-fixture.py --root "$native" --backend "$backend" --report "$work/native-prerequisites.json"
for output in ckvision-sdk ckvision-build frontend-dev frontend-pkg-test frontend-pkg; do
  [[ ! -e "$work/$output" ]] || { echo "Refuse existing output: $work/$output" >&2; exit 1; }
done
# Native fixtures remain distinct from source/build storage. Mac build storage
# may be noowners, but this explicit native parent must actually enforce modes.
mkdir "$native/backend-dev" "$native/frontend-dev" "$native/backend-pkg" "$native/frontend-pkg"
# Explicit invocations are timed; nested CMake commands remain serial too.
run_stage() {
  local name=$1 started begin result
  shift
  started=$(date -u +%Y-%m-%dT%H:%M:%SZ); begin=$SECONDS
  if "$@"; then result=0; else result=$?; fi
  python3 - "$work/stages.jsonl" "$name" "$started" "$((SECONDS-begin))" "$result" <<'RECORD'
import datetime,json,sys
with open(sys.argv[1], 'a') as f:
    f.write(json.dumps(dict(stage=sys.argv[2], start=sys.argv[3], end=datetime.datetime.now(datetime.timezone.utc).isoformat(), seconds=int(sys.argv[4]), exit=int(sys.argv[5])))+'\n')
RECORD
  return "$result"
}
sdk="$work/ckvision-sdk"
run_stage sdk bash scripts/build_ckvision_sdk.sh "$vision" "$sdk" "$work/ckvision-build"
common=("-DCMAKE_PREFIX_PATH=$sdk" "-DCKTOOLS_CKVISION_PREFIX=$sdk"
  "-DCK_CORE_FS_LIB_SOURCE_DIR=$backend" -DCKTOOLS_VERIFY_CKVISION_PACKAGE=ON
  -DCPACK_THREADS=1 -DCPACK_ARCHIVE_THREADS=1
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF)
run_stage stage-1 cmake --preset dev -B "$work/frontend-dev" "${common[@]}" -DBUILD_TESTING:BOOL=ON \
  "-DCK_CORE_FS_NATIVE_TEST_ROOT=$native/backend-dev" \
  "-DCKTOOLS_TEST_TEMP_ROOT=$native/frontend-dev" \
  -DCKTOOLS_BUILD_DOCUMENTATION_CAPTURES=ON "-DCKTOOLS_CKVISION_SOURCE_DIR=$vision"
run_stage stage-2 cmake --build "$work/frontend-dev" --parallel 1
run_stage stage-3 cmake --build "$work/frontend-dev" --target verify_ckvision_package --parallel 1
run_stage stage-4 ctest --test-dir "$work/frontend-dev" --output-on-failure --parallel 1 --no-tests=error
mkdir -p docs/generated/screenshots
"$work/frontend-dev/tools/docgen/capture_ckutilities_screenshots" docs/generated/screenshots
"$work/frontend-dev/tools/docgen/capture_todo_screenshots" docs/generated/screenshots src/vision/todo/tests/golden
"$work/frontend-dev/tools/docgen/generate_sysmon_goldens" src/vision/sysmon/tests/golden
"$work/frontend-dev/tools/docgen/capture_sysmon_screenshots" docs/generated/screenshots
git diff --exit-code -- docs/generated/screenshots src/vision/todo/tests/golden src/vision/sysmon/tests/golden
test -z "$(git status --porcelain --untracked-files=all -- docs/generated/screenshots src/vision/todo/tests/golden src/vision/sysmon/tests/golden)"
prefix=()
if [[ "${RUNNER_OS:?}" == macOS ]]; then prefix+=(-DCMAKE_INSTALL_PREFIX=.); fi
run_stage release-tests-configure cmake --preset pkg -B "$work/frontend-pkg-test" "${common[@]}" "${prefix[@]}" \
  -DBUILD_TESTING:BOOL=ON \
  "-DCK_CORE_FS_NATIVE_TEST_ROOT=$native/backend-pkg" \
  "-DCKTOOLS_TEST_TEMP_ROOT=$native/frontend-pkg"
run_stage release-tests-build cmake --build "$work/frontend-pkg-test" --parallel 1
run_stage production-configure cmake --preset pkg -B "$work/frontend-pkg" "${common[@]}" "${prefix[@]}" \
  -DBUILD_TESTING:BOOL=OFF \
  -DCKTOOLS_VERIFY_CKVISION_INSTALL=ON -DCKTOOLS_VERIFY_CKVISION_ARCHIVE=ON -DCKTOOLS_VERIFY_CKVISION_TERMINAL=ON \
  "-DCKTOOLS_REHEARSAL_TEST_BINARY_DIR:PATH=$work/frontend-pkg-test"
run_stage production-build cmake --build "$work/frontend-pkg" --parallel 1
# Rehearsal runs all Release CTests in the paired test tree and verifies the OFF product.
run_stage production-rehearsal cmake --build "$work/frontend-pkg" --target verify_ckvision_rehearsal --parallel 1
run_stage production-terminal cmake --build "$work/frontend-pkg" --target verify_ckvision_terminal --parallel 1
