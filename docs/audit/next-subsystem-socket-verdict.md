# NEXT SUBSYSTEM — the verdict a socket records and nothing can ask for

Date: 2026-09-17. Tree: `main` at c47d353 (after PR #169, the guest's
half of the `net-harness` instrument). Chosen from
`docs/audit/2026-09-deferred-work-inventory.md` §1.1 and §3.

**Subsystem: the pending error. TCP writes one at six sites with four
distinct errnos and it is readable only as the return of the next
`send` or `recv`; `struct socket` has a second field for the same
purpose that five sites test, one function sets, and nothing calls.**

This report takes up two inventory rows. §1.1's *"ICMP errors for a UDP
flow in the host's reply state (**no consumer exists yet**)"* is the
honest one: the consumer is what this unit builds. §3's `net-harness`
row is advanced, not closed — its next step, written into PR #169's
report as "read the socket's pending error rather than inferring it from
a machine-wide counter", is a thing no caller in this tree can do.

## Problem

The stack knows exactly why a connection died. `tcp_input` sets
`pcb->error` to `-ECONNREFUSED` when a reset answers its own SYN
(`tcp.c:1920`), to `-ECONNRESET` or `-ECONNREFUSED` when it accepts a
reset on an open connection (`tcp.c:2010`), to `-ETIMEDOUT` when
retransmission gives up (`tcp.c:1030`, `:1074`), and to `-EPERM` when the
host firewall refuses the flow (`tcp.c:964`, `:987`) — six sites, four
errnos. `struct socket` carries a second field
for the same purpose:

```c
/* kernel/include/kernel/socket.h:38 */
int error;                  /* pending asynchronous error, consumed by the next call */
```

Five facts about that field and that verdict, each checkable in a
minute, and together they are this unit:

### 1. `struct socket::error` is never set

`sock_set_error(s, err)` is declared in `socket.h:70`, defined at
`socket.c:138` — and has **no callers anywhere in the tree**:

```
$ grep -rn sock_set_error kernel kernel-services
kernel/include/kernel/socket.h:70:void sock_set_error(struct socket *s, int err);   /* and wake */
kernel-services/network/socket.c:138:void sock_set_error(struct socket *s, int err)
```

Five sites test `s->error` and act on it — `ksock_accept` twice
(`socket.c:222`, `:230`), the UDP receive path twice (`:371`, `:375`),
and `ksock_ready` (`:463`) — and every one of them is dead code reached
only if something sets a field nothing sets. The field is not vestigial:
its comment names the thing this unit is about, and §1.1's inventory row
says why it is empty. Nothing yet produces an asynchronous error.

### 2. Behind the dead setter is a loaded gun

```c
/* kernel-services/network/socket.c:222-225, in ksock_accept */
if (s->error || (s->shut & 1)) {
    ksock_put(c);
    return take_error(s) ? take_error(s) : -EINVAL;
}
```

`take_error` **clears as it reads** (`socket.c:144-151`): it returns
`s->error` and zeroes it, falling back to `s->tcp->error` only when
`s->error` was already zero. So the expression above calls it twice on
purpose-built sand. With `s->error` set and no TCP error pending:

| call | `s->error` before | returns | `s->error` after |
| --- | --- | --- | --- |
| first `take_error(s)` | `-ECONNABORTED` | `-ECONNABORTED` (truthy) | `0` |
| second `take_error(s)` | `0` | **`0`** | `0` |

`ksock_accept` returns **0** — success — with `*out` never assigned. The
caller reads an uninitialised `struct socket *` and installs it in a
handle table. This is unreachable today for exactly one reason: finding
1. The first caller of `sock_set_error` makes it live, which is why this
unit builds the writer and the fix together, and proves the fix with a
test rather than by reading.

### 3. Nothing consumes an inbound ICMP error

`icmp_input` (`ipv4.c:374`) handles three things: fragmentation-needed,
echo, and echo-reply. A destination-unreachable, a time-exceeded or a
parameter-problem for a live flow of this host's is dropped on the floor
at the end of the function.

This is the §1.1 row, and it is not for want of machinery. The
fragmentation-needed path already does the whole job for TCP:

