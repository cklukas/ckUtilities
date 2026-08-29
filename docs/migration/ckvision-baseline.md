# ckVision baseline

This file records the clean ckVision package accepted for the first
ckUtilities migration slices. It is deliberately separate from the local
developer checkout path: production builds consume an installed package and
never a neighbouring source tree.

## Selected baseline

| Field | Value |
|---|---|
| Repository | ckVision |
| Commit | `e40c9beee7b0be8450fd417aa0d55480418807bb` |
| Branch at selection | `main` |
| Selection date | 2026-08-29 |
| CMake package target | `ckvision::cvision` |
| Client language level | C++20 |

The commit was selected from a detached clean worktree. The ordinary ckVision
checkout contained unrelated staged, unstaged, and untracked work at the time
of selection and is not a valid dependency input.

## Local SDK workflow

Build and install the exact selected source revision to a disposable prefix:

```sh
cmake -S /path/to/clean/ckvision -B /path/to/ckvision-build -DCMAKE_BUILD_TYPE=Release
cmake --build /path/to/ckvision-build --parallel
cmake --install /path/to/ckvision-build --prefix /path/to/ckvision-sdk
```

Verify the consumer from the ckUtilities build tree:

```sh
cmake --preset dev \
  -DCKTOOLS_VERIFY_CKVISION_PACKAGE=ON \
  -DCKTOOLS_CKVISION_PREFIX=/path/to/ckvision-sdk
cmake --build build/dev --target verify_ckvision_package
```

The verifier configures, builds, and runs a separate CMake project under
`tests/integration/ckvision_consumer`. Passing it proves the public installed
package is consumable; it does not substitute for ckVision's own acceptance
suite or an application migration slice.

## Pin update rule

Raise this baseline only after the requested ckVision change is complete in
ckVision: its specification, tests, documentation, examples, benchmarks, and
provenance gates must be accepted there first. Update this table, run the
independent consumer verification, and record the affected migration/gap IDs
in the ckUtilities commit.
