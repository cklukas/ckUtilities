# Files — Commander parity inventory

Status: CP-00 review record, refreshed 2026-11-02. This is an implementation inventory,
not a claim of feature-complete Norton Commander or Midnight Commander parity.
Every row is either implemented and qualified, in a named follow-on package,
or explicitly out of scope/blocked. A disabled operation remains disabled when
the native adapter cannot prove the required guarantee.

## Reference and ownership

The target interaction is the keyboard-first two-pane workflow used by Norton
Commander and Midnight Commander, adapted to ckUtilities terminology and
ckVision controls. Product ownership remains in `ck-files`; reusable editor,
syntax, terminal and shell behavior belongs in ckVision or shared ckUtilities
components. A sibling utility executable is never used as an in-process
implementation.

The pinned public comparison reference is GNU Midnight Commander **4.8.33**:
the [official release announcement](https://lists.midnight-commander.org/pipermail/mc/2025-January/005798.html)
is the 2025-01 release notice, and the [official online `mc(1)`
manual](https://source.midnight-commander.org/man/mc.html) was captured on
2026-11-02. The manual is used
for the two-panel model, panel modes/columns, selection, navigation, menus,
copy/move/view/edit, search, hotlist/history, archives/VFS and terminal
workflows. The repository contains no licensed Norton Commander 5.x manual or
other lawful reference asset. Therefore this inventory does not claim
exhaustive Norton Commander parity; a licensed NC reference remains an open
CP-00 input and every NC-specific row stays explicitly mapped or deferred.

| Reusable seam | Verified public/current consumer | Ownership and boundary |
|---|---|---|
| Application, command registry, menus, scopes, dialogs and standard roles | `src/vision/files/files_app.cpp`, `src/vision/find/find_app.hpp`, `src/vision/du/disk_usage_app.hpp` use ckVision UI APIs | ckVision owns dispatch/focus/presentation primitives; Files owns `ck.files.*` commands and operation availability |
| Completion and editor Right-to-accept | `src/vision/files/files_location_dialog.cpp` uses `cvision/widgets/completion.hpp`; `files_app_tests.cpp` covers exact proposal acceptance | ckVision owns completion widget behavior; Files owns native location query, revision/ticket validation and route commit |
| Native terminal and host lifecycle | `src/vision/host/native_host.hpp`, `src/vision/files/main.cpp`, `src/vision/tests/terminal_smoke.cpp` | ckVision/host owns terminal mode and lifecycle; Files owns one application/session and joins workers outside dispatch |
| Text editor/document | `src/vision/todo/todo_app.cpp` uses `cvision/widgets/editor_document.hpp` and `text_editor.hpp` | reusable editor behavior is a candidate for CP-10; Files does not duplicate or launch `ck-edit` |
| Search services | `src/vision/find/find_services.hpp/.cpp`, `ck_find_core` | CP-07 must extract/use the service in-process; no sibling executable shell-out |
| Disk-usage/tree services | `src/vision/du/disk_usage_services.hpp/.cpp`, `ck_du_core` | CP-07 may reuse bounded discovery/tree logic; Files retains native identity and mutation rules |
| Filesystem identity, preservation, journals and recovery | `src/native-core/files`, `src/vision/files/files_operation_*` | ckUtilities owns native adapters and safety protocol; ckVision filesystem APIs are discovery/presentation inputs, not transactional mutation guarantees |

| Area | Current implementation | Owner / next package | Acceptance evidence |
|---|---|---|---|
| Two panes, locations, sorting, filtering and marks | Implemented in the Files app and pane model; navigation/completion is worker-owned | `ck-files`; CP-00/CP-04 for real-window limits | Files suite and generated ck-files guide |
| Above-root volumes/mounts | Implemented as a pane-local volume view with mount/letter, filesystem, size, free and read-only columns | `ck-files`; CP-04 provider matrix | `ck-files-volumes.svg`, volume-source tests |
| Copy volume to a folder | Implemented as CKARV01 sparse archive planning/execution with explicit capability refusal | `ck-files`; CP-13 restore and provider completion | archive format/allocation/journal evidence |
| Ordinary file/tree copy | Implemented through frozen inventory, loss review, no-replace staging/publication and verification | `ck-files`; CP-04 filesystem matrix | operation and native Files suites |
| Same-volume rename/move | Implemented with identity-checked native no-replace rename | `ck-files`; CP-04 race/crash matrix | move and native adapter tests |
| Cross-volume file/tree move | Windows writer-open refusal is qualified. The current regular-file adapter still requires a source-stability proof that covers reviewed metadata. Recursive moves retain the original after publication. POSIX retirement needs a journaled capture protocol and source-stability proof. | `ck-files`; CP-01 | operation status explains refusal where unsupported |
| Recursive permanent deletion | Journaled worker freezes the tree, revalidates identities and retires bottom-up; the Windows directory command path is qualified, while POSIX recursive retirement remains disabled until its adapter protocol is qualified | `ck-files`; CP-02/CP-04 | Windows 223-test matrix and native deletion test; macOS/Linux safety-retention evidence |
| Native Trash / Recycle Bin | Single-entry native disposition with restore metadata; recursive Trash remains disabled and never falls back to permanent deletion | `ck-files`; CP-02 for recursive/foreign mounts | native Trash tests; recursive Trash acceptance still open |
| Create files and links | New empty file and identity-bound same-pane hard-link actions are exposed; qualified POSIX symbolic-link creation is exposed with an opaque-target dialog; read-only link/reparse inspection is worker-owned and bounded; Windows reparse publication, junctions and cross-pane hard-link workflow remain open | `ck-files`; CP-05 | CP-05 empty-file, hard-link, symbolic-link and link-inspection qualification; remaining link operations stay disabled with status reasons |
| Attributes, permissions, ownership and ACLs | Read-only capability/metadata inventory and qualified move/copy preservation decisions | `ck-files`; CP-05 | metadata/security tests |
| Selection masks and batch rename/copy/move | Marked batches and destination-name planning exist; glob/regex masks and rename previews remain open | `ck-files`; CP-06 | destination batch/name tests |
| Recursive search | Existing `ck-find` service and UI are not yet integrated into Files | shared search service; CP-07 | `ck-find` suite; no Files parity claim |
| Directory tree, hotlist, history | Basic parent navigation exists; tree/hotlist/history workflows remain open | `ck-files`; CP-07 | WP-02 navigation tests; CP-07 acceptance pending |
| File/directory comparison and synchronization | Not yet exposed as a reviewed Files workflow | shared comparison service; CP-08 | no acceptance evidence |
| Ordinary archive browse/extract/create | Volume archive writer is separate and safety-qualified; ordinary archive provider is not yet integrated | `ck-files`; CP-09 | CKARV01 evidence only |
| Volume archive restore | Archive inspection and recovery boundaries exist; verified restore is not enabled | `ck-files`; CP-13 | CP-13 required |
| Remote VFS / SMB / NFS / package providers | Mounted locations are observed through native adapters; provider-specific semantics are not advertised | provider model; CP-12 and CP-04 | provider matrix required |
| Execute / command menus / user macros | Command registry and dynamic availability are integrated; terminal/execution parity is incomplete | shared shell/terminal; CP-11 | terminal profiles and help smoke only |
| Embedded viewer/editor | ckVision completion/right-to-accept integration is present; full editor command parity is not claimed | ckVision/shared components; CP-10 | ckVision consumer and Files tests |
| Terminal behavior | In-process ckVision terminal path is integrated and Linux/macOS profiles are qualified where recorded | ckVision/shared terminal; CP-11/CP-14 | terminal rehearsal evidence |
| Documentation and screenshots | Utility guides contain feature lists and deterministic real-app screenshots, including the volume view | docs/docgen; WP-08 | documentation contract and screenshot assets |

## Peer-review corrections and dependencies

The review found four safety-sensitive points that must remain explicit:

1. POSIX cross-volume move must not use a pathname unlink, advisory lock or
   unchanged timestamp as a source-stability proof. It remains disabled until
   a same-filesystem private quarantine protocol and its source-stability
   proof are qualified on macOS and Linux. Windows handle-bound sharing
   denial has separate open-writer evidence.
2. Recursive deletion and recursive Trash are distinct operations. Neither may
   traverse links, mounts or newly appeared children, and neither may be
   implemented by calling the direct-child primitive repeatedly without a
   frozen inventory and per-item identity checks.
3. Volume archive recovery and ordinary file-operation recovery remain separate
   protocols. A recovery record with an ambiguous publication or source receipt
   is inspect-only; it may not trigger pathname cleanup or source retirement.
4. CP-04 is a repeated gate, not a one-time checkbox. New providers, archive
   restore, editor saves and terminal workflows each add their own filesystem,
   crash, resource and real-window evidence requirements.

## Package order after CP-00

The next implementation order is CP-01 source retirement and cross-volume
moves, CP-02 recursive deletion/Trash, CP-03 usable recovery, and CP-04
qualification. Only after those P0 packages are accepted do CP-05–CP-14 add
new Commander features. Existing WP-07/WP-08 release checkboxes stay open until
their actual native matrices, sanitizer/resource tests, screenshots, packaging
and real-window acceptance are complete.

