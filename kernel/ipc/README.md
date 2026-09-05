# kernel/ipc

IPC primitives (docs/kernel/ipc/). Today anonymous pipes (`pipe.c`): a
16 KiB byte stream with two kobject ends (`pipe-read`, `pipe-write`),
EOF when the last write end is released, `EPIPE` when the last read end
is, killable waits, atomic writes up to `PIPE_BUF` in the pipe layer.
`pipetest.c` is the `ipc-pipe` self-test. Later: channels, events,
shared memory, futex-like waits.
