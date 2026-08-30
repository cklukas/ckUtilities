# Native ckVision executables

Every configured product links the installed ckVision SDK and uses its
production command name.

| Product | Command | Primary verification |
| --- | --- | --- |
| Utilities launcher | `ck-utilities` | launcher lifecycle and staged child discovery |
| JSON view | `ck-json-view` | provider-backed tree scenario and archive smoke |
| Find | `ck-find` | guided-search, sandboxed action, and teardown scenarios |
| Disk usage | `ck-du` | scan, provider, and cloud-policy scenarios |
| Config | `ck-config` | option/keymap persistence scenarios |
| Edit | `ck-edit` | document transaction and file-safety scenarios |
| Chat | `ck-chat` | streaming, cancellation, and model-service scenarios |

`verify_ckvision_install` stages each command and runs `--help`.
`verify_ckvision_cutover` checks installed linkage and public headers for
non-native UI artifacts. `verify_ckvision_archive` repeats product checks after
extracting the delivered archive, while `verify_ckvision_terminal` exercises
the staged commands in three real terminal profiles.

The exact SDK identity and CI input contract are recorded in
[the baseline](ckvision-baseline.md). The package architecture and remaining
release work are recorded in [the roadmap](../../tv_to_ckvision.md).