```c
/* kernel-services/network/ipv4.c:341-372, abridged */
uint8_t quote[sizeof(struct ipv4_hdr) + 8];
if (!m_copydata(m, sizeof(*ic), sizeof(quote), quote)) return;
const struct ipv4_hdr *q = (const struct ipv4_hdr *)quote;
if ((q->vhl >> 4) != 4 || ihl < 20 || !netif_owns_ipv4(q->src)) return;
...
local.v4 = q->src;  remote.v4 = q->dst;
local.port = th[0] << 8 | th[1];  remote.port = th[2] << 8 | th[3];
if (tcp_pmtu_notify(&local, &remote, seq, mtu))
    ipv4_pmtu_update(q->dst, mtu);   /* confirmed by a live connection */
```

The quoted header is parsed, the source is checked to be an address this
host owns, the four-tuple is rebuilt, and the result **changes nothing
unless a live connection confirms it** — the RFC 5927 discipline this
tree already names at `ipv4.c:687` and states as invariant N18. A UDP
sibling of `tcp_pmtu_notify` is the missing consumer, and it inherits
that discipline rather than inventing one.

The asymmetry is worth saying plainly: this stack **sends** ICMP
port-unreachable (`udp.c:290`) and has never **received** one.

### 4. The verdict is unreadable without doing I/O

`SYS_ioready` (`syscall.h:88`) reports `COSMO_IO_ERROR`
(`syscall.h:453`), and for TCP it is honest — `tcp_ready` raises it from
`pcb->error` (`tcp.c:1403`, `:1411`). So a caller can learn **that** the
connection is broken. To learn **what** broke it, the only path is to
call `send` or `recv` and read the return.

For a blocking program that is usually enough. For the two cases that
matter it is not:

- **A non-blocking connect.** `ksock_connect` returns `-EINPROGRESS`
  (`socket.c:278`). The POSIX idiom from there is `poll`, then
  `getsockopt(SO_ERROR)`. There is no `SO_ERROR` here. A caller must
  instead call `connect` again and decode `-EISCONN` as success
  (`socket.c:265`) — which works, and is not what any ported program
  does.
- **An instrument.** PR #169's `net-harness` line wanted the pending
  error and could not have it, so it printed `rsts_in`, a machine-wide
  per-boot counter, and the report had to spend two paragraphs on why
  `+1` does not bind the reset to that pcb and `+0` does not mean no
  reset arrived. **This pull request's own CI made that concrete**: on
  this documentation-only branch the x86-64 job printed `sent 12,
  recv -104 ... (outstanding 12 then 12), segs_out +1, retransmits +0,
  rsts_in +1` — the first sighting where the guest got the bytes onto
  the wire. Whether `retransmits +0` means a retransmission bug or
  simply a reset that arrived before the timer fired is exactly the
  question `pcb->error` and a timestamp would answer and a machine-wide
  counter cannot (`docs/testing/flakes.md`, "The count").

Neither ABI offers it. The native syscall table has no socket-option
call at all (`SYS_socket` 23 through `SYS_getsockname` 31,
`syscall.h:45-53`). The Linux personality has both entry points and
answers neither:

```c
/* compat/linux/syscalls.c:1885-1901 */
static int64_t lx_setsockopt(struct syscall_args *a)
{
    struct socket *s = sock_of((int)a->a[0], HANDLE_RIGHT_MANAGE);
    if (s == NULL) return -EBADF;
    ksock_put(s);
    return (int)a->a[1] == LX_SOL_SOCKET ? 0 : -ENOPROTOOPT;
}

static int64_t lx_getsockopt(struct syscall_args *a)
{
    ...
    return -ENOPROTOOPT;
}
```

`lx_getsockopt` refuses everything, which is at least true.
**`lx_setsockopt` returns 0 for every `SOL_SOCKET` option while doing
nothing**, which is worse than refusing: a program that sets
`SO_RCVTIMEO` is told it took effect and then blocks forever; one that
sets `SO_RCVBUF` believes it has a buffer it does not have. The tree
asserts this behaviour on purpose —

```c
/* tests/linux/lxtest.c:909 */
CHECKV(sc6(LX_setsockopt, s, LX_SOL_SOCKET, 2, &one, 4, 0) == 0, 0);   /* SO_REUSEADDR: accepted */
```

