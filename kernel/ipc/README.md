# kernel/ipc

IPC primitives (docs/kernel/ipc/). Anonymous pipes (`pipe.c`): a 16 KiB
byte stream with two kobject ends (`pipe-read`, `pipe-write`), EOF when
the last write end is released, `EPIPE` when the last read end is,
killable waits, atomic writes up to `PIPE_BUF` in the pipe layer.
Futexes (`futex.c`): a wait/wake/requeue keyed by what the word maps.
Unix domain sockets (`unix.c`): the transport behind `COSMO_AF_UNIX` --
stream connections and datagram sockets over bounded queues of sends,
names as filesystem nodes or root-scoped abstract names, `socketpair`,
and handles riding in a message under spawn's transfer rule.
`pipetest.c` is the `ipc-pipe` self-test; `unixtest.c` the six `unix-*`
ones. Later: events, shared memory.
