# ckVision baseline

This file records the clean ckVision package accepted for the first
ckUtilities migration slices. It is deliberately separate from the local
developer checkout path: production builds consume an installed package and
never a neighbouring source tree.

## Selected baseline

| Field | Value |
|---|---|
| Repository | ckVision |
| Clean foundation commit | `e40c9beee7b0be8450fd417aa0d55480418807bb` |
| Current migration commit | `91d2776a180d0773ad035bfcfff316a912d81265` (detached clean-worktree integration candidate) |
| Current migration change | `TreeView::reveal_and_select`, validated multiline `Memo` fields in declarative dialogs, command-safe normalized `KeyChord` capture, and checked `FlowView::replace_block` for clients that update one semantic rich-content block |
| Branch at foundation selection | `main` |
| Selection date | 2026-08-29 |
| CMake package target | `ckvision::cvision` |
| Client language level | C++20 |

The foundation commit was selected from a detached clean worktree. The current
migration commits were created in that same clean worktree after the JSON pilot
identified a reusable TreeView result-navigation gap, chat prompt editing
identified a reusable declarative-dialog multiline-field gap, shortcut
configuration identified a reusable command-safe key-capture gap, and
streaming rich content identified a reusable checked block-replacement gap.
All changes have unit coverage, rendering evidence where applicable, and public
documentation in ckVision. The ordinary ckVision checkout contained
unrelated staged, unstaged, and untracked work at the time of selection and is
not a valid dependency input.

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

To verify the staged native product as well, enable the installed-product
gate and run it from the configured build tree:

```sh
cmake --preset dev \
  -DCKTOOLS_VERIFY_CKVISION_INSTALL=ON
cmake --build build/dev --target verify_ckvision_install
```

This gate rebuilds the complete suite, installs it to a disposable prefix,
and starts each ckVision-native executable with `--help`. It verifies the
release artifact's executable layout and its relative runtime-library lookup;
it does not replace the full workflow or platform acceptance suites.

## Pin update rule

Raise this baseline only after the requested ckVision change is complete in
ckVision: its specification, tests, documentation, examples, benchmarks, and
provenance gates must be accepted there first. The current migration commit is
an integration candidate until it is landed on ckVision's trunk; do not treat
the dirty ordinary checkout as an alternative source of truth. After trunk
integration, update this table with the landed commit, run the independent
consumer verification, and record the affected migration/gap IDs in the
ckUtilities commit.