— so changing it is an ABI decision this report has to argue, not a
typo to fix quietly. The argument is in Design §4.

### 5. And the field has no locking rule

The first draft of this report proposed that the new accessor take
`s->lock`. Greptile's review of that draft caught it, and the reason is
worth keeping, because it is a fifth finding rather than a slip in the
fourth.

`take_error` is called five times, and the mutex state differs:

| call site | `s->lock` |
| --- | --- |
| `ksock_accept`, `socket.c:224` | **not held** — `ksock_accept` never takes it |
| `ksock_connect`, `socket.c:267`, `:282`, `:299` | **held** — all three completion paths run inside `mutex_lock(&s->lock)` at `:248` |
| `ksock_recvfrom` (UDP), `socket.c:372` | **not held** |

And the writer holds nothing at all:

```c
/* kernel-services/network/socket.c:138-142 */
void sock_set_error(struct socket *s, int err)
{
    s->error = err;
    sock_wake(s);
}
```

which is deliberate — the header groups it under *"Protocol side: wake
every waiter on the socket (**any context**)"* (`socket.h:68-70`). A
protocol writes this field from packet-receive context, where `s->lock`
is a **mutex** and cannot be taken at all.

So an accessor that takes the mutex is wrong twice: it would recurse on
a non-recursive mutex in the three `ksock_connect` paths, and it could
never be paired with the writer this unit adds, which runs in
`icmp_input`. The field is currently safe only because nothing writes
it. Design §1 settles the rule before the writer exists rather than
after, which is the lesson three earlier units in this repository paid
for one site at a time.

## Why it matters

- **It is one rule broken in four places, and the rule is this
  repository's own.** A verdict that is recorded and unreadable is the
  same defect class as the last four units: PR #164's lock rule stated
  in a document and checked nowhere, PR #162's structure size asserted
  in a comment and by no compiler, PR #167 and #169's failures that knew
  what went wrong and printed something else. Here the stack computes
  the exact errno and then offers the caller a poll bit that says
  "something".
- **The accept path is a real bug with a real trigger.** Not a style
  finding: `return take_error(s) ? take_error(s) : -EINVAL` returns
  success with an unassigned out-parameter, and the only thing standing
  between it and a caller is that nobody has yet called the setter the
  header exports for exactly that purpose. Building the writer without
  fixing the reader would ship the bug.
- **It is the `net-harness` chase's next step, made general.** Three
  units have now narrowed that row, each by making something legible.
  The next narrowing needs the pending error, and adding a private
  peephole to `nettest.c` for it would be the fourth one-off in a row.
  `SO_ERROR` is the general answer, and the instrument becomes a caller
  of it.
- **A connected UDP socket cannot fail.** Send to a closed port on a
  reachable host and this stack waits forever. That is not an exotic
  case; it is the first thing any UDP client does wrong.

## Design

### 1. One reader: `ksock_error`

```c
/* kernel/include/kernel/socket.h */
/* The pending asynchronous error, read once: returns it and clears it,
 * as SO_ERROR does. 0 when there is none. Takes no lock -- see N21 --
 * so it is safe with or without s->lock held, and against a writer in
 * packet context. */
int ksock_error(struct socket *s);
```

**The access rule comes first** (Problem §5): the field is written from
packet-receive context and read with the socket mutex held at three of
five sites and not held at the other two. A mutex cannot serve both, so
the field takes no lock — it becomes an atomic word, and the read-once
semantic *is* the atomic operation:

```c
/* kernel-services/network/socket.c */
int ksock_error(struct socket *s)
{
    int e = __atomic_exchange_n(&s->error, 0, __ATOMIC_ACQ_REL);
    if (e == 0 && s->tcp)
        e = __atomic_load_n(&s->tcp->error, __ATOMIC_RELAXED);
    return e;
}
```

`sock_set_error` stores with `__ATOMIC_RELEASE` and keeps its "any
context" contract. The exchange makes read-and-clear indivisible, so two
readers cannot both be told the same error — a property the present
`take_error` does not have and that no amount of mutex at the call sites
would give it, since two of the five do not hold one. `tcp.c:956`
already reads `pcb->error` this way, so the convention exists.

