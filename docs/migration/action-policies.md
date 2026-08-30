# Native action policies

This record captures the accepted scope for the two native actions that cross
from a ckVision controller into the operating system.  It is deliberately
narrow: the policy does not authorize legacy UI code, arbitrary shell access,
or new cloud providers.

## Find custom commands

`ck-find` may execute only a named, allowlisted, non-destructive command
template.  A saved legacy `execCommand` value is not a shell program and never
becomes executable merely by loading it.  It must identify one of the native
allowlisted templates exactly; all other values remain visible for
compatibility and are refused with a clear reason.

The executor must:

- construct direct argv for a fixed absolute executable; it must never invoke
  a shell, command substitution, expansion, aliases, elevated privileges, or
  interactive input;
- pass each matched path as one literal argv value, run from the selected
  search root, and use only a minimal fixed environment;
- apply a real macOS sandbox profile that denies network and write access,
  permits only the selected root plus the fixed executable/runtime reads, and
  is unavailable rather than weakened when the platform sandbox is absent;
- cap output and execution time, honour cancellation by terminating the child,
  limit the number of command invocations, and return an auditable outcome;
- display the fixed executable/argument template before the user starts it.

Custom-command execution is separate from deletion.  A specification that
requests both is refused; deletion retains its existing explicit confirmation
and never shares a process capability with custom commands.

## Disk Usage cloud actions

`ck-du` supports one provider only: macOS iCloud Drive.  Every action applies
to one explicitly selected directory.  The app rejects paths outside the
current scan root, symbolic links, non-directories, special files, and paths
that the provider does not report as eligible iCloud items.  It does not add
an application-level recursive traversal; the provider's own request scope is
reported honestly.

Download is an explicit request.  Local-copy eviction requires a separate
Yes/No confirmation and is refused unless macOS reports that the item has
already uploaded.  A successful result means that macOS accepted the request,
not that a background sync or recursive provider operation has completed.

The service makes no automatic retry.  Cancellation, failure, provider
acceptance, and the selected action are returned as immutable, user-visible
audit outcomes; a user may explicitly choose to retry after dismissing a
failure.  Other platforms and cloud providers remain unsupported.

## Enablement gate

Neither action is enabled by this policy record alone.  The native capability
may be exposed only after its concrete sandbox/provider adapter and acceptance
tests pass: Find tests must cover allowlist refusal, direct-argv planning,
output/time/cancellation handling, and teardown; Disk Usage tests must cover
scope/provider refusal, confirmation, cancellation, explicit retry,
teardown, and unsupported platforms.
