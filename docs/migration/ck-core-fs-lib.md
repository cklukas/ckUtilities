# ck-core-fs-lib extraction

The reusable filesystem backend now lives in the sibling project
[`ck-core-fs-lib`](https://github.com/cklukas/ck-core-fs-lib). Its canonical
local checkout is `/Volumes/PRO-BLADE/Development/ck-core-fs-lib`; CMake uses
`CK_CORE_FS_LIB_SOURCE_DIR` to select another checkout or an installed source
tree.

The ckUtilities frontend remains here and consumes the sibling's C++ targets:

- `ck_files_core` — the headless identity, metadata, inventory, planning, and
  operation implementation;
- `ck_files_platform` — platform-specific filesystem and journal adapters; and
- `ck_file_engine_c` — the exception-free opaque-handle C ABI for CDE and other
  non-C++ consumers.

The library is LGPL-2.0-or-later. ckUtilities remains GPL-3.0-or-later. CDE
should reference the public repository and installed C ABI rather than copying
backend sources or reopening paths in its adapter.

The library's documentation is built by `ckdocs` and published at
<https://cklukas.github.io/ck-core-fs-lib/> after its CI workflow succeeds.