`take_error` becomes this function's body and every present caller calls
it, with or without the mutex — the point of taking none. The
double-call at `socket.c:224` disappears by construction: there is no
expression in which calling it twice is spellable once the value is
bound:

```c
if (s->error || (s->shut & 1)) {
    ksock_put(c);
    int e = ksock_error(s);
    return e ? e : -EINVAL;
}
```

Read-and-clear is the semantic to match, and it is worth naming the
alternative rejected: TCP's `pcb->error` is **sticky** — `tcp_send`
returns it and leaves it (`tcp.c:1260-1262`) so every subsequent call
fails the same way, which is right for a dead connection. `SO_ERROR`
reports and clears. `ksock_error` therefore clears `s->error` and
*reports without clearing* `s->tcp->error`, which is what `take_error`
already does and what leaves both behaviours correct. The invariant
below says so, because a reader will otherwise assume one rule.

### 2. One writer: ICMP destination-unreachable for a connected UDP socket

A sibling of `tcp_pmtu_notify`, taking the same rebuilt four-tuple from
the same quoted header:

```c
/* kernel/include/kernel/net/udp.h */
/* An ICMP error quoting this flow. True if a connected socket owned it. */
bool udp_error_notify(const struct netaddr *local, const struct netaddr *remote, int err);
```

`icmp_input` gains one branch, after the `M_FW_QUIET` check so a refused
message still changes nothing, parsing the quote exactly as
`icmp_needfrag` does and dispatching on `q->proto`:

| ICMP type / code | errno | delivered to |
| --- | --- | --- |
| dest-unreach / port (3/3) | `-ECONNREFUSED` | a **connected** UDP pcb whose four-tuple matches |
| dest-unreach / host, net, proto (3/0,1,2) | `-EHOSTUNREACH`, `-ENETUNREACH` | the same |
| dest-unreach / needfrag (3/4) | — | unchanged: `icmp_needfrag`, which runs first |
| anything quoting IPPROTO_TCP | — | **unchanged**, and Design §5 says why |
| anything else | — | dropped, as today |

`udp_error_notify` walks `g_pcbs` under the UDP lock exactly as
`udp_input`'s demux does (`udp.c:189`), requires `pcb->remote` to be
**specified and equal** to the quoted destination as well as `local`
matching, and calls `sock_set_error(pcb->sock, err)` — giving that
function its first caller and `s->error` its first writer.

"Connected only" is the whole safety argument and is not a
simplification: an unconnected UDP socket has no flow for the message to
be about, and accepting one would let any host on the path kill a
socket by quoting a plausible port. With the four-tuple required, an
off-path attacker must guess both ports and both addresses — the same
bar invariant N16 sets for a TCP reset, and the reason N18 exists.

### 3. One caller-visible answer: `SO_ERROR`, through one door

Adding it to the Linux personality alone would repeat a mistake this
repository has a note about: a rule enforced in one personality is
half-enforced, because `compat/linux` and `native.c` reach the same
objects. So the kernel path is the shared one and both ABIs are thin:

- **native**: `SYS_getsockopt` **92** — `(int h, int level, int opt,
  void *val, size_t *len) -> 0`. `SYS_COUNT` 92 → **93**.
- **Linux**: `lx_getsockopt` stops returning `-ENOPROTOOPT`
  unconditionally and forwards `SOL_SOCKET`/`SO_ERROR` to the same
  `ksock_error`.

One option is defined by this unit and the surface is deliberately that
small: `COSMO_SOL_SOCKET` / `COSMO_SO_ERROR`, value an `int`, positive
errno as POSIX specifies (so the kernel's `-ECONNRESET` is returned to
userland as `104`). Every other option is `-ENOPROTOOPT`, which is a
true statement about this stack. A door with one thing behind it is
better than a door that lies, which is the next point.

There is no `SYS_setsockopt`. Nothing about a socket is settable today
at all: non-blocking mode is chosen at creation, by ORing
`COSMO_SOCK_NONBLOCK` into the type (`syscall.h:443`, `native.c:787`),
and the native ABI has no runtime toggle for it — only the Linux door
has one, through `fcntl(F_SETFL)` (`syscalls.c:758`). A setter with an
empty option table would be the same empty promise this unit is removing
from the Linux side, and the asymmetry it would paper over — a native
socket cannot change its blocking mode after creation — is a separate
gap that a fake `setsockopt` would hide rather than fix.

