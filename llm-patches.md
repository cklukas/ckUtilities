# LLM Integration Patches

## Adjust llama context batch size
- **Date:** 2025-10-14
- **File:** `lib/ckai_core/src/llm.cpp`
- **Summary:** Set `ctx_params.n_batch` to `max(512, n_ctx)` when initializing the llama context. The previous fixed value of 512 caused llama.cpp to assert (`n_tokens_all <= cparams.n_batch`) once the accumulated prompt plus generated tokens exceeded 512, leading to crashes during longer conversations.
- **Reasoning:** Using the context window size as the batch upper bound keeps the replay batch large enough for any prompt that fits in the configured context, eliminating premature assertion failures without reducing performance on smaller contexts.
