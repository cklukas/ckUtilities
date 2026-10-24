# Files — WP-01 acceptance review

Reviewed 2026-10-24 at `acf7d7e775007ab393914274c245e70d41edbb90` (the complete commit is in the evidence record).
Result: **WP-01 complete**. WP-02 remains unstarted. This is acceptance of the
domain model and native inspection services, not release or mutation acceptance.

## Requirements and evidence

The requirements below come from [WP-01 in the implementation plan](ck-files-plan.md#wp-01--model-and-native-services).
The [machine-readable review](evidence/ck-files-wp01-acceptance-20261024.json)
checks the current source bytes against the three-platform validation snapshot,
matches the 82 registered suites to their executed cases, and records each
requirement's implementation and test files.

| WP-01 requirement | Implemented behavior and proof |
| --- | --- |
| Strict core/platform targets, injected filesystem/time, identities, fake adapters and resource ownership | `ck_files_core` and `ck_files_platform` use strict warnings. `BasicFileSystem`, held cursors/entries and `MonotonicClock` are injected boundaries. Scoped identities, the filesystem/inventory simulator and manual clock exercise native failures, cancellation, exceptions and retirement. Core sanitizer builds compile without linking a UI library; the UI boundary check passes. |
| Entry classification and lossless paths | Native metadata distinguishes files, directories, final links, junctions, opaque reparse objects and POSIX special objects. `NativeName` and `NativeAbsolutePath` retain POSIX bytes or Windows UTF-16. Tests cover malformed UTF-8, every unpaired surrogate, controls, roots, UNC/extended namespaces, invalid addresses and bounds. Display escaping never supplies a native target. |
| Metadata and stream inventory | Held-object queries collect basic fields, storage flags/compression, integrity, inode flags, data layout, links and link limits, channel catalogs, attribute names/values and the six security query parts. `ObjectInventoryJob` collects them cooperatively with explicit limits and ownership checks. Native fixtures include Windows ADS/EAs/security/reparse data, macOS resource forks/xattrs/ACLs and Linux xattrs/ACLs/extents/fscrypt. Each query's coverage remains explicit. |
| Native identity and permissions | Descriptor/handle-relative observations retain exact native owners across rename, replacement and unlink. Tests distinguish adapter scopes, full Windows IDs, missing generations, changed objects and unknown identities. Access queries retain their native family/mask and failures; neither mode bits nor readable ACL bytes are treated as effective-operation permission. No implicit elevation or access downgrade is implemented. |
| Destination capability inspection | Held-directory inventory retains native location, access, name-rule, policy/key and limit observations. Destination assessment qualifies owners, identity/kind, query families and bookends. Feature advertisements remain distinct from preservation capability. Unknown, denied, changed and unsupported results cannot become absence or permission. Native tests compare actual APFS, ext4 and ReFS observations; filesystem labels supply no inferred support. |
| Pane sorting, filtering and marks | Both bounded held-entry models and disk-backed indexed views have deterministic ordering, escaped-text filtering and explicit mark/focus semantics. Indexed row identity survives reorder/filter; hidden marks retain counts and block source candidates pending review. Atomic bitmap publication, stale stamps, quota/corruption/cancellation failures and exact selected-source acquisition are tested. |
| Links and collision rules | Links are inspected without traversal; opaque reparse payloads remain opaque. Exact native-name duplicates, existing-object aliases and distinct-name rules are separate evidence. Tests cover hard-link aliases, missing versus unreadable lookups, self-targets, ancestors and name/mapping limits. Only qualified plain extended-filesystem rules establish distinct names; other policies remain unavailable rather than using an invented case/normalization algorithm. |
| Deterministic acceptance coverage | All 82 registered suites execute on each host, with 750/753/749 cases on macOS/Linux/Windows. The review verifies unique executed case names and suite sets against CMake registration. All inventory categories, unavailable states, name boundaries and selection stability have deterministic tests; native fixtures supply separate platform evidence. |
| Native three-platform tests and UI-free boundary | The unchanged final source snapshot passed normal tests on macOS arm64/APFS, Lima Debian 13 arm64/ext4 and Windows x64/ReFS. Fresh 71-unit sanitizer builds passed 211/209/208 focused cases. Architecture checks passed; diagnostic builds were hashed and retired after exact process/fixture checks. See the [destination assessment evidence](evidence/ck-files-destination-assessment-20261024.json). |

## Guarantees and remaining packages

Completion of an inventory means its defined query families were attempted. It
does not prove a complete preservation inventory, stable contents, an atomic
snapshot or authority to modify anything. Each payload's owner and unavailable
state remain part of its contract. Native APIs without a qualified mapping stay
unsupported/unknown; they are not implemented by a shell or pathname fallback.

The native tests qualify specific hosts and features, not every filesystem,
driver or provider. The 100,000-record index tests use synthetic records on real
native cache files; real directory integration currently uses 259 entries.
These limits remain explicit rather than being promoted to release claims.

The normal host configurations execute 789 distinct declared cases in aggregate.
One additional Linux case requires `CK_FILES_REQUIRE_CASEFOLD` and an enabled
filesystem. A fresh 64-unit strict-warning ASan+UBSan build passed 45 focused
cases on a private ext4 image, including required casefold activation and native
lookup. The review accounts for all 790 declared cases across these runs. The
image was unmounted, its loop attachment checked and its workspace removed.

The following requirements remain open in their original packages:

- **WP-02:** the real two-pane application, worker scheduling/publication,
  lossless navigation/completion, properties, bounded preview, histories and
  bookmarks, responsive views and interactive tests. The indexed pane currently
  clears marks on a new dataset; application refresh must preserve qualified
  selections or report invalidation without silently transferring them to
  replacement objects. The bounded held-entry refresh service alone does not
  satisfy large-view application refresh acceptance.
- **WP-03:** recursive frozen inventories, source/destination preservation
  differences, collision decisions, overlap/descendant policies, execution
  revalidation and scoped user consent. Native advertisements and access checks
  do not supply these decisions.
- **WP-04–06:** verified copy and recovery, remaining mutations, complete job
  interaction and suite integration. No production copy/move/delete API or
  enabled mutation command exists at this milestone.
- **WP-07–08:** the full filesystem/provider/fault matrix, complete application
  performance qualification, packaging and real-terminal release acceptance.

No first-release requirement is removed or waived by closing WP-01.