### 4. `lx_setsockopt` stops saying yes

```c
return (int)a->a[1] == LX_SOL_SOCKET ? 0 : -ENOPROTOOPT;
```

becomes `-ENOPROTOOPT` for every option, with a table of the ones
implemented — which, after this unit, is none for `set`. **This changes
an ABI a test asserts** (`lxtest.c:909`), and the report states the
choice rather than burying it:

- *Keeping 0* means a ported program's `SO_RCVTIMEO`, `SO_SNDTIMEO`,
  `SO_LINGER`, `SO_KEEPALIVE` and `SO_RCVBUF` all appear to succeed and
  none of them do. The failure surfaces later, somewhere else, as a hang
  or as unbounded memory use.
- *Returning `-ENOPROTOOPT`* is what Linux itself returns for an option
  a protocol does not implement, so a program's existing error path
  handles it. `SO_REUSEADDR` is the one real loss — programs set it and
  check — and it is a loss of a **lie**: this stack does not implement
  address reuse, and a program that believes it does will fail its next
  `bind` with `-EADDRINUSE` and no idea why.

`lxtest.c:909` is rewritten to assert the refusal, with a comment
naming this report. If the Linux suite later needs `SO_REUSEADDR` to
succeed, the way to get there is to implement address reuse.

### 5. What this unit does not do

- **It does not deliver ICMP errors to TCP.** RFC 5927 §4.2 is about
  precisely this, `tcp_pmtu_notify`'s "confirmed by a live connection"
  discipline is why the path-MTU path is safe, and a hard error
  (`ECONNREFUSED`, `EHOSTUNREACH`) applied to an established connection
  is a bigger decision than a report should make in passing: RFC 1122
  §4.2.3.9 says a host **must not** abort a connection on a soft error,
  and getting that wrong hands an off-path attacker a connection-killer.
  TCP already has `pcb->error` from the segments themselves, which is
  the evidence this stack trusts. Named as the next unit.
- **It does not add a general option table.** One option, one direction.
- **It does not promise to close the `net-harness` row.** It gives that
  row's next step a way to be taken. PR #169's report learned not to
  promise more, and PR #167's learned it before that.

## Current implementation

| what | where | state |
| --- | --- | --- |
| `struct socket::error` | `kernel/include/kernel/socket.h:38` | declared, commented, never written |
| `sock_set_error` | `kernel-services/network/socket.c:138` | defined, zero callers |
| `take_error` | `socket.c:144` | read-and-clear; called twice in one expression at `:224` |
| `pcb->error` | `tcp.c:964`, `:1030`, `:1074`, `:2010` | written on firewall refusal, timeout, reset |
| `COSMO_IO_ERROR` | `tcp_ready`, `tcp.c:1403`, `:1411` | raised from `pcb->error`; honest, and says nothing about which error |
| `icmp_input` | `ipv4.c:374` | needfrag, echo, echo-reply; everything else dropped |
| `icmp_needfrag` | `ipv4.c:341` | the quoted-header parse this unit reuses |
| `udp.c:290` | `icmp_send_unreach` | this host sends them and cannot receive them |
| `lx_setsockopt` | `compat/linux/syscalls.c:1885` | returns 0 for every `SOL_SOCKET` option, does nothing |
| `lx_getsockopt` | `compat/linux/syscalls.c:1894` | `-ENOPROTOOPT` for everything |
| native sockopt | — | no syscall exists |

## Affected files

