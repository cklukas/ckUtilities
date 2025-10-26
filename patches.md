# LLM Integration Patches

## Adjust llama context batch size
- **Date:** 2025-10-14
- **File:** `lib/ckai_core/src/llm.cpp`
- **Summary:** Set `ctx_params.n_batch` to `max(512, n_ctx)` when initializing the llama context. The previous fixed value of 512 caused llama.cpp to assert (`n_tokens_all <= cparams.n_batch`) once the accumulated prompt plus generated tokens exceeded 512, leading to crashes during longer conversations.
- **Reasoning:** Using the context window size as the batch upper bound keeps the replay batch large enough for any prompt that fits in the configured context, eliminating premature assertion failures without reducing performance on smaller contexts.

## Turbo Vision ncurses include normalization
- **Date:** 2025-10-26
- **File:** `_deps/tvision-src/source/CMakeLists.txt` (applied via `scripts/apply_patches.sh`)
- **Summary:** Normalize the detected ncurses include directory so both the parent and `ncursesw` subdirectory are added to `target_include_directories`, and clean up the temporary variable afterwards.
- **Reasoning:** When Homebrew or SDK headers live under `include/ncursesw`, Turbo Vision previously only added the subdirectory, causing the generated `ncurses.h` to reference `ncursesw/ncurses_dll.h` without exposing the parent include path. The patched CMake logic fixes macOS builds and keeps the change idempotent across build directories.
