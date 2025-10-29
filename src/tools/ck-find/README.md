# Debug

```bash
lldb \
  -o 'settings set target.env-vars TERM=xterm-256color' \
  -o 'process handle SIGTTOU -s false -n false -p false' \
  -o 'process handle SIGTTIN -s false -n false -p false' \
  -o 'process handle SIGTSTP -s false -n false -p false' \
  -o 'process launch --tty /dev/ttys008' \
  -- ./build/dev/bin/ck-find
```

