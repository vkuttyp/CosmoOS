# IPC: architecture

Phase 9 of the roadmap (a shell needs pipelines). Constitution sections
10 and 11 (kernel objects behind handles), 19 (IPC state belongs to the
process), 63 (`kernel/ipc/`), invariant 11 (explicit ownership and
lifetime) and invariant 14 (validate untrusted input).

## Where it sits

```text
   user process       pipe(h[2]); write(h[1]) in one process, read(h[0]) in another
        │             kernel/syscall/native.c → handle table → pipe end kobjects
        ▼
   kernel/ipc/pipe.c   one buffer, two ends: `pipe_read` and `pipe_write` kobjects
        │
   kernel/object/      kobject refcounts, handle rights (READ on the read end, WRITE on the write end)
   kernel/scheduler/   wait queues (killable waits)
```

A pipe is the first IPC primitive; Phase 11 added the second, the futex
(`kernel/ipc/futex.c`: wait on and wake by a 32-bit user word, keyed by
address space and address; used by the Linux personality, documented
in `docs/compat/linux/api.md` and `design.md`). The directory is where
channels, events and shared memory go later. It is deliberately
the classic Unix pipe: a byte stream, blocking, anonymous, passed to
other processes only by handle inheritance at `spawn`.

## Purpose

Connect the standard output of one process to the standard input of
another without a file in between, with the two termination rules that
make shell pipelines work: a reader sees end of file when every write
end is gone, and a writer fails with `EPIPE` when every read end is
gone.

## Responsibilities

- **`pipe()`**: create a buffer and two handles: `h[0]` reads (right
  READ), `h[1]` writes (right WRITE). Each end is its own kobject so that
  the number of open read ends and write ends is known exactly from the
  reference counts, which is what the termination rules need.
- **Blocking byte stream**: `PIPE_SIZE` (16 KiB) ring. `read` blocks
  until data is available or no write end exists (returns 0). `write`
  blocks until space is available; a write of at most `PIPE_BUF` (4096)
  bytes is atomic (never interleaved with another writer's), larger
  writes may be split. `write` with no read end returns `-EPIPE`.
- **Killable waits**: a blocked reader or writer whose process is killed
  returns `-EINTR` and the process exits.
- **Handle semantics**: `dup` and `spawn` copy handles, the kobject
  reference count follows; closing the last handle to an end releases
  it, which wakes the other side.
- **`fstat`** on either end reports `COSMO_DT_FIFO`.

## Non-responsibilities

- Named pipes (FIFOs in the filesystem), `poll`/
  `select`, `splice`, message boundaries, priorities.
- Channels with typed messages and handle passing, events, shared
  memory, futexes: later entries in this directory.
- Signals on `EPIPE` (`SIGPIPE`): there are no signals; the writer gets
  the error and the shell reports it.

## Interfaces at a glance

| Interface | Header | Used by |
|---|---|---|
| `pipe_create(struct kobject **rd, struct kobject **wr)`, `pipe_stats` | `kernel/pipe.h` | `sys_pipe`, self-tests |
| `pipe_read`/`pipe_write` kobject types with `read`/`write`/`stat` | `kernel/object.h` (`kobject_io_type`) | `sys_read`, `sys_write`, `sys_fstat` |
| `SYS_pipe` (35) | `uapi/cosmo/syscall.h` | libc `pipe()` |

Tests (`testing.md`): a self-test (`ipc-pipe`) moves 1 MiB through a
pipe between two kernel threads with mismatched chunk sizes and checks
the data, EOF after the writer closes, `-EPIPE` after the reader closes,
atomicity of `PIPE_BUF` writes from two writers, and kill of a blocked
reader; `init --selftest` does the same through system calls; the shell
test runs pipelines (`cat /etc/rc | wc`-style through `cat` and `echo`).
