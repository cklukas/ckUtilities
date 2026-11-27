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
| Recursive search | Integrated in-process recursive handle-relative name (glob/substring/regex) and content search (`files_search_service.hpp/.cpp`) with binary skipping, symlink-loop refusal, cancellation, and search-within-results (`Alt+F7`, `ck.files.search`) | `ck-files`; CP-07 implemented | `ck_vision_files_parity_tests` (CP-07) + standalone `ck_vision_find_tests` |
| Directory tree, hotlist, history | Back/forward history, bookmarks, swap panes, sync other pane, and in-process `ck_du_core` directory tree & sizes (`Alt+T`, `ck.files.directory_tree`) | `ck-files`; CP-07 implemented | `ck_vision_files_parity_tests` (CP-07) + standalone `ck_vision_du_tests` |
| File/directory comparison and synchronization | Quick size/time and thorough SHA-256 directory compare & reviewed synchronization (`files_compare_sync_service.hpp/.cpp`: `CompareOnly`, `OneWay*`, `TwoWayCopyOnly` with no inferred deletions and conflict preservation) plus Text/Binary file diff (`ck.files.diff_files`, `ck.files.sync_directories`) | `ck-files`; CP-08 implemented | `ck_vision_files_parity_tests` (CP-08) |
| Ordinary archive browse/extract/create | In-process ZIP (Store + Deflate via zlib), TAR (ustar + PAX long names), and TAR.GZ (`files_ordinary_archive.hpp/.cpp`) with traversal/link-escape/collision/bomb guards, staged atomic create/modify, and explicit `CKARV01` separation (`ck.files.archive_browse`, `ck.files.archive_create`, `ck.files.archive_extract`) | `ck-files`; CP-09 implemented | `ck_vision_files_parity_tests` (CP-09) |
| Volume archive restore | `CKARV01` v2 inspector (`inspect_volume_archive_file`) and verified restore (`plan_volume_archive_restore`, `restore_volume_archive`) with raw-device/system-root refusal, sparse hole preservation, hard-link/symlink restore, and SHA-256 readback verification (`ck.files.inspect_volume_archive`, `ck.files.restore_volume_archive`) | `ck-files`; CP-13 implemented | `ck_vision_files_parity_tests` (CP-13) |
| Remote VFS / SMB / NFS / package providers | Bounded provider capability model (`files_remote_provider.hpp/.cpp`), SFTP (`BatchMode=yes`, `StrictHostKeyChecking`), mounted SMB/UNC distinction, mandatory credential redaction, and uncertain-commit fail-closed retry guard (`ck.files.connect_remote`); direct unmounted POSIX SMB protocol client remains explicitly unsupported | `ck-files`; CP-12 implemented | `ck_vision_files_parity_tests` (CP-12) |
| Execute / command menus / user macros | Direct-argv execution (filenames as data, zero shell injection), explicit opt-in shell mode, reviewed user menu placeholders (`%f/%p/%d/%D/%F/%P`), command history, and bounded `NativeProcessRunner` execution (`ck.files.run_command`, `ck.files.user_menu`) | `ck-files`; CP-11 implemented | `ck_vision_files_parity_tests` (CP-11) |
| Embedded viewer/editor | In-app ckVision `TextEditor` + `EditorDocument` + shared `ck-edit` Markdown & standard syntax profiles (`files_embedded_editor.hpp/.cpp`, `Ctrl+E`, `ck.files.edit`, `ck.files.new_edit`) with open-time identity/SHA-256 external-conflict detection, CRLF preservation, staged atomic save, plus bounded Text/Hex/JSON streaming viewer & stream search | `ck-files`; CP-10 implemented | `ck_vision_files_parity_tests` (CP-10) + standalone `ck_vision_edit_tests` |
| Terminal behavior | In-app ckVision `TerminalView` + `Application::launch_terminal_subsession` PTY/ConPTY interactive shell (`Ctrl+O`, `ck.files.terminal`) with `parent_escape` return-to-panes and post-command pane refresh/invalidation | `ck-files`; CP-11/CP-14 implemented | `ck_vision_files_parity_tests` (CP-11/CP-14) |
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

