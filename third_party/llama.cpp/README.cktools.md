# llama.cpp (stubbed vendor)

This directory vendors the llama.cpp backend at a pinned revision. For the Phase 0
scaffolding we ship a **minimal stub** that exposes the build target and headers
expected by the ck-ai runtime while we finish integrating the full upstream.

* Upstream repository: <https://github.com/ggerganov/llama.cpp>
* Pinned commit: `placeholder-stub-phase0`

When replacing the stub with the full source:

1. Drop the upstream files into this directory (no git submodules).
2. Update this README with the actual commit hash.
3. Reapply local patches from `third_party/patches/llama.cpp/` if needed.
4. Ensure the `llama_cpp` CMake target continues to build statically and
   installs headers + archives consumed by `libckai_core`.

The stub keeps the build green until we complete the full vendor step.