| file | change |
| --- | --- |
| `kernel/include/kernel/socket.h` | `ksock_error`; `error` becomes an atomic word with the access rule in its comment; `sock_set_error`'s comment gains its caller |
| `kernel-services/network/socket.c` | `take_error` → `ksock_error` (exported, **lock-free**: an atomic exchange, callable with or without `s->lock`); `sock_set_error` stores with release; the `:224` double call bound to a variable; `ksock_ready`'s `s->error` branch now reachable |
| `kernel/include/kernel/net/udp.h` | `udp_error_notify` |
| `kernel-services/network/udp.c` | the four-tuple walk over `g_pcbs`, connected-only, `sock_set_error` |
| `kernel-services/network/ipv4.c` | `icmp_input`: the dest-unreach branch, after `M_FW_QUIET`, sharing `icmp_needfrag`'s parse |
| `kernel/include/uapi/cosmo/syscall.h` | `SYS_getsockopt` 92, `SYS_COUNT` 93, `COSMO_SOL_SOCKET`, `COSMO_SO_ERROR` |
| `kernel/syscall/native.c` | the entry: handle, rights, `ksock_error`, positive errno out |
| `compat/linux/syscalls.c` | `lx_getsockopt` forwards `SO_ERROR`; `lx_setsockopt` stops returning 0 for what it does not implement |
| `kernel-services/network/nettest.c` | the new tests; and `net-harness` reads the pending error, with `tcp_get_stats` sampled **before** the connect |
| `userland/init/init.c` | `net_selftest`: the UDP and non-blocking-connect cases through the native syscall |
| `tests/linux/lxtest.c` | `:909` asserts the refusal; `SO_ERROR` through the Linux door |
| `docs/kernel-services/network/invariants.md` | N21 |
| `docs/kernel-services/network/api.md`, `design.md`, `testing.md` | the new calls, the ICMP consumer, the tests |
| `docs/kernel/syscalls.md` (and the ABI table) | `SYS_getsockopt` |
| `docs/audit/2026-09-deferred-work-inventory.md` | §1.1's ICMP row struck; §3's `net-harness` row's next step taken |
| `README.md` | the Status entry |

## New APIs

- `int ksock_error(struct socket *)` — kernel. Atomic read-and-clear; safe with or without `s->lock`, and safe against a writer in packet context.
- `bool udp_error_notify(const struct netaddr *local, const struct netaddr *remote, int err)` — kernel. True if a connected socket owned the flow.
- `SYS_getsockopt` (92) — native ABI. `SYS_COUNT` 92 → 93.
- `COSMO_SOL_SOCKET`, `COSMO_SO_ERROR` — uapi.

Nothing is removed. `lx_setsockopt`'s return value changes, which is the
one compatibility break and is argued in Design §4.

## Invariant

**N21. A socket's pending error is delivered once, to one reader, and is
never invented.** `s->error` is written only by `sock_set_error` and
read only by `ksock_error`, both with atomic operations and **neither
holding `s->lock`** — the writer runs in packet-receive context where
that mutex cannot be taken, and two of the five readers do not hold it
(Problem §5). The read is an exchange, so a verdict is delivered to
exactly one caller; `s->tcp->error` is reported without clearing,
because a dead connection must keep failing. An ICMP message sets an
error only when it quotes a four-tuple a **connected** socket of this
host owns, so a caller that is told `ECONNREFUSED` was told so by a
message about its own flow. Check: `net-sockerr-udp`,
`net-sockerr-spoof`, `net-sockerr-accept`, `net-sockerr-once`, and
`net-sockerr-locking`, which calls the accessor from a path holding the
mutex and a path that does not — the one property a `lockdep_assert_*`
cannot state, because both ways are correct here and the assertion would
have to be absent in either direction. Gap: TCP takes no ICMP hard error at all, by decision
(Design §5), so an errno that only ICMP could supply never reaches a
stream socket.

## Migration plan

1. `ksock_error` with the atomic access rule, the `:224` fix, and
   `net-sockerr-accept` — the bug first, with the writer stubbed by the
   test calling `sock_set_error` directly, so the fix is proved before
   anything depends on it. The test exercises `ksock_error` from a
   caller holding `s->lock` and one that does not, because that split is
   the reason the field takes no lock.
2. `udp_error_notify` and the `icmp_input` branch, with
   `net-sockerr-udp` and `net-sockerr-spoof`.
3. `SYS_getsockopt`, the uapi constants, `native.c`, and the Linux
   forward; `lx_setsockopt`'s refusal and `lxtest.c:909`.
4. `net-harness` reads the pending error and samples the counters before
   the connect — the `net-harness` row's next step, which is the reason
   this unit is worth doing now rather than later.
5. Docs: N21, api/design/testing, the syscall table, both inventory
   rows, the README entry.

## Tests

