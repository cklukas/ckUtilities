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
| Current migration commit | `9f097ecde219a35971ccde6f8732b07ef021c35b` (detached clean-worktree integration candidate) |
| Current migration change | `TreeView::reveal_and_select`, retained materialized visible rows, stable-ID provider-backed `TreeModel`, validated multiline `Memo` fields in declarative dialogs, command-safe normalized `KeyChord` capture, checked `FlowView::replace_block` with incremental realized-tail reflow, explicit `ApplicationShell::detach_desktop` lifecycle cleanup with liveness-guarded standard Help, and transactional `TextEditor::set_selection` for a current validated document range |
| Branch at foundation selection | `main` |
| Selection date | 2026-08-29 |
| CMake package target | `ckvision::cvision` |
| Client language level | C++20 |

The foundation commit was selected from a detached clean worktree. The current
migration commits were created in that same clean worktree after the JSON pilot
identified a reusable TreeView result-navigation gap, chat prompt editing
identified a reusable declarative-dialog multiline-field gap, shortcut
configuration identified a reusable command-safe key-capture gap, and
streaming rich content identified reusable checked block-replacement and
realized-tail reflow gaps. Materialized-tree scale checking also identified
repeated visible-row flattening on unchanged views; the current candidate
retains that projection and invalidates it when its roots or expansion state
changes. JSON and disk-usage adoption then justified the generic stable-ID
`TreeModel`; TreeView queries only visible paths while retaining expansion and
selection in the view. A controller teardown scenario also justified explicit
`ApplicationShell::detach_desktop` cleanup. The Markdown editor's generic
controller transaction needs then justified a public `TextEditor::set_selection`
operation: it refuses stale, invalid, or non-grapheme-aligned current-document
ranges before moving the selection and repainting the editor.
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

### Sanitizer SDKs

For an ASan/UBSan client build, create the SDK with ckVision's supported
package-aware option, not only with raw compiler flags:

```sh
cmake -S /path/to/clean/ckvision -B /path/to/ckvision-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCKVISION_SANITIZE=address,undefined
cmake --build /path/to/ckvision-sanitize --parallel
cmake --install /path/to/ckvision-sanitize --prefix /path/to/ckvision-sanitize-sdk
```

`CKVISION_SANITIZE` makes sanitizer compile and link options public usage
requirements of `ckvision::cvision`; an installed static library therefore
brings a client into the same instrumentation domain. A release SDK combined
with an independently sanitized client is not a valid sanitizer result. On
macOS, run ASan with `detect_leaks=0`, because that runtime does not support
leak detection. The clean `9f097ecde219a35971ccde6f8732b07ef021c35b`
integration candidate passed ckVision's full 169-test sanitizer suite and
ckUtilities' 83-test cutover configuration in both normal and ASan/UBSan
builds with this procedure on 2026-08-30.

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

## Cutover rehearsal

`CKTOOLS_CKVISION_CUTOVER=ON` builds only the framework-neutral cores and
ckVision-native executables. In that configuration the seven product binaries
use their production names (`ck-utilities`, `ck-json-view`, `ck-find`,
`ck-du`, `ck-config`, `ck-edit`, and `ck-chat`), and the build neither
configures nor installs the legacy UI runtime. It remains an explicit rehearsal
switch until this candidate is accepted on ckVision trunk and can be supplied
to CI as an installed package.

```sh
cmake -S . -B build/cutover \
  -DCKTOOLS_CKVISION_CUTOVER=ON \
  -DCKTOOLS_VERIFY_CKVISION_INSTALL=ON \
  -DCKTOOLS_CKVISION_PREFIX=/path/to/ckvision-sdk \
  -DCMAKE_PREFIX_PATH=/path/to/ckvision-sdk
cmake --build build/cutover
ctest --test-dir build/cutover --output-on-failure
cmake --build build/cutover --target verify_ckvision_cutover
```

The final command rebuilds and stages the product, verifies each production
binary's runtime linkage, and rejects any installed legacy runtime artifact or
legacy public-header reference. It is a negative gate for the cutover product,
not evidence that the remaining tracked legacy sources have been removed.

## Pin update rule

Raise this baseline only after the requested ckVision change is complete in
ckVision: its specification, tests, documentation, examples, benchmarks, and
provenance gates must be accepted there first. The current migration commit is
an integration candidate until it is landed on ckVision's trunk; do not treat
the dirty ordinary checkout as an alternative source of truth. After trunk
integration, update this table with the landed commit, run the independent
consumer verification, and record the affected migration/gap IDs in the
ckUtilities commit.
