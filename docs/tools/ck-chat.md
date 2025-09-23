# ck-chat

`ck-chat` is the first ck-ai tool. It loads the local AI configuration from
`~/.config/cktools/ckai.toml`, opens the placeholder llama.cpp backend, and
streams a deterministic response so we can validate the runtime plumbing.

## Usage

```bash
ck-chat --prompt "Hello"
```

If no prompt is provided, `ck-chat` will interactively request one on stdin.