| test | claim | how it fails if the change is reverted |
| --- | --- | --- |
| `net-sockerr-accept` | a listening socket with a pending error makes `accept` return that error, not 0 | reverted to `take_error(s) ? take_error(s) : -EINVAL`, it returns **0** with `*out` unassigned — the test checks the return *and* that no socket was produced, because a bug that returns success is not caught by checking the errno |
| `net-sockerr-udp` | a connected UDP socket that sends to a closed loopback port learns `ECONNREFUSED` | without the `icmp_input` branch the socket blocks and the datagram is dropped; the test sends, waits for `COSMO_IO_ERROR` on `ksock_ready`, then reads the errno |
| `net-sockerr-spoof` | an ICMP unreachable quoting a flow this host does not own, or quoting the right ports with the wrong peer, changes nothing | without the four-tuple check the error lands on a live socket; the test builds both messages (`nettest.c:3455-3462` already builds exactly this message — an ICMP port-unreachable quoting a UDP flow — for the NAT tests) and checks the socket is untouched |
| `net-sockerr-once` | the error is read once: a second `ksock_error` is 0, while a TCP `send` on a reset connection keeps failing | without the atomic exchange the first assertion fails; without the sticky `pcb->error` the second does |
| `net-sockerr-locking` | `ksock_error` answers the same from a caller holding `s->lock` and one that does not | with the accessor taking the mutex, the held-lock case recurses on a non-recursive mutex and the kernel stops — which is what the first draft of this report proposed |
| `usertest` (`init --selftest`) | the native `SYS_getsockopt` reports a refused connect's errno as a **positive** number | reverted, `-ENOSYS`; with the sign wrong, `104` vs `-104` is the assertion |
| `lxtest` | `setsockopt` of an unimplemented option is `-ENOPROTOOPT`; `getsockopt(SO_ERROR)` works | reverted, `setsockopt` returns 0 for an option that does nothing |

Every one of these is deterministic on loopback. None of them depends on
the `net-harness` failure reproducing, which is the point: the unit is
testable whether or not the defect it was motivated by appears.

## Benchmarks

None. `ksock_error` is one atomic exchange and, at most, one relaxed
load — no lock, so it adds no contention to the paths that call it and
none to the packet-receive path that writes the field. The
`icmp_input` branch runs only for ICMP type 3, which
this host currently receives at a rate of zero. `SYS_getsockopt` is a
handle lookup and a load.

## Risks

- **An ICMP error is an off-path attacker's lever** (RFC 5927). The
  mitigation is the connected-four-tuple requirement and nothing else,
  which is the same bar N16 sets for a reset and N18 for a path-MTU
  message. It is stated as the invariant rather than as a comment
  because that is what the last four units were about.
- **`lx_setsockopt`'s refusal may break a guest program** that treats a
  failed `SO_REUSEADDR` as fatal. That is the trade in Design §4, taken
  deliberately: the alternative is that every option silently does
  nothing.
- **`SYS_COUNT` 92 → 93 is an ABI addition**, and the syscall-count
  assertions and the filter's table sizes move with it. PR #87 did the
  same for `SYS_fsync` and is the shape to follow.
- **The `net-harness` failure may not recur while this unit is open.**
  It is one boot in twenty-one locally (`docs/testing/flakes.md`, "The
  count") and it appeared on two of PR #169's own CI jobs, so it is not
  unlikely — but step 4 is an instrument, not a proof, and nothing in
  the test table depends on it firing.

## Alternatives considered

- **A private accessor in `nettest.c`.** Smallest possible change, and
  the fourth one-off instrument in a row for this row. It would also
  leave findings 1, 2 and 3 exactly where they are.
- **`SO_ERROR` in the Linux personality only.** Half a door. The
  repository's own experience is that `compat/linux` and `native.c`
  reach the same objects, so a capability in one is a gap in the other.
- **Deliver ICMP errors to TCP as well.** Larger, security-sensitive,
  and RFC 1122 §4.2.3.9 forbids the obvious implementation. It is the
  next unit, not part of this one.
- **Make `pcb->error` read-and-clear too, for symmetry.** Wrong: a
  connection that has been reset must fail every subsequent call, and
  clearing would make the second `send` return `-EPIPE` — a different
  and less informative errno — or worse, succeed into a dead pcb.
