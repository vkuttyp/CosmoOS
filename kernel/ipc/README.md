# kernel/ipc

IPC primitives (docs/kernel/ipc/). The pipe's ring and anonymous pipes
(`pipe.c`): a 16 KiB byte stream counting its readers and writers, EOF
when the last writer goes, `EPIPE` when the last reader does, killable
waits, atomic writes up to `PIPE_BUF` in the pipe layer; the anonymous
pipe is two kobject ends (`pipe-read`, `pipe-write`) over one ring.
Named pipes (`fifo.c`): the same ring behind a `VNODE_FIFO` node, its
counts the opens of each side, POSIX's open rules, a per-open
non-blocking bit, and the readiness a `struct file` learned for it.
Futexes (`futex.c`): a wait/wake/requeue keyed by what the word maps.
Unix domain sockets (`unix.c`): the transport behind `COSMO_AF_UNIX` --
stream connections and datagram sockets over bounded queues of sends,
names as filesystem nodes or root-scoped abstract names, `socketpair`,
and handles riding in a message under spawn's transfer rule.
`pipetest.c` is the `ipc-pipe` self-test, `fifotest.c` the `ipc-fifo`
one; `unixtest.c` the six `unix-*` ones. Later: events, shared memory.
